#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

#include "iclforge/ac3/core/types.hpp"
#include "iclforge/ac3/decoder/associated_service.hpp"
#include "iclforge/ac3/decoder/decoder.hpp"
#include "iclforge/ac3/encoder/eac3_frame.hpp"
#include "iclforge/ac3/meta/mixing.hpp"

// The §E3.10 mixer end to end: a main programme and a description encoded into
// one stream by this project's encoder with mixing metadata, both decoded by
// its decoder, mixed, and the result measured tone by tone. The unit tests next
// door hold the arithmetic; this holds that what the encoder writes, the decoder
// reports and the mixer reads are the same thing.

namespace {

using iclforge::ac3::Acmod;
using iclforge::ac3::AssociatedServiceMixer;
using iclforge::ac3::DecodedAccessUnit;
namespace meta = iclforge::ac3::meta;

// Tones with a whole number of cycles in the 4096-sample measuring window, so
// a single correlation reads each one's amplitude with no leakage to speak of.
constexpr double kBin = 48000.0 / 4096.0;
constexpr double kMainHz = 85.0 * kBin;
constexpr double kDescriptionHz = 26.0 * kBin;
constexpr double kLfeHz = 6.0 * kBin;
constexpr double kToneAmplitude = 0.4;
constexpr int kUnits = 12;
constexpr std::size_t kWindowStart = 3072;
constexpr std::size_t kWindow = 4096;

std::vector<std::byte> encode_main_and_description(const meta::MixMetadata& description_mix) {
    iclforge::ac3::eac3::AccessUnitConfig config;
    config.independent = {.bitrate_kbps = 448, .acmod = Acmod::k3_2, .lfe = true, .dialnorm = 27};
    iclforge::ac3::eac3::ProgrammeConfig description;
    description.independent = {
        .bitrate_kbps = 96, .acmod = Acmod::k1_0, .dialnorm = 20, .mixing = description_mix};
    config.additional.push_back(description);
    iclforge::ac3::eac3::AccessUnitEncoder encoder{config};
    const auto channels = static_cast<std::size_t>(encoder.channel_count());
    REQUIRE(channels == 7);
    // Coded order: the 5.1 bed (L C R Ls Rs LFE), then the description.
    const std::array<double, 7> tone = {kMainHz, kMainHz, kMainHz,       kMainHz,
                                        kMainHz, kLfeHz,  kDescriptionHz};
    std::vector<std::vector<float>> block(channels,
                                          std::vector<float>(iclforge::ac3::kSamplesPerFrame));
    std::vector<std::span<const float>> views(channels);
    std::vector<std::byte> stream;
    std::uint64_t n0 = 0;
    for (int f = 0; f < kUnits; ++f) {
        for (std::size_t ch = 0; ch < channels; ++ch) {
            for (std::size_t i = 0; i < block[ch].size(); ++i) {
                block[ch][i] = static_cast<float>(kToneAmplitude *
                                                  std::sin(2.0 * std::numbers::pi * tone[ch] *
                                                           static_cast<double>(n0 + i) / 48000.0));
            }
            views[ch] = block[ch];
        }
        n0 += iclforge::ac3::kSamplesPerFrame;
        const auto unit = encoder.encode_access_unit(views);
        REQUIRE(unit.has_value());
        stream.insert(stream.end(), unit->bytes.begin(), unit->bytes.end());
    }
    return stream;
}

// What one programme decodes to, one DecodedAccessUnit per frame period.
std::vector<DecodedAccessUnit> decode_programme(std::span<const std::byte> stream, int programme) {
    const auto units = iclforge::ac3::split_access_units(stream, programme);
    REQUIRE(units.has_value());
    iclforge::ac3::Eac3Decoder decoder{{.programme = programme}};
    std::vector<DecodedAccessUnit> out;
    for (const auto& unit : *units) {
        const auto decoded = decoder.decode_access_unit(unit);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        out.push_back(**decoded);
    }
    return out;
}

// The amplitude of the component of `x` at `hz` over the measuring window.
double tone_amplitude(const std::vector<float>& x, double hz) {
    REQUIRE(x.size() >= kWindowStart + kWindow);
    double re = 0.0;
    double im = 0.0;
    for (std::size_t i = 0; i < kWindow; ++i) {
        const double phase = 2.0 * std::numbers::pi * hz * static_cast<double>(i) / 48000.0;
        re += static_cast<double>(x[kWindowStart + i]) * std::cos(phase);
        im -= static_cast<double>(x[kWindowStart + i]) * std::sin(phase);
    }
    return 2.0 * std::hypot(re, im) / static_cast<double>(kWindow);
}

std::vector<float> joined(const std::vector<DecodedAccessUnit>& units, std::size_t channel) {
    std::vector<float> out;
    for (const auto& unit : units) {
        out.insert(out.end(), unit.channels[channel].begin(), unit.channels[channel].end());
    }
    return out;
}

}  // namespace

TEST_CASE("a description encoded with mixing metadata mixes into the main as written",
          "[eac3][mixing][associated]") {
    // The service ducks the main by 10 dB (extpgmscl 41) and sits hard right
    // (panmean 20, 30 degrees clockwise: the right speaker alone in Table E3.16).
    meta::MixMetadata description;
    description.pgmscl = 51;
    description.extpgmscl = 41;
    description.pan = meta::PanInfo{.panmean = 20};
    const auto stream = encode_main_and_description(description);

    auto main = decode_programme(stream, 0);
    const auto service = decode_programme(stream, 1);
    REQUIRE(main.size() == static_cast<std::size_t>(kUnits));
    REQUIRE(service.size() == main.size());
    REQUIRE(main.front().channels.size() == 6);
    REQUIRE(service.front().channels.size() == 1);
    // What the decoder reports is what the encoder wrote, which is what the
    // mixer reads.
    REQUIRE(service.front().mixing.has_value());
    REQUIRE(service.front().mixing->extpgmscl.has_value());
    CHECK(*service.front().mixing->extpgmscl == 41);
    REQUIRE(service.front().mixing->pan.has_value());
    CHECK(service.front().mixing->pan->panmean == 20);

    // The unmixed main, for the ducked level to be measured against.
    const double bed = tone_amplitude(joined(main, 0), kMainHz);
    CHECK(bed == Catch::Approx(kToneAmplitude).margin(0.01));
    CHECK(tone_amplitude(joined(main, 2), kDescriptionHz) < 0.005);

    AssociatedServiceMixer mixer;
    for (std::size_t k = 0; k < main.size(); ++k) {
        const auto result = mixer.mix(main[k], service[k]);
        REQUIRE(result.has_value());
        CHECK(result->main_gain_db == Catch::Approx(-10.0).margin(1e-9));
        REQUIRE(result->panmean.has_value());
        CHECK(*result->panmean == 20);
    }

    const double ducked = bed * std::pow(10.0, -10.0 / 20.0);
    // Every channel of the main carries its own tone 10 dB down, the LFE
    // included: §E3.10.2 says "all audio channels".
    for (const std::size_t channel : {0U, 1U, 2U, 3U, 4U}) {
        CAPTURE(channel);
        CHECK(tone_amplitude(joined(main, channel), kMainHz) ==
              Catch::Approx(ducked).margin(0.005));
    }
    CHECK(tone_amplitude(joined(main, 5), kLfeHz) ==
          Catch::Approx(kToneAmplitude * std::pow(10.0, -10.0 / 20.0)).margin(0.01));
    // The description is in the right speaker, at its own level, and nowhere
    // else.
    CHECK(tone_amplitude(joined(main, 2), kDescriptionHz) ==
          Catch::Approx(kToneAmplitude).margin(0.01));
    for (const std::size_t channel : {0U, 1U, 3U, 4U, 5U}) {
        CAPTURE(channel);
        CHECK(tone_amplitude(joined(main, channel), kDescriptionHz) < 0.005);
    }
}

TEST_CASE("a description with no mixing metadata plays at unity in the centre",
          "[eac3][mixing][associated]") {
    // A stream that sends neither scale nor pan says "no scaling" and "centre"
    // (§E2.3.1.12, §E2.3.1.16 and §E2.3.1.53).
    const auto stream = encode_main_and_description(meta::MixMetadata{});
    auto main = decode_programme(stream, 0);
    const auto service = decode_programme(stream, 1);
    REQUIRE(main.size() == service.size());
    const double bed = tone_amplitude(joined(main, 0), kMainHz);

    AssociatedServiceMixer mixer;
    for (std::size_t k = 0; k < main.size(); ++k) {
        const auto result = mixer.mix(main[k], service[k]);
        REQUIRE(result.has_value());
        CHECK(result->main_gain_db == Catch::Approx(0.0).margin(1e-9));
        CHECK(result->associated_gain_db == Catch::Approx(0.0).margin(1e-9));
    }
    CHECK(tone_amplitude(joined(main, 0), kMainHz) == Catch::Approx(bed).margin(0.005));
    CHECK(tone_amplitude(joined(main, 1), kDescriptionHz) ==
          Catch::Approx(kToneAmplitude).margin(0.01));
    for (const std::size_t channel : {0U, 2U, 3U, 4U, 5U}) {
        CAPTURE(channel);
        CHECK(tone_amplitude(joined(main, channel), kDescriptionHz) < 0.005);
    }
}
