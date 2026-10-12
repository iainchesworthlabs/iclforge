#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "iclforge/ac4_sync_frame.hpp"

// The raw frame inside an AC-4 sync frame (iclforge/ac4_sync_frame.hpp), which the Sendspin
// player's AC-4 path takes from each burst chunk.

namespace {

using iclforge::ac4_raw_frame;

// A sync frame: the sync word, the size (escaped past 0xFFFE), `size` bytes counting up from
// `fill`, and a CRC word where `crc`.
std::vector<std::byte> sync_frame(std::size_t size, bool crc, std::uint8_t fill = 1) {
    std::vector<std::byte> out{std::byte{0xAC},
                               std::byte{crc ? std::uint8_t{0x41} : std::uint8_t{0x40}}};
    if (size >= 0xFFFF) {
        out.insert(out.end(), {std::byte{0xFF}, std::byte{0xFF}});
        out.push_back(static_cast<std::byte>((size >> 16U) & 0xFFU));
        out.push_back(static_cast<std::byte>((size >> 8U) & 0xFFU));
        out.push_back(static_cast<std::byte>(size & 0xFFU));
    } else {
        out.push_back(static_cast<std::byte>((size >> 8U) & 0xFFU));
        out.push_back(static_cast<std::byte>(size & 0xFFU));
    }
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(fill + i)));
    }
    if (crc) {
        out.insert(out.end(), {std::byte{0xEE}, std::byte{0xEE}});
    }
    return out;
}

}  // namespace

TEST_CASE("ac4 sync frame: the raw frame is what the size says, with or without a CRC word",
          "[device][ac4]") {
    for (const bool crc : {false, true}) {
        CAPTURE(crc);
        const std::vector<std::byte> frame = sync_frame(300, crc);
        const auto raw = ac4_raw_frame(frame);
        REQUIRE(raw.size() == 300);
        CHECK(raw.data() == frame.data() + 4);
        CHECK(raw.front() == std::byte{1});
        CHECK(raw.back() == static_cast<std::byte>(static_cast<std::uint8_t>(1 + 299)));
    }
}

TEST_CASE("ac4 sync frame: a size of 0xFFFF escapes to 24 bits", "[device][ac4]") {
    const std::vector<std::byte> frame = sync_frame(70000, true);
    const auto raw = ac4_raw_frame(frame);
    REQUIRE(raw.size() == 70000);
    CHECK(raw.data() == frame.data() + 7);
}

TEST_CASE("ac4 sync frame: the zeros an HBR16 burst pads with are left alone", "[device][ac4]") {
    std::vector<std::byte> frame = sync_frame(1001, true);
    frame.resize(frame.size() + 5, std::byte{0});
    CHECK(ac4_raw_frame(frame).size() == 1001);
}

TEST_CASE("ac4 sync frame: bytes that are not one whole sync frame give nothing", "[device][ac4]") {
    CHECK(ac4_raw_frame({}).empty());
    // AC-3's sync word.
    std::vector<std::byte> ac3 = sync_frame(32, false);
    ac3[0] = std::byte{0x0B};
    ac3[1] = std::byte{0x77};
    CHECK(ac4_raw_frame(ac3).empty());
    // Another sync word of the 0xAC4 family.
    std::vector<std::byte> other = sync_frame(32, false);
    other[1] = std::byte{0x42};
    CHECK(ac4_raw_frame(other).empty());
    // Shorter than the header, and a frame of no bytes.
    CHECK(ac4_raw_frame(std::vector<std::byte>{std::byte{0xAC}, std::byte{0x40}, std::byte{0}})
              .empty());
    CHECK(ac4_raw_frame(sync_frame(0, false)).empty());
    // The size runs past the end: the frame, and for 0xAC41 the CRC word after it.
    std::vector<std::byte> cut = sync_frame(100, false);
    cut.resize(cut.size() - 1);
    CHECK(ac4_raw_frame(cut).empty());
    std::vector<std::byte> no_crc = sync_frame(100, true);
    no_crc.resize(no_crc.size() - 1);
    CHECK(ac4_raw_frame(no_crc).empty());
    // The escape with its three size bytes missing.
    CHECK(ac4_raw_frame(std::vector<std::byte>{std::byte{0xAC}, std::byte{0x40}, std::byte{0xFF},
                                               std::byte{0xFF}, std::byte{0}})
              .empty());
}
