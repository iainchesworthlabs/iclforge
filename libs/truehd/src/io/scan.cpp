#include "iclforge/truehd/io/scan.hpp"

#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/elementary.hpp"
#include "iclforge/truehd/io/major_sync.hpp"

// See scan.hpp.

namespace iclforge::truehd {

namespace {

constexpr std::size_t kAccessUnitHeaderBytes = 4;

// The scan's reading of the stream's facts: a probe that does not reach a sample rate or a unit
// length cannot place any unit in time.
[[nodiscard]] std::expected<ScannedStream, Error> describe(std::span<const std::byte> data) {
    const auto info = probe_stream(data);
    if (!info) {
        return std::unexpected(info.error());
    }
    if (info->sample_rate == 0 || info->samples_per_access_unit <= 0) {
        return std::unexpected(
            Error{ErrorKind::kUnsupported, "the major sync names no sample rate this library knows"});
    }
    ScannedStream scan;
    scan.kind = info->major_sync.kind;
    scan.sample_rate = info->sample_rate;
    scan.samples_per_unit = static_cast<std::uint32_t>(info->samples_per_access_unit);
    scan.num_substreams = info->num_substreams;
    scan.channels = info->channels;
    scan.has_object_substream = info->has_object_substream;
    scan.variable_rate = info->major_sync.is_vbr;
    scan.peak_data_rate = info->major_sync.peak_data_rate;
    return scan;
}

// The end-of-stream marker is in the last unit's segments: decode from the last restart to see it.
// A stream that does not decode to its end has none to read.
[[nodiscard]] std::uint32_t padding_after(std::span<const std::byte> data,
                                          const RestartPoint& last_restart) {
    Decoder decoder;
    ElementaryReader reader(data.subspan(static_cast<std::size_t>(last_restart.offset)));
    std::uint32_t padding = 0;
    while (const auto unit = reader.next()) {
        const auto decoded = decoder.decode(*unit);
        padding = decoded && *decoded ? static_cast<std::uint32_t>((*decoded)->padding_samples) : 0U;
    }
    return padding;
}

}  // namespace

std::expected<ScannedStream, Error> scan_stream(std::span<const std::byte> data,
                                                const ScanOptions& options) {
    auto scan = describe(data);
    if (!scan) {
        return scan;
    }
    const std::uint64_t spacing = std::max<std::uint64_t>(options.restart_spacing, 1);

    ElementaryReader reader(data);
    RestartPoint last_sync{};
    std::uint64_t spacing_from = 0;
    std::uint64_t unit_number = 0;
    while (const auto unit = reader.next()) {
        scan->unit_bytes += unit->size();
        const std::uint64_t number = unit_number++;
        if (unit->size() < kAccessUnitHeaderBytes ||
            !starts_with_major_sync(unit->subspan(kAccessUnitHeaderBytes))) {
            continue;
        }
        ++scan->major_syncs;
        // The reader has moved past the unit, so the unit began its length before.
        last_sync = RestartPoint{.unit = number, .offset = reader.offset() - unit->size()};
        if (scan->restarts.empty() || last_sync.unit - spacing_from >= spacing) {
            scan->restarts.push_back(last_sync);
            spacing_from = last_sync.unit;
        }
    }
    if (scan->restarts.empty()) {
        return std::unexpected(Error{ErrorKind::kBadSync, "no access unit with a major sync"});
    }
    scan->units = unit_number;
    scan->skipped_bytes = reader.skipped_bytes();
    scan->leading_bytes = reader.leading_bytes();
    scan->padding_samples = padding_after(data, last_sync);
    return scan;
}

}  // namespace iclforge::truehd
