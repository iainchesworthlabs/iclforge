#pragma once

#include <cstddef>
#include <span>

// The raw AC-4 frame inside one sync frame, without allocating: what the Sendspin player's AC-4
// path takes from a burst chunk (planning/ac4.md, I6).
//
// A sync frame is (ETSI TS 103 190-1 Annex G.4.1, IEC 61937-14 Annex A) the sync word 0xAC40, or
// 0xAC41 where a CRC word follows the frame, then frame_size in 16 bits - 0xFFFF escaping to 24
// more - then the raw frame of that many bytes, then the CRC word. An HBR16 burst carries the sync
// frame padded with zeros to a whole 8 bytes, which this leaves alone. iclforge::ac4::scan() reads
// the same header, into a list; the player reads one frame per chunk and has no room for a list.

namespace iclforge {

// The raw frame, or nothing for bytes that are not one whole sync frame: not an AC-4 sync word,
// shorter than its own header, a frame of no bytes, or one that runs past the end (the CRC word
// included, for 0xAC41).
[[nodiscard]] constexpr std::span<const std::byte> ac4_raw_frame(
    std::span<const std::byte> sync_frame) noexcept {
    if (sync_frame.size() < 4 || sync_frame[0] != std::byte{0xAC} ||
        (sync_frame[1] != std::byte{0x40} && sync_frame[1] != std::byte{0x41})) {
        return {};
    }
    const auto at = [&](std::size_t i) { return std::to_integer<std::size_t>(sync_frame[i]); };
    std::size_t head = 4;
    std::size_t size = (at(2) << 8U) | at(3);
    if (size == 0xFFFF) {
        if (sync_frame.size() < 7) {
            return {};
        }
        head = 7;
        size = (at(4) << 16U) | (at(5) << 8U) | at(6);
    }
    const std::size_t crc = sync_frame[1] == std::byte{0x41} ? 2U : 0U;
    if (size == 0 || size > sync_frame.size() || head + size + crc > sync_frame.size()) {
        return {};
    }
    return sync_frame.subspan(head, size);
}

}  // namespace iclforge
