#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"
#include "iclforge/truehd/encoder/layout.hpp"
#include "iclforge/truehd/encoder/ratectl.hpp"
#include "iclforge/truehd/io/access_unit.hpp"

// What the rate control and layout tests share: a mixed test signal for a 7.1 bed and object
// channels, and an encode that keeps the bytes, the statistics and the interval figures.

namespace iclforge::truehd::testing {

// One cycle of the signal is six phases of half a second each; every channel has a tone of its own
// and noise from a generator of its own, and the phases change what dominates:
//   0 quiet tones, 1 the objects take a mid tone with a little noise, 2 the objects take noise of
//   `object_noise_bits`, 3 silence, 4 the objects take a loud tone, 5 the bed takes noise of 11
//   bits.
// Values are 24 bit.
struct MixedSignal {
    int bed = 8;
    int objects = 12;
    int object_noise_bits = 14;
    std::uint32_t seed = 1;
    int phase_frames = 24000;

    [[nodiscard]] int width() const noexcept { return bed + objects; }

    // `count` frames starting at frame `first`, interleaved, bed channels first. A frame's values
    // do not depend on how the signal is cut into calls.
    [[nodiscard]] std::vector<std::int32_t> frames(std::uint64_t first, int count) const {
        const auto channels = static_cast<std::size_t>(width());
        std::vector<std::int32_t> pcm(channels * static_cast<std::size_t>(count));
        constexpr double kFull = 8388607.0;
        for (int i = 0; i < count; ++i) {
            const std::uint64_t t = first + static_cast<std::uint64_t>(i);
            const auto phase =
                static_cast<int>((t / static_cast<std::uint64_t>(phase_frames)) % 6U);
            for (int c = 0; c < width(); ++c) {
                // A noise value that is a function of (t, c, seed) alone.
                std::uint64_t h = (t * std::uint64_t{0x9E3779B97F4A7C15ULL}) ^
                                  (static_cast<std::uint64_t>(c + 1) * std::uint64_t{0xC2B2AE3D27D4EB4FULL}) ^
                                  (seed * std::uint64_t{0x165667B19E3779F9ULL});
                h ^= h >> 29U;
                h *= std::uint64_t{0xBF58476D1CE4E5B9ULL};
                h ^= h >> 32U;
                const double noise =
                    static_cast<double>(h & 0xFFFFFFU) / 16777216.0 * 2.0 - 1.0;  // -1 .. 1
                const double tone = std::sin(2.0 * std::numbers::pi * (110.0 + 53.0 * c) *
                                             static_cast<double>(t % 480000U) / 48000.0);
                const bool is_object = c >= bed;
                double v = 0.0;
                // The bed stays cheap (a quiet tone, a little noise in phase 5); the objects carry
                // what changes.
                switch (phase) {
                    case 0:
                        v = 0.002 * tone;
                        break;
                    case 1:
                        v = is_object ? 0.01 * tone + 0.0002 * noise : 0.002 * tone;
                        break;
                    case 2:
                        v = is_object ? noise * std::ldexp(1.0, object_noise_bits - 24)
                                      : 0.002 * tone;
                        break;
                    case 3:
                        v = 0.0;
                        break;
                    case 4:
                        v = is_object ? 0.3 * tone : 0.002 * tone;
                        break;
                    default:
                        v = is_object ? 0.01 * tone : std::ldexp(noise, 11 - 24);
                        break;
                }
                pcm[static_cast<std::size_t>(i) * channels + static_cast<std::size_t>(c)] =
                    static_cast<std::int32_t>(std::lround(std::clamp(v, -0.999, 0.999) * kFull));
            }
        }
        return pcm;
    }
};

struct StreamOut {
    std::vector<std::byte> bytes;
    std::vector<EncodedAccessUnit> units;  // kept only when asked
    EncoderStats stats;
    std::vector<IntervalRate> intervals;
};

// Encodes `frames` frames of `signal` in calls of `chunk` frames and returns what came out. With
// `keep_units` the units are kept apart as well as in `bytes`.
inline StreamOut encode_signal(const EncoderConfig& config, const MixedSignal& signal,
                               std::uint64_t frames, int chunk = 48000, bool keep_units = false) {
    auto encoder = Encoder::create(config);
    REQUIRE(encoder.has_value());
    StreamOut out;
    const auto take = [&](std::vector<EncodedAccessUnit>& units) {
        for (EncodedAccessUnit& unit : units) {
            out.bytes.insert(out.bytes.end(), unit.bytes.begin(), unit.bytes.end());
            if (keep_units) {
                out.units.push_back(std::move(unit));
            }
        }
    };
    for (std::uint64_t at = 0; at < frames; at += static_cast<std::uint64_t>(chunk)) {
        const int n = static_cast<int>(
            std::min<std::uint64_t>(static_cast<std::uint64_t>(chunk), frames - at));
        auto units = encoder->encode(signal.frames(at, n));
        if (!units) {
            UNSCOPED_INFO("encode failed: " << to_string(units.error().kind) << ": "
                                            << units.error().detail << " at frame " << at);
        }
        REQUIRE(units.has_value());
        take(*units);
    }
    auto tail = encoder->flush();
    REQUIRE(tail.has_value());
    take(*tail);
    out.stats = encoder->stats();
    out.intervals.assign(encoder->interval_rates().begin(), encoder->interval_rates().end());
    return out;
}

// The Atmos configuration the tests share: a 7.1 bed and `objects` object channels coded as they
// are (ObjectLayout::kIndependent), 48 kHz, 24 bit.
inline EncoderConfig atmos_config(int objects = 12) {
    EncoderConfig config;
    config.channels = 8;
    config.object_channels = objects;
    config.sample_rate = 48000;
    config.bits = 24;
    config.channel_assignment = 0;
    return config;
}

}  // namespace iclforge::truehd::testing
