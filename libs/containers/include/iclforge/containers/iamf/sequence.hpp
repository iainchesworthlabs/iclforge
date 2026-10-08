#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "iclforge/containers/export.hpp"
#include "iclforge/containers/iamf/model.hpp"

// Reading and writing an IA Sequence as OBUs (AOM IAMF v2.0.0, "Open Bitstream Unit (OBU) Syntax
// and Semantics" and "Standalone IAMF Representation").
//
// A standalone IA Sequence is the Descriptor OBUs (IA Sequence Header, Codec Configs, optional
// Metadata, Audio Elements, Mix Presentations) followed by the IA Data of each Temporal Unit.
// write_sequence() and read_sequence() are that raw OBU stream. The same OBUs, split at the
// Descriptors, are what an ISO-BMFF file holds: container.hpp wraps write_descriptors() in its
// iacb box and each write_temporal_unit() in an IA Sample.
//
// Parameter Block OBUs cannot be read or written without the parameter definitions of the
// Audio Elements and Mix Presentations they refer to (their syntax depends on the definition's
// type and timing mode), so the functions that handle Temporal Units take the Sequence whose
// Descriptors define them as `context`.

namespace iclforge::containers::iamf {

enum class Error : std::uint8_t {
    kTruncated,           // the data ended inside an OBU, box or field
    kBadLeb128,           // a leb128 value used more than 8 bytes or exceeded 32 bits
    kBadObu,              // an OBU header or size is inconsistent
    kNoSequenceHeader,    // the stream does not start with an IA Sequence Header OBU
    kBadSequenceHeader,   // ia_code is not "iamf"
    kBadDescriptor,       // a Descriptor field is out of range or contradicts another
    kUnknownParameter,    // a Parameter Block refers to a parameter_id no definition declares
    kBadParameterBlock,   // a Parameter Block's timing or data is inconsistent with its definition
    kNotIsobmff,          // the data is not an ISO-BMFF file
    kNotIamf,             // an ISO-BMFF file with no IA track
    kBadBox,              // an ISO-BMFF box is inconsistent
    kInvalidArgument,     // writing: a value does not fit the field that carries it
    kUnsupported,         // a construct this module does not read or write
};

[[nodiscard]] ICLFORGE_CONTAINERS_EXPORT std::string_view describe(Error error);

// --- Writing ------------------------------------------------------------------------------------

// The Descriptor OBUs, in the order the specification requires: IA Sequence Header, all Codec
// Config OBUs, any Metadata OBUs, all Audio Element OBUs, all Mix Presentation OBUs. Set
// `redundant_copy` to write them as a repeat of an earlier set (obu_redundant_copy = 1).
[[nodiscard]] ICLFORGE_CONTAINERS_EXPORT std::expected<Bytes, Error> write_descriptors(const Sequence& sequence,
                                                                                  bool redundant_copy = false);

// One Temporal Unit: a Temporal Delimiter OBU when `unit.has_temporal_delimiter` is set (carrying
// is_not_key_frame), then Parameter Block and Metadata OBUs, then the Audio Frame OBUs. Audio
// Frames of the first 18 substreams use the compact OBU_IA_Audio_Frame_ID0..17 types; others
// carry an explicit substream id. `context` supplies the parameter definitions.
[[nodiscard]] ICLFORGE_CONTAINERS_EXPORT std::expected<Bytes, Error> write_temporal_unit(const Sequence& context,
                                                                                    const TemporalUnit& unit);

// The standalone IA Sequence: write_descriptors() followed by every Temporal Unit.
[[nodiscard]] ICLFORGE_CONTAINERS_EXPORT std::expected<Bytes, Error> write_sequence(const Sequence& sequence);

// --- Reading ------------------------------------------------------------------------------------

// Reads Descriptor OBUs (a standalone stream's start, or an `iacb` box's configOBUs) into a
// Sequence with no Temporal Units. Redundant copies of an OBU already read are skipped; Reserved
// OBUs and OBU types this module does not know are ignored. `consumed`, if given, receives the
// number of bytes read: the first OBU that is not a Descriptor ends the Descriptors.
[[nodiscard]] ICLFORGE_CONTAINERS_EXPORT std::expected<Sequence, Error> read_descriptors(
    std::span<const std::byte> data, std::size_t* consumed = nullptr);

// Reads the OBUs of one Temporal Unit (an IA Sample's bytes, or the OBUs between two Temporal
// Delimiters) using the parameter definitions in `context`. Parameter Blocks of a parameter_id
// no definition declares are dropped, as the specification says parsers should.
[[nodiscard]] ICLFORGE_CONTAINERS_EXPORT std::expected<TemporalUnit, Error> read_temporal_unit(
    const Sequence& context, std::span<const std::byte> data);

// Reads a standalone IA Sequence. Temporal Units are split at Temporal Delimiter OBUs; a stream
// without them is split where the next Parameter Block, Metadata or Audio Frame OBU would repeat
// an Audio Substream already in the unit, or follow an Audio Frame.
[[nodiscard]] ICLFORGE_CONTAINERS_EXPORT std::expected<Sequence, Error> read_sequence(std::span<const std::byte> data);

// --- Helpers ------------------------------------------------------------------------------------

// The ParamType of a parameter_id declared in `sequence`'s Audio Elements and Mix Presentations.
[[nodiscard]] ICLFORGE_CONTAINERS_EXPORT std::optional<ParamType> parameter_type(const Sequence& sequence,
                                                                            std::uint32_t parameter_id);

// The channel layout a loudspeaker_layout value names, as the substreams of a single layer in
// the order the Audio Element lists them: coupled stereo substreams first, then mono ones. Each
// entry names the channel(s) it carries. nullopt for values the specification reserves and for
// the expanded layouts (15), which have their own table.
struct SubstreamChannels {
    std::string_view first;
    std::string_view second;  // empty for a mono substream
};
struct LayoutInfo {
    std::string_view name;
    std::vector<SubstreamChannels> substreams;
    std::uint8_t coupled_substream_count = 0;
    std::uint8_t channel_count = 0;
    std::uint8_t sound_system = 0;  // the Layout() sound_system value for the same layout
};
[[nodiscard]] ICLFORGE_CONTAINERS_EXPORT std::optional<LayoutInfo> layout_info(std::uint8_t loudspeaker_layout);

}  // namespace iclforge::containers::iamf
