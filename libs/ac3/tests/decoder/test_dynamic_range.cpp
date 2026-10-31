#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "iclforge/ac3/decoder/decoder.hpp"
#include "iclforge/ac3/decoder/dynamic_range.hpp"
#include "iclforge/ac3/encoder/eac3_frame.hpp"
#include "iclforge/ac3/encoder/encoder.hpp"
#include "iclforge/ac3/io/metadata_edit.hpp"
#include "iclforge/ac3/meta/drc.hpp"

// What a listener's choice of dynamic range does to the AC-3 and E-AC-3 decoders
// (iclforge/ac3/decoder/dynamic_range.hpp): standard is the decode a board made before the choice
// existed, to the bit; late night narrows the distance between a stream's loudest and quietest
// passages by the stream's own compr words, keeps dialogue where line mode puts it, and does nothing
// a stream's words do not allow.

namespace {

using iclforge::ac3::DecoderConfig;
using iclforge::ac3::OperatingMode;
using iclforge::base::DynamicRange;

using Planar = std::vector<std::vector<float>>;

// Real audio with dynamics: a tone whose amplitude steps between two levels (test_drc.cpp's
// reasoning: digital silence trips §7.2.2.1.1 and a steady tone never moves a compressor).
Planar stepped_tone(int frames, int channels, double loud, double quiet, int frames_per_step) {
    Planar out(static_cast<std::size_t>(channels),
               std::vector<float>(static_cast<std::size_t>(frames) * iclforge::ac3::kSamplesPerFrame));
    for (int f = 0; f < frames; ++f) {
        const double amplitude = (f / frames_per_step) % 2 == 0 ? loud : quiet;
        for (int n = 0; n < iclforge::ac3::kSamplesPerFrame; ++n) {
            const auto index = static_cast<std::size_t>(f) * iclforge::ac3::kSamplesPerFrame +
                               static_cast<std::size_t>(n);
            const double value =
                amplitude * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(index) / 48000.0);
            for (int ch = 0; ch < channels; ++ch) {
                out[static_cast<std::size_t>(ch)][index] = static_cast<float>(value);
            }
        }
    }
    return out;
}

std::vector<std::span<const float>> frame_views(const Planar& audio, int frame) {
    std::vector<std::span<const float>> views;
    for (const auto& channel : audio) {
        views.emplace_back(std::span{channel}.subspan(
            static_cast<std::size_t>(frame) * iclforge::ac3::kSamplesPerFrame, iclforge::ac3::kSamplesPerFrame));
    }
    return views;
}

double rms_db(std::span<const float> samples) {
    double sum = 0.0;
    for (const float value : samples) {
        sum += static_cast<double>(value) * static_cast<double>(value);
    }
    return 10.0 * std::log10(std::max(sum / static_cast<double>(samples.size()), 1e-30));
}

DecoderConfig line_config() { return DecoderConfig{.output = {.mode = OperatingMode::kLine}}; }

DecoderConfig late_night_config(DecoderConfig base = line_config()) {
    iclforge::ac3::apply_dynamic_range(DynamicRange::kLateNight, base);
    return base;
}

// What a decoder does with the words and the dialnorm and nothing else: dynrng and compr both off.
DecoderConfig words_ignored_config() {
    return DecoderConfig{.drc_scale = 0.0, .output = {.apply_dialnorm = true}};
}

const DecoderConfig kRfMode{.output = {.mode = OperatingMode::kRf}};

// A stepped programme the way a Dolby encoder sends one for a film: dynrng by the film standard
// profile in every block, and compr in every syncframe - a cut for the loud passage and a lift for
// the quiet one, which is what a Dolby encoder's RF profile writes. (This project's own heavy
// compressor is a limiter: it cuts peaks and never lifts, so its words are rewritten here.)
constexpr int kLoudFrames = 4;
constexpr int kFrames = 8;
constexpr double kLoudCutDb = -8.0;
constexpr double kQuietLiftDb = 10.0;

// What a word is worth once it is quantised: the figure a decode's level moves by.
double word_db(double requested_db) {
    return 20.0 * std::log10(iclforge::ac3::meta::compr_gain(iclforge::ac3::meta::encode_compr(requested_db)));
}

Planar programme() { return stepped_tone(kFrames, 2, 0.5, 0.004, kLoudFrames); }

iclforge::ac3::meta::Profile quick_profile() {
    auto profile = iclforge::ac3::meta::profile(iclforge::ac3::meta::ProfileId::kFilmStandard);
    // Film standard's release is a second: right for a programme, too slow to see in eight frames.
    profile.attack_ms = 5.0;
    profile.release_ms = 40.0;
    return profile;
}

void write_compr_words(std::vector<std::vector<std::byte>>& frames) {
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const bool loud = (i / kLoudFrames) % 2 == 0;
        const std::uint8_t word = iclforge::ac3::meta::encode_compr(loud ? kLoudCutDb : kQuietLiftDb);
        REQUIRE(iclforge::ac3::io::edit_frame_metadata(frames[i], {.compr = word}).has_value());
    }
}

// With `compr`, the frames carry the rewritten compr words; without, an encoder that never wrote
// one: only dynrng is on the wire.
std::vector<std::vector<std::byte>> ac3_frames(bool compr) {
    const auto audio = programme();
    iclforge::ac3::EncoderConfig config{.bitrate_kbps = 448, .dialnorm = 24, .drc = quick_profile()};
    if (compr) {
        config.heavy = iclforge::ac3::meta::HeavyConfig{};
    }
    iclforge::ac3::FrameEncoder encoder{config};
    std::vector<std::vector<std::byte>> frames;
    for (int frame = 0; frame < kFrames; ++frame) {
        auto encoded = encoder.encode_frame(frame_views(audio, frame));
        REQUIRE(encoded.has_value());
        frames.push_back(std::move(*encoded));
    }
    if (compr) {
        write_compr_words(frames);
    }
    return frames;
}

std::vector<std::vector<std::byte>> eac3_frames() {
    const auto audio = programme();
    iclforge::ac3::eac3::FrameEncoder encoder{{.bitrate_kbps = 448,
                                               .acmod = iclforge::ac3::Acmod::k2_0,
                                               .dialnorm = 24,
                                               .drc = quick_profile(),
                                               .heavy = iclforge::ac3::meta::HeavyConfig{}}};
    std::vector<std::vector<std::byte>> frames;
    for (int frame = 0; frame < kFrames; ++frame) {
        auto encoded = encoder.encode_frame(frame_views(audio, frame));
        REQUIRE(encoded.has_value());
        frames.push_back(std::move(*encoded));
    }
    write_compr_words(frames);
    return frames;
}

struct Levels {
    double loud = 0.0;   // frame 3, settled loud
    double quiet = 0.0;  // frame 7, settled quiet
    [[nodiscard]] double range() const { return loud - quiet; }
};

// Every channel of every frame of a decode, and the two settled levels.
struct Decode {
    Planar pcm;
    Levels levels;
};

void append(Planar& into, const Planar& frame) {
    if (into.size() < frame.size()) {
        into.resize(frame.size());
    }
    for (std::size_t ch = 0; ch < frame.size(); ++ch) {
        into[ch].insert(into[ch].end(), frame[ch].begin(), frame[ch].end());
    }
}

Decode decode_ac3(const std::vector<std::vector<std::byte>>& frames, const DecoderConfig& config) {
    iclforge::ac3::FrameDecoder decoder{config};
    Decode out;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const auto decoded = decoder.decode_frame(frames[i]);
        REQUIRE(decoded.has_value());
        append(out.pcm, decoded->channels);
        if (i == 3) {
            out.levels.loud = rms_db(decoded->channels[0]);
        } else if (i == 7) {
            out.levels.quiet = rms_db(decoded->channels[0]);
        }
    }
    return out;
}

Decode decode_eac3(const std::vector<std::vector<std::byte>>& frames, const DecoderConfig& config) {
    iclforge::ac3::Eac3Decoder decoder{config};
    Decode out;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const auto decoded = decoder.decode_access_unit(frames[i]);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        append(out.pcm, (*decoded)->channels);
        if (i == 3) {
            out.levels.loud = rms_db((*decoded)->channels[0]);
        } else if (i == 7) {
            out.levels.quiet = rms_db((*decoded)->channels[0]);
        }
    }
    return out;
}

// Least-squares gain of one decode against another, in dB: two decodes of the same frames that differ
// by a gain alone give that gain to many decimal places (every §7.7 gain is applied to the
// coefficients, and the dither generator is deterministic per decoder instance).
double gain_db(const Planar& reference, const Planar& other) {
    double cross = 0.0;
    double power = 0.0;
    for (std::size_t ch = 0; ch < std::min(reference.size(), other.size()); ++ch) {
        const std::size_t n = std::min(reference[ch].size(), other[ch].size());
        for (std::size_t i = 0; i < n; ++i) {
            cross += static_cast<double>(reference[ch][i]) * static_cast<double>(other[ch][i]);
            power += static_cast<double>(reference[ch][i]) * static_cast<double>(reference[ch][i]);
        }
    }
    REQUIRE(power > 0.0);
    return 20.0 * std::log10(cross / power);
}

}  // namespace

TEST_CASE("dynamic range: the words, the stored numbers and the default", "[dynamic-range]") {
    using iclforge::base::dynamic_range_from_stored;
    using iclforge::base::dynamic_range_name;
    using iclforge::base::parse_dynamic_range;

    // What PUT /dynamic-range takes and GET /dynamic-range says.
    CHECK(dynamic_range_name(DynamicRange::kStandard) == "standard");
    CHECK(dynamic_range_name(DynamicRange::kLateNight) == "late-night");
    CHECK(parse_dynamic_range("standard") == DynamicRange::kStandard);
    CHECK(parse_dynamic_range("late-night") == DynamicRange::kLateNight);
    CHECK(parse_dynamic_range("late-night\n") == DynamicRange::kLateNight);
    CHECK(parse_dynamic_range(" standard\r\n") == DynamicRange::kStandard);
    for (const char* bad : {"", "Standard", "late night", "late_night", "latenight", "wide", "1", "standard standard"}) {
        CHECK_FALSE(parse_dynamic_range(bad).has_value());
    }

    // What NVS holds is a number that is never renumbered: 0 and 1, and nothing else this build knows.
    static_assert(static_cast<int>(DynamicRange::kStandard) == 0);
    static_assert(static_cast<int>(DynamicRange::kLateNight) == 1);
    CHECK(dynamic_range_from_stored(0) == DynamicRange::kStandard);
    CHECK(dynamic_range_from_stored(1) == DynamicRange::kLateNight);
    CHECK_FALSE(dynamic_range_from_stored(2).has_value());
    CHECK_FALSE(dynamic_range_from_stored(-1).has_value());

    // A board that has nothing stored is the board it was before the choice existed.
    CHECK(iclforge::base::kDefaultDynamicRange == DynamicRange::kStandard);
}

TEST_CASE("dynamic range: standard leaves the configuration exactly as it was", "[dynamic-range][decoder]") {
    const DecoderConfig bases[] = {
        DecoderConfig{},
        line_config(),
        kRfMode,
        DecoderConfig{.drc_scale = 0.5, .drc_boost_scale = 0.25, .heavy_compression = true},
        DecoderConfig{.output = {.mode = OperatingMode::kCustom, .apply_dialnorm = true},
                      .concealment = iclforge::ac3::ConcealmentPolicy::kMute},
    };
    for (const DecoderConfig& base : bases) {
        DecoderConfig config = base;
        iclforge::ac3::apply_dynamic_range(DynamicRange::kStandard, config);
        CHECK(config.output.mode == base.output.mode);
        CHECK(config.output.apply_dialnorm == base.output.apply_dialnorm);
        CHECK(config.output.target == base.output.target);
        CHECK(config.drc_scale == base.drc_scale);
        CHECK(config.drc_boost_scale == base.drc_boost_scale);
        CHECK(config.heavy_compression == base.heavy_compression);
        CHECK(config.concealment == base.concealment);
    }
}

TEST_CASE("dynamic range: late night wins over what a build or a server said about compression",
          "[dynamic-range][decoder]") {
    // A server that sent RF mode, or a custom mode with the compression off, or a partial scale: the
    // listener's choice replaces the four things it is about, and nothing else.
    const DecoderConfig bases[] = {
        kRfMode,
        DecoderConfig{},
        DecoderConfig{.drc_scale = 0.25, .drc_boost_scale = 0.0},
        DecoderConfig{.output = {.target = iclforge::ac3::DownmixTarget::kLtRt, .mode = OperatingMode::kCustom},
                      .concealment = iclforge::ac3::ConcealmentPolicy::kMute},
    };
    for (const DecoderConfig& base : bases) {
        const DecoderConfig late = late_night_config(base);
        CHECK(late.output.mode == OperatingMode::kCustom);  // not RF: no 11 dB
        CHECK(late.output.apply_dialnorm);
        CHECK(late.heavy_compression);
        CHECK(late.drc_scale == 1.0);
        CHECK_FALSE(late.drc_boost_scale.has_value());
        CHECK(late.output.target == base.output.target);
        CHECK(late.concealment == base.concealment);
        CHECK(late.fast_imdct == base.fast_imdct);
    }
}

TEST_CASE("dynamic range: late night narrows an AC-3 stream's loudest-to-quietest distance by its compr words",
          "[dynamic-range][decoder][ac3]") {
    const auto frames = ac3_frames(true);
    const Decode coded = decode_ac3(frames, words_ignored_config());
    const Decode standard = decode_ac3(frames, line_config());
    const Decode late = decode_ac3(frames, late_night_config());
    const Decode rf = decode_ac3(frames, kRfMode);

    // Standard is line mode: the dynrng words, in full. Late night is the compr words, bare.
    CHECK(standard.levels.range() < coded.levels.range() - 5.0);
    CHECK(late.levels.range() < standard.levels.range() - 5.0);
    CHECK(late.levels.loud == Catch::Approx(coded.levels.loud + word_db(kLoudCutDb)).margin(0.05));
    CHECK(late.levels.quiet == Catch::Approx(coded.levels.quiet + word_db(kQuietLiftDb)).margin(0.05));

    // Neither pass has the 11 dB: late night is where line mode leaves dialogue, and RF mode, given
    // the same words, is exactly 11 dB above it.
    CHECK(gain_db(late.pcm, rf.pcm) == Catch::Approx(iclforge::ac3::meta::kRfModeGainDb).margin(1e-3));
}

TEST_CASE("dynamic range: a stream with no compr word decodes in late night exactly as in standard",
          "[dynamic-range][decoder][ac3]") {
    // §7.7.2.1's fallback for a syncframe with no compr word is its dynrng, in full, which is what
    // line mode applies: late night can be no stronger than the words the stream carries.
    const auto frames = ac3_frames(false);
    for (const auto& frame : frames) {
        const auto metadata = iclforge::ac3::io::read_frame_metadata(frame);
        REQUIRE(metadata.has_value());
        REQUIRE_FALSE(metadata->compr.has_value());
    }
    const Decode standard = decode_ac3(frames, line_config());
    const Decode late = decode_ac3(frames, late_night_config());
    REQUIRE(standard.pcm.size() == late.pcm.size());
    for (std::size_t ch = 0; ch < standard.pcm.size(); ++ch) {
        CHECK(standard.pcm[ch] == late.pcm[ch]);
    }
    // And the words that were there did compress, so the equality is not two silences.
    const Decode coded = decode_ac3(frames, words_ignored_config());
    CHECK(late.levels.range() < coded.levels.range() - 5.0);
}

TEST_CASE("dynamic range: late night narrows an E-AC-3 stream the same way", "[dynamic-range][decoder][eac3]") {
    const auto frames = eac3_frames();
    const Decode coded = decode_eac3(frames, words_ignored_config());
    const Decode standard = decode_eac3(frames, line_config());
    const Decode late = decode_eac3(frames, late_night_config());
    const Decode rf = decode_eac3(frames, kRfMode);

    CHECK(late.levels.range() < standard.levels.range() - 5.0);
    CHECK(late.levels.loud == Catch::Approx(coded.levels.loud + word_db(kLoudCutDb)).margin(0.05));
    CHECK(late.levels.quiet == Catch::Approx(coded.levels.quiet + word_db(kQuietLiftDb)).margin(0.05));
    CHECK(gain_db(late.pcm, rf.pcm) == Catch::Approx(iclforge::ac3::meta::kRfModeGainDb).margin(1e-3));
}

TEST_CASE("dynamic range: a Dolby-encoded Atmos stream takes the same words, objects and all",
          "[dynamic-range][decoder][eac3][joc]") {
    // The Dolby Encoding Engine's own 5.1.4 JOC stream (testdata/object-fixture): compr 0xFF in every
    // syncframe, which is -0.28 dB, and no dynrng. Standard (line mode) applies nothing to it; late
    // night applies that word to every channel the bed and the reconstructed objects make.
    std::ifstream in{ICLFORGE_GOLDEN_OBJECT_DIR "/dee_joc_514.ec3", std::ios::binary};
    REQUIRE(in.good());
    const std::string bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    std::vector<std::byte> stream(bytes.size());
    std::ranges::transform(bytes, stream.begin(), [](char c) { return static_cast<std::byte>(c); });
    const auto units = iclforge::ac3::split_access_units(stream);
    REQUIRE(units.has_value());
    REQUIRE(units->size() > 8);

    iclforge::ac3::Eac3Decoder standard{line_config()};
    iclforge::ac3::Eac3Decoder late{late_night_config()};
    Planar standard_pcm;
    Planar late_pcm;
    bool saw_objects = false;
    const std::size_t count = std::min<std::size_t>(units->size(), 24);
    for (std::size_t i = 0; i < count; ++i) {
        const auto a = standard.decode_access_unit((*units)[i]);
        const auto b = late.decode_access_unit((*units)[i]);
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        REQUIRE(a->has_value());
        REQUIRE(b->has_value());
        REQUIRE((*b)->compr == std::optional<std::uint8_t>{0xFF});
        saw_objects = saw_objects || (*b)->object_metadata.has_value();
        REQUIRE((*a)->channels.size() == (*b)->channels.size());
        if (i >= 2) {  // past the fade-in
            append(standard_pcm, (*a)->channels);
            append(late_pcm, (*b)->channels);
        }
    }
    CHECK(saw_objects);
    CHECK(gain_db(standard_pcm, late_pcm) ==
          Catch::Approx(20.0 * std::log10(iclforge::ac3::meta::compr_gain(0xFF))).margin(1e-3));
}
