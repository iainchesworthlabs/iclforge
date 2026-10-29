#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

#include "iclforge/truehd/core/error.hpp"
#include "iclforge/truehd/core/format.hpp"
#include "iclforge/truehd/export.hpp"
#include "iclforge/truehd/io/major_sync.hpp"

// An access unit (TH spec §3): a 4-byte header, a major sync when the unit has one, the substream
// directory, the substream segments and, if bytes remain, extra data. The first bytes of the unit
// are the first of the header; units follow one another with no gap (no frame sync word, the length
// says where the next begins).

namespace iclforge::truehd {

// TH spec §6.1: one directory entry.
struct SubstreamEntry {
    bool extra_substream_word = false;
    bool restart_nonexistent =
        false;                 // clear: the segment carries a restart header in its first block
    bool crc_present = false;  // set: the segment ends with a parity byte and a CRC byte
    bool reserved_bit = false;
    std::uint16_t end_ptr =
        0;  // 12 bits: where the segment ends, in 16-bit words from the first segment's start
    // The 16-bit word an entry with extra_substream_word sends: DRC (TH spec §6.1, §12.1).
    std::int16_t drc_gain_update = 0;     // 9 bits, signed
    std::uint8_t drc_time_update = 0;     // 3 bits
    std::uint8_t extra_word_padding = 0;  // the 4 bits left
    friend bool operator==(const SubstreamEntry&, const SubstreamEntry&) = default;
};

// TH spec §16.1: the extra data after the last segment. Read as the specification lays it out; no
// stream here has any, so the framing has no oracle (ERRATA.md). It is never an error: the checks
// are reported, and the bytes are carried as they are.
struct ExtraData {
    std::uint8_t header_check_nibble = 0;
    std::uint16_t length = 0;  // extra_data_length, 12 bits, "in 16-bit words" (the specification
                               // does not say of what)
    bool check_ok = false;     // header_check_nibble against the length (TH spec §16.2)
    std::uint8_t evo_reserved = 0;
    std::uint16_t evo_frame_byte_length = 0;
    bool framing_ok = false;  // the payload and the parity byte fit in `bytes`
    std::span<const std::byte>
        evolution_payload;             // evo_frame_byte_length bytes; empty unless framing_ok
    std::uint8_t parity = 0;           // the last byte of `bytes`
    bool parity_ok = false;            // TH spec §16.4
    std::span<const std::byte> bytes;  // all of it, from the header nibble to the end of the unit
};

struct AccessUnit {
    std::uint8_t check_nibble = 0;
    std::uint16_t length_words = 0;  // the whole unit in 16-bit words, header included
    std::uint16_t input_timing = 0;
    std::optional<MajorSync> major_sync;
    std::array<SubstreamEntry, kMaxSubstreams> entries{};
    int num_substreams = 0;
    // The segments, each without anything this struct has already read. A segment with crc_present
    // still carries its parity and CRC bytes at its end.
    std::array<std::span<const std::byte>, kMaxSubstreams> segments;
    std::optional<ExtraData> extra_data;
    std::span<const std::byte> bytes;  // the whole unit

    [[nodiscard]] std::size_t length_bytes() const noexcept {
        return std::size_t{length_words} * 2U;
    }
};

// Reads the unit that starts at `data[0]`. `inherited_substreams` is the number of substreams to
// expect when the unit has no major sync of its own (the last major sync's), 0 if none has been
// seen: a unit without a major sync then fails with kNeedsRestart. With `strict` clear a wrong
// check nibble or a wrong major sync CRC is not an error.
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT std::expected<AccessUnit, Error> parse_access_unit(
    std::span<const std::byte> data, int inherited_substreams, bool strict = true);

// Writes a unit's header, directory and (when given) major sync: everything before the segments.
// The caller supplies the segments' sizes through `entries[i].end_ptr`, and appends the segments
// and the extra data. The check nibble and length are computed from `total_bytes`, the size of the
// whole unit.
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT std::expected<std::vector<std::byte>, Error>
write_access_unit_prefix(const std::optional<MajorSync>& major_sync,
                         std::span<const SubstreamEntry> entries, std::uint16_t input_timing,
                         std::size_t total_bytes);

// The unit's length in bytes from its first two bytes, 0 if there are fewer than two (TH spec
// §3.3).
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT std::size_t access_unit_length_bytes(
    std::span<const std::byte> data) noexcept;

}  // namespace iclforge::truehd
