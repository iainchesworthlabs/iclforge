#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "item_source.hpp"
#include "decoder_settings.hpp"
#include "iclforge/base/crypto/sha256.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/elementary.hpp"
#include "session.hpp"
#include "stream_decoder.hpp"
#include "truehd_test_support.hpp"

// TrueHD and MLP through Hearth's engine: an item opened by a Session and decoded and rendered by
// a StreamDecoder, against what the library decodes (`forge decode` is the library's decoder over
// its reader, strict, presentation best) and against the digests the manifests state. The output
// layout is the stream's own, so that the renderer's placement is the identity and what reaches
// the sink can be held to the decoder's samples.

namespace {

using namespace iclforge::hearth;
using namespace iclforge::hearth::testing;
namespace th = iclforge::truehd;

// What came out of a render, in the order it came.
struct Played {
    std::vector<std::vector<float>> slots;
    std::vector<std::size_t> block_sizes;
    std::vector<UnitReport> reports;
    std::vector<std::string> errors;
    std::size_t frames() const { return slots.empty() ? 0 : slots.front().size(); }
};

ItemLoader loader_of(std::vector<std::byte> bytes) {
    return [bytes = std::make_shared<std::vector<std::byte>>(std::move(bytes))](
               const std::string&) -> std::expected<LoadedItem, std::string> {
        return LoadedItem{.source = std::make_shared<MemoryByteSource>(*bytes), .truehd = true};
    };
}

Played play(Session& session, StreamDecoder& decoder, std::size_t chunk = 4096) {
    Played out;
    out.slots.resize(decoder.layout().slots());
    const StreamDecoder::BlockFn deliver = [&out](std::span<const std::span<const float>> slots,
                                                  std::size_t frames) {
        for (std::size_t slot = 0; slot < out.slots.size(); ++slot) {
            out.slots[slot].insert(out.slots[slot].end(), slots[slot].begin(),
                                   slots[slot].begin() + static_cast<std::ptrdiff_t>(frames));
        }
        out.block_sizes.push_back(frames);
    };
    const Session::ReportFn reported = [&out](const UnitReport& report, std::size_t) {
        out.reports.push_back(report);
    };
    for (std::size_t guard = 0; !session.finished() && guard < 1'000'000; ++guard) {
        const auto got = session.render(decoder, deliver, chunk, reported);
        if (!got) {
            out.errors.push_back(got.error());
        }
    }
    REQUIRE(session.finished());
    return out;
}

// What the library decodes of a stream the way `forge decode` does: its reader, strict checks,
// the presentation given. The channels interleaved as the decoder gives them.
struct Decoded {
    std::vector<std::int32_t> pcm;
    std::vector<th::Channel> layout;
    int channels = 0;
    std::uint32_t rate = 0;
    std::size_t samples = 0;
    std::size_t units = 0;
};

Decoded decode_like_forge(const std::vector<std::byte>& stream,
                          th::Presentation presentation = th::Presentation::kBest) {
    Decoded out;
    th::Decoder decoder(th::DecoderConfig{.presentation = presentation});
    th::ElementaryReader reader(stream);
    while (const auto unit = reader.next()) {
        ++out.units;
        const auto decoded = decoder.decode(*unit);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        const th::DecodedAccessUnit& pcm = **decoded;
        if (out.channels == 0) {
            out.channels = pcm.channels;
            out.rate = pcm.sample_rate;
            out.layout.assign(pcm.layout.begin(), pcm.layout.end());
        }
        out.pcm.insert(out.pcm.end(), pcm.pcm.begin(), pcm.pcm.end());
        out.samples += static_cast<std::size_t>(pcm.samples);
    }
    return out;
}

// The output layout that has exactly the decoded channels: the stream's own speakers where the
// major sync names them, else the first full-bandwidth speakers in order, which is where the
// renderer puts channels it cannot place.
iclforge::render::OutputLayout layout_for(const Decoded& decoded) {
    if (decoded.layout.size() == static_cast<std::size_t>(decoded.channels)) {
        return layout_of(decoded.layout);
    }
    using iclforge::base::Location;
    constexpr std::array<Location, 16> kOrder{
        Location::kLeft, Location::kRight, Location::kCentre, Location::kLeftSurround,
        Location::kRightSurround, Location::kLrs, Location::kRrs, Location::kLc, Location::kRc,
        Location::kCs, Location::kTs, Location::kLsd, Location::kRsd, Location::kLw, Location::kRw,
        Location::kVhl};
    const auto layout = iclforge::render::OutputLayout::from_locations(
        std::span<const Location>(kOrder).first(static_cast<std::size_t>(decoded.channels)));
    REQUIRE(layout.has_value());
    return *layout;
}

// A 24-bit sample from a float that must have been one: the renderer's identity placement keeps
// every bit, so the product is an integer.
std::int32_t sample_of(float value, bool& exact) {
    const float scaled = value * 8388608.0F;
    const auto rounded = static_cast<std::int32_t>(std::lrintf(scaled));
    exact = exact && static_cast<float>(rounded) == scaled;
    return rounded;
}

std::vector<std::int32_t> interleaved(const Played& played, bool& exact) {
    std::vector<std::int32_t> out;
    out.reserve(played.frames() * played.slots.size());
    for (std::size_t frame = 0; frame < played.frames(); ++frame) {
        for (const auto& slot : played.slots) {
            out.push_back(sample_of(slot[frame], exact));
        }
    }
    return out;
}

std::string s32le_digest(std::span<const std::int32_t> pcm) {
    std::vector<std::byte> bytes;
    for (const std::int32_t sample : pcm) {
        append_s32le(bytes, sample);
    }
    return hex(iclforge::base::crypto::sha256(bytes));
}

}  // namespace

TEST_CASE("truehd playback: every FFmpeg stream through the engine is forge decode's output, sample for sample",
          "[hearth][truehd][interop]") {
    for (const nlohmann::json& fixture : manifest()["fixtures"]) {
        DYNAMIC_SECTION(fixture["name"].get<std::string>()) {
            const std::vector<std::byte> stream =
                read_file(truehd_dir() + "/" + fixture["file"].get<std::string>());
            const Decoded forge = decode_like_forge(stream);
            REQUIRE(forge.samples == fixture["pcm_samples"].get<std::size_t>());

            auto session = Session::open("fixture", loader_of(stream));
            REQUIRE(session.has_value());
            StreamDecoder decoder(layout_for(forge), forge.rate);
            const Played played = play(*session, decoder);

            CHECK(played.errors.empty());
            REQUIRE(played.frames() == forge.samples);
            bool exact = true;
            const std::vector<std::int32_t> heard = interleaved(played, exact);
            CHECK(exact);
            CHECK(heard == forge.pcm);
            // And the digest the manifest states of FFmpeg's strict decode of the same file.
            CHECK(s32le_digest(heard) == fixture["pcm_sha256"].get<std::string>());
        }
    }
}

TEST_CASE("truehd playback: the hand-built streams the decoder reads play as the signal they were built from",
          "[hearth][truehd][interop]") {
    std::ifstream in(truehd_dir() + "/handbuilt/manifest.json");
    REQUIRE(in.good());
    const nlohmann::json handbuilt = nlohmann::json::parse(in);
    // Counted outside the sections, each of which runs on a pass of its own.
    CHECK(std::ranges::count_if(handbuilt["fixtures"], [](const nlohmann::json& fixture) {
              return fixture["presentation"].get<std::string>() == "best";
          }) >= 10);
    for (const nlohmann::json& fixture : handbuilt["fixtures"]) {
        const std::string presentation = fixture["presentation"].get<std::string>();
        if (presentation != "best") {
            continue;  // the sixteen-channel presentation is not one the player offers
        }
        DYNAMIC_SECTION(fixture["name"].get<std::string>()) {
            const std::vector<std::byte> stream =
                read_file(truehd_dir() + "/handbuilt/" + fixture["file"].get<std::string>());
            const Decoded forge = decode_like_forge(stream);
            REQUIRE(forge.samples == fixture["pcm_samples"].get<std::size_t>());

            auto session = Session::open("handbuilt", loader_of(stream));
            REQUIRE(session.has_value());
            StreamDecoder decoder(layout_for(forge), forge.rate);
            const Played played = play(*session, decoder);
            CHECK(played.errors.empty());
            REQUIRE(played.frames() == forge.samples);
            bool exact = true;
            const std::vector<std::int32_t> heard = interleaved(played, exact);
            CHECK(exact);
            CHECK(heard == forge.pcm);
            CHECK(s32le_digest(heard) == fixture["pcm_sha256"].get<std::string>());
        }
    }
}

TEST_CASE("truehd playback: a unit is one block of its own length, 40, 80 or 160 samples",
          "[hearth][truehd]") {
    // The block-size policy (planning/truehd-parity-hearth-player.md, H1): units are not repacked
    // to the 256 the AC-3 path delivers, so that a unit's report, its position and a seek stay
    // exact; nothing after the decoder needs 256.
    for (const auto& [file, unit] : {std::pair{"truehd_st48_s24.thd", 40U},
                                     std::pair{"truehd_st96_s24.thd", 80U},
                                     std::pair{"truehd_st192_s24.thd", 160U},
                                     std::pair{"truehd_st44_s24.thd", 40U},
                                     std::pair{"truehd_st176_s24.thd", 160U}}) {
        DYNAMIC_SECTION(file) {
            const std::vector<std::byte> stream = read_file(truehd_dir() + "/" + file);
            const Decoded forge = decode_like_forge(stream);
            auto session = Session::open("rates", loader_of(stream));
            REQUIRE(session.has_value());
            StreamDecoder decoder(layout_for(forge), forge.rate);
            const Played played = play(*session, decoder);
            REQUIRE_FALSE(played.block_sizes.empty());
            CHECK(played.block_sizes.size() == forge.units);
            // Every block is a whole unit, but for the last, which is short by the padding.
            for (std::size_t i = 0; i + 1 < played.block_sizes.size(); ++i) {
                CHECK(played.block_sizes[i] == unit);
            }
            CHECK(played.block_sizes.back() <= unit);
            CHECK(played.reports.size() == forge.units);
        }
    }
}

TEST_CASE("truehd playback: the item's facts are the stream's", "[hearth][truehd]") {
    const std::vector<std::byte> stream = read_file(truehd_dir() + "/truehd_m51_s24.thd");
    auto session = Session::open("m51", loader_of(stream));
    REQUIRE(session.has_value());
    const ItemFacts& facts = session->facts();
    CHECK(facts.codec == ItemCodec::kTruehd);
    // Passthrough of TrueHD is not available: it is never sent as it is.
    CHECK_FALSE(facts.stream.has_value());
    CHECK(facts.sample_rate == 48000);
    CHECK(facts.channels == 6);
    REQUIRE(facts.duration.has_value());
    CHECK(facts.duration->count() == 250);
    REQUIRE(facts.bitrate_kbps.has_value());
    CHECK(*facts.bitrate_kbps == Catch::Approx(132106.0 * 8.0 / 1000.0 / 0.25).margin(0.01));
    CHECK(facts.frame_rate == Catch::Approx(1200.0));
    CHECK(facts.objects == 0);
    CHECK(facts.unplayable_because.empty());
    CHECK(session->truehd());
    CHECK(session->unit_count() == 300);
    CHECK(session->total_samples() == 12000);

    auto mlp = Session::open("mlp", loader_of(read_file(truehd_dir() + "/mlp_st44_s16.mlp")));
    REQUIRE(mlp.has_value());
    CHECK(mlp->facts().codec == ItemCodec::kMlp);
    CHECK(mlp->facts().sample_rate == 44100);
    // 276 units of 40 samples, and no padding: the manifest states 11,040.
    CHECK(mlp->total_samples() == 11040);
}

TEST_CASE("truehd playback: a bare stream is taken for TrueHD by its first unit, with no word from the loader",
          "[hearth][truehd]") {
    const std::vector<std::byte> stream = read_file(truehd_dir() + "/truehd_st48_s16.thd");
    const ItemLoader loader = [&stream](const std::string&) -> std::expected<LoadedItem, std::string> {
        return LoadedItem{.bytes = stream};
    };
    auto session = Session::open("bare", loader);
    REQUIRE(session.has_value());
    CHECK(session->truehd());
    CHECK(session->facts().codec == ItemCodec::kTruehd);
}

TEST_CASE("truehd playback: the stream is read in windows, not held whole", "[hearth][truehd]") {
    class CountingSource final : public ByteSource {
    public:
        explicit CountingSource(std::vector<std::byte> bytes) : inner_(std::move(bytes)) {}
        [[nodiscard]] std::uint64_t size() const override { return inner_.size(); }
        [[nodiscard]] std::size_t read(std::uint64_t offset, std::span<std::byte> out) override {
            largest = std::max(largest, out.size());
            ++reads;
            return inner_.read(offset, out);
        }
        std::size_t largest = 0;
        std::size_t reads = 0;

    private:
        MemoryByteSource inner_;
    };
    // Two seconds of 5.1 that the encoder cannot make small: more than the window a cursor reads.
    const MadeStream made = make_stream(6, 96000);
    REQUIRE(made.bytes.size() > 2 * TruehdCursor::kDefaultWindowBytes);
    auto counting = std::make_shared<CountingSource>(made.bytes);
    const ItemLoader loader = [counting](const std::string&) -> std::expected<LoadedItem, std::string> {
        return LoadedItem{.source = counting, .truehd = true};
    };
    auto session = Session::open("windows", loader);
    REQUIRE(session.has_value());
    CHECK(session->total_samples() == 96000);
    // The room is the stream's 5.1, in its channel order.
    const std::array<th::Channel, 6> five_one{th::Channel::kL,  th::Channel::kR,  th::Channel::kC,
                                              th::Channel::kLfe, th::Channel::kLs, th::Channel::kRs};
    StreamDecoder decoder(layout_of(five_one), made.sample_rate);
    const Played played = play(*session, decoder);
    // Lossless, so the PCM that went in is what came out; the 5.1 encoder layout is L R C LFE Ls Rs.
    REQUIRE(played.frames() == 96000);
    CHECK(counting->largest <= TruehdCursor::kDefaultWindowBytes);
    CHECK(counting->largest < made.bytes.size());
    // Read in several windows, by the index and by the playing.
    CHECK(counting->reads > 2 * (made.bytes.size() / TruehdCursor::kDefaultWindowBytes));
    bool exact = true;
    CHECK(interleaved(played, exact) == made.pcm);
    CHECK(exact);
}

TEST_CASE("truehd playback: the channels a stream names are placed at the speakers of the same name",
          "[hearth][truehd]") {
    using iclforge::base::Location;
    const std::array<th::Channel, 8> seven_one{th::Channel::kL,  th::Channel::kR,  th::Channel::kC,
                                               th::Channel::kLfe, th::Channel::kLs, th::Channel::kRs,
                                               th::Channel::kLrs, th::Channel::kRrs};
    const auto bed = truehd_bed(seven_one);
    REQUIRE(bed.has_value());
    REQUIRE(bed->count == 8);
    const std::array<Location, 8> expected{Location::kLeft,          Location::kRight,
                                           Location::kCentre,        Location::kLfe,
                                           Location::kLeftSurround,  Location::kRightSurround,
                                           Location::kLrs,           Location::kRrs};
    for (int i = 0; i < 8; ++i) {
        CHECK((*bed)[i] == expected[static_cast<std::size_t>(i)]);
    }
    // No names, no bed; a name twice, no bed.
    CHECK_FALSE(truehd_bed({}).has_value());
    const std::array<th::Channel, 2> twice{th::Channel::kL, th::Channel::kL};
    CHECK_FALSE(truehd_bed(twice).has_value());
}

TEST_CASE("truehd playback: on a layout the stream does not fill, the renderer pans what has no speaker",
          "[hearth][truehd]") {
    // 5.1 on a stereo room: not the A/52 fold (there is none for TrueHD), the renderer's: a coded
    // channel the layout has a speaker for is exact, one it has none for is panned between those it
    // has, so the centre reaches both at equal gain and the LFE reaches no speaker.
    const std::vector<std::byte> stream = read_file(truehd_dir() + "/truehd_m51_s24.thd");
    const Decoded forge = decode_like_forge(stream);
    REQUIRE(forge.channels == 6);
    auto session = Session::open("m51", loader_of(stream));
    REQUIRE(session.has_value());
    StreamDecoder decoder(iclforge::render::OutputLayout::stereo(), forge.rate);
    const Played played = play(*session, decoder);
    REQUIRE(played.slots.size() == 2);
    REQUIRE(played.frames() == forge.samples);
    // Left of the room is the coded left, plus what was panned in: not the coded left alone.
    bool differs = false;
    double energy_in = 0.0;
    double energy_out = 0.0;
    for (std::size_t frame = 0; frame < forge.samples; ++frame) {
        const double coded_left = forge.pcm[frame * 6] / 8388608.0;
        const double coded_centre = forge.pcm[frame * 6 + 2] / 8388608.0;
        const double heard_left = played.slots[0][frame];
        differs = differs || std::abs(heard_left - coded_left) > 1e-6;
        energy_in += coded_centre * coded_centre;
        energy_out += (heard_left - coded_left) * (heard_left - coded_left);
    }
    CHECK(differs);
    // The centre's share of the left speaker is at -3 dB: half its power, not all and not none.
    REQUIRE(energy_in > 0.0);
    CHECK(energy_out > 0.2 * energy_in);
}

TEST_CASE("truehd playback: the 2 channel presentation of a stream with three plays its first substream",
          "[hearth][truehd]") {
    const std::vector<std::byte> stream = read_file(truehd_dir() + "/handbuilt/hb_eight_channels.thd");
    const Decoded best = decode_like_forge(stream);
    const Decoded two = decode_like_forge(stream, th::Presentation::k2ch);
    REQUIRE(best.channels == 8);
    REQUIRE(two.channels == 2);

    DecoderSettings settings;
    settings.truehd.presentation = th::Presentation::k2ch;
    auto session = Session::open("hb", loader_of(stream));
    REQUIRE(session.has_value());
    CHECK(session->facts().channels == 8);  // the item's best; the setting changes what plays
    StreamDecoder decoder(layout_for(two), two.rate, settings);
    const Played played = play(*session, decoder);
    bool exact = true;
    CHECK(interleaved(played, exact) == two.pcm);

    // And the default is the best.
    auto again = Session::open("hb", loader_of(stream));
    REQUIRE(again.has_value());
    StreamDecoder best_decoder(layout_for(best), best.rate);
    const Played all = play(*again, best_decoder);
    CHECK(interleaved(all, exact) == best.pcm);
}

TEST_CASE("truehd playback: a unit's report says what the unit said and what the decoder has counted",
          "[hearth][truehd]") {
    const std::vector<std::byte> stream = read_file(truehd_dir() + "/truehd_st48_s24.thd");
    const Decoded forge = decode_like_forge(stream);
    auto session = Session::open("st48", loader_of(stream));
    REQUIRE(session.has_value());
    StreamDecoder decoder(layout_for(forge), forge.rate);
    const Played played = play(*session, decoder);
    REQUIRE(played.reports.size() == 300);
    const UnitReport& first = played.reports.front();
    REQUIRE(first.truehd.has_value());
    CHECK_FALSE(first.ac4.has_value());
    CHECK(first.truehd->channels == 2);
    CHECK(first.truehd->substream == 0);
    CHECK(first.truehd->restarted);
    CHECK_FALSE(first.truehd->mlp);
    CHECK(first.acmod == iclforge::ac3::Acmod::k2_0);
    CHECK_FALSE(first.lfe);
    CHECK(first.blocks == 0);
    REQUIRE(first.bitrate_kbps.has_value());
    // The 16th unit is not a restart, the 17th is: a restart every 16 units.
    CHECK_FALSE(played.reports[1].truehd->restarted);
    CHECK(played.reports[16].truehd->restarted);
    const TruehdCounters counted = played.reports.back().truehd->counters;
    CHECK(counted.units == 300);
    CHECK(counted.units_decoded == 300);
    CHECK(counted.units_waiting == 0);
    CHECK(counted.restarts == 19);
    CHECK(counted.lossless_check_failures == 0);
    CHECK(counted.lossless_checks_ok == 18);
    CHECK(decoder.truehd_counters() == counted);
}

TEST_CASE("truehd playback: a stream the decoder refuses plays as silence of its length, and says why",
          "[hearth][truehd]") {
    // noise_type 1 with a non-zero noise shift: the library refuses it by name (ERRATA entry 32).
    const std::vector<std::byte> stream =
        read_file(truehd_dir() + "/handbuilt/hb_noise_type1_shift.thd");
    auto session = Session::open("refused", loader_of(stream));
    REQUIRE(session.has_value());
    const auto layout = iclforge::render::OutputLayout::stereo();
    StreamDecoder decoder(layout, 48000);
    const Played played = play(*session, decoder);
    REQUIRE_FALSE(played.errors.empty());
    CHECK(played.errors.front().find("TrueHD") != std::string::npos);
    CHECK(played.frames() == session->total_samples());
    CHECK(std::ranges::all_of(played.slots, [](const std::vector<float>& slot) {
        return std::ranges::all_of(slot, [](float v) { return v == 0.0F; });
    }));
}
