#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <set>
#include <span>
#include <vector>

#include "iclforge/containers/iamf/model.hpp"
#include "iclforge/containers/iamf/sequence.hpp"
#include "sequence_detail.hpp"

// The OBU writer. Each function below writes one syntax structure of AOM IAMF v2.0.0 in the
// field order the specification lists, named after the class it implements.

namespace iclforge::iamf {

namespace {

using detail::Out;

constexpr std::uint8_t kLoudnessInfoTruePeak = 0x01;
constexpr std::uint8_t kLoudnessInfoAnchored = 0x02;
constexpr std::uint8_t kLoudnessInfoMomentary = 0x08;
constexpr std::uint8_t kLoudnessInfoRange = 0x10;

[[nodiscard]] Error invalid() { return Error::kInvalidArgument; }

// OBU Header and payload: obu_type(5), obu_redundant_copy(1), the type dependent flag(1),
// obu_extension_flag(0), leb128() obu_size, then the trimming fields when they apply.
struct ObuFlags {
    bool redundant = false;
    bool type_flag = false;  // optional_fields_flag, is_not_key_frame or obu_trimming_status_flag
    bool trimming = false;
    std::uint32_t trim_end = 0;
    std::uint32_t trim_start = 0;
};

[[nodiscard]] Bytes make_obu(std::uint32_t obu_type, std::span<const std::byte> payload, const ObuFlags& flags = {}) {
    Out out;
    const bool flag = flags.trimming || flags.type_flag;
    out.u8((obu_type << 3) | (flags.redundant ? 0x04U : 0U) | (flag ? 0x02U : 0U));
    std::size_t size = payload.size();
    if (flags.trimming) {
        size += detail::leb128_size(flags.trim_end) + detail::leb128_size(flags.trim_start);
    }
    out.leb128(static_cast<std::uint32_t>(size));
    if (flags.trimming) {
        out.leb128(flags.trim_end);
        out.leb128(flags.trim_start);
    }
    out.bytes(payload);
    return out.take();
}

void append(Bytes& out, const Bytes& more) { out.insert(out.end(), more.begin(), more.end()); }

[[nodiscard]] bool valid_sample_rate(std::uint32_t rate) {
    return rate == 16000 || rate == 32000 || rate == 44100 || rate == 48000 || rate == 96000;
}

// --- Descriptors -------------------------------------------------------------------------------

[[nodiscard]] std::expected<Bytes, Error> codec_config_payload(const CodecConfig& config) {
    if (config.codec_id.size() != 4 || config.num_samples_per_frame == 0) {
        return std::unexpected(invalid());
    }
    Out out;
    out.leb128(config.codec_config_id);
    out.fourcc(config.codec_id);
    out.leb128(config.num_samples_per_frame);
    out.s16(config.audio_roll_distance);
    if (config.codec_id == "ipcm") {
        if (!config.lpcm.has_value()) {
            return std::unexpected(invalid());
        }
        const LpcmConfig& lpcm = *config.lpcm;
        if ((lpcm.sample_size != 16 && lpcm.sample_size != 24 && lpcm.sample_size != 32) ||
            !valid_sample_rate(lpcm.sample_rate) || lpcm.sample_format_flags > 1) {
            return std::unexpected(invalid());
        }
        out.u8(lpcm.sample_format_flags);
        out.u8(lpcm.sample_size);
        out.u32(lpcm.sample_rate);
    } else {
        out.bytes(config.decoder_config);
    }
    return out.take();
}

[[nodiscard]] std::expected<Bytes, Error> metadata_payload(const Metadata& metadata) {
    Out out;
    out.leb128(metadata.metadata_type);
    if (metadata.metadata_type == 1) {
        out.u8(metadata.itu_t_t35_country_code);
        if (metadata.itu_t_t35_country_code == 0xFF) {
            out.u8(metadata.itu_t_t35_country_code_extension.value_or(0));
        }
        out.bytes(metadata.itu_t_t35_payload);
    } else if (metadata.metadata_type == 2) {
        if (metadata.tags.size() > 255) {
            return std::unexpected(invalid());
        }
        out.u8(static_cast<std::uint32_t>(metadata.tags.size()));
        for (const auto& tag : metadata.tags) {
            out.string(tag.name);
            out.string(tag.value);
        }
    } else {
        out.bytes(metadata.other_bytes);
    }
    return out.take();
}

void put_demixing_definition(Out& out, const DemixingParamDefinition& definition) {
    detail::put_param_definition(out, definition.definition);
    out.u8(static_cast<std::uint32_t>(definition.default_dmixp_mode & 0x07U) << 5);  // dmixp_mode(3) reserved(5)
    out.u8(static_cast<std::uint32_t>(definition.default_w & 0x0FU) << 4);           // default_w(4) reserved(4)
}

[[nodiscard]] std::expected<Bytes, Error> audio_element_payload(const AudioElement& element) {
    if (element.audio_substream_ids.empty()) {
        return std::unexpected(Error::kBadDescriptor);
    }
    Out out;
    out.leb128(element.audio_element_id);
    out.u8((static_cast<std::uint32_t>(element.type) << 5) | (element.reserved_type_bits & 0x1FU));
    out.leb128(element.codec_config_id);
    out.leb128(static_cast<std::uint32_t>(element.audio_substream_ids.size()));
    for (const std::uint32_t id : element.audio_substream_ids) {
        out.leb128(id);
    }

    const std::size_t parameters = (element.demixing.has_value() ? 1U : 0U) +
                                   (element.recon_gain.has_value() ? 1U : 0U) + element.unknown_parameters.size();
    out.leb128(static_cast<std::uint32_t>(parameters));
    if (element.demixing.has_value()) {
        out.leb128(static_cast<std::uint32_t>(ParamType::kDemixing));
        put_demixing_definition(out, *element.demixing);
    }
    if (element.recon_gain.has_value()) {
        out.leb128(static_cast<std::uint32_t>(ParamType::kReconGain));
        detail::put_param_definition(out, element.recon_gain->definition);
    }
    for (const auto& unknown : element.unknown_parameters) {
        out.leb128(unknown.param_definition_type);
        out.leb128(static_cast<std::uint32_t>(unknown.bytes.size()));
        out.bytes(unknown.bytes);
    }

    switch (element.type) {
        case ElementType::kChannelBased: {
            const auto layer_count = element.layers.size();
            if (layer_count == 0 || layer_count > 6) {
                return std::unexpected(Error::kBadDescriptor);
            }
            std::size_t substreams = 0;
            out.u8(static_cast<std::uint32_t>(layer_count) << 5);  // num_layers(3) reserved(5)
            for (std::size_t i = 0; i < layer_count; ++i) {
                const ChannelLayer& layer = element.layers[i];
                if (layer.loudspeaker_layout > 15) {
                    return std::unexpected(Error::kBadDescriptor);
                }
                substreams += layer.substream_count;
                out.u8((static_cast<std::uint32_t>(layer.loudspeaker_layout) << 4) |
                       (layer.output_gain_is_present ? 0x08U : 0U) | (layer.recon_gain_is_present ? 0x04U : 0U));
                out.u8(layer.substream_count);
                out.u8(layer.coupled_substream_count);
                if (layer.output_gain_is_present) {
                    out.u8(static_cast<std::uint32_t>(layer.output_gain_flags & 0x3FU) << 2);
                    out.s16(layer.output_gain);
                }
                if (i == 0 && layer.loudspeaker_layout == 15) {
                    out.u8(layer.expanded_loudspeaker_layout.value_or(0));
                }
            }
            if (substreams != element.audio_substream_ids.size()) {
                return std::unexpected(Error::kBadDescriptor);
            }
            break;
        }
        case ElementType::kSceneBased: {
            const AmbisonicsConfig& config = element.ambisonics;
            out.leb128(config.ambisonics_mode);
            if (config.ambisonics_mode == 0) {
                if (config.channel_mapping.size() != config.output_channel_count) {
                    return std::unexpected(Error::kBadDescriptor);
                }
                out.u8(config.output_channel_count);
                out.u8(config.substream_count);
                for (const std::uint8_t c : config.channel_mapping) {
                    out.u8(c);
                }
            } else if (config.ambisonics_mode == 1) {
                const std::size_t expected = (static_cast<std::size_t>(config.substream_count) +
                                              config.coupled_substream_count) *
                                             config.output_channel_count;
                if (config.demixing_matrix.size() != expected) {
                    return std::unexpected(Error::kBadDescriptor);
                }
                out.u8(config.output_channel_count);
                out.u8(config.substream_count);
                out.u8(config.coupled_substream_count);
                for (const std::int16_t c : config.demixing_matrix) {
                    out.s16(c);
                }
            } else {
                return std::unexpected(Error::kUnsupported);
            }
            if (config.substream_count != element.audio_substream_ids.size()) {
                return std::unexpected(Error::kBadDescriptor);
            }
            break;
        }
        case ElementType::kObjectBased: {
            const ObjectsConfig& config = element.objects;
            if (config.num_objects < 1 || config.num_objects > 2) {
                return std::unexpected(Error::kBadDescriptor);
            }
            out.leb128(static_cast<std::uint32_t>(1 + config.extension_bytes.size()));  // objects_config_size
            out.u8(config.num_objects);
            out.bytes(config.extension_bytes);
            break;
        }
        default:
            return std::unexpected(Error::kUnsupported);
    }
    return out.take();
}

void put_mix_gain_definition(Out& out, const MixGainParamDefinition& definition) {
    detail::put_param_definition(out, definition.definition);
    out.s16(definition.default_mix_gain);
}

void put_element_gain_offset(Out& out, const ElementGainOffset& offset) {
    out.u8(offset.type);
    if (offset.type == 0) {
        out.s16(offset.offset);
    } else if (offset.type == 1) {
        out.s16(offset.offset);
        out.s16(offset.min_offset);
        out.s16(offset.max_offset);
    } else {
        out.leb128(static_cast<std::uint32_t>(offset.unknown_bytes.size()));
        out.bytes(offset.unknown_bytes);
    }
}

void put_rendering_config(Out& out, const RenderingConfig& config) {
    out.u8((static_cast<std::uint32_t>(config.headphones_rendering_mode & 0x03U) << 6) |
           (config.element_gain_offset.has_value() ? 0x20U : 0U) |
           (static_cast<std::uint32_t>(config.binaural_filter_profile & 0x03U) << 3));
    // Everything after rendering_config_extension_size is counted by it, so a reader that only
    // knows the older syntax skips the position parameters and the gain offset with it.
    Out body;
    body.leb128(config.position.has_value() ? 1U : 0U);  // num_parameters
    if (config.position.has_value()) {
        body.leb128(static_cast<std::uint32_t>(config.position->type));
        detail::put_param_definition(body, config.position->definition);
        detail::put_position_defaults(body, *config.position);
    }
    if (config.element_gain_offset.has_value()) {
        put_element_gain_offset(body, *config.element_gain_offset);
    }
    body.bytes(config.extension_bytes);
    out.leb128(static_cast<std::uint32_t>(body.size()));
    out.bytes(body.data());
}

void put_layout(Out& out, const Layout& layout) {
    if (layout.layout_type == 2) {
        out.u8((2U << 6) | (static_cast<std::uint32_t>(layout.sound_system & 0x0FU) << 2));
    } else {
        out.u8(static_cast<std::uint32_t>(layout.layout_type & 0x03U) << 6);
    }
}

[[nodiscard]] std::expected<void, Error> put_loudness_info(Out& out, const LoudnessData& loudness) {
    std::uint8_t info_type = loudness.info_type;
    // The bits that say which optional parts follow are set from the parts present.
    if (loudness.true_peak.has_value()) {
        info_type |= kLoudnessInfoTruePeak;
    }
    if (!loudness.anchored.empty()) {
        info_type |= kLoudnessInfoAnchored;
    }
    if (loudness.momentary.has_value()) {
        info_type |= kLoudnessInfoMomentary;
    }
    if (loudness.loudness_range.has_value()) {
        info_type |= kLoudnessInfoRange;
    }
    out.u8(info_type);
    out.s16(loudness.integrated_loudness);
    out.s16(loudness.digital_peak);
    if ((info_type & kLoudnessInfoTruePeak) != 0) {
        out.s16(loudness.true_peak.value_or(0));
    }
    if ((info_type & kLoudnessInfoAnchored) != 0) {
        if (loudness.anchored.size() > 255) {
            return std::unexpected(invalid());
        }
        out.u8(static_cast<std::uint32_t>(loudness.anchored.size()));
        for (const auto& anchored : loudness.anchored) {
            out.u8(anchored.anchor_element);
            out.s16(anchored.anchored_loudness);
        }
    }
    if ((info_type & 0xFCU) != 0) {
        Out extra;
        if ((info_type & kLoudnessInfoMomentary) != 0) {
            const MomentaryLoudnessInfo& momentary = *loudness.momentary;
            if (momentary.counts.size() != (static_cast<std::size_t>(momentary.num_bin_pairs_minus_one) + 1) * 2) {
                return std::unexpected(invalid());
            }
            detail::put_param_definition(extra, momentary.definition);
            extra.bits(momentary.num_bin_pairs_minus_one & 0x07U, 3);
            extra.bits(momentary.bin_width_minus_one & 0x3FU, 6);
            extra.bits(momentary.first_bin_center & 0x3FU, 6);
            extra.bits(0, 1);
            for (const std::uint32_t count : momentary.counts) {
                extra.leb128(count);
            }
        }
        if ((info_type & kLoudnessInfoRange) != 0) {
            extra.u8(static_cast<std::uint32_t>(loudness.loudness_range.value_or(0) & 0x3FU) << 2);
        }
        extra.bytes(loudness.info_type_bytes);
        out.leb128(static_cast<std::uint32_t>(extra.size()));
        out.bytes(extra.data());
    }
    return {};
}

[[nodiscard]] std::expected<Bytes, Error> mix_presentation_payload(const MixPresentation& mix, bool& optional_fields) {
    const std::size_t labels = mix.annotations_language.size();
    if (mix.localized_presentation_annotations.size() != labels || mix.sub_mixes.empty()) {
        return std::unexpected(invalid());
    }
    Out out;
    out.leb128(mix.mix_presentation_id);
    out.leb128(static_cast<std::uint32_t>(labels));
    for (const auto& language : mix.annotations_language) {
        out.string(language);
    }
    for (const auto& annotation : mix.localized_presentation_annotations) {
        out.string(annotation);
    }
    out.leb128(static_cast<std::uint32_t>(mix.sub_mixes.size()));
    for (const SubMix& sub_mix : mix.sub_mixes) {
        if (sub_mix.elements.empty()) {
            return std::unexpected(invalid());
        }
        out.leb128(static_cast<std::uint32_t>(sub_mix.elements.size()));
        for (const SubMixElement& element : sub_mix.elements) {
            if (element.localized_element_annotations.size() != labels) {
                return std::unexpected(invalid());
            }
            out.leb128(element.audio_element_id);
            for (const auto& annotation : element.localized_element_annotations) {
                out.string(annotation);
            }
            put_rendering_config(out, element.rendering);
            put_mix_gain_definition(out, element.element_mix_gain);
        }
        put_mix_gain_definition(out, sub_mix.output_mix_gain);
        out.leb128(static_cast<std::uint32_t>(sub_mix.layouts.size()));
        for (const SubMixLayout& layout : sub_mix.layouts) {
            put_layout(out, layout.layout);
            if (auto status = put_loudness_info(out, layout.loudness); !status.has_value()) {
                return std::unexpected(status.error());
            }
        }
    }

    optional_fields = mix.preferred_renderers.has_value() || !mix.optional_fields_remaining_bytes.empty();
    if (mix.tags.has_value() || optional_fields) {
        const auto& tags = mix.tags.has_value() ? *mix.tags : std::vector<MixTag>{};
        if (tags.size() > 255) {
            return std::unexpected(invalid());
        }
        out.u8(static_cast<std::uint32_t>(tags.size()));
        for (const auto& tag : tags) {
            out.string(tag.name);
            out.string(tag.value);
        }
    }
    if (optional_fields) {
        const auto [loudspeaker, binaural] = mix.preferred_renderers.value_or(std::pair<std::uint8_t, std::uint8_t>{0, 0});
        out.leb128(static_cast<std::uint32_t>(2 + mix.optional_fields_remaining_bytes.size()));
        out.u8(loudspeaker);
        out.u8(binaural);
        out.bytes(mix.optional_fields_remaining_bytes);
    }
    return out.take();
}

// Checks the cross references the writer's output depends on, so a mistake in a Sequence is an
// error here rather than a file a parser rejects.
[[nodiscard]] std::expected<void, Error> validate(const Sequence& sequence) {
    std::set<std::uint32_t> codec_ids;
    for (const auto& config : sequence.codec_configs) {
        if (!codec_ids.insert(config.codec_config_id).second) {
            return std::unexpected(Error::kBadDescriptor);
        }
    }
    if (sequence.codec_configs.empty() || sequence.audio_elements.empty() || sequence.mix_presentations.empty()) {
        return std::unexpected(Error::kBadDescriptor);
    }

    std::map<std::uint32_t, const AudioElement*> elements;
    std::set<std::uint32_t> substreams;
    for (const auto& element : sequence.audio_elements) {
        if (!elements.try_emplace(element.audio_element_id, &element).second ||
            !codec_ids.contains(element.codec_config_id)) {
            return std::unexpected(Error::kBadDescriptor);
        }
        for (const std::uint32_t id : element.audio_substream_ids) {
            if (!substreams.insert(id).second) {
                return std::unexpected(Error::kBadDescriptor);
            }
        }
    }

    std::set<std::uint32_t> parameter_ids;
    const auto claim = [&parameter_ids](std::uint32_t id) { return parameter_ids.insert(id).second; };
    for (const auto& element : sequence.audio_elements) {
        if (element.demixing.has_value() && !claim(element.demixing->definition.parameter_id)) {
            return std::unexpected(Error::kBadDescriptor);
        }
        if (element.recon_gain.has_value() && !claim(element.recon_gain->definition.parameter_id)) {
            return std::unexpected(Error::kBadDescriptor);
        }
    }
    std::set<std::uint32_t> mix_ids;
    for (const auto& mix : sequence.mix_presentations) {
        if (!mix_ids.insert(mix.mix_presentation_id).second) {
            return std::unexpected(Error::kBadDescriptor);
        }
        for (const auto& sub_mix : mix.sub_mixes) {
            if (!claim(sub_mix.output_mix_gain.definition.parameter_id)) {
                return std::unexpected(Error::kBadDescriptor);
            }
            for (const auto& element : sub_mix.elements) {
                const auto found = elements.find(element.audio_element_id);
                if (found == elements.end() || !claim(element.element_mix_gain.definition.parameter_id)) {
                    return std::unexpected(Error::kBadDescriptor);
                }
                const AudioElement& referenced = *found->second;
                const bool dual = element.rendering.position.has_value() &&
                                  (element.rendering.position->type == ParamType::kDualPolar ||
                                   element.rendering.position->type == ParamType::kDualCart8 ||
                                   element.rendering.position->type == ParamType::kDualCart16);
                if (element.rendering.position.has_value()) {
                    // A position definition belongs to an object-based element, single or dual to
                    // match its num_objects (Rendering Config semantics).
                    if (referenced.type != ElementType::kObjectBased ||
                        dual != (referenced.objects.num_objects == 2) ||
                        detail::position_fields(element.rendering.position->type).empty() ||
                        !claim(element.rendering.position->definition.parameter_id)) {
                        return std::unexpected(Error::kBadDescriptor);
                    }
                }
            }
        }
    }
    return {};
}

// --- Parameter Blocks --------------------------------------------------------------------------

// num_subblocks: implied by the durations when constant_subblock_duration is set, otherwise listed.
[[nodiscard]] std::uint32_t subblock_count(std::uint32_t duration, std::uint32_t constant, std::size_t explicit_count) {
    if (constant != 0) {
        return (duration + constant - 1) / constant;
    }
    return static_cast<std::uint32_t>(explicit_count);
}

[[nodiscard]] std::expected<Bytes, Error> parameter_block_payload(
    const ParameterBlock& block, const std::map<std::uint32_t, detail::ParamInfo>& index) {
    const auto found = index.find(block.parameter_id);
    if (found == index.end()) {
        return std::unexpected(Error::kUnknownParameter);
    }
    const detail::ParamInfo& info = found->second;
    const ParamDefinition& definition = *info.definition;

    Out out;
    out.leb128(block.parameter_id);

    std::uint32_t duration = definition.duration;
    std::uint32_t constant = definition.constant_subblock_duration;
    if (definition.param_definition_mode != 0) {
        duration = block.duration;
        constant = block.constant_subblock_duration;
        if (duration == 0) {
            return std::unexpected(Error::kBadParameterBlock);
        }
        out.leb128(duration);
        out.leb128(constant);
        if (constant == 0) {
            out.leb128(static_cast<std::uint32_t>(block.subblocks.size()));
        }
    }
    const std::size_t explicit_count = definition.param_definition_mode != 0 ? block.subblocks.size()
                                                                            : definition.subblock_durations.size();
    if (block.subblocks.size() != subblock_count(duration, constant, explicit_count)) {
        return std::unexpected(Error::kBadParameterBlock);
    }

    for (const ParameterSubblock& subblock : block.subblocks) {
        if (definition.param_definition_mode != 0 && constant == 0) {
            out.leb128(subblock.subblock_duration);
        }
        switch (info.type) {
            case ParamType::kMixGain: {
                out.leb128(subblock.animation_type);
                if (!detail::known_animation(subblock.animation_type) || subblock.components.size() != 1) {
                    return std::unexpected(Error::kBadParameterBlock);
                }
                detail::put_animated(out, subblock.animation_type, subblock.components[0], 16);
                break;
            }
            case ParamType::kDemixing:
                out.u8(static_cast<std::uint32_t>(subblock.dmixp_mode & 0x07U) << 5);
                break;
            case ParamType::kReconGain: {
                const AudioElement& element = *info.element;
                if (subblock.recon_layers.size() != element.layers.size()) {
                    return std::unexpected(Error::kBadParameterBlock);
                }
                for (std::size_t i = 0; i < element.layers.size(); ++i) {
                    if (!element.layers[i].recon_gain_is_present) {
                        continue;
                    }
                    const ReconLayerData& layer = subblock.recon_layers[i];
                    out.leb128(layer.recon_gain_flags);
                    // Bit j of the flags names a channel; the 7 or 12 low bits (one or two
                    // bytes of leb128) are the flag bits.
                    const unsigned flag_bits = layer.recon_gain_flags < 0x80U ? 7U : 12U;
                    std::size_t next = 0;
                    for (unsigned j = 0; j < flag_bits; ++j) {
                        if (((layer.recon_gain_flags >> j) & 1U) == 0) {
                            continue;
                        }
                        if (next >= layer.recon_gains.size()) {
                            return std::unexpected(Error::kBadParameterBlock);
                        }
                        out.u8(layer.recon_gains[next++]);
                    }
                    if (next != layer.recon_gains.size()) {
                        return std::unexpected(Error::kBadParameterBlock);
                    }
                }
                break;
            }
            case ParamType::kMomentaryLoudness:
                out.u8(static_cast<std::uint32_t>(subblock.momentary_loudness & 0x3FU) << 2);
                break;
            default: {
                const auto fields = detail::position_fields(info.type);
                out.leb128(subblock.animation_type);
                if (!detail::known_animation(subblock.animation_type) || subblock.components.size() != fields.size()) {
                    return std::unexpected(Error::kBadParameterBlock);
                }
                for (std::size_t i = 0; i < fields.size(); ++i) {
                    detail::put_animated(out, subblock.animation_type, subblock.components[i], fields[i].width);
                }
                break;
            }
        }
    }
    return out.take();
}

[[nodiscard]] Bytes metadata_obu(const Bytes& payload) { return make_obu(24, payload); }

}  // namespace

std::expected<Bytes, Error> write_descriptors(const Sequence& sequence, bool redundant_copy) {
    if (auto status = validate(sequence); !status.has_value()) {
        return std::unexpected(status.error());
    }
    ObuFlags flags;
    flags.redundant = redundant_copy;

    Bytes out;
    {
        Out header;
        header.fourcc("iamf");
        header.u8(sequence.header.primary_profile);
        header.u8(sequence.header.additional_profile);
        append(out, make_obu(31, header.data(), flags));
    }
    for (const auto& config : sequence.codec_configs) {
        auto payload = codec_config_payload(config);
        if (!payload.has_value()) {
            return std::unexpected(payload.error());
        }
        append(out, make_obu(0, *payload, flags));
    }
    for (const auto& metadata : sequence.descriptor_metadata) {
        auto payload = metadata_payload(metadata);
        if (!payload.has_value()) {
            return std::unexpected(payload.error());
        }
        append(out, make_obu(24, *payload, flags));
    }
    for (const auto& element : sequence.audio_elements) {
        auto payload = audio_element_payload(element);
        if (!payload.has_value()) {
            return std::unexpected(payload.error());
        }
        append(out, make_obu(1, *payload, flags));
    }
    for (const auto& mix : sequence.mix_presentations) {
        bool optional_fields = false;
        auto payload = mix_presentation_payload(mix, optional_fields);
        if (!payload.has_value()) {
            return std::unexpected(payload.error());
        }
        ObuFlags mix_flags = flags;
        mix_flags.type_flag = optional_fields;
        append(out, make_obu(2, *payload, mix_flags));
    }
    return out;
}

namespace detail {

std::expected<Bytes, Error> write_unit_obus(const std::map<std::uint32_t, ParamInfo>& index, const TemporalUnit& unit,
                                            bool delimiter) {
    Bytes out;
    if (delimiter) {
        ObuFlags flags;
        flags.type_flag = unit.is_not_key_frame;
        append(out, make_obu(4, {}, flags));
    }
    for (const ParameterBlock& block : unit.parameter_blocks) {
        auto payload = parameter_block_payload(block, index);
        if (!payload.has_value()) {
            return std::unexpected(payload.error());
        }
        append(out, make_obu(3, *payload));
    }
    for (const Metadata& metadata : unit.metadata) {
        auto payload = metadata_payload(metadata);
        if (!payload.has_value()) {
            return std::unexpected(payload.error());
        }
        append(out, metadata_obu(*payload));
    }
    for (const AudioFrame& frame : unit.audio_frames) {
        ObuFlags flags;
        flags.trimming = frame.has_trimming;
        flags.trim_end = frame.num_samples_to_trim_at_end;
        flags.trim_start = frame.num_samples_to_trim_at_start;
        if (frame.audio_substream_id <= kMaxImplicitSubstreamId) {
            append(out, make_obu(6 + frame.audio_substream_id, frame.data, flags));
        } else {
            Out payload;
            payload.leb128(frame.audio_substream_id);
            payload.bytes(frame.data);
            append(out, make_obu(5, payload.data(), flags));
        }
    }
    return out;
}

}  // namespace detail

std::expected<Bytes, Error> write_temporal_unit(const Sequence& context, const TemporalUnit& unit) {
    return detail::write_unit_obus(detail::index_parameters(context), unit, unit.has_temporal_delimiter);
}

std::expected<Bytes, Error> write_sequence(const Sequence& sequence) {
    auto out = write_descriptors(sequence);
    if (!out.has_value()) {
        return out;
    }
    const auto index = detail::index_parameters(sequence);
    for (const TemporalUnit& unit : sequence.temporal_units) {
        auto bytes = detail::write_unit_obus(index, unit, unit.has_temporal_delimiter);
        if (!bytes.has_value()) {
            return std::unexpected(bytes.error());
        }
        append(*out, *bytes);
    }
    return out;
}

}  // namespace iclforge::iamf
