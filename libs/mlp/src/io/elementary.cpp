#include "iclforge/mlp/io/elementary.hpp"

#include <algorithm>

#include "core/access_unit.hpp"
#include "iclforge/mlp/decoder/decoder.hpp"

namespace iclforge::mlp {

namespace {

// The offset of the first major sync at or after `from`, an access unit's header (4 bytes)
// before the sync word.
std::optional<std::size_t> next_major_sync(std::span<const std::byte> data, std::size_t from) {
    for (std::size_t at = from + 4; at + 4 <= data.size(); ++at) {
        if (detail::starts_with_major_sync(data.subspan(at))) {
            return at - 4;
        }
    }
    return std::nullopt;
}

}  // namespace

std::expected<MajorSync, Error> parse_major_sync(std::span<const std::byte> data) {
    auto read = detail::read_major_sync(data);
    if (!read) {
        return std::unexpected(read.error());
    }
    return read->sync;
}

ScanResult scan(std::span<const std::byte> data) {
    ScanResult result;
    const auto start = next_major_sync(data, 0);
    if (!start) {
        result.stopped_at = Error::kNoMajorSync;
        return result;
    }
    std::size_t at = *start;
    std::optional<MajorSync> sync;
    while (at < data.size()) {
        auto au = detail::parse_access_unit(data.subspan(at), sync ? &*sync : nullptr);
        if (!au) {
            result.stopped_at = au.error();
            result.stopped_at_offset = at;
            return result;
        }
        if (au->major_sync) {
            sync = au->major_sync;
        }
        AccessUnitInfo info;
        info.offset = at;
        info.size = au->bytes.size();
        info.input_timing = au->header.input_timing;
        info.major_sync = au->major_sync.has_value();
        info.extra_data = !au->extra_data.empty();
        result.units.push_back(info);
        at += info.size;
    }
    return result;
}

std::expected<StreamSummary, Error> probe(std::span<const std::byte> data) {
    const ScanResult walk = scan(data);
    if (walk.units.empty()) {
        return std::unexpected(walk.stopped_at.value_or(Error::kNoMajorSync));
    }
    StreamSummary s;
    auto first = parse_major_sync(data.subspan(walk.units[0].offset + 4));
    if (!first) {
        return std::unexpected(first.error());
    }
    s.major_sync = *first;
    s.access_units = walk.units.size();
    s.stopped_at = walk.stopped_at;
    for (const AccessUnitInfo& u : walk.units) {
        s.major_syncs += u.major_sync ? 1U : 0U;
        s.bytes += u.size;
        s.peak_access_unit_bytes = std::max<std::uint64_t>(s.peak_access_unit_bytes, u.size);
        s.has_extra_data = s.has_extra_data || u.extra_data;
    }
    const auto spa = static_cast<std::uint64_t>(s.major_sync.samples_per_access_unit());
    s.samples = s.access_units * spa;
    const std::uint32_t rate = s.major_sync.sample_rate();
    if (rate > 0) {
        s.duration_seconds = static_cast<double>(s.samples) / rate;
        s.mean_bits_per_second = s.samples > 0 ? s.bytes * 8U * rate / s.samples : 0U;
    }
    // The presentations: the channel count the first access unit decodes to for each request.
    const auto unit = data.subspan(walk.units[0].offset, walk.units[0].size);
    for (const int want : {2, 6, 8, 0}) {
        Decoder decoder({.presentation_channels = want, .verify_lossless_check = false});
        auto decoded = decoder.decode(unit);
        if (decoded && std::find(s.presentations.begin(), s.presentations.end(),
                                 decoded->channels) == s.presentations.end()) {
            s.presentations.push_back(decoded->channels);
        }
    }
    std::sort(s.presentations.begin(), s.presentations.end());
    return s;
}

}  // namespace iclforge::mlp
