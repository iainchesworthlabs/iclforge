#include "iclforge/truehd/core/crc.hpp"

#include <array>

namespace iclforge::truehd {

namespace {

// MSB-first, non-reflected CRC tables for polynomial `poly` (the polynomial without its top bit).
constexpr std::array<std::uint16_t, 256> make_crc16_table(std::uint16_t poly) {
    std::array<std::uint16_t, 256> table{};
    for (unsigned i = 0; i < 256; ++i) {
        auto reg = static_cast<std::uint16_t>(i << 8U);
        for (int bit = 0; bit < 8; ++bit) {
            reg = (reg & 0x8000U) != 0 ? static_cast<std::uint16_t>((reg << 1U) ^ poly)
                                       : static_cast<std::uint16_t>(reg << 1U);
        }
        table[i] = reg;
    }
    return table;
}

constexpr std::array<std::uint8_t, 256> make_crc8_table(std::uint8_t poly) {
    std::array<std::uint8_t, 256> table{};
    for (unsigned i = 0; i < 256; ++i) {
        auto reg = static_cast<std::uint8_t>(i);
        for (int bit = 0; bit < 8; ++bit) {
            reg = (reg & 0x80U) != 0 ? static_cast<std::uint8_t>((reg << 1U) ^ poly)
                                     : static_cast<std::uint8_t>(reg << 1U);
        }
        table[i] = reg;
    }
    return table;
}

constexpr auto kCrc16Table = make_crc16_table(0x002D);
constexpr auto kCrc8SubstreamTable = make_crc8_table(0x63);

// A table wrong for a known input fails the build rather than a test (CONTRIBUTING.md): the first
// entries of a CRC table are fixed by the polynomial alone.
static_assert(kCrc16Table[1] == 0x002D);
static_assert(kCrc8SubstreamTable[1] == 0x63);

[[nodiscard]] std::uint8_t bit_at(std::span<const std::byte> data, std::size_t bit) noexcept {
    const std::size_t byte = bit >> 3U;
    if (byte >= data.size()) {
        return 0;
    }
    return static_cast<std::uint8_t>((std::to_integer<unsigned>(data[byte]) >> (7U - (bit & 7U))) &
                                     1U);
}

}  // namespace

std::uint16_t major_sync_crc(std::span<const std::byte> covered) noexcept {
    if (covered.size() < 2) {
        return 0;
    }
    std::uint16_t reg = 0;
    const std::size_t run = covered.size() - 2;
    for (std::size_t i = 0; i < run; ++i) {
        reg = static_cast<std::uint16_t>(
            (reg << 8U) ^
            kCrc16Table[((reg >> 8U) ^ std::to_integer<unsigned>(covered[i])) & 0xFFU]);
    }
    const auto last = static_cast<std::uint16_t>((std::to_integer<unsigned>(covered[run]) << 8U) |
                                                 std::to_integer<unsigned>(covered[run + 1]));
    return static_cast<std::uint16_t>(reg ^ last);
}

std::uint8_t restart_header_crc(std::span<const std::byte> data, std::size_t first_bit,
                                std::size_t crc_bit) noexcept {
    // The register runs over every covered bit but the last eight; headers are rare, so bit by bit.
    constexpr std::uint8_t kPoly = 0x1D;
    std::uint8_t reg = 0;
    if (crc_bit < first_bit + 8) {
        return 0;
    }
    const std::size_t run_end = crc_bit - 8;
    for (std::size_t bit = first_bit; bit < run_end; ++bit) {
        const bool feedback = ((reg >> 7U) & 1U) != (bit_at(data, bit) != 0);
        reg = static_cast<std::uint8_t>(reg << 1U);
        if (feedback) {
            reg ^= kPoly;
        }
    }
    std::uint8_t last = 0;
    for (std::size_t bit = run_end; bit < crc_bit; ++bit) {
        last = static_cast<std::uint8_t>((last << 1U) | bit_at(data, bit));
    }
    return static_cast<std::uint8_t>(reg ^ last);
}

std::uint8_t substream_parity(std::span<const std::byte> payload) noexcept {
    std::uint8_t xor_all = 0;
    for (const std::byte b : payload) {
        xor_all ^= static_cast<std::uint8_t>(b);
    }
    return static_cast<std::uint8_t>(xor_all ^ 0xA9U);
}

std::uint8_t substream_crc(std::span<const std::byte> payload) noexcept {
    if (payload.empty()) {
        return 0;
    }
    std::uint8_t reg = 0x3C;
    const std::size_t run = payload.size() - 1;
    for (std::size_t i = 0; i < run; ++i) {
        reg = kCrc8SubstreamTable[reg ^ std::to_integer<unsigned>(payload[i])];
    }
    return static_cast<std::uint8_t>(reg ^ std::to_integer<unsigned>(payload[run]));
}

std::uint8_t extra_data_check_nibble(std::uint32_t extra_data_length) noexcept {
    return static_cast<std::uint8_t>(
        ((extra_data_length >> 8U) ^ (extra_data_length >> 4U) ^ extra_data_length) & 0xFU);
}

}  // namespace iclforge::truehd
