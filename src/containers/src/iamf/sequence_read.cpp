#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <span>
#include <utility>
#include <vector>

#include "iclforge/iamf/model.hpp"
#include "iclforge/iamf/sequence.hpp"
#include "sequence_detail.hpp"

// The OBU reader: the inverse of sequence_write.cpp, structure by structure. Lenient where the
// specification tells parsers to ignore what they do not recognise (unknown OBU types, reserved
// values, bytes past the syntax they know) and strict about anything that would make it read past
// an OBU or allocate from an unchecked count.

namespace iclforge::iamf {

namespace {

using detail::Cursor;

template <typename T>
using Parsed = std::expected<T, Error>;
// A structure the specification says a parser ignores when it does not understand it comes back
// as nullopt rather than an error.
template <typename T>
using MaybeParsed = std::expected<std::optional<T>, Error>;

#define IAMF_TRY(var, expr)                \
    auto var = (expr);                     \
    if (!var.has_value()) {                \
        return std::unexpected(var.error()); \
    }

struct ObuView {
    std::uint8_t type = 0;
    bool redundant = false;
    bool type_flag = false;  // optional_fields_flag, is_not_key_frame or obu_trimming_status_flag
    std::uint32_t trim_end = 0;
    std::uint32_t trim_start = 0;
    std::span<const std::byte> payload;
};

[[nodiscard]] bool is_audio_frame_type(std::uint8_t type) { return type >= 5 && type <= 23; }

// OBUHeader() and the OBU's bytes.
[[nodiscard]] Parsed<ObuView> read_obu(Cursor& in) {
    IAMF_TRY(header, in.u8());
    ObuView view;
    view.type = static_cast<std::uint8_t>(*header >> 3);
    view.redundant = (*header & 0x04U) != 0;
    view.type_flag = (*header & 0x02U) != 0;
    const bool extension = (*header & 0x01U) != 0;
    IAMF_TRY(size, in.leb128());
    IAMF_TRY(body_bytes, in.bytes(*size));
    Cursor body(*body_bytes);
    if (is_audio_frame_type(view.type) && view.type_flag) {
        IAMF_TRY(trim_end, body.leb128());
        IAMF_TRY(trim_start, body.leb128());
        view.trim_end = *trim_end;
        view.trim_start = *trim_start;
    }
    if (extension) {
        IAMF_TRY(extension_size, body.leb128());
        IAMF_TRY(skipped, body.bytes(*extension_size));
        (void)skipped;
    }
    view.payload = body.rest();
    return view;
}

[[nodiscard]] Parsed<std::vector<std::string>> read_strings(Cursor& in, std::uint32_t count) {
    if (count > in.remaining()) {
        return std::unexpected(Error::kTruncated);
    }
    std::vector<std::string> out;
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        IAMF_TRY(text, in.string());
        out.push_back(std::move(*text));
    }
    return out;
}

[[nodiscard]] Parsed<std::vector<MixTag>> read_tags(Cursor& in) {
    IAMF_TRY(count, in.u8());
    std::vector<MixTag> tags;
    for (unsigned i = 0; i < *count; ++i) {
        IAMF_TRY(name, in.string());
        IAMF_TRY(value, in.string());
        tags.push_back({std::move(*name), std::move(*value)});
    }
    return tags;
}

// --- Descriptors -------------------------------------------------------------------------------

[[nodiscard]] Parsed<CodecConfig> read_codec_config(std::span<const std::byte> payload) {
    Cursor in(payload);
    CodecConfig config;
    IAMF_TRY(id, in.leb128());
    config.codec_config_id = *id;
    IAMF_TRY(codec, in.bytes(4));
    config.codec_id.assign(reinterpret_cast<const char*>(codec->data()), 4);
    IAMF_TRY(frame_size, in.leb128());
    config.num_samples_per_frame = *frame_size;
    IAMF_TRY(roll, in.s16());
    config.audio_roll_distance = *roll;
    const auto rest = in.rest();
    config.decoder_config.assign(rest.begin(), rest.end());
    if (config.codec_id == "ipcm") {
        IAMF_TRY(flags, in.u8());
        IAMF_TRY(size, in.u8());
        IAMF_TRY(rate, in.u32());
        config.lpcm = LpcmConfig{.sample_format_flags = *flags, .sample_size = *size, .sample_rate = *rate};
        // The syntax ends after sample_rate; anything further is not part of decoder_config.
        config.decoder_config.resize(6);
    }
    return config;
}

[[nodiscard]] Parsed<Metadata> read_metadata(std::span<const std::byte> payload) {
    Cursor in(payload);
    Metadata metadata;
    IAMF_TRY(type, in.leb128());
    metadata.metadata_type = *type;
    if (*type == 1) {
        IAMF_TRY(country, in.u8());
        metadata.itu_t_t35_country_code = *country;
        if (*country == 0xFF) {
            IAMF_TRY(extension, in.u8());
            metadata.itu_t_t35_country_code_extension = *extension;
        }
        const auto rest = in.rest();
        metadata.itu_t_t35_payload.assign(rest.begin(), rest.end());
    } else if (*type == 2) {
        IAMF_TRY(tags, read_tags(in));
        metadata.tags = std::move(*tags);
    } else {
        const auto rest = in.rest();
        metadata.other_bytes.assign(rest.begin(), rest.end());
    }
    return metadata;
}

[[nodiscard]] Parsed<MixGainParamDefinition> read_mix_gain_definition(Cursor& in) {
    MixGainParamDefinition definition;
    IAMF_TRY(base, detail::read_param_definition(in));
    definition.definition = std::move(*base);
    IAMF_TRY(gain, in.s16());
    definition.default_mix_gain = *gain;
    return definition;
}

[[nodiscard]] MaybeParsed<AudioElement> read_audio_element(std::span<const std::byte> payload) {
    Cursor in(payload);
    AudioElement element;
    IAMF_TRY(id, in.leb128());
    element.audio_element_id = *id;
    IAMF_TRY(type_byte, in.u8());
    const unsigned type = *type_byte >> 5;
    element.reserved_type_bits = static_cast<std::uint8_t>(*type_byte & 0x1FU);
    IAMF_TRY(codec_config_id, in.leb128());
    element.codec_config_id = *codec_config_id;
    IAMF_TRY(num_substreams, in.leb128());
    if (*num_substreams == 0 || *num_substreams > in.remaining()) {
        return std::unexpected(*num_substreams == 0 ? Error::kBadDescriptor : Error::kTruncated);
    }
    for (std::uint32_t i = 0; i < *num_substreams; ++i) {
        IAMF_TRY(substream, in.leb128());
        element.audio_substream_ids.push_back(*substream);
    }

    IAMF_TRY(num_parameters, in.leb128());
    if (*num_parameters > in.remaining()) {
        return std::unexpected(Error::kTruncated);
    }
    for (std::uint32_t i = 0; i < *num_parameters; ++i) {
        IAMF_TRY(param_type, in.leb128());
        if (*param_type == static_cast<std::uint32_t>(ParamType::kDemixing)) {
            DemixingParamDefinition demixing;
            IAMF_TRY(base, detail::read_param_definition(in));
            demixing.definition = std::move(*base);
            IAMF_TRY(mode, in.u8());
            IAMF_TRY(weight, in.u8());
            demixing.default_dmixp_mode = static_cast<std::uint8_t>(*mode >> 5);
            demixing.default_w = static_cast<std::uint8_t>(*weight >> 4);
            element.demixing = std::move(demixing);
        } else if (*param_type == static_cast<std::uint32_t>(ParamType::kReconGain)) {
            ReconGainParamDefinition recon;
            IAMF_TRY(base, detail::read_param_definition(in));
            recon.definition = std::move(*base);
            element.recon_gain = std::move(recon);
        } else if (*param_type > 9) {
            IAMF_TRY(size, in.leb128());
            IAMF_TRY(bytes, in.bytes(*size));
            element.unknown_parameters.push_back({*param_type, Bytes(bytes->begin(), bytes->end())});
        } else {
            return std::unexpected(Error::kBadDescriptor);  // no other type may appear in an Audio Element
        }
    }

    switch (type) {
        case 0: {
            element.type = ElementType::kChannelBased;
            IAMF_TRY(layers_byte, in.u8());
            const unsigned layers = *layers_byte >> 5;
            if (layers == 0 || layers > 6) {
                return std::unexpected(Error::kBadDescriptor);
            }
            for (unsigned i = 0; i < layers; ++i) {
                ChannelLayer layer;
                IAMF_TRY(flags, in.u8());
                layer.loudspeaker_layout = static_cast<std::uint8_t>(*flags >> 4);
                layer.output_gain_is_present = (*flags & 0x08U) != 0;
                layer.recon_gain_is_present = (*flags & 0x04U) != 0;
                IAMF_TRY(substreams, in.u8());
                IAMF_TRY(coupled, in.u8());
                layer.substream_count = *substreams;
                layer.coupled_substream_count = *coupled;
                if (layer.output_gain_is_present) {
                    IAMF_TRY(gain_flags, in.u8());
                    IAMF_TRY(gain, in.s16());
                    layer.output_gain_flags = static_cast<std::uint8_t>(*gain_flags >> 2);
                    layer.output_gain = *gain;
                }
                if (i == 0 && layer.loudspeaker_layout == 15) {
                    IAMF_TRY(expanded, in.u8());
                    layer.expanded_loudspeaker_layout = *expanded;
                }
                element.layers.push_back(layer);
            }
            break;
        }
        case 1: {
            element.type = ElementType::kSceneBased;
            AmbisonicsConfig& config = element.ambisonics;
            IAMF_TRY(mode, in.leb128());
            config.ambisonics_mode = *mode;
            IAMF_TRY(channels, in.u8());
            IAMF_TRY(substreams, in.u8());
            config.output_channel_count = *channels;
            config.substream_count = *substreams;
            if (*mode == 0) {
                IAMF_TRY(mapping, in.bytes(*channels));
                for (const std::byte b : *mapping) {
                    config.channel_mapping.push_back(std::to_integer<std::uint8_t>(b));
                }
            } else if (*mode == 1) {
                IAMF_TRY(coupled, in.u8());
                config.coupled_substream_count = *coupled;
                const std::size_t count = (static_cast<std::size_t>(*substreams) + *coupled) * *channels;
                if (count * 2 > in.remaining()) {
                    return std::unexpected(Error::kTruncated);
                }
                for (std::size_t i = 0; i < count; ++i) {
                    IAMF_TRY(value, in.s16());
                    config.demixing_matrix.push_back(*value);
                }
            } else {
                return std::optional<AudioElement>{};
            }
            break;
        }
        case 2: {
            element.type = ElementType::kObjectBased;
            IAMF_TRY(size, in.leb128());
            if (*size == 0) {
                return std::unexpected(Error::kBadDescriptor);
            }
            IAMF_TRY(config_bytes, in.bytes(*size));
            element.objects.num_objects = std::to_integer<std::uint8_t>((*config_bytes)[0]);
            element.objects.extension_bytes.assign(config_bytes->begin() + 1, config_bytes->end());
            break;
        }
        default:
            return std::optional<AudioElement>{};  // an audio_element_type this version does not define
    }
    return std::optional<AudioElement>{std::move(element)};
}

[[nodiscard]] Parsed<ElementGainOffset> read_element_gain_offset(Cursor& in) {
    ElementGainOffset offset;
    IAMF_TRY(type, in.u8());
    offset.type = *type;
    if (*type == 0) {
        IAMF_TRY(value, in.s16());
        offset.offset = *value;
    } else if (*type == 1) {
        IAMF_TRY(value, in.s16());
        IAMF_TRY(min, in.s16());
        IAMF_TRY(max, in.s16());
        offset.offset = *value;
        offset.min_offset = *min;
        offset.max_offset = *max;
    } else {
        IAMF_TRY(size, in.leb128());
        IAMF_TRY(bytes, in.bytes(*size));
        offset.unknown_bytes.assign(bytes->begin(), bytes->end());
    }
    return offset;
}

// RenderingConfig(): nullopt for one the specification says to ignore (a reserved
// headphones_rendering_mode or an unknown parameter definition type).
[[nodiscard]] MaybeParsed<RenderingConfig> read_rendering_config(Cursor& in) {
    RenderingConfig config;
    IAMF_TRY(flags, in.u8());
    config.headphones_rendering_mode = static_cast<std::uint8_t>(*flags >> 6);
    const bool has_offset = (*flags & 0x20U) != 0;
    config.binaural_filter_profile = static_cast<std::uint8_t>((*flags >> 3) & 0x03U);
    IAMF_TRY(extension_size, in.leb128());
    IAMF_TRY(extension_bytes, in.bytes(*extension_size));
    if (config.headphones_rendering_mode == 3) {
        return std::optional<RenderingConfig>{};
    }
    // An extension of size 0 holds nothing, including no num_parameters.
    if (!extension_bytes->empty()) {
        Cursor body(*extension_bytes);
        IAMF_TRY(num_parameters, body.leb128());
        if (*num_parameters > body.remaining()) {
            return std::unexpected(Error::kTruncated);
        }
        for (std::uint32_t i = 0; i < *num_parameters; ++i) {
            IAMF_TRY(param_type, body.leb128());
            const auto type = static_cast<ParamType>(*param_type);
            if (*param_type < 3 || *param_type > 8) {
                return std::optional<RenderingConfig>{};  // unknown type: the OBU is ignored
            }
            IAMF_TRY(position, detail::read_position_definition(type, body));
            config.position = std::move(*position);
        }
        if (has_offset) {
            IAMF_TRY(offset, read_element_gain_offset(body));
            config.element_gain_offset = std::move(*offset);
        }
        const auto rest = body.rest();
        config.extension_bytes.assign(rest.begin(), rest.end());
    }
    return std::optional<RenderingConfig>{std::move(config)};
}

[[nodiscard]] Parsed<LoudnessData> read_loudness_info(Cursor& in) {
    LoudnessData loudness;
    IAMF_TRY(info_type, in.u8());
    loudness.info_type = *info_type;
    IAMF_TRY(integrated, in.s16());
    IAMF_TRY(peak, in.s16());
    loudness.integrated_loudness = *integrated;
    loudness.digital_peak = *peak;
    if ((*info_type & 0x01U) != 0) {
        IAMF_TRY(true_peak, in.s16());
        loudness.true_peak = *true_peak;
    }
    if ((*info_type & 0x02U) != 0) {
        IAMF_TRY(count, in.u8());
        for (unsigned i = 0; i < *count; ++i) {
            IAMF_TRY(anchor, in.u8());
            IAMF_TRY(value, in.s16());
            loudness.anchored.push_back({*anchor, *value});
        }
    }
    if ((*info_type & 0xFCU) != 0) {
        IAMF_TRY(size, in.leb128());
        IAMF_TRY(extra_bytes, in.bytes(*size));
        Cursor extra(*extra_bytes);
        if ((*info_type & 0x08U) != 0) {
            MomentaryLoudnessInfo momentary;
            IAMF_TRY(definition, detail::read_param_definition(extra));
            momentary.definition = std::move(*definition);
            IAMF_TRY(pairs, extra.bits(3));
            IAMF_TRY(width, extra.bits(6));
            IAMF_TRY(first, extra.bits(6));
            IAMF_TRY(reserved, extra.bits(1));
            (void)reserved;
            momentary.num_bin_pairs_minus_one = static_cast<std::uint8_t>(*pairs);
            momentary.bin_width_minus_one = static_cast<std::uint8_t>(*width);
            momentary.first_bin_center = static_cast<std::uint8_t>(*first);
            for (std::uint32_t i = 0; i < (static_cast<std::uint32_t>(*pairs) + 1) * 2; ++i) {
                IAMF_TRY(count, extra.leb128());
                momentary.counts.push_back(*count);
            }
            loudness.momentary = std::move(momentary);
        }
        if ((*info_type & 0x10U) != 0) {
            IAMF_TRY(range, extra.u8());
            loudness.loudness_range = static_cast<std::uint8_t>(*range >> 2);
        }
        const auto rest = extra.rest();
        loudness.info_type_bytes.assign(rest.begin(), rest.end());
    }
    return loudness;
}

[[nodiscard]] MaybeParsed<MixPresentation> read_mix_presentation(std::span<const std::byte> payload,
                                                                  bool optional_fields) {
    Cursor in(payload);
    MixPresentation mix;
    IAMF_TRY(id, in.leb128());
    mix.mix_presentation_id = *id;
    IAMF_TRY(count_label, in.leb128());
    IAMF_TRY(languages, read_strings(in, *count_label));
    IAMF_TRY(annotations, read_strings(in, *count_label));
    mix.annotations_language = std::move(*languages);
    mix.localized_presentation_annotations = std::move(*annotations);

    IAMF_TRY(num_sub_mixes, in.leb128());
    if (*num_sub_mixes == 0 || *num_sub_mixes > in.remaining()) {
        return std::unexpected(*num_sub_mixes == 0 ? Error::kBadDescriptor : Error::kTruncated);
    }
    bool ignore = false;
    for (std::uint32_t i = 0; i < *num_sub_mixes; ++i) {
        SubMix sub_mix;
        IAMF_TRY(num_elements, in.leb128());
        if (*num_elements == 0 || *num_elements > in.remaining()) {
            return std::unexpected(*num_elements == 0 ? Error::kBadDescriptor : Error::kTruncated);
        }
        for (std::uint32_t j = 0; j < *num_elements; ++j) {
            SubMixElement element;
            IAMF_TRY(element_id, in.leb128());
            element.audio_element_id = *element_id;
            IAMF_TRY(element_annotations, read_strings(in, *count_label));
            element.localized_element_annotations = std::move(*element_annotations);
            IAMF_TRY(rendering, read_rendering_config(in));
            if (rendering->has_value()) {
                element.rendering = std::move(**rendering);
            } else {
                ignore = true;
            }
            IAMF_TRY(gain, read_mix_gain_definition(in));
            element.element_mix_gain = std::move(*gain);
            sub_mix.elements.push_back(std::move(element));
        }
        IAMF_TRY(output_gain, read_mix_gain_definition(in));
        sub_mix.output_mix_gain = std::move(*output_gain);
        IAMF_TRY(num_layouts, in.leb128());
        if (*num_layouts > in.remaining()) {
            return std::unexpected(Error::kTruncated);
        }
        for (std::uint32_t j = 0; j < *num_layouts; ++j) {
            SubMixLayout layout;
            IAMF_TRY(layout_byte, in.u8());
            layout.layout.layout_type = static_cast<std::uint8_t>(*layout_byte >> 6);
            layout.layout.sound_system = layout.layout.layout_type == 2 ? static_cast<std::uint8_t>((*layout_byte >> 2) & 0x0FU) : 0;
            IAMF_TRY(loudness, read_loudness_info(in));
            layout.loudness = std::move(*loudness);
            sub_mix.layouts.push_back(std::move(layout));
        }
        mix.sub_mixes.push_back(std::move(sub_mix));
    }

    // MixPresentationTags are present when the OBU holds anything past the sub mixes.
    if (!in.at_end()) {
        IAMF_TRY(tags, read_tags(in));
        mix.tags = std::move(*tags);
    }
    if (optional_fields) {
        IAMF_TRY(size, in.leb128());
        if (*size < 2) {
            return std::unexpected(Error::kBadDescriptor);
        }
        IAMF_TRY(optional_bytes, in.bytes(*size));
        mix.preferred_renderers = std::pair<std::uint8_t, std::uint8_t>{
            std::to_integer<std::uint8_t>((*optional_bytes)[0]), std::to_integer<std::uint8_t>((*optional_bytes)[1])};
        mix.optional_fields_remaining_bytes.assign(optional_bytes->begin() + 2, optional_bytes->end());
    }
    if (ignore) {
        return std::optional<MixPresentation>{};
    }
    return std::optional<MixPresentation>{std::move(mix)};
}

[[nodiscard]] bool is_descriptor_type(std::uint8_t type) { return type == 31 || type == 0 || type == 1 || type == 2; }

// --- IA Data ----------------------------------------------------------------------------------

// ParameterBlockOBU(): nullopt for a block this reader cannot interpret (an unknown parameter_id
// or animation_type), which the specification says parsers ignore.
[[nodiscard]] MaybeParsed<ParameterBlock> read_parameter_block(
    std::span<const std::byte> payload, const std::map<std::uint32_t, detail::ParamInfo>& index) {
    Cursor in(payload);
    ParameterBlock block;
    IAMF_TRY(id, in.leb128());
    block.parameter_id = *id;
    const auto found = index.find(*id);
    if (found == index.end()) {
        return std::optional<ParameterBlock>{};
    }
    const detail::ParamInfo& info = found->second;
    const ParamDefinition& definition = *info.definition;

    std::uint32_t duration = definition.duration;
    std::uint32_t constant = definition.constant_subblock_duration;
    std::uint32_t count = 0;
    if (definition.param_definition_mode != 0) {
        IAMF_TRY(block_duration, in.leb128());
        IAMF_TRY(block_constant, in.leb128());
        duration = *block_duration;
        constant = *block_constant;
        block.duration = duration;
        block.constant_subblock_duration = constant;
        if (constant == 0) {
            IAMF_TRY(block_count, in.leb128());
            count = *block_count;
        }
    } else if (constant == 0) {
        count = static_cast<std::uint32_t>(definition.subblock_durations.size());
    }
    if (constant != 0) {
        if (duration == 0) {
            return std::unexpected(Error::kBadParameterBlock);
        }
        count = static_cast<std::uint32_t>((static_cast<std::uint64_t>(duration) + constant - 1) / constant);
    }
    if (count > 65536) {
        return std::unexpected(Error::kBadParameterBlock);  // far beyond any real block, and bounds the loop below
    }

    for (std::uint32_t i = 0; i < count; ++i) {
        ParameterSubblock subblock;
        if (definition.param_definition_mode != 0 && constant == 0) {
            IAMF_TRY(subblock_duration, in.leb128());
            subblock.subblock_duration = *subblock_duration;
        }
        switch (info.type) {
            case ParamType::kMixGain: {
                IAMF_TRY(animation, in.leb128());
                if (!detail::known_animation(*animation)) {
                    return std::optional<ParameterBlock>{};
                }
                subblock.animation_type = *animation;
                IAMF_TRY(value, detail::read_animated(in, *animation, 16, true));
                subblock.components.push_back(*value);
                break;
            }
            case ParamType::kDemixing: {
                IAMF_TRY(mode, in.u8());
                subblock.dmixp_mode = static_cast<std::uint8_t>(*mode >> 5);
                break;
            }
            case ParamType::kReconGain: {
                for (const ChannelLayer& layer : info.element->layers) {
                    ReconLayerData data;
                    if (layer.recon_gain_is_present) {
                        // The flag byte(s): 7 bits in one byte, or 12 in two, by the continuation bit.
                        IAMF_TRY(first, in.u8());
                        std::uint32_t flags = *first & 0x7FU;
                        unsigned flag_bits = 7;
                        if ((*first & 0x80U) != 0) {
                            IAMF_TRY(second, in.u8());
                            flags |= static_cast<std::uint32_t>(*second & 0x7FU) << 7;
                            flag_bits = 12;
                        }
                        data.recon_gain_flags = flags;
                        for (unsigned j = 0; j < flag_bits; ++j) {
                            if (((flags >> j) & 1U) != 0) {
                                IAMF_TRY(gain, in.u8());
                                data.recon_gains.push_back(*gain);
                            }
                        }
                    }
                    subblock.recon_layers.push_back(std::move(data));
                }
                break;
            }
            case ParamType::kMomentaryLoudness: {
                IAMF_TRY(value, in.u8());
                subblock.momentary_loudness = static_cast<std::uint8_t>(*value >> 2);
                break;
            }
            default: {
                IAMF_TRY(animation, in.leb128());
                if (!detail::known_animation(*animation)) {
                    return std::optional<ParameterBlock>{};
                }
                subblock.animation_type = *animation;
                for (const auto& field : detail::position_fields(info.type)) {
                    IAMF_TRY(value, detail::read_animated(in, *animation, field.width, field.is_signed));
                    subblock.components.push_back(*value);
                }
                in.align();
                break;
            }
        }
        block.subblocks.push_back(std::move(subblock));
    }
    return std::optional<ParameterBlock>{std::move(block)};
}

// Adds one IA Data OBU to `unit`. Descriptor OBUs repeated mid-sequence and Reserved OBUs are
// ignored.
[[nodiscard]] Parsed<void> add_data_obu(const ObuView& obu, const std::map<std::uint32_t, detail::ParamInfo>& index,
                                         TemporalUnit& unit) {
    if (obu.type == 4) {
        unit.has_temporal_delimiter = true;
        unit.is_not_key_frame = obu.type_flag;
    } else if (obu.type == 3) {
        IAMF_TRY(block, read_parameter_block(obu.payload, index));
        if (block->has_value()) {
            unit.parameter_blocks.push_back(std::move(**block));
        }
    } else if (obu.type == 24) {
        IAMF_TRY(metadata, read_metadata(obu.payload));
        unit.metadata.push_back(std::move(*metadata));
    } else if (is_audio_frame_type(obu.type)) {
        AudioFrame frame;
        Cursor in(obu.payload);
        if (obu.type == 5) {
            IAMF_TRY(id, in.leb128());
            frame.audio_substream_id = *id;
        } else {
            frame.audio_substream_id = static_cast<std::uint32_t>(obu.type - 6);
        }
        frame.has_trimming = obu.type_flag;
        frame.num_samples_to_trim_at_end = obu.trim_end;
        frame.num_samples_to_trim_at_start = obu.trim_start;
        const auto rest = in.rest();
        frame.data.assign(rest.begin(), rest.end());
        unit.audio_frames.push_back(std::move(frame));
    }
    return {};
}

}  // namespace

std::expected<Sequence, Error> read_descriptors(std::span<const std::byte> data, std::size_t* consumed) {
    Cursor in(data);
    Sequence sequence;
    bool have_header = false;
    bool have_mix = false;
    std::size_t end = 0;

    while (!in.at_end()) {
        IAMF_TRY(obu, read_obu(in));
        if (!have_header) {
            if (obu->type != 31) {
                return std::unexpected(Error::kNoSequenceHeader);
            }
        } else if (obu->type == 31 && !obu->redundant) {
            break;  // a new IA Sequence begins
        }
        const bool metadata_descriptor = obu->type == 24 && !have_mix;
        if (!is_descriptor_type(obu->type) && !metadata_descriptor && !(obu->type >= 25 && obu->type <= 30)) {
            // The first OBU of IA Data ends the Descriptors; it is not consumed.
            break;
        }
        end = in.position();
        if (obu->redundant && have_header) {
            continue;
        }
        switch (obu->type) {
            case 31: {
                Cursor payload(obu->payload);
                IAMF_TRY(code, payload.bytes(4));
                if (std::to_integer<char>((*code)[0]) != 'i' || std::to_integer<char>((*code)[1]) != 'a' ||
                    std::to_integer<char>((*code)[2]) != 'm' || std::to_integer<char>((*code)[3]) != 'f') {
                    return std::unexpected(Error::kBadSequenceHeader);
                }
                IAMF_TRY(primary, payload.u8());
                IAMF_TRY(additional, payload.u8());
                sequence.header = {*primary, *additional};
                have_header = true;
                break;
            }
            case 0: {
                IAMF_TRY(config, read_codec_config(obu->payload));
                sequence.codec_configs.push_back(std::move(*config));
                break;
            }
            case 24: {
                IAMF_TRY(metadata, read_metadata(obu->payload));
                sequence.descriptor_metadata.push_back(std::move(*metadata));
                break;
            }
            case 1: {
                IAMF_TRY(element, read_audio_element(obu->payload));
                if (element->has_value()) {
                    sequence.audio_elements.push_back(std::move(**element));
                }
                break;
            }
            case 2: {
                IAMF_TRY(mix, read_mix_presentation(obu->payload, obu->type_flag));
                have_mix = true;
                if (mix->has_value()) {
                    sequence.mix_presentations.push_back(std::move(**mix));
                }
                break;
            }
            default:
                break;  // a Reserved OBU
        }
    }
    if (!have_header) {
        return std::unexpected(Error::kNoSequenceHeader);
    }
    if (consumed != nullptr) {
        *consumed = end;
    }
    return sequence;
}

std::expected<TemporalUnit, Error> read_temporal_unit(const Sequence& context, std::span<const std::byte> data) {
    const auto index = detail::index_parameters(context);
    Cursor in(data);
    TemporalUnit unit;
    while (!in.at_end()) {
        IAMF_TRY(obu, read_obu(in));
        IAMF_TRY(status, add_data_obu(*obu, index, unit));
        (void)status;
    }
    return unit;
}

std::expected<Sequence, Error> read_sequence(std::span<const std::byte> data) {
    std::size_t consumed = 0;
    IAMF_TRY(descriptors, read_descriptors(data, &consumed));
    Sequence sequence = std::move(*descriptors);
    const auto index = detail::index_parameters(sequence);

    Cursor in(data.subspan(consumed));
    TemporalUnit unit;
    std::set<std::uint32_t> unit_substreams;
    const auto finish = [&]() {
        const bool has_content = !unit.parameter_blocks.empty() || !unit.metadata.empty() || !unit.audio_frames.empty();
        if (has_content || unit.has_temporal_delimiter) {
            sequence.temporal_units.push_back(std::move(unit));
        }
        unit = TemporalUnit{};
        unit_substreams.clear();
    };

    while (!in.at_end()) {
        IAMF_TRY(obu, read_obu(in));
        if (obu->type == 31 && !obu->redundant) {
            break;  // a non-redundant IA Sequence Header starts the next IA Sequence
        }
        if (is_descriptor_type(obu->type) || (obu->type >= 25 && obu->type <= 30)) {
            continue;  // descriptors repeated mid-sequence, and Reserved OBUs
        }
        if (obu->type == 4) {
            finish();
        } else if (!unit.has_temporal_delimiter) {
            // Without delimiters a unit ends where a frame would repeat a substream, or where
            // parameter or metadata OBUs follow the audio.
            const bool is_frame = is_audio_frame_type(obu->type);
            bool starts_new = false;
            if (is_frame) {
                std::uint32_t substream = static_cast<std::uint32_t>(obu->type - 6);
                if (obu->type == 5) {
                    Cursor peek(obu->payload);
                    IAMF_TRY(id, peek.leb128());
                    substream = *id;
                }
                starts_new = unit_substreams.contains(substream);
                if (!starts_new) {
                    unit_substreams.insert(substream);
                }
            } else {
                starts_new = !unit.audio_frames.empty();
            }
            if (starts_new) {
                finish();
                if (is_frame) {
                    std::uint32_t substream = static_cast<std::uint32_t>(obu->type - 6);
                    if (obu->type == 5) {
                        Cursor peek(obu->payload);
                        IAMF_TRY(id, peek.leb128());
                        substream = *id;
                    }
                    unit_substreams.insert(substream);
                }
            }
        }
        IAMF_TRY(status, add_data_obu(*obu, index, unit));
        (void)status;
    }
    finish();
    return sequence;
}

}  // namespace iclforge::iamf
