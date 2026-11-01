#include "iclforge/truehd/io/presentation_cut.hpp"

#include <algorithm>
#include <optional>

#include "iclforge/truehd/io/access_unit.hpp"
#include "iclforge/truehd/io/major_sync.hpp"

namespace iclforge::truehd {

std::expected<std::vector<std::byte>, Error> keep_substreams(std::span<const std::byte> unit,
                                                             int keep, int inherited_substreams) {
    if (keep < 1 || keep > kMaxSubstreams) {
        return std::unexpected(Error{ErrorKind::kBadParameter, "a unit keeps 1 to 4 substreams"});
    }
    const std::expected<AccessUnit, Error> parsed = parse_access_unit(unit, inherited_substreams);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    const AccessUnit& au = *parsed;
    if (au.num_substreams <= keep) {
        return std::vector<std::byte>(unit.begin(), unit.begin() + static_cast<std::ptrdiff_t>(au.length_bytes()));
    }

    std::optional<MajorSync> major_sync = au.major_sync;
    if (major_sync) {
        major_sync->num_substreams = static_cast<std::uint8_t>(keep);
        // The extension bits describe a substream the cut stream no longer has.
        major_sync->extended_substream_info = 0;
        // Written by write_access_unit_prefix(); a major sync it cannot write (the sixteen-channel
        // extension) is refused there.
    }

    std::size_t segment_bytes = 0;
    for (int i = 0; i < keep; ++i) {
        segment_bytes += au.segments[static_cast<std::size_t>(i)].size();
    }
    const std::span<const SubstreamEntry> entries(au.entries.data(), static_cast<std::size_t>(keep));
    // The prefix's size does not depend on the total it is told, so the first write finds it.
    const std::expected<std::vector<std::byte>, Error> sized =
        write_access_unit_prefix(major_sync, entries, au.input_timing, 4);
    if (!sized) {
        return std::unexpected(sized.error());
    }
    std::expected<std::vector<std::byte>, Error> prefix =
        write_access_unit_prefix(major_sync, entries, au.input_timing, sized->size() + segment_bytes);
    if (!prefix) {
        return std::unexpected(prefix.error());
    }
    std::vector<std::byte> out = std::move(*prefix);
    for (int i = 0; i < keep; ++i) {
        const auto segment = au.segments[static_cast<std::size_t>(i)];
        out.insert(out.end(), segment.begin(), segment.end());
    }
    return out;
}

}  // namespace iclforge::truehd
