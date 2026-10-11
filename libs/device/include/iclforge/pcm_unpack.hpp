#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

// A PCM chunk's samples as planar floats, and the levels they make, with integer operations only.
//
// A Sendspin player@v1 stream arrives as interleaved little-endian 16- or 24-bit samples, and the
// player's pipeline is float from the renderer on. On a part without a floating-point unit (the
// ESP32-C6) every float operation is a call into the soft-float library, and the obvious
// conversion - `static_cast<float>(sample) / 32768.0F` - is two of them, one of them a division:
// measured on a C6 playing a 16-bit stereo stream in 1,000-frame chunks, the conversion loop and
// the level meter that followed it were the whole of the 5.3 ms a chunk took to render, a quarter
// of the core and more than it had to spare.
//
// Both are done here without a float operation. A 16- or 24-bit integer is exactly representable
// as a float, and dividing by a power of two only moves the exponent, so the float is built from
// its bits: the result is bit for bit what the division gives, for every sample
// (libs/device/tests/test_pcm_unpack.cpp checks all 65,536 16-bit values and every 24-bit one).
// The level of a channel is its peak and its sum of squares in the samples' own integer units,
// which a caller turns into the meter's floats once per block rather than once per sample.
//
// Free of ESP-IDF, as the other headers in this directory are, so that the arithmetic runs on the
// host.

namespace iclforge {

// The float a signed `Bits`-bit sample is as a PCM value: sample / 2^(Bits - 1), so that full scale
// negative is exactly -1.0. `sample` must be in [-2^(Bits-1), 2^(Bits-1) - 1].
template <unsigned Bits>
[[nodiscard]] inline float pcm_to_float(std::int32_t sample) {
    static_assert(Bits >= 2 && Bits <= 24, "a float holds a 24-bit integer exactly and no more");
    if (sample == 0) {
        return 0.0F;
    }
    const std::uint32_t sign = sample < 0 ? 0x80000000U : 0U;
    const std::uint32_t magnitude = sample < 0
                                        ? std::uint32_t{0} - static_cast<std::uint32_t>(sample)
                                        : static_cast<std::uint32_t>(sample);
    // magnitude = 1.f x 2^top, and the value is that over 2^(Bits - 1).
    const int top = 31 - std::countl_zero(magnitude);
    const std::uint32_t exponent =
        static_cast<std::uint32_t>(127 + top - static_cast<int>(Bits - 1));
    const std::uint32_t fraction = (magnitude << (23 - top)) & 0x7FFFFFU;
    return std::bit_cast<float>(sign | (exponent << 23) | fraction);
}

// What one channel of a chunk measured: its largest absolute sample and the sum of the squares of
// its samples, both in the samples' integer units (so a 16-bit channel's peak is at most 32,768
// and its sum of squares at most 2^30 a frame).
struct PcmLevel {
    std::uint32_t peak = 0;
    std::uint64_t sum_squares = 0;
};

// A level's peak as the float a meter holds, where 1.0 is full scale: the peak over 2^(bits - 1).
// One float operation for a whole block, where measuring the floats was several a sample.
[[nodiscard]] inline float pcm_level_peak(const PcmLevel& level, unsigned bits) {
    return static_cast<float>(level.peak) / static_cast<float>(std::uint32_t{1} << (bits - 1));
}

// A level's sum of squares in the meter's units, where full scale squared is 1: the sum over
// 2^(2 (bits - 1)), which is exact in a double (a power of two).
[[nodiscard]] inline double pcm_level_square_sum(const PcmLevel& level, unsigned bits) {
    return static_cast<double>(level.sum_squares) /
           static_cast<double>(std::uint64_t{1} << (2 * (bits - 1)));
}

// `frames` frames of `channels` interleaved little-endian 16-bit samples from `bytes`, as planar
// floats into out[0..channels-1] (each at least `frames` long), and each channel's level added to
// levels[0..channels-1]. `bytes` holds at least frames * channels * 2.
inline void unpack_pcm16(std::span<const std::uint8_t> bytes, std::size_t channels,
                         std::size_t frames, std::span<const std::span<float>> out,
                         std::span<PcmLevel> levels) {
    const std::uint8_t* at = bytes.data();
    for (std::size_t c = 0; c < channels; ++c) {
        std::uint32_t peak = levels[c].peak;
        std::uint64_t sum = levels[c].sum_squares;
        float* const dst = out[c].data();
        const std::uint8_t* sample = at + (c * 2);
        const std::size_t stride = channels * 2;
        for (std::size_t k = 0; k < frames; ++k, sample += stride) {
            const auto value = static_cast<std::int16_t>(
                static_cast<std::uint16_t>(sample[0] | (sample[1] << 8U)));
            dst[k] = pcm_to_float<16>(value);
            const std::int32_t wide = value;
            const auto magnitude = static_cast<std::uint32_t>(wide < 0 ? -wide : wide);
            peak = std::max(peak, magnitude);
            sum += static_cast<std::uint64_t>(magnitude * magnitude);
        }
        levels[c].peak = peak;
        levels[c].sum_squares = sum;
    }
}

// The same for 24-bit samples, three bytes each: the floats are sample / 2^23, and the level's
// units are 24-bit ones (its peak at most 2^23, its sum of squares at most 2^46 a frame).
inline void unpack_pcm24(std::span<const std::uint8_t> bytes, std::size_t channels,
                         std::size_t frames, std::span<const std::span<float>> out,
                         std::span<PcmLevel> levels) {
    const std::uint8_t* at = bytes.data();
    for (std::size_t c = 0; c < channels; ++c) {
        std::uint32_t peak = levels[c].peak;
        std::uint64_t sum = levels[c].sum_squares;
        float* const dst = out[c].data();
        const std::uint8_t* sample = at + (c * 3);
        const std::size_t stride = channels * 3;
        for (std::size_t k = 0; k < frames; ++k, sample += stride) {
            std::int32_t value =
                static_cast<std::int32_t>(sample[0] | (sample[1] << 8U) | (sample[2] << 16U));
            if ((value & 0x800000) != 0) {
                value -= 0x1000000;
            }
            dst[k] = pcm_to_float<24>(value);
            const auto magnitude = static_cast<std::uint32_t>(value < 0 ? -value : value);
            peak = std::max(peak, magnitude);
            sum += static_cast<std::uint64_t>(magnitude) * magnitude;
        }
        levels[c].peak = peak;
        levels[c].sum_squares = sum;
    }
}

}  // namespace iclforge
