#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "iclforge/truehd/core/syntax.hpp"

// TH spec App. C: the three Huffman codebooks the residuals of a channel can use (huff_type 1..3),
// as the (code, length) of each symbol and as 9-bit lookup tables built from them at compile time.

namespace iclforge::truehd::detail {

struct BookEntry {
    std::int8_t symbol;
    std::uint16_t code;
    std::uint8_t bits;
};

// The codes are App. C's, a 1 followed by zeros at the low end for the negative symbols, so each is
// the number App. C prints. A codebook of the specification is not complete (its codes use 255/256
// of the code space), so the lookup table has unused entries, which a decode reports as an error.
inline constexpr std::array<BookEntry, 18> kBook1{{
    {-7, 0x01, 9},
    {-6, 0x01, 8},
    {-5, 0x01, 7},
    {-4, 0x01, 6},
    {-3, 0x01, 5},
    {-2, 0x01, 4},
    {-1, 0x01, 3},
    {0, 0x04, 3},
    {1, 0x05, 3},
    {2, 0x06, 3},
    {3, 0x07, 3},
    {4, 0x03, 3},
    {5, 0x05, 4},
    {6, 0x09, 5},
    {7, 0x11, 6},
    {8, 0x21, 7},
    {9, 0x41, 8},
    {10, 0x81, 9},
}};

inline constexpr std::array<BookEntry, 16> kBook2{{
    {-7, 0x01, 9},
    {-6, 0x01, 8},
    {-5, 0x01, 7},
    {-4, 0x01, 6},
    {-3, 0x01, 5},
    {-2, 0x01, 4},
    {-1, 0x01, 3},
    {0, 0x02, 2},
    {1, 0x03, 2},
    {2, 0x03, 3},
    {3, 0x05, 4},
    {4, 0x09, 5},
    {5, 0x11, 6},
    {6, 0x21, 7},
    {7, 0x41, 8},
    {8, 0x81, 9},
}};

inline constexpr std::array<BookEntry, 15> kBook3{{
    {-7, 0x01, 9},
    {-6, 0x01, 8},
    {-5, 0x01, 7},
    {-4, 0x01, 6},
    {-3, 0x01, 5},
    {-2, 0x01, 4},
    {-1, 0x01, 3},
    {0, 0x01, 1},
    {1, 0x03, 3},
    {2, 0x05, 4},
    {3, 0x09, 5},
    {4, 0x11, 6},
    {5, 0x21, 7},
    {6, 0x41, 8},
    {7, 0x81, 9},
}};

struct DecodeEntry {
    std::int8_t symbol = 0;
    std::uint8_t bits = 0;  // 0: no code starts with these nine bits
};

struct DecodeTable {
    std::array<DecodeEntry, 512> entries{};
    bool consistent = true;  // false if two codes overlap
};

template <std::size_t N>
constexpr DecodeTable make_decode_table(const std::array<BookEntry, N>& book) {
    DecodeTable table;
    for (const BookEntry& entry : book) {
        const unsigned shift = 9U - entry.bits;
        const unsigned first = static_cast<unsigned>(entry.code) << shift;
        const unsigned count = 1U << shift;
        for (unsigned i = 0; i < count; ++i) {
            DecodeEntry& slot = table.entries[first + i];
            if (slot.bits != 0) {
                table.consistent = false;
            }
            slot.symbol = entry.symbol;
            slot.bits = entry.bits;
        }
    }
    return table;
}

inline constexpr DecodeTable kDecode1 = make_decode_table(kBook1);
inline constexpr DecodeTable kDecode2 = make_decode_table(kBook2);
inline constexpr DecodeTable kDecode3 = make_decode_table(kBook3);

// A codebook of the specification that overlapped itself would decode wrongly without a test
// noticing: the build checks it.
static_assert(kDecode1.consistent && kDecode2.consistent && kDecode3.consistent,
              "TH spec App. C: a codebook's codes overlap");

[[nodiscard]] constexpr const DecodeTable& decode_table(int huff_type) noexcept {
    return huff_type == 1 ? kDecode1 : huff_type == 2 ? kDecode2 : kDecode3;
}

}  // namespace iclforge::truehd::detail
