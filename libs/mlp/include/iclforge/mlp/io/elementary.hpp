#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

#include "iclforge/mlp/core/error.hpp"
#include "iclforge/mlp/core/stream_info.hpp"
#include "iclforge/mlp/export.hpp"

// TrueHD (MLP) as an elementary stream (.thd, .mlp): access units back to back, each starting
// with its 4-byte header (3.1), the first of a stream and every random access point carrying a
// major sync (4). scan() walks them; probe() summarises a stream without decoding its audio.

namespace iclforge::mlp {

struct AccessUnitInfo {
    std::size_t offset = 0;
    std::size_t size = 0;  // access_unit_length * 2
    std::uint16_t input_timing = 0;
    bool major_sync = false;
    bool extra_data = false;  // bytes after the last substream segment (16)
};

struct ScanResult {
    std::vector<AccessUnitInfo> units;
    // Set when the walk stopped before the end: the error, and where.
    std::optional<Error> stopped_at;
    std::size_t stopped_at_offset = 0;
};

// Walks the access units of `data`, checking each one's framing (its check nibble, its major
// sync's CRC, its substream end pointers). The walk starts at the first major sync, so a stream
// cut from the middle of another is found from its next random access point.
[[nodiscard]] ICLFORGE_MLP_EXPORT ScanResult scan(std::span<const std::byte> data);

// The major sync at the start of `data` (its sync word included).
[[nodiscard]] ICLFORGE_MLP_EXPORT std::expected<MajorSync, Error> parse_major_sync(
    std::span<const std::byte> data);

// What a stream holds, from its major syncs and framing.
struct StreamSummary {
    MajorSync major_sync;  // the first
    std::size_t access_units = 0;
    std::size_t major_syncs = 0;
    std::uint64_t samples = 0;  // access units times samples per access unit
    double duration_seconds = 0.0;
    std::uint64_t bytes = 0;
    std::uint64_t mean_bits_per_second = 0;
    std::uint64_t peak_access_unit_bytes = 0;
    // The presentations' channel counts: 2, 6, 8 and 16 where present (2.2).
    std::vector<int> presentations;
    bool has_extra_data = false;  // some access unit carries evolution payloads (Atmos, say)
    std::optional<Error> stopped_at;
};
[[nodiscard]] ICLFORGE_MLP_EXPORT std::expected<StreamSummary, Error> probe(
    std::span<const std::byte> data);

}  // namespace iclforge::mlp
