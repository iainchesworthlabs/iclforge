#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "iclforge/ac4/encoder/encoder.hpp"
#include "iclforge/base/crc16.hpp"

// Part 1 Annex G.3 and G.4 (Part 2 Annex G refers to it): the sync word, the
// frame_size, the raw frame and, after sync word 0xAC41, a CRC over frame_size
// and the raw frame.

namespace iclforge::ac4 {
namespace {

void put(std::vector<std::byte>& out, std::uint32_t value, int bytes) {
    for (int b = bytes - 1; b >= 0; --b) {
        out.push_back(static_cast<std::byte>((value >> (8U * static_cast<unsigned>(b))) & 0xFFU));
    }
}

}  // namespace

std::vector<std::byte> sync_frame(std::span<const std::byte> raw_ac4_frame, bool crc) {
    std::vector<std::byte> out;
    out.reserve(raw_ac4_frame.size() + 9);
    put(out, crc ? 0xAC41U : 0xAC40U, 2);
    const std::size_t protected_start = out.size();
    // G.3.2: frame_size escapes to 24 bits at 0xFFFF.
    if (raw_ac4_frame.size() < 0xFFFF) {
        put(out, static_cast<std::uint32_t>(raw_ac4_frame.size()), 2);
    } else {
        put(out, 0xFFFFU, 2);
        put(out, static_cast<std::uint32_t>(raw_ac4_frame.size()), 3);
    }
    out.insert(out.end(), raw_ac4_frame.begin(), raw_ac4_frame.end());
    if (crc) {
        // G.4.2's CRC (iclforge/base/crc16.hpp).
        put(out, base::crc16(std::span<const std::byte>(out).subspan(protected_start)), 2);
    }
    return out;
}

}  // namespace iclforge::ac4
