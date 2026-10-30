#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"

// What the encoder's tests share: a test signal, a drain over the streaming API and a decode of
// the result.

namespace iclforge::truehd::testing {

// A different signal in every channel: a tone of its own, a slow amplitude change, noise from a
// generator of its own and, in the second half, a burst that changes the character of the
// material inside a restart interval. Values are `bits` wide.
inline std::vector<std::int32_t> make_pcm(int channels, int frames, int bits, std::uint32_t seed) {
    std::vector<std::int32_t> pcm(static_cast<std::size_t>(channels) *
                                  static_cast<std::size_t>(frames));
    const double full = static_cast<double>((1 << (bits - 1)) - 1);
    std::uint32_t state = seed * 2654435761U + 12345U;
    for (int t = 0; t < frames; ++t) {
        for (int c = 0; c < channels; ++c) {
            state = state * 1664525U + 1013904223U;
            const double noise = (static_cast<double>(state >> 8U) / 16777216.0 - 0.5);
            const double tone =
                std::sin(2.0 * std::numbers::pi * (220.0 + 137.0 * c) * t / 48000.0);
            const double level =
                0.05 + 0.4 * (0.5 + 0.5 * std::sin(2.0 * std::numbers::pi * 0.7 * t / 48000.0));
            double v = level * tone + 0.01 * noise;
            if (t > frames / 2 && t < frames / 2 + frames / 10) {
                v += 0.3 * noise;
            }
            pcm[static_cast<std::size_t>(t) * static_cast<std::size_t>(channels) +
                static_cast<std::size_t>(c)] =
                static_cast<std::int32_t>(std::lround(std::clamp(v, -0.999, 0.999) * full));
        }
    }
    return pcm;
}

struct Encoded {
    std::vector<EncodedAccessUnit> units;
    EncoderStats stats;
};

inline Encoded encode_all(const EncoderConfig& config, std::span<const std::int32_t> pcm,
                          std::size_t chunk_frames = 0) {
    auto encoder = Encoder::create(config);
    REQUIRE(encoder.has_value());
    Encoded out;
    const auto channels = static_cast<std::size_t>(config.channels);
    const std::size_t step = chunk_frames == 0 ? pcm.size() : chunk_frames * channels;
    for (std::size_t at = 0; at < pcm.size(); at += step) {
        auto units = encoder->encode(pcm.subspan(at, std::min(step, pcm.size() - at)));
        REQUIRE(units.has_value());
        for (auto& unit : *units) {
            out.units.push_back(std::move(unit));
        }
    }
    auto tail = encoder->flush();
    REQUIRE(tail.has_value());
    for (auto& unit : *tail) {
        out.units.push_back(std::move(unit));
    }
    out.stats = encoder->stats();
    return out;
}

// Decodes every unit and returns the PCM (the 24 bit domain shifted down to `bits`).
inline std::vector<std::int32_t> decode_all(const std::vector<EncodedAccessUnit>& units, int bits,
                                            int channels,
                                            Presentation presentation = Presentation::kBest,
                                            int* padding_total = nullptr) {
    DecoderConfig config;
    config.presentation = presentation;
    Decoder decoder(config);
    std::vector<std::int32_t> out;
    for (const EncodedAccessUnit& unit : units) {
        auto decoded = decoder.decode(unit.bytes);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        const DecodedAccessUnit& got = **decoded;
        REQUIRE(got.channels == channels);
        for (const std::int32_t sample : got.pcm) {
            out.push_back(sample >> (24 - bits));
        }
        if (padding_total != nullptr) {
            *padding_total += got.padding_samples;
        }
    }
    return out;
}

}  // namespace iclforge::truehd::testing
