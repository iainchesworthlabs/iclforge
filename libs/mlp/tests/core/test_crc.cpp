#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/access_unit.hpp"
#include "core/crc.hpp"
#include "core/huffman.hpp"

// The checks of an access unit against values FFmpeg's encoder wrote (planning/truehd.md, P12 and
// P13): the first access unit of `ffmpeg -f lavfi -i anullsrc=r=48000:cl=stereo -t 0.05 -c:a
// truehd -strict -2`, two channels of silence, whose major sync, restart header and segment check
// bytes are all present in 80 bytes. Each test also breaks the value it checks and requires the
// check to notice, so a check that accepts anything cannot pass.

namespace {

using iclforge::mlp::detail::crc16_bytes;
using iclforge::mlp::detail::crc8_bits;
using iclforge::mlp::detail::nibble_xor;
using iclforge::mlp::detail::segment_check;

std::vector<std::byte> bytes(std::initializer_list<unsigned> list) {
    std::vector<std::byte> out;
    for (unsigned v : list) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

// The first access unit of the silent stereo stream: 4 header bytes, the 28-byte major sync, one
// directory entry (end pointer 23 words), and the 46-byte segment.
const std::vector<std::byte> kSilentAccessUnit = bytes({
    0x40, 0x28, 0xFF, 0xD8, 0xF8, 0x72, 0x6F, 0xBA, 0x00, 0x50, 0xA0, 0x01, 0xB7, 0x52,
    0x00, 0x00, 0x00, 0x00, 0x8C, 0x7F, 0x10, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x34, 0x28, 0x20, 0x17, 0xF1, 0xEA, 0x00, 0x00, 0x01, 0x10, 0x00, 0x00,
    0x00, 0x01, 0x08, 0x40, 0x00, 0x00, 0x00, 0x03, 0x6A, 0x82, 0x04, 0x80, 0x01, 0x03,
    0x20, 0x00, 0x40, 0x80, 0x00, 0x24, 0x40, 0x31, 0xD4, 0x74, 0x02, 0x00, 0x00, 0x06,
    0x3A, 0x8E, 0x80, 0x40, 0x00, 0x00, 0x80, 0x00, 0x62, 0xCE,
});

}  // namespace

TEST_CASE("mlp major sync CRC-16 is the unaugmented remainder over 26 bytes", "[mlp][crc]") {
    const std::span<const std::byte> ms = std::span(kSilentAccessUnit).subspan(4, 28);
    CHECK(crc16_bytes(ms.first(26)) == 0x3428);
    std::vector<std::byte> broken(ms.begin(), ms.end());
    broken[24] ^= std::byte{0x10};
    CHECK(crc16_bytes(std::span(broken).first(26)) != 0x3428);
}

TEST_CASE("mlp restart header CRC-8 runs from the sync word to the end of ch_assign",
          "[mlp][crc]") {
    // The segment starts at byte 34; its first two bits are block_header_exists and
    // restart_header_exists, then the 14-bit sync. max_matrix_chan is 1, so the header is
    // 113 + 2 * 6 = 125 bits and its CRC the 8 bits after them.
    const std::span<const std::byte> seg = std::span(kSilentAccessUnit).subspan(34);
    const std::size_t first = 2;
    const std::size_t count = 125;
    unsigned stored = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        const std::size_t at = first + count + i;
        stored = (stored << 1U) |
                 ((std::to_integer<unsigned>(seg[at / 8]) >> (7U - at % 8U)) & 1U);
    }
    CHECK(crc8_bits(seg, first, count) == stored);
    CHECK(crc8_bits(seg, first, count - 1) != stored);
}

TEST_CASE("mlp substream parity and CRC cover the segment before them", "[mlp][crc]") {
    const std::span<const std::byte> seg = std::span(kSilentAccessUnit).subspan(34, 46);
    const auto check = segment_check(seg.first(44));
    CHECK(check.parity == std::to_integer<std::uint8_t>(seg[44]));
    CHECK(check.crc == std::to_integer<std::uint8_t>(seg[45]));
    std::vector<std::byte> broken(seg.begin(), seg.begin() + 44);
    broken[10] ^= std::byte{0x01};
    const auto bad = segment_check(broken);
    CHECK(bad.parity != std::to_integer<std::uint8_t>(seg[44]));
    CHECK(bad.crc != std::to_integer<std::uint8_t>(seg[45]));
}

TEST_CASE("mlp check nibble of header and directory gives 0xF", "[mlp][crc]") {
    const auto header = std::span(kSilentAccessUnit).first(4);
    const auto directory = std::span(kSilentAccessUnit).subspan(32, 2);
    CHECK((nibble_xor(header) ^ nibble_xor(directory)) == 0x0F);
}

TEST_CASE("mlp access unit framing parses and reassembles byte for byte", "[mlp][crc]") {
    using namespace iclforge::mlp::detail;
    auto au = parse_access_unit(kSilentAccessUnit, nullptr);
    REQUIRE(au.has_value());
    REQUIRE(au->major_sync.has_value());
    CHECK(au->major_sync->sample_rate() == 48000U);
    CHECK(au->major_sync->num_substreams == 1);
    CHECK(au->major_sync->peak_data_rate == 3199);
    CHECK(au->num_substreams == 1);
    CHECK(au->segments[0].bytes.size() == 46);
    CHECK(au->extra_data.empty());

    AccessUnitParts parts;
    parts.input_timing = au->header.input_timing;
    parts.major_sync = &*au->major_sync;
    parts.num_substreams = 1;
    parts.entries[0] = au->segments[0].entry;
    parts.segments[0] = au->segments[0].bytes;
    auto again = assemble_access_unit(parts);
    REQUIRE(again.has_value());
    CHECK(*again == kSilentAccessUnit);

    std::vector<std::byte> broken = kSilentAccessUnit;
    broken[2] ^= std::byte{0x01};
    CHECK(parse_access_unit(broken, nullptr).error() == iclforge::mlp::Error::kCheckNibble);
    broken = kSilentAccessUnit;
    broken[20] ^= std::byte{0x01};
    CHECK(parse_access_unit(broken, nullptr).error() == iclforge::mlp::Error::kMajorSyncCrc);
}

TEST_CASE("mlp Huffman codewords decode through the 9-bit lookup", "[mlp][huffman]") {
    using namespace iclforge::mlp::detail;
    for (int book = 0; book < kHuffmanCodebooks; ++book) {
        for (int symbol = kMinSymbol[static_cast<std::size_t>(book)];
             symbol <= kMaxSymbol[static_cast<std::size_t>(book)]; ++symbol) {
            const Codeword w = codeword(book, symbol);
            REQUIRE(w.length > 0);
            const unsigned window = w.code << (kHuffmanMaxLength - w.length);
            const auto& entry = kHuffmanLookup[static_cast<std::size_t>(book)][window];
            CHECK(entry.symbol == symbol);
            CHECK(entry.length == w.length);
        }
    }
}
