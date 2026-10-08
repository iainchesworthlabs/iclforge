#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "iclforge/base/bitwriter.hpp"

// IAB's writer over iclforge::BitWriter (libs/base), the mirror of bitreader.hpp: what it adds is
// Plex(n), and a field's bits above its width ignored, as this format's writers rely on. The first
// bit written is the most significant bit of the first byte (SMPTE ST 2098-2:2022 §5.1). Position 0
// is the start of the element being built, so align() pads relative to the element, as the
// AlignBits fields in §9 require.

namespace iclforge::iab::detail {

class BitWriter {
public:
    void put_bits(std::uint64_t value, unsigned count) {
        bits_.put_wide(count >= 64 ? value : value & ((std::uint64_t{1} << count) - 1),
                       static_cast<int>(count));
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

    void align() { bits_.align(); }

    // Aligns, then appends whole bytes.
    void put_bytes(std::span<const std::byte> bytes) { bits_.put_bytes(bytes); }

    [[nodiscard]] std::size_t bit_count() const { return bits_.bit_count(); }

    // Aligns and returns the bytes written so far.
    [[nodiscard]] std::vector<std::byte> take() { return bits_.take(); }

private:
    iclforge::BitWriter bits_;
};

}  // namespace iclforge::iab::detail
