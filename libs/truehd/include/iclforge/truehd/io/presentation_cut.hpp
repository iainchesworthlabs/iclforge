#pragma once

#include <cstddef>
#include <expected>
#include <span>
#include <vector>

#include "iclforge/truehd/core/error.hpp"
#include "iclforge/truehd/export.hpp"

// A stream cut down to a presentation (TH spec §2.2): substream 0 is the 2 channel presentation,
// substream 1 builds on it for 6 channels, substream 2 for 8, so a sender that knows a receiver
// decodes fewer channels can leave the later substreams out. The bytes it saves are the ones the
// receiver would decode and drop: 19 to 29% of a stream for the 2 channel presentation of a
// 5.1 or 7.1 stream with downmix substreams.

namespace iclforge::truehd {

// The access unit that starts at `unit[0]` with only its first `keep` substreams: the header and the
// directory rewritten for them (and the major sync, when the unit has one, for `keep` substreams,
// with its CRC), the segments of those substreams as they are, and no extra data. A unit that
// already has `keep` substreams or fewer comes back as it is. A decoder of the result with
// Presentation::kBest makes what a decoder of the whole stream makes with the presentation `keep`
// substreams stand for.
//
// `inherited_substreams` is what parse_access_unit takes: the count to expect when the unit has no
// major sync of its own. `keep` is 1 to 4; a unit with a major sync that carries the sixteen-channel
// extension, which the major sync writer does not write, is kUnsupported.
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT std::expected<std::vector<std::byte>, Error> keep_substreams(
    std::span<const std::byte> unit, int keep, int inherited_substreams);

}  // namespace iclforge::truehd
