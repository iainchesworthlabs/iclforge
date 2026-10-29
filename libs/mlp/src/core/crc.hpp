#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// The checks of a TrueHD (MLP) access unit (Appendix B, as planning/truehd.md's register settles
// them, P12 and P13). Every MLP CRC is the unaugmented polynomial remainder taken MSB first: each
// message bit is shifted into the register's low end, and the register is reduced by the generator
// when a bit leaves its top. A byte therefore costs one table step: the register shifted up eight
// bits, reduced, then the byte added in. Appendix B gives only the generators; the initial values
// and the coverage are what FFmpeg's streams and its decoder agree on.

namespace iclforge::mlp::detail {

// Restart header (polynomial 0x1D, initial 0) and block data CRC-8.
inline constexpr std::uint8_t kCrc8Poly = 0x1D;
// The substream CRC's generator is Appendix B's "CRC-8 (parity) 0x63", not 0x1D (P13).
inline constexpr std::uint8_t kSubstreamCrcPoly = 0x63;
inline constexpr std::uint8_t kSubstreamCrcInit = 0xA2;
// Section 6.2: the parity byte is the XOR of the segment's bytes before it, XOR 0xA9.
inline constexpr std::uint8_t kParityXor = 0xA9;
// Major sync CRC-16, polynomial 0x002D, initial 0, over the 26 bytes (and the extra channel
// meaning) before it.
inline constexpr std::uint16_t kCrc16Poly = 0x002D;

template <typename T, T Poly>
consteval std::array<T, 256> make_shift_table() {
    // table[r] = (r * x^8) mod P for an 8-bit register value r placed in the top byte.
    constexpr int kWidth = static_cast<int>(sizeof(T) * 8);
    std::array<T, 256> table{};
    for (unsigned r = 0; r < 256; ++r) {
        unsigned reg = r << (kWidth - 8);
        for (int bit = 0; bit < 8; ++bit) {
            const bool top = ((reg >> (kWidth - 1)) & 1U) != 0;
            reg = (reg << 1U) & ((1U << kWidth) - 1U);
            if (top) {
                reg ^= Poly;
            }
        }
        table[r] = static_cast<T>(reg);
    }
    return table;
}

inline constexpr auto kCrc8Table = make_shift_table<std::uint8_t, kCrc8Poly>();
inline constexpr auto kSubstreamCrcTable = make_shift_table<std::uint8_t, kSubstreamCrcPoly>();
inline constexpr auto kCrc16Table = make_shift_table<std::uint16_t, kCrc16Poly>();

// The unaugmented remainder of whole bytes through an 8-bit generator's table.
[[nodiscard]] constexpr std::uint8_t crc8_bytes(const std::array<std::uint8_t, 256>& table,
                                                std::span<const std::byte> data,
                                                std::uint8_t init) noexcept {
    std::uint8_t reg = init;
    for (const std::byte b : data) {
        reg = static_cast<std::uint8_t>(table[reg] ^ std::to_integer<std::uint8_t>(b));
    }
    return reg;
}

[[nodiscard]] constexpr std::uint16_t crc16_bytes(std::span<const std::byte> data,
                                                  std::uint16_t init = 0) noexcept {
    std::uint16_t reg = init;
    for (const std::byte b : data) {
        reg = static_cast<std::uint16_t>(kCrc16Table[reg >> 8U] ^
                                         static_cast<std::uint16_t>((reg & 0xFFU) << 8U) ^
                                         std::to_integer<std::uint16_t>(b));
    }
    return reg;
}

// The unaugmented CRC-8 0x1D remainder of `bit_count` bits from bit `first_bit` of `data`, MSB
// first: the restart header's check, which starts two bits into a substream segment (P12).
[[nodiscard]] constexpr std::uint8_t crc8_bits(std::span<const std::byte> data,
                                               std::size_t first_bit, std::size_t bit_count,
                                               std::uint8_t init = 0) noexcept {
    unsigned reg = init;
    for (std::size_t i = 0; i < bit_count; ++i) {
        const std::size_t at = first_bit + i;
        const unsigned bit =
            (std::to_integer<unsigned>(data[at / 8U]) >> (7U - static_cast<unsigned>(at % 8U))) &
            1U;
        reg = (reg << 1U) | bit;
        if ((reg & 0x100U) != 0) {
            reg ^= 0x100U | kCrc8Poly;
        }
    }
    return static_cast<std::uint8_t>(reg);
}

[[nodiscard]] constexpr std::uint8_t xor_bytes(std::span<const std::byte> data) noexcept {
    std::uint8_t x = 0;
    for (const std::byte b : data) {
        x = static_cast<std::uint8_t>(x ^ std::to_integer<std::uint8_t>(b));
    }
    return x;
}

// Section 3.2: the XOR of every nibble of the access unit header and the substream directory
// (the major sync excluded) is 0xF.
[[nodiscard]] constexpr std::uint8_t nibble_xor(std::span<const std::byte> data) noexcept {
    unsigned x = 0;
    for (const std::byte b : data) {
        const auto v = std::to_integer<unsigned>(b);
        x ^= (v >> 4U) ^ (v & 0x0FU);
    }
    return static_cast<std::uint8_t>(x);
}

// The substream segment's trailing check bytes for its body (6.2, P13).
struct SegmentCheck {
    std::uint8_t parity = 0;
    std::uint8_t crc = 0;
};
[[nodiscard]] constexpr SegmentCheck segment_check(std::span<const std::byte> body) noexcept {
    return {static_cast<std::uint8_t>(xor_bytes(body) ^ kParityXor),
            crc8_bytes(kSubstreamCrcTable, body, kSubstreamCrcInit)};
}

}  // namespace iclforge::mlp::detail
