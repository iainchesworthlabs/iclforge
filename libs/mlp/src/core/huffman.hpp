#pragma once

#include <array>
#include <cstdint>

// The three Huffman codebooks of Appendix C, used for a channel's MSBs when its huff_type is 1, 2
// or 3 (11.2), and the 9-bit lookup a decoder reads them through.
//
// The table's symbol is the MSB value as coded; the reconstruction subtracts a bias that centres
// each codebook, 1 << (lsbs_bits + 2 - huff_type) (P23). Every codebook is checked at compile time
// to be prefix-free with a Kraft sum of 255/256: exactly two 9-bit words are unused in each,
// 000000000 and 010000000, and both have a zero ninth bit, which is what 11.4's rule that the
// ninth bit of a 9-bit MSB code "SHALL be 1" excludes (P4).

namespace iclforge::mlp::detail {

struct HuffmanCode {
    std::int8_t symbol = 0;
    std::uint8_t code = 0;
    std::uint8_t length = 0;
};

inline constexpr int kHuffmanCodebooks = 3;
inline constexpr int kHuffmanMaxLength = 9;

// Appendix C, codebook 0 (huff_type 1): symbols -7 to +10.
inline constexpr std::array<HuffmanCode, 18> kCodebook0{{
    {-7, 0x01, 9}, {-6, 0x01, 8}, {-5, 0x01, 7}, {-4, 0x01, 6}, {-3, 0x01, 5}, {-2, 0x01, 4},
    {-1, 0x01, 3}, {0, 0x04, 3},  {1, 0x05, 3},  {2, 0x06, 3},  {3, 0x07, 3},  {4, 0x03, 3},
    {5, 0x05, 4},  {6, 0x09, 5},  {7, 0x11, 6},  {8, 0x21, 7},  {9, 0x41, 8},  {10, 0x81, 9},
}};
// Codebook 1 (huff_type 2): symbols -7 to +8.
inline constexpr std::array<HuffmanCode, 16> kCodebook1{{
    {-7, 0x01, 9}, {-6, 0x01, 8}, {-5, 0x01, 7}, {-4, 0x01, 6}, {-3, 0x01, 5}, {-2, 0x01, 4},
    {-1, 0x01, 3}, {0, 0x02, 2},  {1, 0x03, 2},  {2, 0x03, 3},  {3, 0x05, 4},  {4, 0x09, 5},
    {5, 0x11, 6},  {6, 0x21, 7},  {7, 0x41, 8},  {8, 0x81, 9},
}};
// Codebook 2 (huff_type 3): symbols -7 to +7.
inline constexpr std::array<HuffmanCode, 15> kCodebook2{{
    {-7, 0x01, 9}, {-6, 0x01, 8}, {-5, 0x01, 7}, {-4, 0x01, 6}, {-3, 0x01, 5},
    {-2, 0x01, 4}, {-1, 0x01, 3}, {0, 0x01, 1},  {1, 0x03, 3},  {2, 0x05, 4},
    {3, 0x09, 5},  {4, 0x11, 6},  {5, 0x21, 7},  {6, 0x41, 8},  {7, 0x81, 9},
}};

inline constexpr std::array<int, kHuffmanCodebooks> kMinSymbol{-7, -7, -7};
inline constexpr std::array<int, kHuffmanCodebooks> kMaxSymbol{10, 8, 7};

// One entry per 9-bit window: the symbol and the length of the codeword the window starts with,
// length 0 where no codeword starts it.
struct HuffmanEntry {
    std::int8_t symbol = 0;
    std::uint8_t length = 0;
};
using HuffmanLookup = std::array<HuffmanEntry, 1U << kHuffmanMaxLength>;

template <std::size_t N>
consteval HuffmanLookup make_lookup(const std::array<HuffmanCode, N>& book) {
    HuffmanLookup lookup{};
    unsigned kraft = 0;  // in units of 2^-9
    for (const HuffmanCode& c : book) {
        const unsigned span = 1U << (kHuffmanMaxLength - c.length);
        const unsigned first = static_cast<unsigned>(c.code) << (kHuffmanMaxLength - c.length);
        for (unsigned w = first; w < first + span; ++w) {
            if (lookup[w].length != 0) {
                throw "Appendix C codebook is not prefix-free";
            }
            lookup[w] = HuffmanEntry{c.symbol, c.length};
        }
        kraft += span;
    }
    if (kraft != 510U) {
        throw "Appendix C codebook does not leave exactly two 9-bit words unused";
    }
    if (lookup[0b000000000].length != 0 || lookup[0b010000000].length != 0) {
        throw "Appendix C codebook's unused words are not the two with a zero ninth bit";
    }
    return lookup;
}

inline constexpr std::array<HuffmanLookup, kHuffmanCodebooks> kHuffmanLookup{
    make_lookup(kCodebook0), make_lookup(kCodebook1), make_lookup(kCodebook2)};

// The codeword of `symbol` in codebook `book` (0 to 2), for the encoder. The symbol must be in the
// codebook's range.
struct Codeword {
    std::uint32_t code = 0;
    int length = 0;
};

template <std::size_t N>
consteval std::array<Codeword, 18> make_codewords(const std::array<HuffmanCode, N>& book) {
    std::array<Codeword, 18> words{};
    for (const HuffmanCode& c : book) {
        words[static_cast<std::size_t>(c.symbol + 7)] = Codeword{c.code, c.length};
    }
    return words;
}

inline constexpr std::array<std::array<Codeword, 18>, kHuffmanCodebooks> kCodewords{
    make_codewords(kCodebook0), make_codewords(kCodebook1), make_codewords(kCodebook2)};

[[nodiscard]] constexpr Codeword codeword(int book, int symbol) noexcept {
    return kCodewords[static_cast<std::size_t>(book)][static_cast<std::size_t>(symbol + 7)];
}

}  // namespace iclforge::mlp::detail
