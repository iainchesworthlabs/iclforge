#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

#include "encoder/coding.hpp"
#include "encoder/encoder_support.hpp"
#include "encoder/verify.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"

// The encoder's inner rules, checked where the round trip cannot reach them: a rule the encoder
// keeps whatever the signal (a coefficient floor no real signal gets near), a limit that only
// shows in the choice the encoder makes, and the comparison that stands guard over its own output.

using namespace iclforge::truehd;
using namespace iclforge::truehd::testing;

TEST_CASE("a filter's coeff_q is never below 8, and a big coefficient is clamped instead",
          "[truehd][encoder][coding]") {
    // TH spec §9.3: coeff_q is 8..15 and a coefficient is within ±32767.
    const std::array<double, 1> huge{200.0};
    const FilterSpec clamped = detail::quantise_fir(huge);
    CHECK(clamped.order == 1);
    CHECK(clamped.shift == 8);
    CHECK(clamped.coeff[0] == 32767);

    const std::array<double, 1> edge{127.0};  // 127 * 2^8 still fits
    const FilterSpec fits = detail::quantise_fir(edge);
    CHECK(fits.shift == 8);
    CHECK(fits.coeff[0] == 127 * 256);

    const std::array<double, 1> one{1.0};  // 1.0 * 2^15 does not fit, 2^14 does
    CHECK(detail::quantise_fir(one).shift == 14);
    const std::array<double, 1> quarter{0.25};
    CHECK(detail::quantise_fir(quarter).shift == 15);

    // Whatever comes in, what goes out is legal: coeff_q 8..15, coeff_bits + coeff_shift <= 16,
    // and the coefficients the decoder will rebuild are the stored ones.
    std::uint32_t state = 12345;
    for (int round = 0; round < 200; ++round) {
        std::vector<double> coefficients(static_cast<std::size_t>(1 + round % 8));
        for (double& c : coefficients) {
            state = state * 1664525U + 1013904223U;
            c = (static_cast<double>(state >> 8U) / 16777216.0 - 0.5) *
                (round % 3 == 0 ? 400.0 : 4.0);
        }
        const FilterSpec filter = detail::quantise_fir(coefficients);
        CAPTURE(round);
        CHECK(filter.shift >= 8);
        CHECK(filter.shift <= 15);
        CHECK(filter.coeff_bits >= 1);
        CHECK(filter.coeff_bits + filter.coeff_shift <= 16);
        for (std::size_t i = 0; i < coefficients.size(); ++i) {
            CHECK(filter.coeff[i] >= -32767);
            CHECK(filter.coeff[i] <= 32767);
            CHECK(filter.coeff[i] % (1 << filter.coeff_shift) == 0);
        }
    }
}

namespace {

// Four tones that only a filter of order 8 (two taps a tone) predicts well, in both channels.
std::vector<std::int32_t> tones(int frames) {
    std::vector<std::int32_t> pcm(static_cast<std::size_t>(frames) * 2);
    for (int t = 0; t < frames; ++t) {
        for (int c = 0; c < 2; ++c) {
            double v = 0.0;
            for (int k = 0; k < 4; ++k) {
                v += 0.2 * std::sin(2.0 * std::numbers::pi * (310.0 + 777.0 * k + 91.0 * c) * t /
                                    48000.0 * (1.0 + 0.0));
            }
            pcm[static_cast<std::size_t>(t) * 2 + static_cast<std::size_t>(c)] =
                static_cast<std::int32_t>(std::lround(v * 8000000.0));
        }
    }
    return pcm;
}

std::vector<std::vector<std::byte>> bytes_of(const Encoded& encoded) {
    std::vector<std::vector<std::byte>> out;
    for (const EncodedAccessUnit& unit : encoded.units) {
        out.push_back(unit.bytes);
    }
    return out;
}

Encoded encode_at(std::uint32_t rate, int max_fir_order, std::span<const std::int32_t> pcm) {
    EncoderConfig config;
    config.sample_rate = rate;
    config.channels = 2;
    config.restart_interval_units = 16;
    config.max_fir_order = max_fir_order;
    config.allow_rate_violations = true;
    return encode_all(config, pcm);
}

}  // namespace

TEST_CASE("at 176.4 and 192 kHz the encoder's FIR order stops at 4 (TH spec section 9.6)",
          "[truehd][encoder][coding]") {
    const std::vector<std::int32_t> pcm = tones(40000);
    // At 48 kHz the order matters for this signal: the stream differs between 4 and 8 ...
    CHECK(bytes_of(encode_at(48000, 8, pcm)) != bytes_of(encode_at(48000, 4, pcm)));
    // ... and at the two highest rates 8 is held to 4, so asking for either gives the same bytes.
    for (const std::uint32_t rate : {176400U, 192000U}) {
        CAPTURE(rate);
        CHECK(bytes_of(encode_at(rate, 8, pcm)) == bytes_of(encode_at(rate, 4, pcm)));
    }
    // 96 kHz has no such limit.
    CHECK(bytes_of(encode_at(96000, 8, pcm)) != bytes_of(encode_at(96000, 4, pcm)));
}

TEST_CASE("the verify comparison names a decode that is not the source",
          "[truehd][encoder][coding]") {
    // The encoder never writes a unit its decoder reads back wrong, so the comparison behind
    // EncoderConfig::verify is handed decodes that are.
    const std::vector<std::vector<std::int32_t>> source{{9, 1, 2, 3, 9}, {9, 4, 5, 6, 9}};
    std::vector<std::int32_t> pcm{1, 4, 2, 5, 3, 6};  // samples 1..3 of both channels, interleaved
    DecodedAccessUnit got;
    got.channels = 2;
    got.samples = 3;
    got.pcm = pcm;
    CHECK(detail::verify_mismatch(got, 2, 3, source, 1).empty());

    for (std::size_t at = 0; at < pcm.size(); ++at) {  // any one sample off, first to last
        CAPTURE(at);
        std::vector<std::int32_t> wrong = pcm;
        wrong[at] += 1;
        got.pcm = wrong;
        CHECK(detail::verify_mismatch(got, 2, 3, source, 1) ==
              "the decoded PCM differs from the input");
    }
    got.pcm = pcm;
    CHECK(detail::verify_mismatch(got, 2, 3, source, 0) ==
          "the decoded PCM differs from the input");

    DecodedAccessUnit shorter = got;
    shorter.samples = 2;
    CHECK(detail::verify_mismatch(shorter, 2, 3, source, 1) ==
          "the decoded unit has a different shape");
    DecodedAccessUnit narrower = got;
    narrower.channels = 1;
    CHECK(detail::verify_mismatch(narrower, 2, 3, source, 1) ==
          "the decoded unit has a different shape");
    DecodedAccessUnit truncated = got;
    truncated.pcm = std::span<const std::int32_t>(pcm).first(5);
    CHECK(detail::verify_mismatch(truncated, 2, 3, source, 1) ==
          "the decoded unit has a different shape");
}

TEST_CASE("verify has the decoder read back every unit it writes", "[truehd][encoder][coding]") {
    EncoderConfig config;
    config.restart_interval_units = 16;
    config.verify = true;
    const Encoded encoded = encode_all(config, make_pcm(2, 40 * 40, 24, 5));
    CHECK(encoded.stats.verified_units == encoded.stats.access_units);
    CHECK(encoded.stats.access_units == 40);
}

TEST_CASE("a refused call leaves the encoder as it was", "[truehd][encoder][coding]") {
    EncoderConfig config;
    config.bits = 16;
    config.restart_interval_units = 8;
    const std::vector<std::int32_t> pcm = make_pcm(2, 40 * 8, 16, 9);
    const Encoded whole = encode_all(config, pcm);

    // The same input, with a call in the middle whose last sample is out of range: nothing of
    // that call may have been kept, or the stream that follows would differ.
    auto encoder = Encoder::create(config);
    REQUIRE(encoder.has_value());
    std::vector<EncodedAccessUnit> units;
    const std::size_t half = pcm.size() / 2;
    auto first = encoder->encode(std::span<const std::int32_t>(pcm).first(half));
    REQUIRE(first.has_value());
    for (auto& unit : *first) {
        units.push_back(std::move(unit));
    }
    std::vector<std::int32_t> bad(pcm.begin() + static_cast<std::ptrdiff_t>(half), pcm.end());
    bad.back() = 40000;  // outside 16 bits
    auto refused = encoder->encode(bad);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().kind == EncodeErrorKind::kBadInput);
    auto rest = encoder->encode(std::span<const std::int32_t>(pcm).subspan(half));
    REQUIRE(rest.has_value());
    for (auto& unit : *rest) {
        units.push_back(std::move(unit));
    }
    auto tail = encoder->flush();
    REQUIRE(tail.has_value());
    for (auto& unit : *tail) {
        units.push_back(std::move(unit));
    }
    REQUIRE(units.size() == whole.units.size());
    for (std::size_t i = 0; i < units.size(); ++i) {
        CHECK(units[i].bytes == whole.units[i].bytes);
    }
    CHECK(encoder->stats().input_samples == 40 * 8);
}
