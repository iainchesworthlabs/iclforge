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

#include "iclforge/containers/iec61937/mat.hpp"
#include "iclforge/render/layout.hpp"
#include "bitstream_sink.hpp"
#include "item_source.hpp"
#include "player.hpp"
#include "session.hpp"
#include "truehd_stream.hpp"
#include "truehd_test_support.hpp"

// TrueHD sent as MAT to a local output:
// the player packs a TrueHD item's access units 24 to a burst, the link opens as kMat at the item's
// rate, two items at one rate join into one stream of bursts, a rate change reopens, and a seek
// starts the bursts at the restart unit before the place asked for. The oracle is the item itself:
// unwrapping the bursts gives back its access units, in order and unaltered.

namespace {

using namespace iclforge::hearth;
using namespace iclforge::hearth::testing;
namespace mat = iclforge::containers::iec61937;
using iclforge::audio::BitstreamFormat;

using Bytes = std::vector<std::byte>;
using Units = std::vector<Bytes>;

// A MAT link whose clock the test runs. A burst is a fixed span of the programme: 24 units of
// rate / 1,200 samples, which is rate / 50 content frames.
class FakeMatLink final : public BitstreamSink {
public:
    struct Log {
        std::vector<BitstreamFormat> formats{};
        std::vector<std::uint32_t> rates{};
        std::vector<Bytes> bursts{};
        std::vector<std::size_t> bursts_at_open{};
        std::size_t capacity_bursts = 6;
        std::uint32_t opens = 0;
        std::uint32_t flushes = 0;
        std::uint64_t submitted = 0;
        std::uint64_t heard = 0;
        std::uint64_t clock = 0;
        std::uint32_t burst_frames = 960;
        bool open = false;
        bool wrong_size = false;

        void advance(std::uint64_t frames) {
            if (!open) {
                return;
            }
            heard += std::min(submitted - heard, frames);
            clock += frames;
        }
    };

    explicit FakeMatLink(std::shared_ptr<Log> log) : log_(std::move(log)) {}

    std::expected<OpenOutputFormat, std::string> open(const Format& format) override {
        ++log_->opens;
        log_->bursts_at_open.push_back(log_->bursts.size());
        log_->formats.push_back(format.format);
        log_->rates.push_back(format.sample_rate);
        log_->burst_frames = format.sample_rate / 50;
        log_->open = true;
        log_->submitted = log_->heard = log_->clock = 0;
        return OpenOutputFormat{.sample_rate = format.sample_rate,
                                .channels = 2,
                                .mode = OutputMode::kBitstream,
                                .stream = format.format};
    }
    void close() override { log_->open = false; }
    [[nodiscard]] bool is_open() const override { return log_->open; }
    bool submit(std::span<const std::byte> burst) override {
        const std::uint64_t held = (log_->submitted - log_->heard) / log_->burst_frames;
        if (!log_->open || held + 1 > log_->capacity_bursts) {
            return false;
        }
        if (burst.size() != mat::kMatBurstBytes) {
            if (!log_->wrong_size) {
                log_->wrong_size = true;
                FAIL_CHECK("a burst of " << burst.size() << " bytes, not " << mat::kMatBurstBytes);
            }
            return false;
        }
        log_->bursts.emplace_back(burst.begin(), burst.end());
        log_->submitted += log_->burst_frames;
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
    }
    bool pause() override { return log_->open; }
    bool resume() override { return log_->open; }

private:
    std::shared_ptr<Log> log_;
};

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

// The access units of a stream, as the library's own reader hands them out.
Units units_of(const std::vector<std::byte>& bytes) {
    MemoryByteSource source{bytes};
    iclforge::apps::TruehdCursor cursor{source};
    Units out;
    REQUIRE(cursor.rewind());
    while (const auto unit = cursor.next()) {
        out.emplace_back(unit->begin(), unit->end());
    }
    return out;
}

// Every access unit the bursts carry, in order.
Units units_in(const std::vector<Bytes>& bursts) {
    Bytes carrier;
    for (const Bytes& burst : bursts) {
        carrier.insert(carrier.end(), burst.begin(), burst.end());
    }
    const auto unwrapped = mat::unwrap_mat_stream(carrier);
    REQUIRE(unwrapped.has_value());
    return *unwrapped;
}

struct Rig {
    std::shared_ptr<FakeMatLink::Log> link = std::make_shared<FakeMatLink::Log>();
    std::unique_ptr<Player> player;
    std::vector<ItemFacts> asked{};

    explicit Rig(const Library& library) {
        const auto layout = iclforge::render::OutputLayout::parse("2.0");
        REQUIRE(layout.has_value());
        PlayerOutputs outputs;
        outputs.bitstream = std::make_unique<FakeMatLink>(link);
        outputs.choose = [this](const ItemFacts& facts, const HeldOutput&) {
            asked.push_back(facts);
            // What the output decision gives a TrueHD item on an output that takes MAT.
            return OutputChoice{.mode = OutputMode::kBitstream,
                                .endpoint_id = "hdmi",
                                .endpoint_name = "HDMI",
                                .reason = "test",
                                .stream = BitstreamFormat::kMat};
        };
        player = std::make_unique<Player>(std::move(outputs), library.loader(), *layout);
    }

    bool play_out(std::uint64_t period = 480) {
        for (int step = 0; step < 400000; ++step) {
            player->pump();
            link->advance(period);
            if (player->transport().state() == TransportState::kStopped && !link->open) {
                return true;
            }
        }
        return false;
    }
};

}  // namespace

TEST_CASE("truehd mat: a TrueHD item goes out as MAT bursts that unwrap to its access units",
          "[hearth][truehd][player][mat]") {
    const MadeStream made = make_stream(6, 24013, 48000, 16, 3);
    Library library;
    library.files["one.thd"] = made.bytes;
    Rig rig{library};
    rig.player->queue().add(item("one.thd"));
    rig.player->play();
    REQUIRE(rig.play_out());

    CHECK(rig.link->opens == 1);
    REQUIRE(rig.link->formats.size() == 1);
    CHECK(rig.link->formats.front() == BitstreamFormat::kMat);
    CHECK(rig.link->rates.front() == 48000);
    CHECK_FALSE(rig.link->wrong_size);

    const Units expected = units_of(made.bytes);
    REQUIRE(expected.size() > 24 * 2);
    // 24 units to a burst, the last one padded: no unit is lost, and none is added.
    CHECK(rig.link->bursts.size() == (expected.size() + 23) / 24);
    CHECK(units_in(rig.link->bursts) == expected);
}

TEST_CASE("truehd mat: two items at one rate join into one stream of bursts, a burst holding the end of one and the start of the next",
          "[hearth][truehd][player][mat]") {
    // 24,013 and 31,007 samples: 601 and 776 units at 40 samples, neither a multiple of 24.
    const MadeStream first = make_stream(2, 24013, 48000, 16, 3);
    const MadeStream second = make_stream(2, 31007, 48000, 16, 5);
    Library library;
    library.files["a.thd"] = first.bytes;
    library.files["b.thd"] = second.bytes;
    Rig rig{library};
    rig.player->queue().add(item("a.thd"));
    rig.player->queue().add(item("b.thd"));
    rig.player->play();
    REQUIRE(rig.play_out());

    CHECK(rig.link->opens == 1);  // joined: no reopen, no gap
    Units expected = units_of(first.bytes);
    const Units second_units = units_of(second.bytes);
    expected.insert(expected.end(), second_units.begin(), second_units.end());
    CHECK(rig.link->bursts.size() == (expected.size() + 23) / 24);
    CHECK(units_in(rig.link->bursts) == expected);
}

TEST_CASE("truehd mat: an item at another rate reopens the link, and each item's units reach it whole",
          "[hearth][truehd][player][mat]") {
    const MadeStream low = make_stream(2, 9600, 44100, 16, 1);
    const MadeStream high = make_stream(2, 19200, 96000, 16, 2);
    Library library;
    library.files["low.thd"] = low.bytes;
    library.files["high.thd"] = high.bytes;
    Rig rig{library};
    rig.player->queue().add(item("low.thd"));
    rig.player->queue().add(item("high.thd"));
    rig.player->play();
    REQUIRE(rig.play_out());

    CHECK(rig.link->opens == 2);
    REQUIRE(rig.link->rates.size() == 2);
    CHECK(rig.link->rates[0] == 44100);
    CHECK(rig.link->rates[1] == 96000);
    // The first item's last burst is padded out, not left in the packer, before the reopen.
    const std::size_t second_start = rig.link->bursts_at_open[1];
    const std::vector<Bytes> first_bursts(rig.link->bursts.begin(),
                                          rig.link->bursts.begin() + static_cast<std::ptrdiff_t>(second_start));
    const std::vector<Bytes> second_bursts(rig.link->bursts.begin() + static_cast<std::ptrdiff_t>(second_start),
                                           rig.link->bursts.end());
    CHECK(units_in(first_bursts) == units_of(low.bytes));
    CHECK(units_in(second_bursts) == units_of(high.bytes));
}

TEST_CASE("truehd mat: the bursts of an item opened part-way start at a restart, and the first unit is a major sync",
          "[hearth][truehd][player][mat]") {
    const MadeStream made = make_stream(2, 96000, 48000, 16, 9);
    Library library;
    library.files["long.thd"] = made.bytes;
    Rig rig{library};
    rig.player->queue().add(item("long.thd"));
    rig.player->play();
    rig.player->pump();
    rig.player->seek(std::chrono::milliseconds{1000});
    REQUIRE(rig.play_out());

    // After the seek the link is flushed and the bursts begin again: the unit that opens them is a
    // restart (a major sync, the four bytes 0xF8726FBA after the 4-byte header), the last unit sent
    // is the stream's last, and the units between are consecutive.
    REQUIRE(rig.link->flushes >= 1);
    const Units all = units_of(made.bytes);
    const Units sent = units_in(rig.link->bursts);
    REQUIRE_FALSE(sent.empty());
    CHECK(sent.back() == all.back());
    // Find where the post-seek run begins: the last unit with a major sync that follows a unit
    // that is not its predecessor in the stream.
    bool found_restart = false;
    for (std::size_t i = 1; i < sent.size(); ++i) {
        const auto place = std::find(all.begin(), all.end(), sent[i]);
        const auto before = std::find(all.begin(), all.end(), sent[i - 1]);
        if (place != all.end() && before != all.end() && place != before + 1) {
            // A jump: the unit that begins after it is a restart.
            CHECK(sent[i].size() > 8);
            CHECK(std::to_integer<unsigned>(sent[i][4]) == 0xF8U);
            CHECK(std::to_integer<unsigned>(sent[i][5]) == 0x72U);
            CHECK(std::to_integer<unsigned>(sent[i][6]) == 0x6FU);
            CHECK(std::to_integer<unsigned>(sent[i][7]) == 0xBAU);
            found_restart = true;
        }
    }
    CHECK(found_restart);
}

TEST_CASE("truehd mat: a player with no passthrough output refuses a MAT choice with its reason",
          "[hearth][truehd][player][mat]") {
    const MadeStream made = make_stream(2, 4800);
    Library library;
    library.files["one.thd"] = made.bytes;
    const auto layout = iclforge::render::OutputLayout::parse("2.0");
    REQUIRE(layout.has_value());
    PlayerOutputs outputs;
    outputs.choose = [](const ItemFacts&, const HeldOutput&) {
        return OutputChoice{.mode = OutputMode::kBitstream,
                            .endpoint_id = "hdmi",
                            .endpoint_name = "HDMI",
                            .reason = "test",
                            .stream = BitstreamFormat::kMat};
    };
    Player player{std::move(outputs), library.loader(), *layout};
    player.queue().add(item("one.thd"));
    player.play();
    CHECK(player.transport().state() == TransportState::kStopped);
    CHECK(player.last_error() == "This player has no passthrough output.");
}
