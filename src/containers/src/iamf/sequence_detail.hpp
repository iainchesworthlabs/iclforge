#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <span>
#include <vector>

#include "iclforge/iamf/sequence.hpp"
#include "obu_io.hpp"

// Shared by the OBU writer (sequence_write.cpp) and reader (sequence_read.cpp): the OBU header
// fields, parameter definition coding and the parameter index. Internal to src/iamf/src.

namespace iclforge::iamf::detail {

// OBU Header: obu_type(5) obu_redundant_copy(1) <type dependent flag>(1) obu_extension_flag(1).
// The type dependent flag is optional_fields_flag for a Mix Presentation, is_not_key_frame for a
// Temporal Delimiter and obu_trimming_status_flag for an Audio Frame.
inline constexpr std::uint32_t kMaxImplicitSubstreamId = 17;  // OBU_IA_Audio_Frame_ID0..ID17

// A parameter definition found in the Descriptors.
struct ParamInfo {
    ParamType type = ParamType::kMixGain;
    const ParamDefinition* definition = nullptr;
    const AudioElement* element = nullptr;  // the element a demixing or recon gain definition is in
};

// Every parameter_id the Sequence's Audio Elements and Mix Presentations define. The pointers
// refer into `sequence`, which must outlive the map.
[[nodiscard]] std::map<std::uint32_t, ParamInfo> index_parameters(const Sequence& sequence);

// One Temporal Unit's OBUs, with a Temporal Delimiter OBU in front when `delimiter` is set (the
// unit's has_temporal_delimiter is not consulted: an IA Sample never holds one). `index` is
// index_parameters() of the Sequence the unit belongs to.
[[nodiscard]] std::expected<Bytes, Error> write_unit_obus(const std::map<std::uint32_t, ParamInfo>& index,
                                                           const TemporalUnit& unit, bool delimiter);

// ParamDefinition() (parameter_id through the sub block durations).
void put_param_definition(Out& out, const ParamDefinition& definition);
[[nodiscard]] std::expected<ParamDefinition, Error> read_param_definition(Cursor& in);

// The coded values per object of a position definition: azimuth, elevation, distance (polar) or x, y,
// z. Each entry is the field's bit width and whether it is signed.
struct FieldCoding {
    unsigned width;
    bool is_signed;
};
[[nodiscard]] std::vector<FieldCoding> position_fields(ParamType type);

// A position parameter definition's defaults, packed as its syntax lays them out.
void put_position_defaults(Out& out, const PositionParamDefinition& definition);
[[nodiscard]] std::expected<PositionParamDefinition, Error> read_position_definition(ParamType type, Cursor& in);

// AnimatedParameterData<T> for one component.
void put_animated(Out& out, std::uint32_t animation_type, const AnimatedValue& value, unsigned width);
[[nodiscard]] std::expected<AnimatedValue, Error> read_animated(Cursor& in, std::uint32_t animation_type,
                                                                unsigned width, bool is_signed);
[[nodiscard]] bool known_animation(std::uint32_t animation_type);

}  // namespace iclforge::iamf::detail
