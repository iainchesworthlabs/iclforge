#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// The ISO/IEC 14496-12 box primitives the containers' writers share: big-endian fields, a box's
// type, a Box and a FullBox. MP4's muxer and fragmenter (mp4/isobmff_detail.hpp) and IAMF's
// encapsulation (iamf/container.cpp) build their boxes with them. Internal to libs/containers.
//
// Every free function here is `inline`: this header is included by more than one .cpp in the
// same target, so ODR requires it.

namespace iclforge::containers::detail {

using Bytes = std::vector<std::byte>;

inline void put_u8(Bytes& out, std::uint8_t value) { out.push_back(static_cast<std::byte>(value)); }

inline void put_u16(Bytes& out, std::uint16_t value) {
    put_u8(out, static_cast<std::uint8_t>(value >> 8));
    put_u8(out, static_cast<std::uint8_t>(value & 0xFF));
}

inline void put_u32(Bytes& out, std::uint32_t value) {
    put_u8(out, static_cast<std::uint8_t>(value >> 24));
    put_u8(out, static_cast<std::uint8_t>((value >> 16) & 0xFF));
    put_u8(out, static_cast<std::uint8_t>((value >> 8) & 0xFF));
    put_u8(out, static_cast<std::uint8_t>(value & 0xFF));
}

inline void put_u64(Bytes& out, std::uint64_t value) {
    put_u32(out, static_cast<std::uint32_t>(value >> 32));
    put_u32(out, static_cast<std::uint32_t>(value & 0xFFFFFFFFU));
}

// §4.2: a box type is 4 printable-ASCII bytes.
inline void put_fourcc(Bytes& out, std::string_view fourcc) {
    assert(fourcc.size() == 4);
    for (const char c : fourcc) {
        put_u8(out, static_cast<std::uint8_t>(c));
    }
}

inline void put_bytes(Bytes& out, std::span<const std::byte> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

// §4.2's Box: a 32-bit size (the WHOLE box, header included), then the 4-byte type, then the
// body. Every box these writers build stays well under 4 GiB (they refuse anything that would
// not), so the 64-bit largesize escape (size field == 1) is never needed.
inline void put_box(Bytes& out, std::string_view fourcc, std::span<const std::byte> body) {
    put_u32(out, static_cast<std::uint32_t>(8 + body.size()));
    put_fourcc(out, fourcc);
    put_bytes(out, body);
}

// §4.2's FullBox: a Box with a 1-byte version and 3-byte flags prepended to the body.
inline void put_fullbox(Bytes& out, std::string_view fourcc, std::uint8_t version,
                        std::uint32_t flags, std::span<const std::byte> body) {
    Bytes full;
    put_u8(full, version);
    put_u8(full, static_cast<std::uint8_t>(flags >> 16));
    put_u8(full, static_cast<std::uint8_t>((flags >> 8) & 0xFF));
    put_u8(full, static_cast<std::uint8_t>(flags & 0xFF));
    put_bytes(full, body);
    put_box(out, fourcc, full);
}

}  // namespace iclforge::containers::detail
