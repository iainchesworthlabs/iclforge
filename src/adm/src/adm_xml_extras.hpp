#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "iclforge/adm/model.hpp"

// The one ADM element libadm neither parses nor writes: audioBlockFormat's zoneExclusion
// (ITU-R BS.2076-2 §10.4). libadm's own parser and formatter each carry a "TODO: zoneExclusion"
// at the point it would go, and its XML tokenizer is private, so this module reads and writes
// that one element itself, on the axml text, around libadm's own parse and write.
//
// Neither function validates the document. On the read side libadm has already accepted the
// text before the scan runs; on the write side the text is libadm's own output. A scan that
// meets something it cannot tokenize stops and returns what it found so far.
namespace iclforge::adm::detail {

using ZonesByBlockId = std::unordered_map<std::string, std::vector<ExclusionZone>>;

// Every audioBlockFormat that has a non-empty zoneExclusion, keyed by its audioBlockFormatID.
[[nodiscard]] ZonesByBlockId scan_zone_exclusions(std::string_view xml);

// Returns `xml` with a <zoneExclusion> element added to each audioBlockFormat named in `zones`.
// The element goes immediately before the block's <importance> child when there is one, and
// otherwise at the end of the block, which is where libadm's formatter leaves the gap. A block
// written as a self-closing tag cannot take children and is left alone.
[[nodiscard]] std::string inject_zone_exclusions(std::string_view xml, const ZonesByBlockId& zones);

}  // namespace iclforge::adm::detail
