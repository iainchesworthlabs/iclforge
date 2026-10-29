#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "iclforge/containers/export.hpp"

// TrueHD in an IEC 61937 burst, the way HDMI carries it: MAT 1.0 (Metadata-enhanced Audio Transport), the
// coded bitstream and no metadata beyond it. TH spec §21.1 gives the data type (22) and §21.3 the frame size
// (61,440 bytes) and the number of access units in it (24). It gives no syntax: the start, middle and end code
// words, where each access unit sits, what Pd counts and how the words are ordered are all read off the output
// of FFmpeg's `spdif` muxer for TrueHD, the one oracle there is (libs/containers/ERRATA.md, entries M1 to M6).
// Every constant below carries the entry that records it.
//
// The layout, in the payload's own byte order (a burst written as 16-bit little-endian words has each pair of
// bytes swapped relative to this):
//
//     offset 0       20-byte start code
//     offset 20      access unit 0, then zero padding
//     offset 2560*k  access unit k (k = 1 .. 23), then zero padding; a 12-byte middle code sits at 30708,
//                    just before access unit 12
//     offset 61408  16-byte end code
//     offset 61424  8 bytes of zero stuffing, which Pd does not count
//
// The library knows no codec. A TrueHD access unit is delimited here by the only fields this file reads: the
// 12-bit length in 16-bit words that opens it (TH spec §3.1).

namespace iclforge::containers::iec61937 {

// One burst, preamble included: the period of a MAT frame in the carrier (TH spec §21.3). ERRATA M1.
inline constexpr std::size_t kMatBurstBytes = 61440;
// Pa, Pb, Pc and Pd.
inline constexpr std::size_t kMatPreambleBytes = 8;
// What follows the preamble in a burst.
inline constexpr std::size_t kMatPayloadBytes = kMatBurstBytes - kMatPreambleBytes;
// TH spec §21.3. ERRATA M2.
inline constexpr std::size_t kMatAccessUnitsPerFrame = 24;
// Access unit k opens at k * kMatSlotBytes, except access unit 0, which opens after the start code. ERRATA M3.
inline constexpr std::size_t kMatSlotBytes = 2560;
inline constexpr std::size_t kMatStartCodeBytes = 20;
inline constexpr std::size_t kMatMiddleCodeBytes = 12;
inline constexpr std::size_t kMatMiddleCodeOffset = 12 * kMatSlotBytes - kMatMiddleCodeBytes;
inline constexpr std::size_t kMatEndCodeBytes = 16;
// The end code ends where Pd's count ends; the 8 bytes after it are stuffing. ERRATA M4.
inline constexpr std::size_t kMatCountedPayloadBytes = 61424;
inline constexpr std::size_t kMatEndCodeOffset = kMatCountedPayloadBytes - kMatEndCodeBytes;

// Pc: IEC 61937 data type 22 (TH spec §21.1), no other bits. Pd: kMatCountedPayloadBytes. ERRATA M4.
inline constexpr std::uint16_t kMatPc = 0x0016;
inline constexpr std::uint16_t kMatPd = 0xEFF0;

// The largest access unit a slot holds: slot 0 loses the start code and slot 11 the middle code, slot 23 the
// end code. ERRATA M3.
[[nodiscard]] ICLFORGE_CONTAINERS_EXPORT std::size_t mat_slot_capacity(std::size_t slot);

enum class MatError : std::uint8_t {
    kNotAnAccessUnit,    // fewer than four bytes, an odd length, or a length field that disagrees with the bytes
    kAccessUnitTooLarge, // longer than its slot holds
    kNoSync,             // no Pa/Pb preamble where a burst has to start
    kNotMat,             // a preamble whose Pc names another data type
    kTruncatedBurst,     // the carrier ends part-way through a burst
    kBadCode,            // a start, middle or end code that is not the one the layout needs
    kBadSlot,            // a slot whose access unit overruns it
};

[[nodiscard]] ICLFORGE_CONTAINERS_EXPORT std::string_view describe(MatError error);

// Packs whole access units, 24 to a burst. The burst is in the carrier's order: 16-bit little-endian words,
// Pa 0xF872 first, the order FFmpeg's `spdif` muxer writes and the one a PCM16 carrier expects.
class ICLFORGE_CONTAINERS_EXPORT MatPacker {
   public:
    // Adds one access unit (the bytes the elementary stream holds, header first). Returns the burst when this
    // access unit was the 24th of its frame.
    [[nodiscard]] std::expected<std::optional<std::vector<std::byte>>, MatError> push(
        std::span<const std::byte> access_unit);

    // The burst of the access units held so far, the empty slots left as zero padding; nothing when none are
    // held. FFmpeg's muxer drops a last frame that is not full. This pads it instead, so no audio is lost; a
    // receiver reads an empty slot as no access unit. ERRATA M5.
    [[nodiscard]] std::optional<std::vector<std::byte>> flush();

    [[nodiscard]] std::size_t pending() const { return held_; }

   private:
    std::vector<std::byte> payload_ = std::vector<std::byte>(kMatPayloadBytes);
    std::size_t held_ = 0;
};

// All of a stream's access units packed into bursts, the last frame padded (MatPacker::flush).
[[nodiscard]] ICLFORGE_CONTAINERS_EXPORT std::expected<std::vector<std::byte>, MatError> wrap_mat_stream(
    std::span<const std::span<const std::byte>> access_units);

// The access units of a carrier of MAT bursts, in order. Accepts either word order, found from the first
// burst's Pa and Pb. Empty slots yield nothing. Every start, middle and end code must be the expected one.
[[nodiscard]] ICLFORGE_CONTAINERS_EXPORT std::expected<std::vector<std::vector<std::byte>>, MatError>
unwrap_mat_stream(std::span<const std::byte> carrier);

}  // namespace iclforge::containers::iec61937
