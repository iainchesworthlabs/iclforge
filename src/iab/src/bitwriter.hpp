#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// The MSB-first bit writer that mirrors bitreader.hpp (SMPTE ST 2098-2:2022 §5.1): the first bit
// written is the most significant bit of the first byte. Position 0 is the start of the element
// being built, so align() pads relative to the element, as the AlignBits fields in §9 require.

namespace iclforge::iab::detail {

class BitWriter {
public:
    void put_bits(std::uint64_t value, unsigned count) {
        for (unsigned i = 0; i < count; ++i) {
            put_bit(static_cast<unsigned>((value >> (count - 1 - i)) & 0x1U));
        }
    }

    // §5.2 Plex(n): the value is written in `initial_width` bits unless it equals or exceeds the
    // all-ones escape for that width, in which case the escape is written and the width doubles.
    // A symbol is at most 0xFFFFFFFE (§5.2); the caller guarantees it.
    void put_plex(std::uint64_t value, unsigned initial_width) {
        unsigned width = initial_width;
        while (width <= 32) {
            const std::uint64_t escape = (std::uint64_t{1} << width) - 1;
            if (value < escape) {
                put_bits(value, width);
                return;
            }
            put_bits(escape, width);
            width *= 2;
        }
    }

    void align() {
        while (bit_count_ % 8 != 0) {
            put_bit(0);
        }
    }

    // Aligns, then appends whole bytes.
    void put_bytes(std::span<const std::byte> bytes) {
        align();
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
        bit_count_ += bytes.size() * 8;
    }

    [[nodiscard]] std::size_t bit_count() const { return bit_count_; }

    // Aligns and returns the bytes written so far.
    [[nodiscard]] std::vector<std::byte> take() {
        align();
        return std::move(bytes_);
    }

private:
    void put_bit(unsigned bit) {
        if (bit_count_ % 8 == 0) {
            bytes_.push_back(std::byte{0});
        }
        if (bit != 0) {
            bytes_.back() |= static_cast<std::byte>(0x80U >> (bit_count_ % 8));
        }
        ++bit_count_;
    }

    std::vector<std::byte> bytes_;
    std::size_t bit_count_ = 0;
};

}  // namespace iclforge::iab::detail
