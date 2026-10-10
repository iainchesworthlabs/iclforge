#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "iclforge/adm/model.hpp"

// The ADM elements libadm neither parses nor writes: audioBlockFormat's zoneExclusion
// (ITU-R BS.2076-2 §10.4), a Matrix block's outputChannelFormatIDRef, `matrix` and jumpPosition
// (§5.4.3.2), and a Matrix or HOA audioPackFormat's own sub-elements (§5.5.4, §5.5.5). libadm's own
// parser and formatter each carry a "TODO: zoneExclusion" at the point it would go, its Matrix
// block has no parameters for the rest, and its XML tokenizer is private, so this module reads
// (and, for zones, writes) those elements itself, on the axml text, around libadm's own parse and
// write.
//
// None of these functions validates the document. On the read side libadm has already accepted
// the text before a scan runs; on the write side the text is libadm's own output. A scan that
// meets something it cannot tokenize stops and returns what it found so far. Keys are the
// element's ID attribute with ASCII letters upper-cased, which is how libadm prints an ID, so a
// file that spells a hexadecimal digit in lower case still finds its block.
namespace iclforge::adm::detail {

using ZonesByBlockId = std::unordered_map<std::string, std::vector<ExclusionZone>>;

// Every audioBlockFormat that has a non-empty zoneExclusion, keyed by its audioBlockFormatID.
[[nodiscard]] ZonesByBlockId scan_zone_exclusions(std::string_view xml);

// Returns `xml` with a <zoneExclusion> element added to each audioBlockFormat named in `zones`.
// The element goes immediately before the block's <importance> child when there is one, and
// otherwise at the end of the block, which is where libadm's formatter leaves the gap. A block
// written as a self-closing tag cannot take children and is left alone.
[[nodiscard]] std::string inject_zone_exclusions(std::string_view xml, const ZonesByBlockId& zones);

// One audioBlockFormat of a Matrix audioChannelFormat, as written. libadm's parser skips every
// block of a Matrix channel (the loop that would build them is commented out in its
// parseAudioChannelFormat) and its writer has no Matrix block at all, so the block is read here
// whole. `rtime`, `duration`, `gain` and `importance` are the attribute or element text, left
// empty when absent: turning a timecode into seconds is libadm's parseTimecode(), which this file
// stays clear of, so adm_model.cpp does it.
struct MatrixBlockText {
    std::string id;  // audioBlockFormatID, upper-cased
    std::string rtime;
    std::string duration;
    std::string gain;
    std::string gain_unit;  // the <gain> element's gainUnit attribute
    std::string importance;
    std::string output_channel_format_ref;
    std::vector<MatrixCoefficient> matrix;
    bool has_jump_position = false;
    bool jump_position = false;
    bool has_interpolation_length = false;
    double interpolation_length_s = 0.0;
};
using MatrixBlocksByChannelId = std::unordered_map<std::string, std::vector<MatrixBlockText>>;

// The blocks of every audioChannelFormat whose ID carries the Matrix type label (AC_0002xxxx),
// in document order, keyed by the audioChannelFormatID. libadm takes the type from the ID in the
// same way and requires typeLabel and typeDefinition to agree with it.
[[nodiscard]] MatrixBlocksByChannelId scan_matrix_blocks(std::string_view xml);

// What a Matrix or HOA audioPackFormat carries that libadm drops.
struct PackExtras {
    std::vector<std::string> encode_pack_format_refs;
    std::vector<std::string> decode_pack_format_refs;
    std::string input_pack_format_ref;
    std::string output_pack_format_ref;
    std::string hoa_normalization;
    bool has_nfc_ref_dist = false;
    double nfc_ref_dist = 0.0;
    bool screen_ref = false;
};
using PackExtrasById = std::unordered_map<std::string, PackExtras>;

// Every audioPackFormat that names one of the four matrix pack references, or sets normalization,
// nfcRefDist or screenRef, keyed by its audioPackFormatID.
[[nodiscard]] PackExtrasById scan_pack_extras(std::string_view xml);

}  // namespace iclforge::adm::detail
