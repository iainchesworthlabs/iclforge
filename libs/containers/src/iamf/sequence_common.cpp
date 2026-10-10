#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/containers/iamf/model.hpp"
#include "iclforge/containers/iamf/sequence.hpp"
#include "sequence_detail.hpp"

namespace iclforge::containers::iamf {

std::string_view describe(Error error) {
    switch (error) {
        case Error::kTruncated:
            return "the data ended inside an OBU, box or field";
        case Error::kBadLeb128:
            return "a leb128 value used more than 8 bytes or exceeded 32 bits";
        case Error::kBadObu:
            return "an OBU header or size is inconsistent";
        case Error::kNoSequenceHeader:
            return "the stream does not start with an IA Sequence Header OBU";
        case Error::kBadSequenceHeader:
            return "the IA Sequence Header's ia_code is not 'iamf'";
        case Error::kBadDescriptor:
            return "a Descriptor field is out of range or contradicts another";
        case Error::kUnknownParameter:
            return "a Parameter Block refers to a parameter_id no definition declares";
        case Error::kBadParameterBlock:
            return "a Parameter Block's timing or data is inconsistent with its definition";
        case Error::kNotIsobmff:
            return "the data is not an ISO-BMFF file";
        case Error::kNotIamf:
            return "the ISO-BMFF file has no IA track";
        case Error::kBadBox:
            return "an ISO-BMFF box is inconsistent";
        case Error::kInvalidArgument:
            return "a value does not fit the field that carries it";
        case Error::kUnsupported:
            return "a construct this module does not read or write";
    }
    return "unknown iclforge::iamf error";
}

std::optional<std::string> codecs_string(const Sequence& sequence) {
    if (sequence.audio_elements.empty()) {
        return std::nullopt;
    }
    const CodecConfig* codec = nullptr;
    for (const CodecConfig& candidate : sequence.codec_configs) {
        if (candidate.codec_config_id == sequence.audio_elements.front().codec_config_id) {
            codec = &candidate;
        }
    }
    if (codec == nullptr) {
        return std::nullopt;
    }
    std::string name;
    if (codec->codec_id == "Opus" || codec->codec_id == "fLaC" || codec->codec_id == "ipcm") {
        name = codec->codec_id;
    } else if (codec->codec_id == "mp4a") {
        name = "mp4a.40.2";  // AAC-LC: the audio object type 2 of MPEG-4 Audio
    } else {
        return std::nullopt;
    }
    const auto three_digits = [](std::uint8_t value) {
        std::string digits = std::to_string(value);
        return std::string(3 - digits.size(), '0') + digits;
    };
    return "iamf." + three_digits(sequence.header.primary_profile) + "." +
           three_digits(sequence.header.additional_profile) + "." + name;
}

std::int16_t double_to_q7_8(double value) {
    const double scaled = std::round(value * 256.0);
    return static_cast<std::int16_t>(std::clamp(scaled, -32768.0, 32767.0));
}

std::optional<ParamType> parameter_type(const Sequence& sequence, std::uint32_t parameter_id) {
    const auto index = detail::index_parameters(sequence);
    const auto it = index.find(parameter_id);
    if (it == index.end()) {
        return std::nullopt;
    }
    return it->second.type;
}

// The substream order of a single layer follows "Ordering of Audio Substream Identifiers": coupled
// substreams first (surround before top, front before side before rear), then the mono ones
// (centre, then LFE).
std::optional<LayoutInfo> layout_info(std::uint8_t loudspeaker_layout) {
    const auto make = [](std::string_view name, std::vector<SubstreamChannels> substreams,
                         std::uint8_t coupled, std::uint8_t sound_system) {
        LayoutInfo info;
        info.name = name;
        info.coupled_substream_count = coupled;
        info.sound_system = sound_system;
        for (const auto& s : substreams) {
            info.channel_count = static_cast<std::uint8_t>(info.channel_count + (s.second.empty() ? 1 : 2));
        }
        info.substreams = std::move(substreams);
        return info;
    };
    constexpr std::uint8_t kBinaural = 255;
    switch (loudspeaker_layout) {
        case 0:
            return make("Mono", {{"C", ""}}, 0, 12);
        case 1:
            return make("Stereo", {{"L", "R"}}, 1, 0);
        case 2:
            return make("5.1ch", {{"L", "R"}, {"Ls", "Rs"}, {"C", ""}, {"LFE", ""}}, 2, 1);
        case 3:
            return make("5.1.2ch", {{"L", "R"}, {"Ls", "Rs"}, {"Ltf", "Rtf"}, {"C", ""}, {"LFE", ""}}, 3, 2);
        case 4:
            return make("5.1.4ch",
                        {{"L", "R"}, {"Ls", "Rs"}, {"Ltf", "Rtf"}, {"Ltr", "Rtr"}, {"C", ""}, {"LFE", ""}}, 4, 3);
        case 5:
            return make("7.1ch", {{"L", "R"}, {"Lss", "Rss"}, {"Lrs", "Rrs"}, {"C", ""}, {"LFE", ""}}, 3, 8);
        case 6:
            return make("7.1.2ch",
                        {{"L", "R"}, {"Lss", "Rss"}, {"Lrs", "Rrs"}, {"Ltf", "Rtf"}, {"C", ""}, {"LFE", ""}}, 4, 10);
        case 7:
            return make("7.1.4ch",
                        {{"L", "R"}, {"Lss", "Rss"}, {"Lrs", "Rrs"}, {"Ltf", "Rtf"}, {"Ltb", "Rtb"}, {"C", ""},
                         {"LFE", ""}},
                        5, 9);
        case 8:
            return make("3.1.2ch", {{"L", "R"}, {"Ltf", "Rtf"}, {"C", ""}, {"LFE", ""}}, 2, 11);
        case 9:
            return make("Binaural", {{"L", "R"}}, 1, kBinaural);
        default:
            return std::nullopt;
    }
}

// The expanded layouts follow the same ordering rules: coupled substreams first (surround before
// top before bottom, front before side before rear), then the mono ones (the centres in the same
// order, then the LFEs). 3.6.2.3 gives 10.2.9.3ch and 7.1.5.4ch as examples, which fix the order of
// the rest.
std::optional<LayoutInfo> expanded_layout_info(std::uint8_t expanded_loudspeaker_layout) {
    const auto make = [](std::string_view name, std::vector<SubstreamChannels> substreams, std::uint8_t sound_system) {
        LayoutInfo info;
        info.name = name;
        info.sound_system = sound_system;
        for (const auto& s : substreams) {
            if (s.second.empty()) {
                info.channel_count = static_cast<std::uint8_t>(info.channel_count + 1);
            } else {
                info.channel_count = static_cast<std::uint8_t>(info.channel_count + 2);
                ++info.coupled_substream_count;
            }
        }
        info.substreams = std::move(substreams);
        return info;
    };
    constexpr std::uint8_t kNone = 255;
    switch (expanded_loudspeaker_layout) {
        case 0:
            return make("LFE", {{"LFE", ""}}, kNone);
        case 1:
            return make("Stereo-S", {{"Ls", "Rs"}}, kNone);
        case 2:
            return make("Stereo-SS", {{"Lss", "Rss"}}, kNone);
        case 3:
            return make("Stereo-RS", {{"Lrs", "Rrs"}}, kNone);
        case 4:
            return make("Stereo-TF", {{"Ltf", "Rtf"}}, kNone);
        case 5:
            return make("Stereo-TB", {{"Ltb", "Rtb"}}, kNone);
        case 6:
            return make("Top-4ch", {{"Ltf", "Rtf"}, {"Ltb", "Rtb"}}, kNone);
        case 7:
            return make("3.0ch", {{"L", "R"}, {"C", ""}}, kNone);
        case 8:
            return make("9.1.6ch",
                        {{"FLc", "FRc"}, {"FL", "FR"}, {"SiL", "SiR"}, {"BL", "BR"}, {"TpFL", "TpFR"},
                         {"TpSiL", "TpSiR"}, {"TpBL", "TpBR"}, {"FC", ""}, {"LFE1", ""}},
                        kNone);
        case 9:
            return make("Stereo-F", {{"FL", "FR"}}, kNone);
        case 10:
            return make("Stereo-Si", {{"SiL", "SiR"}}, kNone);
        case 11:
            return make("Stereo-TpSi", {{"TpSiL", "TpSiR"}}, kNone);
        case 12:
            return make("Top-6ch", {{"TpFL", "TpFR"}, {"TpSiL", "TpSiR"}, {"TpBL", "TpBR"}}, kNone);
        case 13:
            return make("10.2.9.3ch",
                        {{"FLc", "FRc"}, {"FL", "FR"}, {"SiL", "SiR"}, {"BL", "BR"}, {"TpFL", "TpFR"},
                         {"TpSiL", "TpSiR"}, {"TpBL", "TpBR"}, {"BtFL", "BtFR"}, {"FC", ""}, {"BC", ""},
                         {"TpFC", ""}, {"TpC", ""}, {"TpBC", ""}, {"BtFC", ""}, {"LFE1", ""}, {"LFE2", ""}},
                        7);  // Sound System H
        case 14:
            return make("LFE-Pair", {{"LFE1", ""}, {"LFE2", ""}}, kNone);
        case 15:
            return make("Bottom-3ch", {{"BtFL", "BtFR"}, {"BtFC", ""}}, kNone);
        case 16:
            return make("7.1.5.4ch",
                        {{"L", "R"}, {"Lss", "Rss"}, {"Lrs", "Rrs"}, {"Ltf", "Rtf"}, {"Ltb", "Rtb"},
                         {"BtFL", "BtFR"}, {"BtBL", "BtBR"}, {"C", ""}, {"TpC", ""}, {"LFE", ""}},
                        kNone);
        case 17:
            return make("Bottom-4ch", {{"BtFL", "BtFR"}, {"BtBL", "BtBR"}}, kNone);
        case 18:
            return make("Top-1ch", {{"TpC", ""}}, kNone);
        case 19:
            return make("Top-5ch", {{"Ltf", "Rtf"}, {"Ltb", "Rtb"}, {"TpC", ""}}, kNone);
        default:
            return std::nullopt;
    }
}

}  // namespace iclforge::containers::iamf

namespace iclforge::containers::iamf::detail {

std::map<std::uint32_t, ParamInfo> index_parameters(const Sequence& sequence) {
    std::map<std::uint32_t, ParamInfo> index;
    const auto add = [&index](ParamType type, const ParamDefinition& definition, const AudioElement* element) {
        index.try_emplace(definition.parameter_id, ParamInfo{type, &definition, element});
    };
    for (const auto& element : sequence.audio_elements) {
        if (element.demixing.has_value()) {
            add(ParamType::kDemixing, element.demixing->definition, &element);
        }
        if (element.recon_gain.has_value()) {
            add(ParamType::kReconGain, element.recon_gain->definition, &element);
        }
    }
    for (const auto& mix : sequence.mix_presentations) {
        for (const auto& sub_mix : mix.sub_mixes) {
            for (const auto& element : sub_mix.elements) {
                add(ParamType::kMixGain, element.element_mix_gain.definition, nullptr);
                if (element.rendering.position.has_value()) {
                    add(element.rendering.position->type, element.rendering.position->definition, nullptr);
                }
            }
            add(ParamType::kMixGain, sub_mix.output_mix_gain.definition, nullptr);
            for (const auto& layout : sub_mix.layouts) {
                if (layout.loudness.momentary.has_value()) {
                    add(ParamType::kMomentaryLoudness, layout.loudness.momentary->definition, nullptr);
                }
            }
        }
    }
    return index;
}

void put_param_definition(Out& out, const ParamDefinition& definition) {
    out.leb128(definition.parameter_id);
    out.leb128(definition.parameter_rate);
    out.u8(static_cast<std::uint32_t>(definition.param_definition_mode & 1U) << 7);
    if (definition.param_definition_mode == 0) {
        out.leb128(definition.duration);
        out.leb128(definition.constant_subblock_duration);
        if (definition.constant_subblock_duration == 0) {
            out.leb128(static_cast<std::uint32_t>(definition.subblock_durations.size()));
            for (const std::uint32_t d : definition.subblock_durations) {
                out.leb128(d);
            }
        }
    }
}

std::expected<ParamDefinition, Error> read_param_definition(Cursor& in) {
    ParamDefinition definition;
    auto id = in.leb128();
    auto rate = in.leb128();
    auto mode = in.u8();
    if (!id.has_value() || !rate.has_value() || !mode.has_value()) {
        return std::unexpected(!id.has_value() ? id.error() : !rate.has_value() ? rate.error() : mode.error());
    }
    definition.parameter_id = *id;
    definition.parameter_rate = *rate;
    definition.param_definition_mode = static_cast<std::uint8_t>(*mode >> 7);
    if (definition.param_definition_mode == 0) {
        auto duration = in.leb128();
        auto constant = in.leb128();
        if (!duration.has_value() || !constant.has_value()) {
            return std::unexpected(!duration.has_value() ? duration.error() : constant.error());
        }
        definition.duration = *duration;
        definition.constant_subblock_duration = *constant;
        if (*constant == 0) {
            auto count = in.leb128();
            if (!count.has_value()) {
                return std::unexpected(count.error());
            }
            // Each sub block duration is at least one byte: a count beyond what remains is
            // truncation, and caps the allocation.
            if (*count > in.remaining()) {
                return std::unexpected(Error::kTruncated);
            }
            for (std::uint32_t i = 0; i < *count; ++i) {
                auto d = in.leb128();
                if (!d.has_value()) {
                    return std::unexpected(d.error());
                }
                definition.subblock_durations.push_back(*d);
            }
        }
    }
    return definition;
}

std::vector<FieldCoding> position_fields(ParamType type) {
    switch (type) {
        case ParamType::kPolar:
            return {{9, true}, {8, true}, {7, false}};
        case ParamType::kDualPolar:
            return {{9, true}, {8, true}, {7, false}, {9, true}, {8, true}, {7, false}};
        case ParamType::kCart8:
            return {{8, true}, {8, true}, {8, true}};
        case ParamType::kDualCart8:
            return {{8, true}, {8, true}, {8, true}, {8, true}, {8, true}, {8, true}};
        case ParamType::kCart16:
            return {{16, true}, {16, true}, {16, true}};
        case ParamType::kDualCart16:
            return {{16, true}, {16, true}, {16, true}, {16, true}, {16, true}, {16, true}};
        default:
            return {};
    }
}

void put_position_defaults(Out& out, const PositionParamDefinition& definition) {
    const auto fields = position_fields(definition.type);
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const std::int32_t value = i < definition.defaults.size() ? definition.defaults[i] : 0;
        out.bits(static_cast<std::uint64_t>(static_cast<std::uint32_t>(value)) & ((std::uint64_t{1} << fields[i].width) - 1),
                 fields[i].width);
    }
}

std::expected<PositionParamDefinition, Error> read_position_definition(ParamType type, Cursor& in) {
    PositionParamDefinition definition;
    definition.type = type;
    auto base = read_param_definition(in);
    if (!base.has_value()) {
        return std::unexpected(base.error());
    }
    definition.definition = std::move(*base);
    for (const auto& field : position_fields(type)) {
        auto raw = in.bits(field.width);
        if (!raw.has_value()) {
            return std::unexpected(raw.error());
        }
        definition.defaults.push_back(field.is_signed ? sign_extend(*raw, field.width)
                                                      : static_cast<std::int32_t>(*raw));
    }
    in.align();
    return definition;
}

bool known_animation(std::uint32_t animation_type) { return animation_type <= 4; }

void put_animated(Out& out, std::uint32_t animation_type, const AnimatedValue& value, unsigned width) {
    const std::uint64_t mask = (std::uint64_t{1} << width) - 1;
    const auto put = [&](std::int32_t v) {
        out.bits(static_cast<std::uint64_t>(static_cast<std::uint32_t>(v)) & mask, width);
    };
    switch (animation_type) {
        case 0:  // STEP
            put(value.start);
            break;
        case 1:  // LINEAR
            put(value.start);
            put(value.end);
            break;
        case 2:  // BEZIER
            put(value.start);
            put(value.end);
            put(value.control);
            out.bits(value.control_relative_time, 8);
            break;
        case 3:  // INTER_LINEAR
            put(value.end);
            break;
        case 4:  // INTER_BEZIER
            put(value.end);
            put(value.control);
            out.bits(value.control_relative_time, 8);
            break;
        default:
            break;
    }
}

std::expected<AnimatedValue, Error> read_animated(Cursor& in, std::uint32_t animation_type, unsigned width,
                                                  bool is_signed) {
    AnimatedValue value;
    const auto get = [&]() -> std::expected<std::int32_t, Error> {
        auto raw = in.bits(width);
        if (!raw.has_value()) {
            return std::unexpected(raw.error());
        }
        return is_signed ? sign_extend(*raw, width) : static_cast<std::int32_t>(*raw);
    };
    const auto assign = [&](std::int32_t& field) -> std::expected<void, Error> {
        auto v = get();
        if (!v.has_value()) {
            return std::unexpected(v.error());
        }
        field = *v;
        return {};
    };
    const auto relative_time = [&]() -> std::expected<void, Error> {
        auto t = in.bits(8);
        if (!t.has_value()) {
            return std::unexpected(t.error());
        }
        value.control_relative_time = static_cast<std::uint8_t>(*t);
        return {};
    };

    std::expected<void, Error> status;
    switch (animation_type) {
        case 0:
            status = assign(value.start);
            break;
        case 1:
            status = assign(value.start);
            if (status.has_value()) {
                status = assign(value.end);
            }
            break;
        case 2:
            status = assign(value.start);
            if (status.has_value()) {
                status = assign(value.end);
            }
            if (status.has_value()) {
                status = assign(value.control);
            }
            if (status.has_value()) {
                status = relative_time();
            }
            break;
        case 3:
            status = assign(value.end);
            break;
        case 4:
            status = assign(value.end);
            if (status.has_value()) {
                status = assign(value.control);
            }
            if (status.has_value()) {
                status = relative_time();
            }
            break;
        default:
            return std::unexpected(Error::kUnsupported);
    }
    if (!status.has_value()) {
        return std::unexpected(status.error());
    }
    return value;
}

}  // namespace iclforge::containers::iamf::detail
