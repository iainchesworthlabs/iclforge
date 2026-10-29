#pragma once

#include <cstdint>

#include "iclforge/base/bitreader.hpp"
#include "iclforge/base/bitwriter.hpp"

// Field helpers the syntax readers and writers of this library share.

namespace iclforge::truehd::detail {

// A two's-complement field of `bits` bits (1..32).
[[nodiscard]] inline std::int32_t read_signed(BitReader& reader, int bits) noexcept {
    const std::uint32_t value = reader.read(bits);
    const std::uint32_t sign = 1U << (bits - 1);
    return static_cast<std::int32_t>((value ^ sign) - sign);
}

// A field is written from its low bits: a value a caller built wider than the syntax allows is cut
// to the field, never an assertion in the bit writer.
inline void put_field(BitWriter& writer, std::uint32_t value, int bits) {
    writer.put(bits >= 32 ? value : (value & ((1U << bits) - 1U)), bits);
}

inline void put_signed(BitWriter& writer, std::int32_t value, int bits) {
    put_field(writer, static_cast<std::uint32_t>(value), bits);
}

}  // namespace iclforge::truehd::detail
