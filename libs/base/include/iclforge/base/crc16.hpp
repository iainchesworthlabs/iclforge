#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace iclforge::base {

// CRC-16 with generator polynomial x^16 + x^15 + x^2 + 1 (0x8005), MSB-first bit order, initial
// register 0, no reflection, no final XOR: in CRC-catalogue terms CRC-16/UMTS (check value 0xFEE8
// for the ASCII string "123456789"). AC-3 and E-AC-3 protect a syncframe with it (A/52 §7.10.1:
// crc1 and crc2, iclforge/ac3/core/crc16.hpp), and AC-4 a sync frame (TS 103 190-1 Annex G.4.2).

namespace detail {

consteval std::array<std::uint16_t, 256> make_crc16_table() {
    std::array<std::uint16_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t reg = i << 8;
        for (int bit = 0; bit < 8; ++bit) {
            reg = (reg & 0x8000) != 0 ? ((reg << 1) ^ 0x8005) & 0xFFFF : (reg << 1) & 0xFFFF;
        }
        table[i] = static_cast<std::uint16_t>(reg);
    }
    return table;
}

inline constexpr auto kCrc16Table = make_crc16_table();

}  // namespace detail

[[nodiscard]] constexpr std::uint16_t crc16(std::span<const std::byte> data,
                                            std::uint16_t crc = 0) {
    for (std::byte b : data) {
        const auto index = ((crc >> 8) ^ std::to_integer<std::uint32_t>(b)) & 0xFF;
        crc = static_cast<std::uint16_t>((crc << 8) ^ detail::kCrc16Table[index]);
    }
    return crc;
}

}  // namespace iclforge::base
