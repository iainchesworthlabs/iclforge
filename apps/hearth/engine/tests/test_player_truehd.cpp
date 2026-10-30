#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "iclforge/render/layout.hpp"
#include "diagnostics_report.hpp"
#include "engine_thread.hpp"
#include "item_source.hpp"
#include "pcm_sink.hpp"
#include "player.hpp"
#include "session.hpp"
#include "stream_decoder.hpp"
#include "truehd_test_support.hpp"

// TrueHD through the player and the session (planning/truehd-parity-hearth-player.md, H1 and H2):
// a queue of TrueHD items plays to a fake device and ends, two of them join without a gap at the
// expected sample, a seek lands in the unit that holds the place asked for, and a unit that does
// not read plays as silence until the stream's next restart. The streams are the library's
// encoder's, so the PCM that went in is the oracle: the codec is lossless.

namespace {

using namespace iclforge::hearth;
using namespace iclforge::hearth::testing;
namespace th = iclforge::truehd;

// A stereo room whose slots are the stream's channels, L then R.
iclforge::render::OutputLayout stereo_room() {
    const std::array<th::Channel, 2> channels{th::Channel::kL, th::Channel::kR};
    return layout_of(channels);
}

// A device whose clock the test runs, recording what it was given.
class FakeDevice final : public PcmSink {
public:
    struct Log {
        std::vector<std::vector<float>> kept{};
        std::uint64_t submitted = 0;
        std::uint64_t heard = 0;
        std::uint64_t clock = 0;
        std::uint32_t opens = 0;
        std::uint32_t flushes = 0;
        bool open = false;
    };
    FakeDevice(std::shared_ptr<Log> log, std::size_t capacity)
        : log_(std::move(log)), capacity_(capacity) {}

    std::expected<OpenOutputFormat, std::string> open(const Format& format) override {
        ++log_->opens;
        log_->open = true;
        log_->submitted = log_->heard = log_->clock = 0;
        for (auto& slot : log_->kept) {
            slot.clear();
        }
        width_ = static_cast<std::uint16_t>(format.layout.slots());
        return OpenOutputFormat{.sample_rate = format.sample_rate,
                                .channels = width_,
                                .mode = OutputMode::kLocalPcm};
    }
    void close() override { log_->open = false; }
    [[nodiscard]] bool is_open() const override { return log_->open; }
    bool submit(std::span<const std::span<const float>> slots, std::size_t frames) override {
        if (!log_->open || log_->submitted - log_->heard + frames > capacity_) {
            return false;
        }
        REQUIRE(slots.size() == width_);
        log_->kept.resize(std::max(log_->kept.size(), slots.size()));
        for (std::size_t slot = 0; slot < slots.size(); ++slot) {
            REQUIRE(slots[slot].size() == frames);
            log_->kept[slot].insert(log_->kept[slot].end(), slots[slot].begin(), slots[slot].end());
        }
        log_->submitted += frames;
        return true;
    }
    [[nodiscard]] std::optional<iclforge::audio::MonitorPosition> position() const override {
        if (!log_->open) {
            return std::nullopt;
        }
        return iclforge::audio::MonitorPosition{.frames_played = log_->clock,
                                                .frames_queued = log_->submitted - log_->heard,
                                                .latency_frames = 0};
    }
    void flush() override {
        ++log_->flushes;
        log_->submitted = log_->heard = log_->clock = 0;
        for (auto& slot : log_->kept) {
            slot.clear();
        }
    }
    bool pause() override { return log_->open; }
    bool resume() override { return log_->open; }

private:
    std::shared_ptr<Log> log_;
    std::size_t capacity_;
    std::uint16_t width_ = 0;
};

void advance(FakeDevice::Log& log, std::uint64_t frames) {
    if (!log.open) {
        return;
    }
    log.heard += std::min(log.submitted - log.heard, frames);
    log.clock += frames;
}

struct Library {
    std::map<std::string, std::vector<std::byte>> files;
    [[nodiscard]] ItemLoader loader() const {
        return [this](const std::string& path) -> std::expected<LoadedItem, std::string> {
            const auto found = files.find(path);
            if (found == files.end()) {
                return std::unexpected("no such file: " + path);
            }
            return LoadedItem{.source = std::make_shared<MemoryByteSource>(found->second),
                              .truehd = true};
        };
    }
};

QueueItem item(const std::string& path) {
    QueueItem entry;
    entry.path = path;
    entry.title = path;
    return entry;
}

bool play_out(Player& player, FakeDevice::Log& log, std::size_t period = 480) {
    for (int step = 0; step < 400000; ++step) {
        player.pump();
        advance(log, period);
        if (player.transport().state() == TransportState::kStopped && !log.open) {
            return true;
        }
    }
    return false;
}

// What the device was given, as 24-bit samples, interleaved.
std::vector<std::int32_t> heard_by(const FakeDevice::Log& log) {
    std::vector<std::int32_t> out;
    const std::size_t frames = log.kept.empty() ? 0 : log.kept.front().size();
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (const auto& slot : log.kept) {
            out.push_back(static_cast<std::int32_t>(slot[frame] * 8388608.0F));
        }
    }
    return out;
}

ItemLoader loader_of(std::vector<std::byte> bytes) {
    return [bytes = std::make_shared<std::vector<std::byte>>(std::move(bytes))](
               const std::string&) -> std::expected<LoadedItem, std::string> {
        return LoadedItem{.source = std::make_shared<MemoryByteSource>(*bytes), .truehd = true};
    };
}

// The PCM of frames [from, from + count) of a made stream.
std::vector<std::int32_t> pcm_part(const MadeStream& made, std::uint64_t from, std::uint64_t count) {
    const auto channels = static_cast<std::size_t>(made.channels);
    return std::vector<std::int32_t>(
        made.pcm.begin() + static_cast<std::ptrdiff_t>(from * channels),
        made.pcm.begin() + static_cast<std::ptrdiff_t>((from + count) * channels));
}

// Plays `want` frames of the session through the decoder and returns them as 24-bit samples.
std::vector<std::int32_t> render_frames(Session& session, StreamDecoder& decoder, std::size_t want) {
    std::vector<std::int32_t> out;
    std::size_t got_frames = 0;
    const StreamDecoder::BlockFn deliver = [&](std::span<const std::span<const float>> slots,
                                               std::size_t frames) {
        for (std::size_t frame = 0; frame < frames; ++frame) {
            for (const auto slot : slots) {
                out.push_back(static_cast<std::int32_t>(slot[frame] * 8388608.0F));
            }
        }
        got_frames += frames;
    };
    while (got_frames < want && !session.finished()) {
        const auto got = session.render(decoder, deliver, want - got_frames);
        REQUIRE(got.has_value());
    }
    return out;
}

}  // namespace

TEST_CASE("truehd player: a queue of a TrueHD item plays to its end, and the device is given the stream's samples",
          "[hearth][truehd][player]") {
    const MadeStream made = make_stream(2, 48000);
    Library library;
    library.files["one.thd"] = made.bytes;
    auto log = std::make_shared<FakeDevice::Log>();
    Player player(std::make_unique<FakeDevice>(log, 8192), library.loader(), stereo_room());
    player.queue().add(item("one.thd"));
    player.play();
    REQUIRE(play_out(player, *log));

    // The queue row knows the item: its duration, its rate, its channels, and that it is TrueHD.
    const ItemFacts& facts = player.queue().items()[0].facts;
    CHECK(facts.codec == ItemCodec::kTruehd);
    REQUIRE(facts.duration.has_value());
    CHECK(facts.duration->count() == 1000);
    CHECK(facts.sample_rate == 48000);
    CHECK(facts.channels == 2);

    CHECK(log->opens == 1);
    CHECK(log->kept.front().size() == 48000);
    CHECK(heard_by(*log) == made.pcm);
}

TEST_CASE("truehd player: two TrueHD items join without a gap, at the sample the first one ends",
          "[hearth][truehd][player]") {
    // Lengths that are not whole units: 24,013 and 31,007 samples, the last unit of each short.
    const MadeStream first = make_stream(2, 24013, 48000, 16, 3);
    const MadeStream second = make_stream(2, 31007, 48000, 16, 5);
    Library library;
    library.files["a.thd"] = first.bytes;
    library.files["b.thd"] = second.bytes;
    auto log = std::make_shared<FakeDevice::Log>();
    Player player(std::make_unique<FakeDevice>(log, 8192), library.loader(), stereo_room());
    player.queue().add(item("a.thd"));
    player.queue().add(item("b.thd"));
    player.play();
    REQUIRE(play_out(player, *log));

    CHECK(log->opens == 1);  // one output for both: no reopen, no gap
    std::vector<std::int32_t> expected = first.pcm;
    expected.insert(expected.end(), second.pcm.begin(), second.pcm.end());
    const std::vector<std::int32_t> heard = heard_by(*log);
    REQUIRE(heard.size() == expected.size());
    CHECK(log->kept.front().size() == 24013 + 31007);
    CHECK(heard == expected);
}

TEST_CASE("truehd player: items at two rates reopen the output, after the first has been heard out",
          "[hearth][truehd][player]") {
    const MadeStream low = make_stream(2, 9600, 44100, 16, 1);
    const MadeStream high = make_stream(2, 9600, 96000, 16, 2);
    Library library;
    library.files["low.thd"] = low.bytes;
    library.files["high.thd"] = high.bytes;
    auto log = std::make_shared<FakeDevice::Log>();
    Player player(std::make_unique<FakeDevice>(log, 8192), library.loader(), stereo_room());
    player.queue().add(item("low.thd"));
    player.queue().add(item("high.thd"));
    player.play();
    REQUIRE(play_out(player, *log));
    CHECK(log->opens == 2);
    // What the device was given since the last open is the second item, whole.
    CHECK(heard_by(*log) == high.pcm);
}

TEST_CASE("truehd session: a seek lands in the unit that holds the place asked for, at every restart and either side",
          "[hearth][truehd][seek]") {
    // A restart every 8 units (the least the encoder writes but 1), 3 s of stereo.
    const MadeStream made = make_stream(2, 144000, 48000, 8, 11);
    const auto unit = static_cast<std::uint64_t>(made.samples_per_unit);
    REQUIRE(unit == 40);
    auto session = Session::open("seek", loader_of(made.bytes));
    REQUIRE(session.has_value());
    REQUIRE(session->truehd_index()->major_syncs >= 400);
    StreamDecoder decoder(stereo_room(), made.sample_rate);

    std::size_t seeks = 0;
    for (std::size_t u = 0; u < made.has_major_sync.size(); ++u) {
        if (!made.has_major_sync[u]) {
            continue;
        }
        for (const std::int64_t delta : {-1, 0, 1}) {
            const std::int64_t at = static_cast<std::int64_t>(u * unit) + delta;
            if (at < 0) {
                continue;
            }
            const auto position = static_cast<std::uint64_t>(at);
            session->seek_to_sample(position, decoder);
            // The first frame delivered is the first of the unit that holds the place.
            const std::uint64_t landed = position / unit * unit;
            CHECK(session->position_samples() == landed);
            const std::size_t want = 200;
            const std::vector<std::int32_t> heard = render_frames(*session, decoder, want);
            REQUIRE(heard.size() == want * 2);
            CHECK(heard == pcm_part(made, landed, want));
            ++seeks;
        }
    }
    CHECK(seeks > 1000);
}

TEST_CASE("truehd session: a seek far into the stream and one to the end", "[hearth][truehd][seek]") {
    const MadeStream made = make_stream(2, 96000, 48000, 128, 13);
    auto session = Session::open("seek", loader_of(made.bytes));
    REQUIRE(session.has_value());
    StreamDecoder decoder(stereo_room(), made.sample_rate);

    // Two thirds in, the unit that holds 64,010 samples starts at 64,000.
    session->seek(std::chrono::milliseconds{1333}, decoder);
    CHECK(session->position_samples() == 63984 / 40 * 40);
    std::vector<std::int32_t> rest;
    const StreamDecoder::BlockFn deliver = [&rest](std::span<const std::span<const float>> slots,
                                                   std::size_t frames) {
        for (std::size_t frame = 0; frame < frames; ++frame) {
            for (const auto slot : slots) {
                rest.push_back(static_cast<std::int32_t>(slot[frame] * 8388608.0F));
            }
        }
    };
    while (!session->finished()) {
        REQUIRE(session->render(decoder, deliver, 4096).has_value());
    }
    // Everything from there to the end, and the end is the stream's: nothing repeated or lost.
    CHECK(rest == pcm_part(made, 63960, 96000 - 63960));

    // To the end, or past it: nothing left, and the decoder is not asked for anything.
    session->seek(std::chrono::milliseconds{5000}, decoder);
    CHECK(session->position_samples() == session->total_samples());
    rest.clear();
    while (!session->finished()) {
        REQUIRE(session->render(decoder, deliver, 4096).has_value());
    }
    CHECK(rest.empty());

    // Back to the start.
    session->seek(std::chrono::milliseconds{0}, decoder);
    rest.clear();
    while (!session->finished()) {
        REQUIRE(session->render(decoder, deliver, 4096).has_value());
    }
    CHECK(rest == made.pcm);
}

TEST_CASE("truehd session: the restart a coded output would start at is the last indexed one at or before the place",
          "[hearth][truehd][seek]") {
    const MadeStream made = make_stream(2, 144000, 48000, 128, 17);
    auto session = Session::open("restart", loader_of(made.bytes));
    REQUIRE(session.has_value());
    const TruehdIndex& index = *session->truehd_index();
    REQUIRE(index.restarts.size() >= 3);
    // The index keeps restarts at least kTruehdRestartSpacing units apart: 128-unit intervals give
    // every other one.
    const TruehdRestart second = index.restarts[1];
    const auto at_ms = [&](std::uint64_t unit, std::int64_t extra_samples) {
        return std::chrono::milliseconds{
            static_cast<std::int64_t>((unit * 40 + static_cast<std::uint64_t>(extra_samples)) * 1000 / 48000)};
    };
    // A place well inside the interval that starts at `second`.
    const auto landed = session->restart_at_or_before(at_ms(second.unit, 1500));
    CHECK(landed == at_ms(second.unit, 0));
    // Before the first indexed restart that is not unit 0, the stream's start.
    CHECK(session->restart_at_or_before(at_ms(second.unit, -200)) == std::chrono::milliseconds{0});
    // A seek does not stop there: the audio starts in the unit that holds the place.
    StreamDecoder decoder(stereo_room(), made.sample_rate);
    session->seek(at_ms(second.unit, 1500), decoder);
    CHECK(session->position_samples() > second.unit * 40 + 1000);
}

TEST_CASE("truehd session: a decoder handed the stream part-way takes up where the old one stopped, and loses nothing",
          "[hearth][truehd][seek]") {
    const MadeStream made = make_stream(2, 48000, 48000, 16, 19);
    auto session = Session::open("handover", loader_of(made.bytes));
    REQUIRE(session.has_value());
    DecoderSettings settings;
    auto old_decoder = std::make_unique<StreamDecoder>(stereo_room(), made.sample_rate, settings);
    std::vector<std::int32_t> heard;
    const StreamDecoder::BlockFn deliver = [&heard](std::span<const std::span<const float>> slots,
                                                    std::size_t frames) {
        for (std::size_t frame = 0; frame < frames; ++frame) {
            for (const auto slot : slots) {
                heard.push_back(static_cast<std::int32_t>(slot[frame] * 8388608.0F));
            }
        }
    };
    REQUIRE(session->render(*old_decoder, deliver, 20000).has_value());
    // A change of setting a TrueHD decoder cannot take in place.
    DecoderSettings changed = settings;
    changed.truehd.lenient = true;
    CHECK_FALSE(old_decoder->apply(changed));
    // A change it does not read can be taken in place.
    DecoderSettings unrelated = settings;
    unrelated.dual_mono = DualMonoChoice::kFirst;
    CHECK(old_decoder->apply(unrelated));

    session->hand_over(*old_decoder, deliver);
    auto new_decoder = std::make_unique<StreamDecoder>(stereo_room(), made.sample_rate, changed);
    while (!session->finished()) {
        REQUIRE(session->render(*new_decoder, deliver, 4096).has_value());
    }
    CHECK(heard == made.pcm);
}

TEST_CASE("truehd session: a unit that does not decode plays as silence until the next restart, and the rest is exact",
          "[hearth][truehd][loss]") {
    // A restart every 16 units; damage unit 20, so that the next restart is unit 32.
    const MadeStream made = make_stream(2, 48000, 48000, 16, 23);
    REQUIRE(made.has_major_sync[16]);
    REQUIRE(made.has_major_sync[32]);
    REQUIRE_FALSE(made.has_major_sync[20]);
    std::vector<std::byte> damaged = made.bytes;
    // A byte in the middle of a segment of unit 20: the unit still reads as a unit (its length and
    // header are sound) and fails a check of its segment.
    const std::uint64_t at = made.unit_offsets[20] + (made.unit_offsets[21] - made.unit_offsets[20]) / 2;
    damaged[at] = static_cast<std::byte>(std::to_integer<unsigned>(damaged[at]) ^ 0x55U);

    auto session = Session::open("damaged", loader_of(damaged));
    REQUIRE(session.has_value());
    CHECK(session->total_samples() == 48000);
    StreamDecoder decoder(stereo_room(), made.sample_rate);
    std::vector<std::int32_t> heard;
    std::vector<std::string> errors;
    const StreamDecoder::BlockFn deliver = [&heard](std::span<const std::span<const float>> slots,
                                                    std::size_t frames) {
        for (std::size_t frame = 0; frame < frames; ++frame) {
            for (const auto slot : slots) {
                heard.push_back(static_cast<std::int32_t>(slot[frame] * 8388608.0F));
            }
        }
    };
    while (!session->finished()) {
        const auto got = session->render(decoder, deliver, 4096);
        if (!got) {
            errors.push_back(got.error());
        }
    }
    // The item's time still passes: as many samples as the stream holds.
    REQUIRE(heard.size() == made.pcm.size());
    REQUIRE_FALSE(errors.empty());
    CHECK(errors.front().find("TrueHD") != std::string::npos);

    const auto channels = static_cast<std::size_t>(made.channels);
    const auto frames_of = [&](std::size_t from_unit, std::size_t to_unit) {
        return std::pair{from_unit * 40 * channels, to_unit * 40 * channels};
    };
    // Before the damaged unit: exact. From the next restart: exact.
    const auto [a0, a1] = frames_of(0, 20);
    CHECK(std::equal(heard.begin() + static_cast<std::ptrdiff_t>(a0),
                     heard.begin() + static_cast<std::ptrdiff_t>(a1),
                     made.pcm.begin() + static_cast<std::ptrdiff_t>(a0)));
    const auto [b0, b1] = frames_of(32, made.has_major_sync.size());
    CHECK(std::equal(heard.begin() + static_cast<std::ptrdiff_t>(b0), heard.end(),
                     made.pcm.begin() + static_cast<std::ptrdiff_t>(b0)));
    (void)b1;
    // Between them: silence, units 20 to 31.
    const auto [c0, c1] = frames_of(20, 32);
    CHECK(std::all_of(heard.begin() + static_cast<std::ptrdiff_t>(c0),
                      heard.begin() + static_cast<std::ptrdiff_t>(c1),
                      [](std::int32_t sample) { return sample == 0; }));
    // The decoder counted the units that waited for the restart.
    CHECK(decoder.truehd_counters().units_waiting >= 11);
}

TEST_CASE("truehd session: bytes that do not read as a unit are skipped, the decoder is told, and the item says so",
          "[hearth][truehd][loss]") {
    const MadeStream made = make_stream(2, 48000, 48000, 16, 29);
    std::vector<std::byte> damaged = made.bytes;
    // The check nibble and length of unit 20 broken: it does not read as a unit at all, and the
    // reader goes on to the next unit with a major sync, unit 32.
    damaged[made.unit_offsets[20]] = static_cast<std::byte>(std::to_integer<unsigned>(damaged[made.unit_offsets[20]]) ^ 0xF0U);
    auto session = Session::open("framing", loader_of(damaged));
    REQUIRE(session.has_value());
    CHECK(session->truehd_index()->skipped_bytes > 0);
    CHECK(session->facts().note.find("did not read as access units") != std::string::npos);
    // Units 20 to 31 are not in the item: its time is shorter by them.
    CHECK(session->unit_count() == made.has_major_sync.size() - 12);

    StreamDecoder decoder(stereo_room(), made.sample_rate);
    std::vector<std::int32_t> heard;
    const StreamDecoder::BlockFn deliver = [&heard](std::span<const std::span<const float>> slots,
                                                    std::size_t frames) {
        for (std::size_t frame = 0; frame < frames; ++frame) {
            for (const auto slot : slots) {
                heard.push_back(static_cast<std::int32_t>(slot[frame] * 8388608.0F));
            }
        }
    };
    while (!session->finished()) {
        REQUIRE(session->render(decoder, deliver, 4096).has_value());
    }
    // Everything the reader returned decodes: units 0 to 19, then 32 on - the restart that the
    // skip led to - exactly, and nothing in between to hear.
    std::vector<std::int32_t> expected = pcm_part(made, 0, 20 * 40);
    const std::vector<std::int32_t> after = pcm_part(made, 32 * 40, 48000 - 32 * 40);
    expected.insert(expected.end(), after.begin(), after.end());
    CHECK(heard == expected);
}

TEST_CASE("truehd player: what the decoder counted for an item is on its record, and in the diagnostics",
          "[hearth][truehd][player]") {
    // A restart every 16 units; unit 20 damaged, so 12 units wait for the restart at unit 32.
    const MadeStream made = make_stream(2, 48000, 48000, 16, 31);
    std::vector<std::byte> damaged = made.bytes;
    const std::uint64_t at = made.unit_offsets[20] + (made.unit_offsets[21] - made.unit_offsets[20]) / 2;
    damaged[at] = static_cast<std::byte>(std::to_integer<unsigned>(damaged[at]) ^ 0x55U);
    Library library;
    library.files["clean.thd"] = made.bytes;
    library.files["damaged.thd"] = damaged;
    auto log = std::make_shared<FakeDevice::Log>();
    Player player(std::make_unique<FakeDevice>(log, 8192), library.loader(), stereo_room());
    player.queue().add(item("clean.thd"));
    player.queue().add(item("damaged.thd"));
    player.play();
    REQUIRE(play_out(player, *log));

    // Each item's own, though one decoder served both.
    REQUIRE(player.history().size() == 2);
    REQUIRE(player.history()[0].truehd.has_value());
    CHECK(player.history()[0].truehd->units == 1200);
    CHECK(player.history()[0].truehd->units_waiting == 0);
    CHECK(player.history()[0].truehd->restarts == 75);
    CHECK(player.history()[0].truehd->lossless_check_failures == 0);
    REQUIRE(player.history()[1].truehd.has_value());
    CHECK(player.history()[1].truehd->units == 1200);
    CHECK(player.history()[1].truehd->units_waiting >= 11);
    CHECK(player.history()[1].truehd->units_decoded < 1200);

    // The diagnostics file says it, beside the frames the item delivered.
    EngineStatus status;
    status.history = std::vector<PlayedItem>(player.history());
    const std::string report =
        render_report(ReportFacts{}, status, DiagnosticLog{}, Secrets{});
    CHECK(report.find("TrueHD: 75 restarts, 0 units waited for a restart, 0 lossless checks failed") !=
          std::string::npos);
    CHECK(report.find("units waited for a restart, 0 lossless checks failed, 1 other checks failed") !=
          std::string::npos);
}

TEST_CASE("truehd player: the meters read the decoded channels, one to a speaker",
          "[hearth][truehd][player]") {
    // Stereo with the right channel silent: the left meter moves and the right does not.
    const MadeStream made = make_stream(2, 24000, 48000, 128, 37, /*silent_channel=*/1);
    Library library;
    library.files["quiet-right.thd"] = made.bytes;
    auto log = std::make_shared<FakeDevice::Log>();
    Player player(std::make_unique<FakeDevice>(log, 8192), library.loader(), stereo_room());
    player.queue().add(item("quiet-right.thd"));
    player.play();
    MeterSnapshot latest;
    bool seen = false;
    for (int step = 0; step < 100000; ++step) {
        player.pump();
        advance(*log, 480);
        seen = player.meters(latest) || seen;
        if (player.transport().state() == TransportState::kStopped && !log->open) {
            break;
        }
    }
    REQUIRE(seen);
    REQUIRE(latest.levels.size() == 2);
    // The left channel's tone peaks at about -6 dBFS; the right is digital silence.
    CHECK(latest.levels[0].peak_db > -20.0);
    CHECK(latest.levels[0].peak_db < 0.0);
    CHECK(latest.levels[1].peak_db < -90.0);
    CHECK_FALSE(latest.levels[0].clipped);
}
