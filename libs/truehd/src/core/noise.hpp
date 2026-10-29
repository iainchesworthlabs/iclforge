#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// TH spec §17.3, §17.4: noise generator 1 (restart sync 0x31EB, noise_type 1). A buffer of int8
// values is filled from a table, indexed by the top bits of an LFSR, once for each access unit of a
// substream, and a matrix with a non-zero matrix_noise_shift adds a value of it, scaled, to its
// sum before the sum is shifted down.

namespace iclforge::truehd::detail {

// TH spec §17.4: the 256 signed 8-bit values, indexed by seed >> 15 (low 8 bits). Values from -28
// to +92.
inline constexpr std::array<std::int8_t, 256> kNoiseTable{{
    30,  51,  22,  54,  3,   7,   -4, 38,  14,  55, 46,  81, 22,  58,  -3,  2,   //
    52,  31,  -7,  51,  15,  44,  74, 30,  85,  -17, 10, 33, 18,  80,  28,  62,  //
    10,  32,  23,  69,  72,  26,  35, 17,  73,  60, 8,   56, 2,   6,   -2,  -5,  //
    51,  4,   11,  50,  66,  76,  21, 44,  33,  47, 1,   26, 64,  48,  57,  40,  //
    38,  16,  -10, -28, 92,  22,  -18, 29, -10, 5,  -13, 49, 19,  24,  70,  34,  //
    61,  48,  30,  14,  -6,  25,  58, 33,  42,  60, 67,  17, 54,  17,  22,  30,  //
    67,  44,  -9,  50,  -11, 43,  40, 32,  59,  82, 13,  49, -14, 55,  60,  36,  //
    48,  49,  31,  47,  15,  12,  4,  65,  1,   23, 29,  39, 45,  -2,  84,  69,  //
    0,   72,  37,  57,  27,  41,  -15, -16, 35, 31, 14,  61, 24,  0,   27,  24,  //
    16,  41,  55,  34,  53,  9,   56, 12,  25,  29, 53,  5,  20,  -20, -8,  20,  //
    13,  28,  -3,  78,  38,  16,  11, 62,  46,  29, 21,  24, 46,  65,  43,  -23, //
    89,  18,  74,  21,  38,  -12, 19, 12,  -19, 8,  15,  33, 4,   57,  9,   -8,  //
    36,  35,  26,  28,  7,   83,  63, 79,  75,  11, 3,   87, 37,  47,  34,  40,  //
    39,  19,  20,  42,  27,  34,  39, 77,  13,  42, 59,  64, 45,  -1,  32,  37,  //
    45,  -5,  53,  -6,  7,   36,  50, 23,  6,   32, 9,   -21, 18, 71,  27,  52,  //
    -25, 31,  35,  42,  -1,  68,  63, 52,  26,  43, 66,  37, 41,  25,  40,  70,  //
}};

// The most entries a buffer has: 64 for each doubling of the sample rate over 44.1 and 48 kHz.
inline constexpr std::size_t kMaxNoiseBuffer = 256;

// The entries of the buffer at a rate factor (0 for 44.1 and 48 kHz, 1 for 88.2 and 96, 2 for 176.4
// and 192): a power of two at least as large as the access unit, 64 << factor. TH spec §17.3 calls
// it access_unit_size_pow2 and does not give it (ERRATA.md, question about it).
[[nodiscard]] constexpr std::size_t noise_buffer_size(int rate_factor) noexcept {
    return std::size_t{64} << rate_factor;
}

// TH spec §17.3: fills `count` entries of `buffer` from `seed` and returns the seed after them.
[[nodiscard]] inline std::uint32_t fill_noise_buffer(std::uint32_t seed, std::size_t count,
                                                     std::int8_t* buffer) noexcept {
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t index = (seed >> 15U) & 0xFFU;
        buffer[i] = kNoiseTable[index];
        seed = (seed << 8U) ^ index ^ (index << 5U);
    }
    return seed;
}

}  // namespace iclforge::truehd::detail
