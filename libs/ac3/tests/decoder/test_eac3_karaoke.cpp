#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <span>
#include <vector>

#include "iclforge/ac3/core/eac3_tables.hpp"
#include "iclforge/ac3/core/tables.hpp"
#include "iclforge/ac3/decoder/decoder.hpp"
#include "iclforge/ac3/decoder/output.hpp"
#include "iclforge/ac3/encoder/eac3_frame.hpp"
#include "iclforge/ac3/meta/bsi.hpp"
#include "iclforge/ac3/meta/mixing.hpp"

// Karaoke in E-AC-3 (planning/karaoke-eac3.md, D1 to D3).
//
// A/52 carries a karaoke stream's signalling in E-AC-3 field for field - bsmod 7 above 1/0 in
// Table E1.2's infomdate group, whose meaning Annex E takes from Table 5.7 - and defines no
// reproduction for it. Dolby's own decoder reproduces Annex C's for one independent substream, and
// the ratios below are what it did on streams of this shape (planning/karaoke-eac3-findings.md,
// measured 2026-10-12 with the Dolby Reference Player's dlbac3dec in line mode): V1 into L and V2
// into R at the stream's Lo/Ro surround level (-3 dB when mixmdate is absent, E-AC-3's default),
// the melody in the centre at the same gain as L and R, the LFE untouched in its ratio to them.
// The AC-3 decoder's own reproduction has its tests in test_output_stage.cpp; what is held here is
// that Eac3Decoder reaches the same stage, on the same rules, from an E-AC-3 stream, and gates it
// as D3 says.

namespace {

namespace ac3 = iclforge::ac3;
namespace cm = iclforge::ac3::eac3::chanmap;

constexpr double kAmplitude = 0.25;
// Coded order of a 3/2 bed: L, C, R, Ls, Rs - Annex C's L, M, R, V1, V2 - and the LFE after them.
constexpr double kHzL = 300.0;
constexpr double kHzM = 700.0;
constexpr double kHzR = 500.0;
constexpr double kHzV1 = 900.0;
constexpr double kHzV2 = 1100.0;
constexpr double kHzLfe = 60.0;  // below the LFE's 120 Hz band limit
constexpr int kFrames = 9;
// Dolby's decoder, measured, and Table C3.2's 2/0 and 3/0 columns: the vocal's level against L.
constexpr double kMinus3dB = 0.7071067811865476;
constexpr double kMinus6dB = 0.5;

ac3::eac3::FrameConfig karaoke_bed(ac3::Acmod acmod, bool lfe,
                                   std::optional<ac3::meta::MixMetadata> mixing = std::nullopt,
                                   ac3::meta::BitstreamMode bsmod =
                                       ac3::meta::BitstreamMode::kVoiceOverOrKaraoke) {
    return {.bitrate_kbps = 448,
            .acmod = acmod,
            .lfe = lfe,
            .mixing = mixing,
            .info = ac3::meta::BsiInfo{.bsmod = bsmod}};
}

// Tones for the coded channels of a config, in the order encode_access_unit takes them: the
// independent substream's full-bandwidth channels, its LFE, then each dependent's the same way.
std::vector<double> tones_for(const ac3::eac3::AccessUnitConfig& config,
                              std::span<const double> hz_by_channel) {
    std::vector<double> tones;
    const auto take = [&](const ac3::eac3::FrameConfig& sub) {
        const auto count = static_cast<std::size_t>(ac3::fullbw_channel_count(sub.acmod));
        for (std::size_t i = 0; i < count; ++i) {
            tones.push_back(hz_by_channel[tones.size() % hz_by_channel.size()]);
        }
        if (sub.lfe) {
            tones.push_back(kHzLfe);
        }
    };
    take(config.independent);
    for (const auto& dependent : config.dependents) {
        take(dependent);
    }
    return tones;
}

std::vector<std::vector<std::byte>> encode_units(const ac3::eac3::AccessUnitConfig& config,
                                                 const std::vector<double>& tones, int frames,
                                                 std::optional<std::uint64_t> burst_at = std::nullopt) {
    ac3::eac3::AccessUnitEncoder encoder{config};
    const auto channels = static_cast<std::size_t>(encoder.channel_count());
    REQUIRE(channels == tones.size());
    std::vector<std::vector<float>> block(channels,
                                          std::vector<float>(ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views(channels);
    std::vector<std::vector<std::byte>> units;
    std::uint64_t n0 = 0;
    for (int f = 0; f < frames; ++f) {
        for (std::size_t ch = 0; ch < channels; ++ch) {
            for (std::size_t i = 0; i < block[ch].size(); ++i) {
                double v = kAmplitude * std::sin(2.0 * std::numbers::pi * tones[ch] *
                                                 static_cast<double>(n0 + i) / 48000.0);
                // A short decaying burst on the first channel: what makes the encoder signal a
                // transient correction, so the decoder holds frames back that flush() must return.
                if (ch == 0 && burst_at.has_value() && n0 + i >= *burst_at &&
                    n0 + i < *burst_at + 384) {
                    const auto k = static_cast<double>(n0 + i - *burst_at);
                    v += 0.6 * std::sin(2.0 * std::numbers::pi * 5000.0 * k / 48000.0) *
                         std::exp(-k / 96.0);
                }
                block[ch][i] = static_cast<float>(v);
            }
            views[ch] = block[ch];
        }
        n0 += ac3::kSamplesPerFrame;
        const auto unit = encoder.encode_access_unit(views);
        REQUIRE(unit.has_value());
        units.emplace_back(unit->bytes.begin(), unit->bytes.end());
    }
    return units;
}

// One decode of a whole stream: every access unit's channels joined, and what the first said.
struct Heard {
    std::vector<std::vector<float>> channels;
    ac3::eac3::chanmap::Layout layout;
    ac3::Acmod acmod = ac3::Acmod::k2_0;
    int substreams = 0;
    int units = 0;
    std::size_t flushed = 0;  // how many substreams flush() handed back
};

Heard decode_all(const std::vector<std::vector<std::byte>>& units, const ac3::OutputConfig& output,
                 bool flush_tail = false) {
    ac3::Eac3Decoder decoder{{.output = output}};
    Heard heard;
    const auto append = [&](const std::vector<std::vector<float>>& channels) {
        if (heard.channels.empty()) {
            heard.channels.resize(channels.size());
        }
        REQUIRE(channels.size() == heard.channels.size());
        for (std::size_t ch = 0; ch < channels.size(); ++ch) {
            heard.channels[ch].insert(heard.channels[ch].end(), channels[ch].begin(),
                                      channels[ch].end());
        }
    };
    for (const auto& unit : units) {
        const auto decoded = decoder.decode_access_unit(unit);
        REQUIRE(decoded.has_value());
        if (!decoded->has_value()) {
            continue;
        }
        const auto& got = **decoded;
        if (heard.units == 0) {
            heard.layout = got.layout;
            heard.acmod = got.acmod;
            heard.substreams = got.substream_count;
        }
        ++heard.units;
        append(got.channels);
    }
    if (flush_tail) {
        for (const auto& tail : decoder.flush()) {
            ++heard.flushed;
            append(tail.channels);
        }
    }
    return heard;
}

// The amplitude of a sine at `hz` in `x`, by correlation over the steady-state middle.
double level_at(const std::vector<float>& x, double hz) {
    constexpr std::size_t kStart = 4096;
    constexpr std::size_t kLength = 8192;
    REQUIRE(x.size() >= kStart + kLength);
    const double step = 2.0 * std::numbers::pi * hz / 48000.0;
    double re = 0.0;
    double im = 0.0;
    for (std::size_t i = 0; i < kLength; ++i) {
        const double phase = step * static_cast<double>(kStart + i);
        re += static_cast<double>(x[kStart + i]) * std::cos(phase);
        im += static_cast<double>(x[kStart + i]) * std::sin(phase);
    }
    return 2.0 * std::hypot(re, im) / static_cast<double>(kLength);
}

ac3::OutputConfig aware() {
    ac3::OutputConfig config;
    config.karaoke = ac3::KaraokeReproduction::kAware;
    return config;
}

ac3::OutputConfig capable(ac3::KaraokeVocals vocals) {
    ac3::OutputConfig config;
    config.karaoke = ac3::KaraokeReproduction::kCapable;
    config.karaoke_vocals = vocals;
    return config;
}

constexpr std::array<double, 5> kBedHz = {kHzL, kHzM, kHzR, kHzV1, kHzV2};

ac3::meta::MixMetadata lo_ro(ac3::meta::MixLevel centre, ac3::meta::MixLevel surround) {
    ac3::meta::MixMetadata mixing;
    mixing.lorocmixlev = centre;
    mixing.lorosurmixlev = surround;
    return mixing;
}

}  // namespace

TEST_CASE("E-AC-3 karaoke: aware comes back as L, C, R and the LFE, V1 and V2 at the Lo/Ro level",
          "[eac3][decoder][karaoke]") {
    // 3/2 + LFE, bsmod 7. No mixmdate: E-AC-3's own default for the Lo/Ro surround level, -3 dB,
    // which is what the Dolby decoder used, and not AC-3's -6 dB karaoke default.
    const ac3::eac3::AccessUnitConfig config{
        .independent = karaoke_bed(ac3::Acmod::k3_2, true)};
    const auto units = encode_units(config, tones_for(config, kBedHz), kFrames);

    const Heard coded = decode_all(units, {});
    const Heard heard = decode_all(units, aware());

    REQUIRE(coded.channels.size() == 6);
    REQUIRE(heard.channels.size() == 4);  // L, C, R, LFE
    // The unit still says what was coded; the channels are what was heard.
    CHECK(heard.acmod == ac3::Acmod::k3_2);

    const double l = level_at(heard.channels[0], kHzL);
    CHECK(l > 0.05);  // the scale-down leaves L well above silence
    // Ratios, which is what the reference decoder fixes (the scale-down is an implementation's):
    CHECK(level_at(heard.channels[0], kHzV1) / l == Catch::Approx(kMinus3dB).margin(0.03));
    CHECK(level_at(heard.channels[2], kHzV2) / level_at(heard.channels[2], kHzR) ==
          Catch::Approx(kMinus3dB).margin(0.03));
    CHECK(level_at(heard.channels[1], kHzM) / l == Catch::Approx(1.0).margin(0.03));
    // V1 is on the left only and V2 on the right only; nothing of either in the centre.
    CHECK(level_at(heard.channels[2], kHzV1) < 0.01);
    CHECK(level_at(heard.channels[0], kHzV2) < 0.01);
    CHECK(level_at(heard.channels[1], kHzV1) < 0.01);
    CHECK(level_at(heard.channels[1], kHzV2) < 0.01);
    // The LFE is there, behind the three.
    CHECK(level_at(heard.channels[3], kHzLfe) > 0.05);

    // As coded the vocals are on the surround channels, where the stream put them.
    CHECK(level_at(coded.channels[3], kHzV1) > 0.1);
    CHECK(level_at(coded.channels[4], kHzV2) > 0.1);
}

TEST_CASE("E-AC-3 karaoke: the levels are the mixing group's Lo/Ro levels", "[eac3][decoder][karaoke]") {
    // lorosurmixlev -6 dB: the vocals at half, as the Dolby decoder placed them for such a stream.
    const ac3::eac3::AccessUnitConfig config{
        .independent = karaoke_bed(
            ac3::Acmod::k3_2, false,
            lo_ro(ac3::meta::MixLevel::kMinus6dB, ac3::meta::MixLevel::kMinus6dB))};
    const auto units = encode_units(config, tones_for(config, kBedHz), kFrames);
    const Heard heard = decode_all(units, aware());
    REQUIRE(heard.channels.size() == 3);
    const double l = level_at(heard.channels[0], kHzL);
    CHECK(level_at(heard.channels[0], kHzV1) / l == Catch::Approx(kMinus6dB).margin(0.03));
    CHECK(level_at(heard.channels[2], kHzV2) / level_at(heard.channels[2], kHzR) ==
          Catch::Approx(kMinus6dB).margin(0.03));

    // The caller's own level replaces the stream's, as it does for any fold.
    ac3::OutputConfig override_slev = aware();
    override_slev.mix_override.loro_slev = ac3::meta::level::kMinus3dB;
    const Heard overridden = decode_all(units, override_slev);
    REQUIRE(overridden.channels.size() == 3);
    CHECK(level_at(overridden.channels[0], kHzV1) / level_at(overridden.channels[0], kHzL) ==
          Catch::Approx(kMinus3dB).margin(0.03));
}

TEST_CASE("E-AC-3 karaoke: capable chooses the vocals and the choice reaches the centre",
          "[eac3][decoder][karaoke]") {
    const ac3::eac3::AccessUnitConfig config{
        .independent = karaoke_bed(ac3::Acmod::k3_2, false)};
    const auto units = encode_units(config, tones_for(config, kBedHz), kFrames);

    {
        const Heard none = decode_all(units, capable(ac3::KaraokeVocals::kNone));
        REQUIRE(none.channels.size() == 3);
        CHECK(level_at(none.channels[0], kHzV1) < 0.01);
        CHECK(level_at(none.channels[2], kHzV2) < 0.01);
        CHECK(level_at(none.channels[1], kHzV1) < 0.01);
        CHECK(level_at(none.channels[1], kHzM) > 0.05);  // the melody and the music stay
    }
    {
        // Table C3.3, 3/0: a single chosen vocal sits in the centre beside M at unity.
        const Heard v1 = decode_all(units, capable(ac3::KaraokeVocals::kV1));
        REQUIRE(v1.channels.size() == 3);
        const double m = level_at(v1.channels[1], kHzM);
        CHECK(level_at(v1.channels[1], kHzV1) / m == Catch::Approx(1.0).margin(0.03));
        CHECK(level_at(v1.channels[1], kHzV2) < 0.01);
        CHECK(level_at(v1.channels[0], kHzV1) < 0.01);
    }
    {
        const Heard v2 = decode_all(units, capable(ac3::KaraokeVocals::kV2));
        REQUIRE(v2.channels.size() == 3);
        const double m = level_at(v2.channels[1], kHzM);
        CHECK(level_at(v2.channels[1], kHzV2) / m == Catch::Approx(1.0).margin(0.03));
        CHECK(level_at(v2.channels[1], kHzV1) < 0.01);
    }
    {
        // Both: V1 left and V2 right at unity.
        const Heard both = decode_all(units, capable(ac3::KaraokeVocals::kBoth));
        REQUIRE(both.channels.size() == 3);
        CHECK(level_at(both.channels[0], kHzV1) / level_at(both.channels[0], kHzL) ==
              Catch::Approx(1.0).margin(0.03));
        CHECK(level_at(both.channels[2], kHzV2) / level_at(both.channels[2], kHzR) ==
              Catch::Approx(1.0).margin(0.03));
    }

    // Annex C.2.3's levels: one part's gain moves that part against the rest. With the melody 6 dB
    // down, the vocal beside it in the centre is twice the melody (both are in one output, so the
    // scale-down that holds the sums cancels in the ratio).
    ac3::OutputConfig quieter = capable(ac3::KaraokeVocals::kV1);
    quieter.karaoke_melody_gain = ac3::karaoke_gain_from_db(-6.0);
    const Heard moved = decode_all(units, quieter);
    REQUIRE(moved.channels.size() == 3);
    CHECK(level_at(moved.channels[1], kHzV1) / level_at(moved.channels[1], kHzM) ==
          Catch::Approx(2.0).margin(0.08));
}

TEST_CASE("E-AC-3 karaoke: one vocal is a single V1, and V2 of that stream is nothing",
          "[eac3][decoder][karaoke]") {
    // 3/1: L, M, R, V1. Annex C Table C3.2, one vocal at 3/0: V1 in the centre at slev beside M.
    const ac3::eac3::AccessUnitConfig config{
        .independent = karaoke_bed(ac3::Acmod::k3_1, false)};
    const auto units = encode_units(config, tones_for(config, kBedHz), kFrames);

    const Heard aware_heard = decode_all(units, aware());
    REQUIRE(aware_heard.channels.size() == 3);
    const double m = level_at(aware_heard.channels[1], kHzM);
    CHECK(level_at(aware_heard.channels[1], kHzV1) / m == Catch::Approx(kMinus3dB).margin(0.03));

    // Choosing the vocal the stream lacks reproduces none.
    const Heard v2 = decode_all(units, capable(ac3::KaraokeVocals::kV2));
    REQUIRE(v2.channels.size() == 3);
    CHECK(level_at(v2.channels[1], kHzV1) < 0.01);
    CHECK(level_at(v2.channels[1], kHzM) > 0.05);
}

TEST_CASE("E-AC-3 karaoke: only a programme the stream calls karaoke is reproduced",
          "[eac3][decoder][karaoke]") {
    const std::array<double, 5> hz = kBedHz;

    SECTION("a complete main service with the same audio is decoded as coded") {
        const ac3::eac3::AccessUnitConfig config{.independent = karaoke_bed(
                                                     ac3::Acmod::k3_2, true, std::nullopt,
                                                     ac3::meta::BitstreamMode::kCompleteMain)};
        const auto units = encode_units(config, tones_for(config, hz), kFrames);
        const Heard coded = decode_all(units, {});
        const Heard asked = decode_all(units, aware());
        REQUIRE(asked.channels.size() == 6);
        const bool same = asked.channels == coded.channels;  // not CHECK(a == b): Catch dumps floats
        CHECK(same);
    }
    SECTION("voice over at 1/0 is not karaoke") {
        const ac3::eac3::AccessUnitConfig config{.independent = karaoke_bed(ac3::Acmod::k1_0, false)};
        const auto units = encode_units(config, tones_for(config, hz), kFrames);
        const Heard asked = decode_all(units, aware());
        CHECK(asked.channels.size() == 1);
    }
    SECTION("a karaoke bed that nothing asked to reproduce is decoded as coded") {
        const ac3::eac3::AccessUnitConfig config{.independent = karaoke_bed(ac3::Acmod::k3_2, true)};
        const auto units = encode_units(config, tones_for(config, hz), kFrames);
        const Heard coded = decode_all(units, {});
        REQUIRE(coded.channels.size() == 6);
    }
    SECTION("2/0 karaoke has nothing to reproduce and passes through") {
        const ac3::eac3::AccessUnitConfig config{.independent = karaoke_bed(ac3::Acmod::k2_0, false)};
        const auto units = encode_units(config, tones_for(config, hz), kFrames);
        const Heard asked = decode_all(units, aware());
        CHECK(asked.channels.size() == 2);
    }
}

TEST_CASE("E-AC-3 karaoke: a dependent substream or an object layer is decoded as coded (D3)",
          "[eac3][decoder][karaoke]") {
    // A 7.1 programme: a karaoke-flagged 5.1 bed and the rear pair in a dependent substream. The
    // specification gives a dependent's chanmap no V1 or V2, so nothing is reproduced and the
    // report says it is eight channels as coded; Dolby's decoder scales the bed and folds nothing.
    const ac3::eac3::FrameConfig rear{
        .bitrate_kbps = 320, .acmod = ac3::Acmod::k2_2, .chanmap = cm::k71Rear};
    const ac3::eac3::AccessUnitConfig config{
        .independent = karaoke_bed(ac3::Acmod::k3_2, true), .dependents = {rear}};
    const std::vector<double> tones = {kHzL, kHzM, kHzR, kHzV1, kHzV2, kHzLfe, 1300.0, 1500.0,
                                       1700.0, 1900.0};
    const auto units = encode_units(config, tones, kFrames);

    const Heard coded = decode_all(units, {});
    const Heard asked = decode_all(units, aware());
    REQUIRE(coded.channels.size() == 8);
    REQUIRE(asked.channels.size() == 8);
    CHECK(asked.substreams == 2);
    const bool same = asked.channels == coded.channels;
    CHECK(same);
}

TEST_CASE("E-AC-3 karaoke: a stereo or mono target is the 2/0 reproduction, whatever the layout",
          "[eac3][decoder][karaoke]") {
    const ac3::eac3::AccessUnitConfig config{
        .independent = karaoke_bed(ac3::Acmod::k3_2, false)};
    const auto units = encode_units(config, tones_for(config, kBedHz), kFrames);

    ac3::OutputConfig stereo = aware();
    stereo.target = ac3::DownmixTarget::kLoRo;
    const Heard heard = decode_all(units, stereo);
    REQUIRE(heard.channels.size() == 2);
    // Table C3.2's 2/0 column: V1 only in the left, V2 only in the right, at slev.
    const double l = level_at(heard.channels[0], kHzL);
    CHECK(level_at(heard.channels[0], kHzV1) / l == Catch::Approx(kMinus3dB).margin(0.03));
    CHECK(level_at(heard.channels[1], kHzV1) < 0.01);
    CHECK(level_at(heard.channels[0], kHzV2) < 0.01);
}

TEST_CASE("E-AC-3 karaoke: the last access unit has the channel count the rest had",
          "[eac3][decoder][karaoke]") {
    // flush() folds what the hold-back still has, one substream at a time; for a karaoke stream it
    // has to come out as L, C, R and the LFE too, or a sink opened for four channels would be
    // handed six for the final unit.
    // A burst early on engages transient pre-noise processing, which makes the decoder hold
    // frames back; a second near the end keeps some of them held at the end of the stream.
    ac3::eac3::FrameConfig bed = karaoke_bed(ac3::Acmod::k3_2, true);
    bed.transient_prenoise = true;
    const ac3::eac3::AccessUnitConfig config{.independent = bed};
    const auto units = encode_units(config, tones_for(config, kBedHz), kFrames,
                                    std::uint64_t{1} * ac3::kSamplesPerFrame + 900);
    const Heard heard = decode_all(units, aware(), /*flush_tail=*/true);
    // decode_all() requires every piece, flushed ones included, to have the same channel count
    // as the first, so a tail left as six channels would have failed there.
    REQUIRE(heard.channels.size() == 4);
    CHECK(heard.flushed > 0);
    for (const auto& channel : heard.channels) {
        CHECK(channel.size() >= static_cast<std::size_t>(kFrames) * ac3::kSamplesPerFrame);
    }
}

TEST_CASE("E-AC-3 karaoke: the block form hands the sink the reproduction's channels",
          "[eac3][decoder][karaoke]") {
    const ac3::eac3::AccessUnitConfig config{.independent = karaoke_bed(ac3::Acmod::k3_2, true)};
    const auto units = encode_units(config, tones_for(config, kBedHz), 3);

    ac3::Eac3Decoder decoder{{.output = aware()}};
    std::size_t widest = 0;
    std::size_t narrowest = 99;
    int blocks = 0;
    const auto sink = [&](const iclforge::render::PcmBlock& block) {
        widest = std::max(widest, block.channels.size());
        narrowest = std::min(narrowest, block.channels.size());
        ++blocks;
    };
    for (const auto& unit : units) {
        const auto decoded = decoder.decode_access_unit_by_block(unit, sink);
        REQUIRE(decoded.has_value());
    }
    CHECK(blocks > 0);
    CHECK(widest == 4);
    CHECK(narrowest == 4);
}
