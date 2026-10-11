// The integer PCM unpack, tested on the host (iclforge/pcm_unpack.hpp).
//
// What the C6 player does to a player@v1 chunk without a floating-point unit is only worth having
// if it is the same samples: the float a sample becomes is checked against the division it replaces
// for every 16-bit value and every 24-bit one, and a chunk's unpack and levels against a plain
// loop.

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

#include "iclforge/pcm_unpack.hpp"

namespace {

using iclforge::pcm_to_float;
using iclforge::PcmLevel;
using iclforge::unpack_pcm16;
using iclforge::unpack_pcm24;

template <unsigned Bits>
std::size_t differing_values() {
    constexpr std::int32_t kLow = -(std::int32_t{1} << (Bits - 1));
    constexpr std::int32_t kHigh = (std::int32_t{1} << (Bits - 1)) - 1;
    const float scale = 1.0F / static_cast<float>(std::int32_t{1} << (Bits - 1));
    std::size_t different = 0;
    for (std::int32_t k = kLow; k <= kHigh; ++k) {
        // What the player did before: convert, then divide by the power of two. The divide is
        // exact, so it is the same as the multiply by the reciprocal, which the optimiser may turn
        // it into.
        const float want =
            static_cast<float>(k) / static_cast<float>(std::int32_t{1} << (Bits - 1));
        const float got = pcm_to_float<Bits>(k);
        different +=
            std::bit_cast<std::uint32_t>(want) == std::bit_cast<std::uint32_t>(got) ? 0U : 1U;
        different += want == static_cast<float>(k) * scale ? 0U : 1U;
    }
    return different;
}

std::vector<std::span<float>> planes(std::vector<std::vector<float>>& storage) {
    std::vector<std::span<float>> out;
    for (std::vector<float>& plane : storage) {
        out.emplace_back(plane);
    }
    return out;
}

}  // namespace

TEST_CASE("pcm unpack: every 16-bit sample is the float the division gives, bit for bit",
          "[device][pcm]") {
    CHECK(differing_values<16>() == 0);
    // And the widths between, which nothing uses but the arithmetic is the same for.
    CHECK(differing_values<8>() == 0);
    CHECK(differing_values<12>() == 0);
}

TEST_CASE("pcm unpack: every 24-bit sample is the float the division gives, bit for bit",
          "[device][pcm]") {
    CHECK(differing_values<24>() == 0);
}

TEST_CASE("pcm unpack: the ends of the range, and zero without a sign", "[device][pcm]") {
    CHECK(pcm_to_float<16>(0) == 0.0F);
    CHECK_FALSE(std::signbit(pcm_to_float<16>(0)));
    CHECK(pcm_to_float<16>(-32768) == -1.0F);
    CHECK(pcm_to_float<16>(32767) == 32767.0F / 32768.0F);
    CHECK(pcm_to_float<16>(1) == 1.0F / 32768.0F);
    CHECK(pcm_to_float<24>(-8388608) == -1.0F);
    CHECK(pcm_to_float<24>(8388607) == 8388607.0F / 8388608.0F);
}

TEST_CASE("pcm unpack: a 16-bit stereo chunk is planar floats and its levels, at any alignment",
          "[device][pcm]") {
    std::mt19937 random(7);
    constexpr std::size_t kFrames = 1000;
    std::vector<std::int16_t> samples(kFrames * 2);
    for (std::int16_t& sample : samples) {
        sample = static_cast<std::int16_t>(random());
    }
    samples[10] = -32768;
    samples[11] = 32767;
    // The chunk's bytes start one byte into a buffer, as a payload after an odd-length header
    // would.
    std::vector<std::uint8_t> buffer(1 + (kFrames * 4));
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const auto u = static_cast<std::uint16_t>(samples[i]);
        buffer[1 + (i * 2)] = static_cast<std::uint8_t>(u & 0xFFU);
        buffer[2 + (i * 2)] = static_cast<std::uint8_t>(u >> 8U);
    }
    std::vector<std::vector<float>> storage(2, std::vector<float>(kFrames));
    const std::vector<std::span<float>> out = planes(storage);
    std::array<PcmLevel, 2> levels{};
    unpack_pcm16(std::span<const std::uint8_t>(buffer).subspan(1), 2, kFrames, out, levels);

    for (std::size_t c = 0; c < 2; ++c) {
        std::uint32_t peak = 0;
        std::uint64_t sum = 0;
        for (std::size_t k = 0; k < kFrames; ++k) {
            const std::int32_t value = samples[(k * 2) + c];
            CHECK(storage[c][k] == static_cast<float>(value) / 32768.0F);
            peak = std::max(peak, static_cast<std::uint32_t>(std::abs(value)));
            sum += static_cast<std::uint64_t>(value) * static_cast<std::uint64_t>(value);
        }
        CHECK(levels[c].peak == peak);
        CHECK(levels[c].sum_squares == sum);
    }
    CHECK(levels[0].peak == 32768);

    // Levels carry on across calls: the next block's are added to this one's.
    const std::array<PcmLevel, 2> first = levels;
    unpack_pcm16(std::span<const std::uint8_t>(buffer).subspan(1), 2, kFrames, out, levels);
    for (std::size_t c = 0; c < 2; ++c) {
        CHECK(levels[c].peak == first[c].peak);
        CHECK(levels[c].sum_squares == 2 * first[c].sum_squares);
    }
}

TEST_CASE("pcm unpack: a level is the peak and mean square of the floats it was unpacked to",
          "[device][pcm]") {
    std::mt19937 random(11);
    for (const unsigned bits : {16U, 24U}) {
        constexpr std::size_t kFrames = 480;
        std::vector<std::uint8_t> bytes;
        const std::int32_t range = std::int32_t{1} << (bits - 1);
        for (std::size_t i = 0; i < kFrames * 2; ++i) {
            // Quiet and loud, so the sum is not dominated by a few full-scale samples.
            const std::int32_t value =
                (static_cast<std::int32_t>(random() % static_cast<std::uint32_t>(range * 2)) -
                 range) /
                (i % 7 == 0 ? 1 : 64);
            const auto u = static_cast<std::uint32_t>(value);
            bytes.push_back(static_cast<std::uint8_t>(u & 0xFFU));
            bytes.push_back(static_cast<std::uint8_t>((u >> 8U) & 0xFFU));
            if (bits == 24) {
                bytes.push_back(static_cast<std::uint8_t>((u >> 16U) & 0xFFU));
            }
        }
        std::vector<std::vector<float>> storage(2, std::vector<float>(kFrames));
        const std::vector<std::span<float>> out = planes(storage);
        std::array<PcmLevel, 2> levels{};
        if (bits == 16) {
            unpack_pcm16(bytes, 2, kFrames, out, levels);
        } else {
            unpack_pcm24(bytes, 2, kFrames, out, levels);
        }
        for (std::size_t c = 0; c < 2; ++c) {
            float peak = 0.0F;
            double sum = 0.0;
            for (const float x : storage[c]) {
                peak = std::max(peak, std::fabs(x));
                sum += static_cast<double>(x) * static_cast<double>(x);
            }
            // The peak is exact (a power-of-two scale of an integer), and the sum agrees to the
            // rounding of a double's accumulation.
            CHECK(iclforge::pcm_level_peak(levels[c], bits) == peak);
            CHECK(std::fabs(iclforge::pcm_level_square_sum(levels[c], bits) - sum) <= sum * 1e-12);
        }
    }
}

TEST_CASE("pcm unpack: a mono chunk is one plane", "[device][pcm]") {
    const std::array<std::int16_t, 5> samples{0, 1, -1, 32767, -32768};
    std::vector<std::uint8_t> bytes;
    for (const std::int16_t sample : samples) {
        const auto u = static_cast<std::uint16_t>(sample);
        bytes.push_back(static_cast<std::uint8_t>(u & 0xFFU));
        bytes.push_back(static_cast<std::uint8_t>(u >> 8U));
    }
    std::vector<std::vector<float>> storage(1, std::vector<float>(samples.size()));
    const std::vector<std::span<float>> out = planes(storage);
    std::array<PcmLevel, 1> levels{};
    unpack_pcm16(bytes, 1, samples.size(), out, levels);
    for (std::size_t k = 0; k < samples.size(); ++k) {
        CHECK(storage[0][k] == static_cast<float>(samples[k]) / 32768.0F);
    }
    CHECK(levels[0].peak == 32768);
}

TEST_CASE("pcm unpack: a 24-bit stereo chunk keeps its sign and its levels", "[device][pcm]") {
    const std::array<std::int32_t, 6> samples{0, 1, -1, 8388607, -8388608, -123456};
    std::vector<std::uint8_t> bytes;
    for (const std::int32_t sample : samples) {
        const auto u = static_cast<std::uint32_t>(sample) & 0xFFFFFFU;
        bytes.push_back(static_cast<std::uint8_t>(u & 0xFFU));
        bytes.push_back(static_cast<std::uint8_t>((u >> 8U) & 0xFFU));
        bytes.push_back(static_cast<std::uint8_t>((u >> 16U) & 0xFFU));
    }
    constexpr std::size_t kFrames = 3;
    std::vector<std::vector<float>> storage(2, std::vector<float>(kFrames));
    const std::vector<std::span<float>> out = planes(storage);
    std::array<PcmLevel, 2> levels{};
    unpack_pcm24(bytes, 2, kFrames, out, levels);
    std::uint64_t sum[2] = {0, 0};
    std::uint32_t peak[2] = {0, 0};
    for (std::size_t k = 0; k < kFrames; ++k) {
        for (std::size_t c = 0; c < 2; ++c) {
            const std::int32_t value = samples[(k * 2) + c];
            CHECK(storage[c][k] == static_cast<float>(value) / 8388608.0F);
            peak[c] = std::max(peak[c], static_cast<std::uint32_t>(std::abs(value)));
            sum[c] += static_cast<std::uint64_t>(std::abs(value)) *
                      static_cast<std::uint64_t>(std::abs(value));
        }
    }
    for (std::size_t c = 0; c < 2; ++c) {
        CHECK(levels[c].peak == peak[c]);
        CHECK(levels[c].sum_squares == sum[c]);
    }
    // Full-scale negative on the left, one short of full-scale positive on the right.
    CHECK(levels[0].peak == 8388608);
    CHECK(levels[1].peak == 8388607);
}
