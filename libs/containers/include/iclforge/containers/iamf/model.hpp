#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "iclforge/containers/export.hpp"

// The IAMF element graph, per the AOM Immersive Audio Model and Formats specification v2.0.0
// (https://aomediacodec.github.io/iamf/, the version that adds object-based audio elements).
// Structures are cited by the name the specification gives them.
//
// A Sequence is an IA Sequence: the Descriptors (an IA Sequence Header, Codec Configs,
// Audio Elements, Mix Presentations and optional Metadata) followed by IA Data, held as Temporal
// Units of Audio Frame, Parameter Block and Metadata OBUs. It is plain data. sequence.hpp turns
// it into OBU bytes and back, and container.hpp into and out of an ISO-BMFF file.
//
// Values are kept as the bitstream carries them: Q7.8 gains as raw 16-bit integers, positions as
// their coded integers. The helper functions at the end convert the ones a caller usually wants as
// decibels or normalized coordinates.

namespace iclforge::containers::iamf {

using Bytes = std::vector<std::byte>;

// OBU types (OBU Header, obu_type). Audio frames are 5 (explicit substream id) and 6-23
// (implicit ids 0-17); the model does not keep the distinction, see AudioFrame.
enum class ObuType : std::uint8_t {
    kCodecConfig = 0,
    kAudioElement = 1,
    kMixPresentation = 2,
    kParameterBlock = 3,
    kTemporalDelimiter = 4,
    kAudioFrame = 5,
    kAudioFrameId0 = 6,
    kMetadata = 24,
    kSequenceHeader = 31,
};

// Profile values (IA Sequence Header OBU).
enum class Profile : std::uint8_t {
    kSimple = 0,
    kBase = 1,
    kBaseEnhanced = 2,
    kBaseAdvanced = 3,
    kAdvanced1 = 4,
    kAdvanced2 = 5,
};

struct SequenceHeader {
    std::uint8_t primary_profile = static_cast<std::uint8_t>(Profile::kSimple);
    std::uint8_t additional_profile = static_cast<std::uint8_t>(Profile::kSimple);
};

// LPCM decoder configuration (Codec Specific, LPCM).
struct LpcmConfig {
    std::uint8_t sample_format_flags = 0x01;  // bit 0: 1 little-endian, 0 big-endian
    std::uint8_t sample_size = 24;            // 16, 24 or 32
    std::uint32_t sample_rate = 48000;        // 16000, 32000, 44100, 48000 or 96000
};

// Codec Config OBU. `decoder_config` holds the DecoderConfig() bytes of any codec; for
// "ipcm" `lpcm` is parsed from them as well and is what the writer serializes.
struct CodecConfig {
    std::uint32_t codec_config_id = 0;
    std::string codec_id = "ipcm";             // four characters: ipcm, Opus, mp4a or fLaC
    std::uint32_t num_samples_per_frame = 1024;
    std::int16_t audio_roll_distance = 0;
    std::optional<LpcmConfig> lpcm;
    Bytes decoder_config;
};

// Parameter Definition Types.
enum class ParamType : std::uint8_t {
    kMixGain = 0,
    kDemixing = 1,
    kReconGain = 2,
    kPolar = 3,
    kCart8 = 4,
    kCart16 = 5,
    kDualPolar = 6,
    kDualCart8 = 7,
    kDualCart16 = 8,
    kMomentaryLoudness = 9,
};

// ParamDefinition(). With param_definition_mode 1 the timing fields are carried by each
// Parameter Block instead and `duration` and the sub block fields here are unused.
struct ParamDefinition {
    std::uint32_t parameter_id = 0;
    std::uint32_t parameter_rate = 48000;       // ticks per second
    std::uint8_t param_definition_mode = 0;     // 0: this definition carries the timing
    std::uint32_t duration = 0;                 // in ticks
    std::uint32_t constant_subblock_duration = 0;
    std::vector<std::uint32_t> subblock_durations;  // when constant_subblock_duration is 0
};

// DemixingParamDefinition() with DefaultDemixingInfoParameterData(): the default dmixp_mode and default_w.
struct DemixingParamDefinition {
    ParamDefinition definition;
    std::uint8_t default_dmixp_mode = 0;  // 3 bits
    std::uint8_t default_w = 0;           // 4 bits
};

struct ReconGainParamDefinition {
    ParamDefinition definition;
};

struct MixGainParamDefinition {
    ParamDefinition definition;
    std::int16_t default_mix_gain = 0;  // Q7.8 dB
};

// The position parameter definitions (Polar, Cartesian 8-bit, Cartesian 16-bit and their dual forms). `type` is one of kPolar, kCart8, kCart16,
// kDualPolar, kDualCart8 and kDualCart16; `defaults` holds the coded default values in the order
// the syntax lists them: azimuth, elevation, distance (polar) or x, y, z (Cartesian), twice for
// the dual types.
struct PositionParamDefinition {
    ParamType type = ParamType::kPolar;
    ParamDefinition definition;
    std::vector<std::int32_t> defaults;
};

// An Audio Element parameter definition of a type this model does not know (param_definition_type
// above 9): kept as its size-prefixed bytes so it survives a read and write.
struct UnknownParamDefinition {
    std::uint32_t param_definition_type = 0;
    Bytes bytes;
};

// ChannelAudioLayerConfig().
struct ChannelLayer {
    std::uint8_t loudspeaker_layout = 7;
    bool output_gain_is_present = false;
    bool recon_gain_is_present = false;
    std::uint8_t substream_count = 0;
    std::uint8_t coupled_substream_count = 0;
    std::uint8_t output_gain_flags = 0;  // 6 bits, when output_gain_is_present
    std::int16_t output_gain = 0;        // Q7.8 dB, when output_gain_is_present
    std::optional<std::uint8_t> expanded_loudspeaker_layout;  // first layer, layout 15 only
};

// AmbisonicsConfig().
struct AmbisonicsConfig {
    std::uint32_t ambisonics_mode = 0;  // 0 mono, 1 projection
    std::uint8_t output_channel_count = 0;
    std::uint8_t substream_count = 0;
    std::uint8_t coupled_substream_count = 0;          // projection mode
    std::vector<std::uint8_t> channel_mapping;         // mono mode, one per output channel
    std::vector<std::int16_t> demixing_matrix;         // projection mode, (N + M) x C
};

// ObjectsConfig(): the objects one Audio Substream carries, 1 (mono) or 2 (stereo).
struct ObjectsConfig {
    std::uint8_t num_objects = 1;
    Bytes extension_bytes;
};

enum class ElementType : std::uint8_t { kChannelBased = 0, kSceneBased = 1, kObjectBased = 2 };

// Audio Element OBU.
struct AudioElement {
    std::uint32_t audio_element_id = 0;
    ElementType type = ElementType::kChannelBased;
    std::uint8_t reserved_type_bits = 0;  // the 5 reserved bits after the 3-bit type
    std::uint32_t codec_config_id = 0;
    std::vector<std::uint32_t> audio_substream_ids;
    std::optional<DemixingParamDefinition> demixing;
    std::optional<ReconGainParamDefinition> recon_gain;
    std::vector<UnknownParamDefinition> unknown_parameters;
    std::vector<ChannelLayer> layers;  // kChannelBased
    AmbisonicsConfig ambisonics;       // kSceneBased
    ObjectsConfig objects;             // kObjectBased
};

// ElementGainOffsetConfig().
struct ElementGainOffset {
    std::uint8_t type = 0;  // 0 value, 1 range
    std::int16_t offset = 0;          // value; default_element_gain_offset for a range
    std::int16_t min_offset = 0;      // range only
    std::int16_t max_offset = 0;      // range only
    Bytes unknown_bytes;              // any other type
};

// RenderingConfig().
struct RenderingConfig {
    std::uint8_t headphones_rendering_mode = 0;
    std::uint8_t binaural_filter_profile = 0;
    std::optional<PositionParamDefinition> position;       // object-based elements only
    std::optional<ElementGainOffset> element_gain_offset;
    Bytes extension_bytes;
};

// One Audio Element in a sub-mix of a Mix Presentation OBU.
struct SubMixElement {
    std::uint32_t audio_element_id = 0;
    std::vector<std::string> localized_element_annotations;  // one per mix label
    RenderingConfig rendering;
    MixGainParamDefinition element_mix_gain;
};

// Layout().
struct Layout {
    std::uint8_t layout_type = 2;  // 2 loudspeakers (sound system convention), 3 binaural
    std::uint8_t sound_system = 0;
};

// LoudnessInfo(). Gains and loudness values are Q7.8.
struct AnchoredLoudness {
    std::uint8_t anchor_element = 0;
    std::int16_t anchored_loudness = 0;
};

struct MomentaryLoudnessInfo {
    ParamDefinition definition;
    std::uint8_t num_bin_pairs_minus_one = 0;         // 3 bits
    std::uint8_t bin_width_minus_one = 0;             // 6 bits
    std::uint8_t first_bin_center = 0;                // 6 bits
    std::vector<std::uint32_t> counts;                // (num_bin_pairs_minus_one + 1) * 2
};

struct LoudnessData {
    std::uint8_t info_type = 0;  // bit 0 true peak, 1 anchored, 2 live, 3 momentary, 4 range
    std::int16_t integrated_loudness = 0;
    std::int16_t digital_peak = 0;
    std::optional<std::int16_t> true_peak;
    std::vector<AnchoredLoudness> anchored;
    std::optional<MomentaryLoudnessInfo> momentary;
    std::optional<std::uint8_t> loudness_range;  // 6 bits
    Bytes info_type_bytes;                       // reserved bytes after the known fields
};

struct SubMixLayout {
    Layout layout;
    LoudnessData loudness;
};

struct SubMix {
    std::vector<SubMixElement> elements;
    MixGainParamDefinition output_mix_gain;
    std::vector<SubMixLayout> layouts;
};

struct MixTag {
    std::string name;
    std::string value;
};

// Mix Presentation OBU.
struct MixPresentation {
    std::uint32_t mix_presentation_id = 0;
    std::vector<std::string> annotations_language;
    std::vector<std::string> localized_presentation_annotations;
    std::vector<SubMix> sub_mixes;
    std::optional<std::vector<MixTag>> tags;  // MixPresentationTags(); absent when not written
    // The optional fields (optional_fields_flag): preferred renderers and trailing bytes.
    std::optional<std::pair<std::uint8_t, std::uint8_t>> preferred_renderers;
    Bytes optional_fields_remaining_bytes;
};

// Metadata OBU.
struct Metadata {
    std::uint32_t metadata_type = 0;  // 1 ITU-T T.35, 2 IAMF tags
    std::uint8_t itu_t_t35_country_code = 0;
    std::optional<std::uint8_t> itu_t_t35_country_code_extension;
    Bytes itu_t_t35_payload;
    std::vector<MixTag> tags;  // metadata_type 2: iamf_encoder and others
    Bytes other_bytes;         // any other type
};

// AnimatedParameterData<T>: one component's values. The fields a type uses are set;
// the rest stay 0. Values are the coded integers.
struct AnimatedValue {
    std::int32_t start = 0;
    std::int32_t end = 0;
    std::int32_t control = 0;
    std::uint8_t control_relative_time = 0;  // Q0.8 fraction of the sub block, BEZIER types
};

enum class Animation : std::uint8_t { kStep = 0, kLinear = 1, kBezier = 2, kInterLinear = 3, kInterBezier = 4 };

// One sub block of a Parameter Block OBU. Which fields apply follows the parameter's type:
// mix gain and the position types use `animation` and `components` (1 for mix gain, 3 for single
// object positions, 6 for dual); demixing uses `dmixp_mode`; recon gain `recon_layers`; momentary
// loudness `momentary_loudness`.
struct ReconLayerData {
    std::uint32_t recon_gain_flags = 0;           // leb128 value, bit j set when channel j has a gain
    std::vector<std::uint8_t> recon_gains;        // one per set flag, in order
};

struct ParameterSubblock {
    std::uint32_t subblock_duration = 0;  // written when the block has constant_subblock_duration 0
    std::uint32_t animation_type = 0;
    std::vector<AnimatedValue> components;
    std::uint8_t dmixp_mode = 0;
    std::vector<ReconLayerData> recon_layers;
    std::uint8_t momentary_loudness = 0;  // 6 bits
};

// Parameter Block OBU. `type` is looked up from the definition when reading and must be
// set when writing from a definition the Sequence holds.
struct ParameterBlock {
    std::uint32_t parameter_id = 0;
    // Timing, carried only when the definition has param_definition_mode 1.
    std::uint32_t duration = 0;
    std::uint32_t constant_subblock_duration = 0;
    std::vector<ParameterSubblock> subblocks;
};

// Audio Frame OBU.
struct AudioFrame {
    std::uint32_t audio_substream_id = 0;
    bool has_trimming = false;  // obu_trimming_status_flag
    std::uint32_t num_samples_to_trim_at_end = 0;
    std::uint32_t num_samples_to_trim_at_start = 0;
    Bytes data;                 // the codec's audio_frame bytes
};

// Temporal Unit.
struct TemporalUnit {
    bool has_temporal_delimiter = false;
    bool is_not_key_frame = false;  // the delimiter's flag; also set from a non-sync sample
    std::vector<ParameterBlock> parameter_blocks;
    std::vector<Metadata> metadata;
    std::vector<AudioFrame> audio_frames;
};

// IA Sequence.
struct Sequence {
    SequenceHeader header;
    std::vector<CodecConfig> codec_configs;
    std::vector<Metadata> descriptor_metadata;  // after the header, before the first Mix Presentation
    std::vector<AudioElement> audio_elements;
    std::vector<MixPresentation> mix_presentations;
    std::vector<TemporalUnit> temporal_units;
};

// Q7.8 fixed point to and from decibels or a plain ratio.
[[nodiscard]] constexpr double q7_8_to_double(std::int16_t value) { return static_cast<double>(value) / 256.0; }
[[nodiscard]] ICLFORGE_CONTAINERS_EXPORT std::int16_t double_to_q7_8(double value);

// Position codings to normalized coordinates: polar azimuth and elevation in degrees and
// distance as value / 127; Cartesian axes as value / 127 (8 bit) or value / 32767 (16 bit).
[[nodiscard]] constexpr double normalized_distance(std::int32_t coded) { return static_cast<double>(coded) / 127.0; }
[[nodiscard]] constexpr double normalized_cart8(std::int32_t coded) { return static_cast<double>(coded) / 127.0; }
[[nodiscard]] constexpr double normalized_cart16(std::int32_t coded) { return static_cast<double>(coded) / 32767.0; }

}  // namespace iclforge::containers::iamf
