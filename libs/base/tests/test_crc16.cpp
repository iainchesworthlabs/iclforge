#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

#include "iclforge/base/crc16.hpp"

// planning/consolidation.md's C4 made one CRC-16 of the four the tree had: AC-3's table (now
// base's), and the bit-at-a-time registers of the AC-4 scanner, the AC-4 sync frame writer and the
// EMDF signer. One of those, as the scanner had it, is the oracle here.

namespace {

std::uint16_t bitwise_crc16(std::span<const std::byte> data) {
    std::uint16_t crc = 0x0000;
    constexpr std::uint16_t kPoly = 0x8005;
    for (const std::byte b : data) {
        crc ^= static_cast<std::uint16_t>(std::to_integer<unsigned>(b) << 8);
        for (int i = 0; i < 8; ++i) {
            crc = (crc & 0x8000) ? static_cast<std::uint16_t>((crc << 1) ^ kPoly)
                                 : static_cast<std::uint16_t>(crc << 1);
        }
    }
    return crc;
}

}  // namespace

TEST_CASE("base's CRC-16 is the AC-4 scanner's bit-at-a-time register", "[base][crc16]") {
    std::mt19937 rng(0x8005);
    for (int trial = 0; trial < 2000; ++trial) {
        std::vector<std::byte> data(std::uniform_int_distribution<std::size_t>(0, 300)(rng));
        for (auto& b : data) {
            b = static_cast<std::byte>(rng() & 0xFFU);
        }
        REQUIRE(iclforge::base::crc16(data) == bitwise_crc16(data));
    }
}
