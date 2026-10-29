#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

#include "iclforge/mlp/core/error.hpp"
#include "iclforge/mlp/core/stream_info.hpp"
#include "iclforge/mlp/core/syntax.hpp"

// An access unit's framing (3.1, 4, 6): the header, the major sync when there is one, the
// substream directory, each substream's segment and the extra data after the last one. Parsing
// checks the framing's own checks (the check nibble, the major sync CRC, the end pointers); a
// segment's contents are the decoder's.

namespace iclforge::mlp::detail {

struct Segment {
    SubstreamDirectoryEntry entry;
    std::span<const std::byte> bytes;  // from the segment's first byte to its end pointer
};

struct AccessUnitLayout {
    AccessUnitHeader header;
    std::optional<MajorSync> major_sync;
    int num_substreams = 0;
    std::array<Segment, kMaxSubstreams> segments{};
    // The bytes between the last segment's end and the access unit's end: the extra data (16).
    std::span<const std::byte> extra_data;
    std::span<const std::byte> bytes;  // the whole access unit
};

// The major sync at the start of `data` (its sync word included), and the bytes it takes.
struct MajorSyncRead {
    MajorSync sync;
    std::size_t size = 0;
};
[[nodiscard]] std::expected<MajorSyncRead, Error> read_major_sync(std::span<const std::byte> data);
[[nodiscard]] std::expected<std::vector<std::byte>, Error> write_major_sync(const MajorSync& sync);

// Whether `data` starts with a major sync word (0xF8726FBA or 0xF8726FBB).
[[nodiscard]] bool starts_with_major_sync(std::span<const std::byte> data) noexcept;

// The access unit at the start of `data`. `current` is the stream's last major sync, which says
// how many substreams an access unit without one has.
[[nodiscard]] std::expected<AccessUnitLayout, Error> parse_access_unit(
    std::span<const std::byte> data, const MajorSync* current);

// The parts of an access unit to assemble. Each segment's bytes are its whole contents, check
// bytes included and padded to a 16-bit boundary; extra_data likewise.
struct AccessUnitParts {
    std::uint16_t input_timing = 0;
    const MajorSync* major_sync = nullptr;
    std::array<SubstreamDirectoryEntry, kMaxSubstreams> entries{};
    std::array<std::span<const std::byte>, kMaxSubstreams> segments{};
    int num_substreams = 0;
    std::span<const std::byte> extra_data;
};

// Assembles an access unit: fills each entry's end pointer, the length and the check nibble.
[[nodiscard]] std::expected<std::vector<std::byte>, Error> assemble_access_unit(
    const AccessUnitParts& parts);

}  // namespace iclforge::mlp::detail
