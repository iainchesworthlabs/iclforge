#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>

#include "iclforge/base/syntax_trace.hpp"

namespace iclforge {

// The MSB-first bit reader every bitstream in the tree is read through (A/52 §5.1, TS 103 190-1
// clause 3.4, ST 2098-2 §5.1: the first bit is the most significant bit of the first byte), the
// mirror of BitWriter. Reading past the end does not throw and does not read out of bounds: it
// yields zeros and sets a sticky flag, which a caller checks once at a boundary where carrying on
// would loop on a count read from nothing.
//
// The bits are read through a cache of 8 bytes, as a 64-bit word: a peek of up to 32 bits is a
// shift of it, and the word is loaded again only when the position has moved 4 bytes on. The bytes
// past the end of the data are zeros in it, which is what a read past the end returns.
//
// A read given a name is a syntax element: with a base::SyntaxSink attached, the reader emits one
// base::SyntaxRecord for it (iclforge/base/syntax_trace.hpp). A read without one, skip() and align()
// record nothing, and cost nothing for the sink.
class BitReader {
   public:
    explicit BitReader(std::span<const std::byte> data) noexcept : data_(data) {}
    BitReader(std::span<const std::byte> data, int substream, base::SyntaxSink sink) noexcept
        : data_(data), substream_(substream), sink_(sink) {}

    // A field of up to 32 bits (a wider one yields its low 32), not recorded.
    [[nodiscard]] std::uint32_t read(int bits) noexcept {
        const std::uint32_t value = peek(bits);
        advance(bits);
        return value;
    }

    [[nodiscard]] std::uint32_t read_bit() noexcept { return read(1); }

    // A fixed-width syntax element of up to 32 bits.
    std::uint32_t read(int bits, std::string_view name) noexcept {
        const std::size_t start = pos_;
        const std::uint32_t value = peek(bits);
        advance(bits);
        record(start, bits, value, name);
        return value;
    }

    bool read_flag(std::string_view name) noexcept { return read(1, name) != 0; }

    // variable_bits(n_bits), one element: groups of n_bits, the most significant first, each
    // followed by a continuation bit, each group after the first adding 1 << n_bits to the value
    // shifted before it (TS 102 366 Annex H's and TS 103 190-1 clause 4.2.2's coding, which are
    // the same). The loop ends at a clear continuation bit, at the end of the data, or after
    // `max_groups` groups when that is above 0 (TS 103 420 §5.5.1's variable_bits_max). A value too
    // large for 64 bits is returned, and recorded, modulo 2^64.
    std::uint64_t variable_bits(int n_bits, std::string_view name = {}, int max_groups = 0) noexcept {
        const std::size_t start = pos_;
        std::uint64_t value = 0;
        for (int group = 1;; ++group) {
            value += peek(n_bits);
            advance(n_bits);
            const bool more = peek(1) != 0;
            advance(1);
            if (!more || overflow_ || group == max_groups) {
                break;
            }
            value <<= n_bits;
            value += std::uint64_t{1} << n_bits;
        }
        record_element(start, pos_, value, name);
        return value;
    }

    // A field whose width the stream sets and whose bits the syntax does not interpret: one record
    // of the whole width, valued at its last 64 bits. A record's width is 16 bits, so a run longer
    // than 65535 bits is recorded as consecutive 65535-bit records, the last shorter.
    void read_run(std::uint64_t bits, std::string_view name) noexcept {
        while (bits > 0) {
            const std::uint64_t width = bits < kMaxRecordBits ? bits : kMaxRecordBits;
            const std::size_t start = pos_;
            std::uint64_t value = 0;
            for (std::uint64_t i = 0; i < width; ++i) {
                value = (value << 1U) | bit_at(pos_ + static_cast<std::size_t>(i));
            }
            advance_bits(static_cast<std::size_t>(width));
            record(start, static_cast<int>(width), value, name);
            bits -= width;
        }
    }

    // Unrecorded bits: alignment, fill, reserved payload skipped over.
    void skip(std::size_t bits) noexcept { advance_bits(bits); }

    void align() noexcept { advance_bits((8 - (pos_ % 8)) % 8); }

    // The next `bits` bits without moving, for a decoder that decides a width before it knows it
    // (a Huffman or an escape code): up to 32 of them, zeros past the end.
    [[nodiscard]] std::uint32_t peek(int bits) const noexcept {
        if (bits <= 0) {
            return 0;
        }
        if (bits > 32) {
            return peek_slow(bits);
        }
        return static_cast<std::uint32_t>(window() >> static_cast<unsigned>(64 - bits));
    }

    void consume(int bits) noexcept { advance(bits); }

    // A zero-width element is not a record: it occupies no bits. (Not called emit(): Qt defines
    // `emit` as a macro, and a Qt program includes this header.) Nor is an element that runs past
    // the end of the data: the reader hands back zeros for it and the caller fails at its next
    // check, so the trace ends with the last element the data holds.
    void record(std::size_t start, int bits, std::uint64_t value, std::string_view name) const {
        if (sink_ && bits > 0 && start + static_cast<std::size_t>(bits) <= size_bits()) {
            sink_(base::SyntaxRecord{substream_, static_cast<std::uint32_t>(start),
                                     static_cast<std::uint16_t>(bits), value, name});
        }
    }

    // One element, whatever its width. An element wider than 65535 bits is recorded as
    // consecutive 65535-bit records, the last shorter, each valued at its own last 64 bits - the
    // shape read_run() uses.
    void record_element(std::size_t start, std::size_t end, std::uint64_t value,
                      std::string_view name) const {
        const std::size_t total = end - start;
        if (total <= kMaxRecordBits) {
            record(start, static_cast<int>(total), value, name);
            return;
        }
        for (std::size_t at = start; at < end;) {
            const std::size_t width = std::min<std::size_t>(kMaxRecordBits, end - at);
            std::uint64_t chunk = 0;
            for (std::size_t i = 0; i < width; ++i) {
                chunk = (chunk << 1U) | bit_at(at + i);
            }
            record(at, static_cast<int>(width), chunk, name);
            at += width;
        }
    }

    // The bit at an absolute position, zero past the end of the data, without moving the reader:
    // for a decoder that reads ahead of what its syntax consumes.
    [[nodiscard]] std::uint32_t bit(std::size_t index) const noexcept { return bit_at(index); }

    [[nodiscard]] std::size_t bit_position() const noexcept { return pos_; }
    [[nodiscard]] std::size_t size_bits() const noexcept { return data_.size() * 8U; }
    [[nodiscard]] std::size_t remaining_bits() const noexcept {
        return pos_ >= size_bits() ? 0 : size_bits() - pos_;
    }
    [[nodiscard]] bool overflowed() const noexcept { return overflow_; }
    [[nodiscard]] int substream() const noexcept { return substream_; }
    [[nodiscard]] base::SyntaxSink sink() const noexcept { return sink_; }

    // Moves to an absolute bit position inside the data; one past the end overflows, and leaves
    // the reader at the end.
    void seek(std::size_t bit_position) noexcept {
        if (bit_position > size_bits()) {
            overflow_ = true;
            pos_ = size_bits();
        } else {
            pos_ = bit_position;
        }
    }

   private:
    static constexpr std::uint64_t kMaxRecordBits = 65535;

    // The 64 bits from the current position on, the first in the top bit, zeros past the end of
    // the data. The cache holds the 8 bytes from cache_byte_ on; the position is inside its first 4
    // bytes, so at least 33 of the bits are real.
    [[nodiscard]] std::uint64_t window() const noexcept {
        const std::size_t byte = pos_ >> 3U;
        if (byte < cache_byte_ || byte - cache_byte_ > 3U) {
            load(byte);
        }
        return cache_ << (pos_ - cache_byte_ * 8U);
    }

    void load(std::size_t byte) const noexcept {
        cache_byte_ = byte;
        std::uint64_t word = 0;
        if (byte + 8U <= data_.size()) {
            std::memcpy(&word, data_.data() + byte, sizeof word);
            if constexpr (std::endian::native == std::endian::little) {
                word = std::byteswap(word);
            }
        } else {
            for (std::size_t i = 0; i < 8U; ++i) {
                word = (word << 8U) |
                       (byte + i < data_.size() ? static_cast<std::uint64_t>(data_[byte + i]) : 0U);
            }
        }
        cache_ = word;
    }

    // A peek wider than the word holds, bit by bit: the low 32 bits of the value.
    [[nodiscard]] std::uint32_t peek_slow(int bits) const noexcept {
        std::uint32_t value = 0;
        for (int i = 0; i < bits; ++i) {
            value = (value << 1U) | bit_at(pos_ + static_cast<std::size_t>(i));
        }
        return value;
    }

    [[nodiscard]] std::uint32_t bit_at(std::size_t bit) const noexcept {
        if (bit >= size_bits()) {
            return 0;
        }
        const auto byte = static_cast<std::uint32_t>(data_[bit / 8U]);
        return (byte >> (7U - static_cast<unsigned>(bit % 8U))) & 1U;
    }

    void advance(int bits) noexcept { advance_bits(static_cast<std::size_t>(bits)); }

    void advance_bits(std::size_t bits) noexcept {
        pos_ += bits;
        if (pos_ > size_bits()) {
            overflow_ = true;
        }
    }

    std::span<const std::byte> data_;
    std::size_t pos_ = 0;
    int substream_ = 0;
    base::SyntaxSink sink_{};
    bool overflow_ = false;
    // The cache of window(): the 8 bytes from cache_byte_ on, as a big-endian word. The data does
    // not change, so it needs no invalidating; it is mutable because a peek fills it.
    mutable std::uint64_t cache_ = 0;
    mutable std::size_t cache_byte_ = std::numeric_limits<std::size_t>::max();
};

}  // namespace iclforge
