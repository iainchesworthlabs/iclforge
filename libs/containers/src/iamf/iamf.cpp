#include "iclforge/containers/iamf/iamf.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/containers/iamf/container.hpp"
#include "iclforge/containers/iamf/model.hpp"
#include "iclforge/containers/iamf/sequence.hpp"
#include "sequence_detail.hpp"

namespace iclforge::containers::iamf {

namespace {

constexpr std::uint32_t kCodecConfigId = 0;
constexpr std::uint32_t kMaxObjectChannels = 18;  // the Base-Advanced and Advanced-1 profiles

[[nodiscard]] bool valid_sample_rate(std::uint32_t rate) {
    return rate == 44100 || rate == 16000 || rate == 32000 || rate == 48000 || rate == 96000;
}

[[nodiscard]] bool valid_bit_depth(int depth) { return depth == 16 || depth == 24 || depth == 32; }

[[nodiscard]] CodecConfig lpcm_codec_config(std::uint32_t sample_rate, int bit_depth, std::uint32_t samples_per_frame) {
    CodecConfig config;
    config.codec_config_id = kCodecConfigId;
    config.codec_id = "ipcm";
    config.num_samples_per_frame = samples_per_frame;
    config.audio_roll_distance = 0;  // 0 for ipcm
    config.lpcm = LpcmConfig{.sample_format_flags = 0x01, .sample_size = static_cast<std::uint8_t>(bit_depth),
                             .sample_rate = sample_rate};
    return config;
}

// A constant mix gain: no Parameter Block ever varies it, so the definition carries the timing and
// the value. duration and constant_subblock_duration are the frame length, as the Audio Element
// parameter definitions require, and parameter_rate the sample rate so a frame is a whole number of
// ticks.
[[nodiscard]] MixGainParamDefinition constant_mix_gain(std::uint32_t parameter_id, std::uint32_t sample_rate,
                                                       std::uint32_t samples_per_frame) {
    MixGainParamDefinition definition;
    definition.definition.parameter_id = parameter_id;
    definition.definition.parameter_rate = sample_rate;
    definition.definition.param_definition_mode = 0;
    definition.definition.duration = samples_per_frame;
    definition.definition.constant_subblock_duration = samples_per_frame;
    definition.default_mix_gain = 0;  // 0 dB: transparent
    return definition;
}

[[nodiscard]] LoudnessData loudness_data(const LoudnessInfo& loudness) {
    LoudnessData data;
    data.integrated_loudness =
        double_to_q7_8(static_cast<double>(loudness.integrated_loudness_lkfs));
    data.digital_peak = double_to_q7_8(static_cast<double>(loudness.digital_peak_dbfs));
    return data;
}

// Appends one little-endian signed PCM sample at `bit_depth` bits, scaled from [-1, 1] and rounded
// and clipped the way the rest of this codebase converts to integer PCM.
void put_pcm_sample(Bytes& out, float sample, int bit_depth) {
    const auto full_scale = static_cast<double>((std::uint32_t{1} << (bit_depth - 1)) - 1);
    const double scaled = std::round(static_cast<double>(sample) * full_scale);
    const double clamped = std::clamp(scaled, -full_scale - 1.0, full_scale);
    const auto bits = static_cast<std::uint32_t>(static_cast<std::int32_t>(clamped));
    for (int byte = 0; byte < bit_depth / 8; ++byte) {
        out.push_back(static_cast<std::byte>((bits >> (8 * byte)) & 0xFFU));
    }
}

// The index into Frame::channels of each speaker.
[[nodiscard]] int frame_channel(std::string_view name) {
    constexpr std::array<std::string_view, 12> kOrder{"L",   "C",   "R",   "Lss", "Rss", "Lrs",
                                                      "Rrs", "Ltf", "Rtf", "Ltb", "Rtb", "LFE"};
    for (std::size_t i = 0; i < kOrder.size(); ++i) {
        if (kOrder[i] == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// The trimming each Temporal Unit's Audio Frames carry for a programme of `frames` frames:
// the start trim spreads across the leading frames (each fully trimmed frame trims
// samples_per_frame), the end trim sits on the last frame.
struct FrameTrim {
    std::uint32_t at_start = 0;
    std::uint32_t at_end = 0;
};

[[nodiscard]] std::expected<std::vector<FrameTrim>, MuxError> frame_trims(std::size_t frames,
                                                                          std::uint32_t samples_per_frame,
                                                                          std::uint32_t trim_start,
                                                                          std::uint32_t trim_end) {
    std::vector<FrameTrim> trims(frames);
    if (trim_end > samples_per_frame ||
        static_cast<std::uint64_t>(trim_start) + trim_end > static_cast<std::uint64_t>(frames) * samples_per_frame) {
        return std::unexpected(MuxError::kInvalidTrim);
    }
    std::uint32_t remaining = trim_start;
    for (auto& trim : trims) {
        trim.at_start = std::min(remaining, samples_per_frame);
        remaining -= trim.at_start;
    }
    trims.back().at_end = trim_end;
    // Within one frame the two trims must not overlap.
    if (static_cast<std::uint64_t>(trims.back().at_start) + trims.back().at_end > samples_per_frame) {
        return std::unexpected(MuxError::kInvalidTrim);
    }
    return trims;
}

void apply_trim(AudioFrame& frame, const FrameTrim& trim) {
    frame.has_trimming = trim.at_start != 0 || trim.at_end != 0;
    frame.num_samples_to_trim_at_start = trim.at_start;
    frame.num_samples_to_trim_at_end = trim.at_end;
}

}  // namespace

std::string_view describe(MuxError error) {
    switch (error) {
        case MuxError::kNoFrames:
            return "no frames to mux";
        case MuxError::kInvalidTrack:
            return "invalid track: sample_rate must be one of {44100,16000,32000,48000,96000}, "
                   "bit_depth one of {16,24,32} (IAMF LPCM DecoderConfig), and samples_per_frame non-zero";
        case MuxError::kFrameSizeMismatch:
            return "a frame did not carry exactly samples_per_frame samples on every channel";
        case MuxError::kInvalidTrim:
            return "the requested trimming does not fit the Audio Frame OBU rules";
        case MuxError::kNoObjects:
            return "no object-based Audio Elements, or an element without objects";
        case MuxError::kBadObjectElement:
            return "an object-based Audio Element holds one or two objects";
        case MuxError::kObjectLengthMismatch:
            return "the objects do not all have the same number of samples";
        case MuxError::kTooManyChannels:
            return "more than 18 channels across the Audio Elements";
        case MuxError::kBadPositions:
            return "an object's positions must be empty or one per frame";
        case MuxError::kWriteFailed:
            return "the Sequence built could not be written";
        case MuxError::kInvalidLayout:
            return "a coded programme takes one layer of loudspeaker_layout 0 to 8";
        case MuxError::kSubstreamCountMismatch:
            return "a coded frame did not hold one packet per Audio Substream of the layout";
        case MuxError::kBadCodedPacket:
            return "a coded packet empty, or breaking the codec's constraints in 3.13 (frame count, duration, "
                   "block size, channel coding)";
    }
    return "unknown error";
}

std::expected<Sequence, MuxError> build_sequence(const AudioTrack& track, std::span<const Frame> frames) {
    if (frames.empty()) {
        return std::unexpected(MuxError::kNoFrames);
    }
    if (!valid_sample_rate(track.sample_rate) || !valid_bit_depth(track.bit_depth) || track.samples_per_frame == 0) {
        return std::unexpected(MuxError::kInvalidTrack);
    }
    for (const auto& frame : frames) {
        for (const auto& channel : frame.channels) {
            if (channel.size() != track.samples_per_frame) {
                return std::unexpected(MuxError::kFrameSizeMismatch);
            }
        }
    }
    auto trims = frame_trims(frames.size(), track.samples_per_frame, track.trim_start_samples, track.trim_end_samples);
    if (!trims.has_value()) {
        return std::unexpected(trims.error());
    }

    const auto layout = layout_info(7);  // 7.1.4ch
    Sequence sequence;
    sequence.header = {};  // Simple Profile: one Audio Element of at most 16 channels
    sequence.codec_configs.push_back(lpcm_codec_config(track.sample_rate, track.bit_depth, track.samples_per_frame));

    AudioElement element;
    element.audio_element_id = 0;
    element.type = ElementType::kChannelBased;
    element.codec_config_id = kCodecConfigId;
    ChannelLayer layer;
    layer.loudspeaker_layout = 7;
    layer.substream_count = static_cast<std::uint8_t>(layout->substreams.size());
    layer.coupled_substream_count = layout->coupled_substream_count;
    element.layers.push_back(layer);
    for (std::uint32_t id = 0; id < layout->substreams.size(); ++id) {
        element.audio_substream_ids.push_back(id);
    }
    sequence.audio_elements.push_back(std::move(element));

    MixPresentation mix;
    mix.mix_presentation_id = 0;
    SubMix sub_mix;
    SubMixElement mix_element;
    mix_element.audio_element_id = 0;
    mix_element.element_mix_gain = constant_mix_gain(0, track.sample_rate, track.samples_per_frame);
    sub_mix.elements.push_back(std::move(mix_element));
    sub_mix.output_mix_gain = constant_mix_gain(1, track.sample_rate, track.samples_per_frame);
    // The Stereo loudness layout every sub-mix carries (Sound System A), then this element's own.
    sub_mix.layouts.push_back({{.layout_type = 2, .sound_system = 0}, loudness_data(track.stereo_loudness)});
    sub_mix.layouts.push_back({{.layout_type = 2, .sound_system = layout->sound_system},
                               loudness_data(track.layout_714_loudness)});
    mix.sub_mixes.push_back(std::move(sub_mix));
    sequence.mix_presentations.push_back(std::move(mix));

    sequence.temporal_units.reserve(frames.size());
    for (std::size_t i = 0; i < frames.size(); ++i) {
        TemporalUnit unit;
        unit.has_temporal_delimiter = track.temporal_delimiters;
        for (std::uint32_t id = 0; id < layout->substreams.size(); ++id) {
            const SubstreamChannels& channels = layout->substreams[id];
            AudioFrame frame;
            frame.audio_substream_id = id;
            const auto& first = frames[i].channels[static_cast<std::size_t>(frame_channel(channels.first))];
            if (channels.second.empty()) {
                for (const float sample : first) {
                    put_pcm_sample(frame.data, sample, track.bit_depth);
                }
            } else {
                // Stereo is sample interleaved: the i-th Left sample, then the i-th Right sample.
                const auto& second = frames[i].channels[static_cast<std::size_t>(frame_channel(channels.second))];
                for (std::size_t n = 0; n < first.size(); ++n) {
                    put_pcm_sample(frame.data, first[n], track.bit_depth);
                    put_pcm_sample(frame.data, second[n], track.bit_depth);
                }
            }
            apply_trim(frame, (*trims)[i]);
            unit.audio_frames.push_back(std::move(frame));
        }
        sequence.temporal_units.push_back(std::move(unit));
    }
    return sequence;
}

std::expected<std::vector<std::byte>, MuxError> mux(const AudioTrack& track, std::span<const Frame> frames) {
    auto sequence = build_sequence(track, frames);
    if (!sequence.has_value()) {
        return std::unexpected(sequence.error());
    }
    auto file = write_isobmff(*sequence, {.writing_app = track.writing_app});
    if (!file.has_value()) {
        return std::unexpected(MuxError::kWriteFailed);
    }
    return std::move(*file);
}

// --- Object-based audio -------------------------------------------------------------------------

namespace {

constexpr std::uint8_t kInterLinear = 3;
constexpr std::uint8_t kStep = 0;

// The coded values of a position, in the order the position syntax lists them (twice for a dual
// type).
[[nodiscard]] std::vector<std::int32_t> coded_position(const ObjectPosition& position, PositionCoding coding) {
    const auto round_clamped = [](double value, double low, double high) {
        return static_cast<std::int32_t>(std::lround(std::clamp(value, low, high)));
    };
    switch (coding) {
        case PositionCoding::kPolar:
            return {round_clamped(position.azimuth_deg, -180.0, 180.0), round_clamped(position.elevation_deg, -90.0, 90.0),
                    round_clamped(position.distance * 127.0, 0.0, 127.0)};
        case PositionCoding::kCartesian8:
            return {round_clamped(position.x * 127.0, -127.0, 127.0), round_clamped(position.y * 127.0, -127.0, 127.0),
                    round_clamped(position.z * 127.0, -127.0, 127.0)};
        case PositionCoding::kCartesian16:
            break;
    }
    return {round_clamped(position.x * 32767.0, -32767.0, 32767.0), round_clamped(position.y * 32767.0, -32767.0, 32767.0),
            round_clamped(position.z * 32767.0, -32767.0, 32767.0)};
}

[[nodiscard]] ParamType position_type(PositionCoding coding, bool dual) {
    switch (coding) {
        case PositionCoding::kPolar:
            return dual ? ParamType::kDualPolar : ParamType::kPolar;
        case PositionCoding::kCartesian8:
            return dual ? ParamType::kDualCart8 : ParamType::kCart8;
        case PositionCoding::kCartesian16:
            break;
    }
    return dual ? ParamType::kDualCart16 : ParamType::kCart16;
}

// The angle between two polar directions, in degrees.
[[nodiscard]] double direction_angle_deg(const ObjectPosition& a, const ObjectPosition& b) {
    const auto unit = [](const ObjectPosition& p) {
        const double az = p.azimuth_deg * std::numbers::pi / 180.0;
        const double el = p.elevation_deg * std::numbers::pi / 180.0;
        return std::array<double, 3>{std::cos(el) * std::cos(az), std::cos(el) * std::sin(az), std::sin(el)};
    };
    const auto ua = unit(a);
    const auto ub = unit(b);
    const double dot = std::clamp(ua[0] * ub[0] + ua[1] * ub[1] + ua[2] * ub[2], -1.0, 1.0);
    return std::acos(dot) * 180.0 / std::numbers::pi;
}

}  // namespace

std::expected<Sequence, MuxError> build_object_sequence(const ObjectTrack& track, std::span<const ObjectElement> elements) {
    if (!valid_sample_rate(track.sample_rate) || !valid_bit_depth(track.bit_depth) || track.samples_per_frame == 0) {
        return std::unexpected(MuxError::kInvalidTrack);
    }
    if (elements.empty()) {
        return std::unexpected(MuxError::kNoObjects);
    }
    std::size_t channels = 0;
    std::size_t length = 0;
    bool have_length = false;
    for (const ObjectElement& element : elements) {
        if (element.objects.empty()) {
            return std::unexpected(MuxError::kNoObjects);
        }
        if (element.objects.size() > 2) {
            return std::unexpected(MuxError::kBadObjectElement);
        }
        channels += element.objects.size();
        for (const ObjectSource& object : element.objects) {
            if (!have_length) {
                length = object.samples.size();
                have_length = true;
            } else if (object.samples.size() != length) {
                return std::unexpected(MuxError::kObjectLengthMismatch);
            }
        }
    }
    if (length == 0) {
        return std::unexpected(MuxError::kNoFrames);
    }
    if (channels > kMaxObjectChannels) {
        return std::unexpected(MuxError::kTooManyChannels);
    }
    const std::size_t frames = (length + track.samples_per_frame - 1) / track.samples_per_frame;
    for (const ObjectElement& element : elements) {
        for (const ObjectSource& object : element.objects) {
            if (!object.positions.empty() && object.positions.size() != frames) {
                return std::unexpected(MuxError::kBadPositions);
            }
        }
    }
    const auto padding = static_cast<std::uint32_t>(frames * track.samples_per_frame - length);
    auto trims = frame_trims(frames, track.samples_per_frame, 0, padding);
    if (!trims.has_value()) {
        return std::unexpected(trims.error());
    }

    Sequence sequence;
    sequence.header = {static_cast<std::uint8_t>(Profile::kBaseAdvanced), static_cast<std::uint8_t>(Profile::kBaseAdvanced)};
    sequence.codec_configs.push_back(lpcm_codec_config(track.sample_rate, track.bit_depth, track.samples_per_frame));

    MixPresentation mix;
    mix.mix_presentation_id = 0;
    SubMix sub_mix;
    const auto element_count = static_cast<std::uint32_t>(elements.size());
    for (std::uint32_t k = 0; k < element_count; ++k) {
        const ObjectElement& source = elements[k];
        const bool dual = source.objects.size() == 2;

        AudioElement element;
        element.audio_element_id = k;
        element.type = ElementType::kObjectBased;
        element.codec_config_id = kCodecConfigId;
        element.audio_substream_ids = {k};
        element.objects.num_objects = static_cast<std::uint8_t>(source.objects.size());
        sequence.audio_elements.push_back(std::move(element));

        SubMixElement mix_element;
        mix_element.audio_element_id = k;
        mix_element.element_mix_gain = constant_mix_gain(2 * k, track.sample_rate, track.samples_per_frame);
        PositionParamDefinition position;
        position.type = position_type(track.position_coding, dual);
        position.definition.parameter_id = 2 * k + 1;
        position.definition.parameter_rate = track.sample_rate;
        position.definition.param_definition_mode = 0;
        position.definition.duration = track.samples_per_frame;
        position.definition.constant_subblock_duration = track.samples_per_frame;
        for (const ObjectSource& object : source.objects) {
            const auto values = coded_position(object.initial_position, track.position_coding);
            position.defaults.insert(position.defaults.end(), values.begin(), values.end());
        }
        mix_element.rendering.position = std::move(position);
        sub_mix.elements.push_back(std::move(mix_element));
    }
    sub_mix.output_mix_gain = constant_mix_gain(2 * element_count, track.sample_rate, track.samples_per_frame);
    sub_mix.layouts.push_back({{.layout_type = 2, .sound_system = 0}, loudness_data(track.stereo_loudness)});
    mix.sub_mixes.push_back(std::move(sub_mix));
    sequence.mix_presentations.push_back(std::move(mix));

    sequence.temporal_units.reserve(frames);
    for (std::size_t f = 0; f < frames; ++f) {
        TemporalUnit unit;
        unit.has_temporal_delimiter = track.temporal_delimiters;
        const std::size_t begin = f * track.samples_per_frame;

        for (std::uint32_t k = 0; k < element_count; ++k) {
            const ObjectElement& source = elements[k];
            const bool moves = std::any_of(source.objects.begin(), source.objects.end(),
                                           [](const ObjectSource& o) { return !o.positions.empty(); });
            if (!moves) {
                continue;  // the definition's default holds, so no Parameter Block is written
            }
            ParameterBlock block;
            block.parameter_id = 2 * k + 1;
            ParameterSubblock subblock;
            subblock.animation_type = kInterLinear;
            for (const ObjectSource& object : source.objects) {
                const ObjectPosition& end = object.positions.empty() ? object.initial_position : object.positions[f];
                const ObjectPosition& start = f == 0 || object.positions.empty() ? object.initial_position
                                                                                 : object.positions[f - 1];
                if (track.position_coding == PositionCoding::kPolar) {
                    // The angle between the coded directions must stay under 180 degrees for
                    // INTER_LINEAR; stay a degree clear of it for rounding.
                    const auto from = coded_position(start, track.position_coding);
                    const auto to = coded_position(end, track.position_coding);
                    ObjectPosition a;
                    a.azimuth_deg = from[0];
                    a.elevation_deg = from[1];
                    ObjectPosition b;
                    b.azimuth_deg = to[0];
                    b.elevation_deg = to[1];
                    if (direction_angle_deg(a, b) >= 179.0) {
                        subblock.animation_type = kStep;
                    }
                }
            }
            for (const ObjectSource& object : source.objects) {
                const ObjectPosition& end = object.positions.empty() ? object.initial_position : object.positions[f];
                for (const std::int32_t value : coded_position(end, track.position_coding)) {
                    AnimatedValue component;
                    // INTER_LINEAR carries only the end point; STEP holds its start point.
                    (subblock.animation_type == kStep ? component.start : component.end) = value;
                    subblock.components.push_back(component);
                }
            }
            block.subblocks.push_back(std::move(subblock));
            unit.parameter_blocks.push_back(std::move(block));
        }

        for (std::uint32_t k = 0; k < element_count; ++k) {
            const ObjectElement& source = elements[k];
            AudioFrame frame;
            frame.audio_substream_id = k;
            for (std::size_t n = 0; n < track.samples_per_frame; ++n) {
                for (const ObjectSource& object : source.objects) {
                    const std::size_t at = begin + n;
                    put_pcm_sample(frame.data, at < object.samples.size() ? object.samples[at] : 0.0F, track.bit_depth);
                }
            }
            apply_trim(frame, (*trims)[f]);
            unit.audio_frames.push_back(std::move(frame));
        }
        sequence.temporal_units.push_back(std::move(unit));
    }
    return sequence;
}

std::expected<std::vector<std::byte>, MuxError> mux_objects(const ObjectTrack& track,
                                                             std::span<const ObjectElement> elements) {
    auto sequence = build_object_sequence(track, elements);
    if (!sequence.has_value()) {
        return std::unexpected(sequence.error());
    }
    auto file = write_isobmff(*sequence, {.writing_app = track.writing_app});
    if (!file.has_value()) {
        return std::unexpected(MuxError::kWriteFailed);
    }
    return std::move(*file);
}

// --- Carrying coded audio -----------------------------------------------------------------------

namespace {

// Appends `value` in `bytes` big-endian bytes.
void put_be(Bytes& out, std::uint64_t value, int bytes) {
    for (int shift = 8 * (bytes - 1); shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
    }
}

// FLAC's "common" sample rates, with the Sample Rate Bits of a frame header (RFC 9639, 9.1.2).
struct FlacRate {
    std::uint32_t rate;
    unsigned code;
};
constexpr std::array<FlacRate, 11> kFlacRates{{{88200, 0b0001}, {176400, 0b0010}, {192000, 0b0011}, {8000, 0b0100},
                                               {16000, 0b0101}, {22050, 0b0110},  {24000, 0b0111},  {32000, 0b1000},
                                               {44100, 0b1001}, {48000, 0b1010},  {96000, 0b1011}}};

[[nodiscard]] std::optional<unsigned> flac_rate_code(std::uint32_t rate) {
    for (const FlacRate& entry : kFlacRates) {
        if (entry.rate == rate) {
            return entry.code;
        }
    }
    return std::nullopt;
}

// 3.13.1: an Opus packet holds one frame (frame count code 0) of the duration `samples_per_frame`
// at 48 kHz. The TOC byte's configuration names the duration (RFC 6716, 3.1).
[[nodiscard]] bool opus_packet_ok(const Bytes& packet, std::uint32_t samples_per_frame) {
    if (packet.empty()) {
        return false;
    }
    const auto toc = std::to_integer<unsigned>(packet[0]);
    if ((toc & 3U) != 0) {
        return false;
    }
    const unsigned config = toc >> 3;
    unsigned samples = 0;
    if (config < 12) {  // SILK: 10, 20, 40 and 60 ms
        constexpr std::array<unsigned, 4> kSilk{480, 960, 1920, 2880};
        samples = kSilk[config % 4];
    } else if (config < 16) {  // Hybrid: 10 and 20 ms
        samples = config % 2 == 0 ? 480 : 960;
    } else {  // CELT: 2.5, 5, 10 and 20 ms
        constexpr std::array<unsigned, 4> kCelt{120, 240, 480, 960};
        samples = kCelt[config % 4];
    }
    return samples == samples_per_frame;
}

// 3.13.3: a FLAC frame of the block size, sample rate and depth the STREAMINFO names, as one mono
// or (independent L/R coding) stereo frame.
[[nodiscard]] bool flac_packet_ok(const Bytes& packet, const CodedTrack& track, bool coupled) {
    if (packet.size() < 6) {
        return false;
    }
    const auto at = [&packet](std::size_t i) { return std::to_integer<unsigned>(packet[i]); };
    if (at(0) != 0xFFU || (at(1) & 0xFEU) != 0xF8U) {
        return false;  // the 14 bit frame sync code and a reserved 0
    }
    const unsigned block_code = at(2) >> 4;
    const unsigned rate_code = at(2) & 0xFU;
    const unsigned channel_code = at(3) >> 4;
    const unsigned size_code = (at(3) >> 1) & 7U;
    if (channel_code != (coupled ? 1U : 0U) || rate_code != flac_rate_code(track.sample_rate).value_or(0xFFU)) {
        return false;
    }
    const unsigned expected_size = track.bit_depth == 16 ? 0b100U : track.bit_depth == 24 ? 0b110U : 0b111U;
    if (size_code != expected_size) {
        return false;
    }
    // The block size: a table value, or 8 or 16 bits after the UTF-8 coded frame or sample number.
    std::uint32_t block = 0;
    if (block_code == 0b0001) {
        block = 192;
    } else if (block_code >= 0b0010 && block_code <= 0b0101) {
        block = 576U << (block_code - 2);
    } else if (block_code >= 0b1000) {
        block = 256U << (block_code - 8);
    } else if (block_code == 0b0110 || block_code == 0b0111) {
        const unsigned lead = at(4);
        std::size_t length = 1;
        if (lead >= 0xFCU) {
            length = lead == 0xFEU ? 7 : 6;
        } else if (lead >= 0xF8U) {
            length = 5;
        } else if (lead >= 0xF0U) {
            length = 4;
        } else if (lead >= 0xE0U) {
            length = 3;
        } else if (lead >= 0xC0U) {
            length = 2;
        }
        const std::size_t width = block_code == 0b0110 ? 1 : 2;
        if (packet.size() < 4 + length + width) {
            return false;
        }
        block = 1;
        for (std::size_t i = 0; i < width; ++i) {
            block += at(4 + length + i) << (8 * (width - 1 - i));
        }
    }
    return block == track.samples_per_frame;
}

}  // namespace

Bytes opus_decoder_config(std::uint16_t pre_skip, std::uint32_t input_sample_rate) {
    Bytes out;
    out.push_back(std::byte{1});  // Version
    out.push_back(std::byte{2});  // Output Channel Count: 2 (3.13.1)
    put_be(out, pre_skip, 2);
    put_be(out, input_sample_rate, 4);
    put_be(out, 0, 2);            // Output Gain: 0 dB
    out.push_back(std::byte{0});  // Channel Mapping Family
    return out;
}

std::expected<Bytes, MuxError> aac_lc_decoder_config(std::uint32_t sample_rate, std::uint32_t max_bitrate,
                                                     std::uint32_t average_bitrate) {
    const auto index = detail::aac_sampling_frequency_index(sample_rate);
    if (!index.has_value()) {
        return std::unexpected(MuxError::kInvalidTrack);
    }
    Bytes out;
    out.push_back(std::byte{0x04});  // DecoderConfigDescriptor, 17 bytes
    out.push_back(std::byte{0x11});
    out.push_back(std::byte{0x40});  // objectTypeIndication: MPEG-4 Audio
    out.push_back(std::byte{0x15});  // streamType 5 (Audio), upStream 0, reserved 1
    put_be(out, 0, 3);               // bufferSizeDB
    put_be(out, max_bitrate, 4);
    put_be(out, average_bitrate, 4);
    out.push_back(std::byte{0x05});  // DecoderSpecificInfo, 2 bytes: the AudioSpecificConfig
    out.push_back(std::byte{0x02});
    // audioObjectType 2 (5 bits), samplingFrequencyIndex (4), channelConfiguration 2 (4), then
    // frameLengthFlag, dependsOnCoreCoder and extensionFlag all 0 (3.13.2).
    put_be(out, (2U << 11) | (*index << 7) | (2U << 3), 2);
    return out;
}

std::expected<Bytes, MuxError> flac_decoder_config(std::uint32_t sample_rate, int bit_depth,
                                                   std::uint32_t samples_per_frame) {
    if (!flac_rate_code(sample_rate).has_value() || !valid_bit_depth(bit_depth) || samples_per_frame < 16 ||
        samples_per_frame > 65535) {
        return std::unexpected(MuxError::kInvalidTrack);
    }
    Bytes out;
    out.push_back(std::byte{0x80});  // the last metadata block, type 0: STREAMINFO
    put_be(out, 34, 3);
    put_be(out, samples_per_frame, 2);  // minimum and maximum block size
    put_be(out, samples_per_frame, 2);
    put_be(out, 0, 3);  // minimum and maximum frame size: unknown
    put_be(out, 0, 3);
    // sample rate (20 bits), channels - 1 (3 bits: stereo), bits per sample - 1 (5), total samples (36)
    const std::uint64_t packed = (static_cast<std::uint64_t>(sample_rate) << 44) | (std::uint64_t{1} << 41) |
                                 (static_cast<std::uint64_t>(bit_depth - 1) << 36);
    put_be(out, packed, 8);
    out.insert(out.end(), 16, std::byte{0});  // MD5 signature: not set
    return out;
}

std::expected<Sequence, MuxError> build_coded_sequence(const CodedTrack& track, std::span<const CodedFrame> frames) {
    if (frames.empty()) {
        return std::unexpected(MuxError::kNoFrames);
    }
    if (track.loudspeaker_layout > 8) {
        return std::unexpected(MuxError::kInvalidLayout);
    }
    const auto layout = layout_info(track.loudspeaker_layout);
    const std::uint32_t frame_length = track.samples_per_frame;

    CodecConfig config;
    config.codec_config_id = kCodecConfigId;
    config.num_samples_per_frame = frame_length;
    switch (track.codec) {
        case CodedCodec::kOpus: {
            constexpr std::array<std::uint32_t, 6> kDurations{120, 240, 480, 960, 1920, 2880};
            if (track.sample_rate != 48000 ||
                std::find(kDurations.begin(), kDurations.end(), frame_length) == kDurations.end()) {
                return std::unexpected(MuxError::kInvalidTrack);
            }
            if (track.trim_start_samples > 0xFFFFU) {
                return std::unexpected(MuxError::kInvalidTrim);  // the pre-skip is 16 bits
            }
            config.codec_id = "Opus";
            // 3.5: -R with R = ceil(3840 / num_samples_per_frame): 80 ms of pre-roll.
            config.audio_roll_distance =
                static_cast<std::int16_t>(-static_cast<int>((3840U + frame_length - 1) / frame_length));
            config.decoder_config = opus_decoder_config(static_cast<std::uint16_t>(track.trim_start_samples));
            break;
        }
        case CodedCodec::kAacLc: {
            if (frame_length != 1024) {
                return std::unexpected(MuxError::kInvalidTrack);  // frameLengthFlag 0: 1024 lines
            }
            auto decoder_config = aac_lc_decoder_config(track.sample_rate);
            if (!decoder_config.has_value()) {
                return std::unexpected(decoder_config.error());
            }
            config.codec_id = "mp4a";
            config.audio_roll_distance = -1;
            config.decoder_config = std::move(*decoder_config);
            break;
        }
        case CodedCodec::kFlac: {
            auto decoder_config = flac_decoder_config(track.sample_rate, track.bit_depth, frame_length);
            if (!decoder_config.has_value()) {
                return std::unexpected(decoder_config.error());
            }
            config.codec_id = "fLaC";
            config.audio_roll_distance = 0;
            config.decoder_config = std::move(*decoder_config);
            break;
        }
    }

    for (const CodedFrame& frame : frames) {
        if (frame.substreams.size() != layout->substreams.size()) {
            return std::unexpected(MuxError::kSubstreamCountMismatch);
        }
        for (std::size_t s = 0; s < frame.substreams.size(); ++s) {
            const Bytes& packet = frame.substreams[s];
            bool ok = !packet.empty();
            if (ok && track.codec == CodedCodec::kOpus) {
                ok = opus_packet_ok(packet, frame_length);
            } else if (ok && track.codec == CodedCodec::kFlac) {
                ok = flac_packet_ok(packet, track, !layout->substreams[s].second.empty());
            }
            if (!ok) {
                return std::unexpected(MuxError::kBadCodedPacket);
            }
        }
    }
    auto trims = frame_trims(frames.size(), frame_length, track.trim_start_samples, track.trim_end_samples);
    if (!trims.has_value()) {
        return std::unexpected(trims.error());
    }

    Sequence sequence;
    sequence.header = {};  // Simple Profile: one Audio Element of at most 16 channels
    sequence.codec_configs.push_back(std::move(config));

    AudioElement element;
    element.audio_element_id = 0;
    element.type = ElementType::kChannelBased;
    element.codec_config_id = kCodecConfigId;
    ChannelLayer layer;
    layer.loudspeaker_layout = track.loudspeaker_layout;
    layer.substream_count = static_cast<std::uint8_t>(layout->substreams.size());
    layer.coupled_substream_count = layout->coupled_substream_count;
    element.layers.push_back(layer);
    for (std::uint32_t id = 0; id < layout->substreams.size(); ++id) {
        element.audio_substream_ids.push_back(id);
    }
    sequence.audio_elements.push_back(std::move(element));

    MixPresentation mix;
    mix.mix_presentation_id = 0;
    SubMix sub_mix;
    SubMixElement mix_element;
    mix_element.audio_element_id = 0;
    mix_element.element_mix_gain = constant_mix_gain(0, track.sample_rate, frame_length);
    sub_mix.elements.push_back(std::move(mix_element));
    sub_mix.output_mix_gain = constant_mix_gain(1, track.sample_rate, frame_length);
    // The Stereo loudness layout every sub-mix carries (Sound System A), then this element's own
    // unless it is Stereo.
    sub_mix.layouts.push_back({{.layout_type = 2, .sound_system = 0}, loudness_data(track.stereo_loudness)});
    if (layout->sound_system != 0) {
        sub_mix.layouts.push_back({{.layout_type = 2, .sound_system = layout->sound_system},
                                   loudness_data(track.layout_loudness)});
    }
    mix.sub_mixes.push_back(std::move(sub_mix));
    sequence.mix_presentations.push_back(std::move(mix));

    sequence.temporal_units.reserve(frames.size());
    for (std::size_t i = 0; i < frames.size(); ++i) {
        TemporalUnit unit;
        unit.has_temporal_delimiter = track.temporal_delimiters;
        for (std::uint32_t id = 0; id < layout->substreams.size(); ++id) {
            AudioFrame frame;
            frame.audio_substream_id = id;
            frame.data = frames[i].substreams[id];
            apply_trim(frame, (*trims)[i]);
            unit.audio_frames.push_back(std::move(frame));
        }
        sequence.temporal_units.push_back(std::move(unit));
    }
    return sequence;
}

std::expected<std::vector<std::byte>, MuxError> mux_coded(const CodedTrack& track, std::span<const CodedFrame> frames) {
    auto sequence = build_coded_sequence(track, frames);
    if (!sequence.has_value()) {
        return std::unexpected(sequence.error());
    }
    auto file = write_isobmff(*sequence, {.writing_app = track.writing_app});
    if (!file.has_value()) {
        return std::unexpected(MuxError::kWriteFailed);
    }
    return std::move(*file);
}

// --- Reading PCM back ---------------------------------------------------------------------------

namespace {

// The `ipcm` Audio Substreams of a scalable channel element as reconstruct_channels() takes them:
// one entry per substream, every frame of it concatenated and none of the trimming applied. A layer
// codes its coupled substreams first (two channels each), then the mono ones.
[[nodiscard]] std::expected<std::vector<SubstreamPcm>, Error> read_ipcm_substreams(const Sequence& sequence,
                                                                                   const AudioElement& element,
                                                                                   const CodecConfig& codec,
                                                                                   const LpcmConfig& lpcm) {
    std::vector<std::size_t> channel_counts;
    for (const ChannelLayer& layer : element.layers) {
        if (layer.coupled_substream_count > layer.substream_count) {
            return std::unexpected(Error::kBadDescriptor);
        }
        for (std::size_t s = 0; s < layer.substream_count; ++s) {
            channel_counts.push_back(s < layer.coupled_substream_count ? 2 : 1);
        }
    }
    if (channel_counts.size() != element.audio_substream_ids.size()) {
        return std::unexpected(Error::kBadDescriptor);
    }
    std::vector<SubstreamPcm> substreams(channel_counts.size());
    for (std::size_t s = 0; s < substreams.size(); ++s) {
        substreams[s].audio_substream_id = element.audio_substream_ids[s];
        substreams[s].channels.resize(channel_counts[s]);
    }
    for (const TemporalUnit& unit : sequence.temporal_units) {
        for (const AudioFrame& frame : unit.audio_frames) {
            const auto it = std::find(element.audio_substream_ids.begin(), element.audio_substream_ids.end(),
                                      frame.audio_substream_id);
            if (it == element.audio_substream_ids.end()) {
                continue;  // another Audio Element's substream
            }
            const auto s = static_cast<std::size_t>(it - element.audio_substream_ids.begin());
            auto planar = detail::decode_ipcm_frame(frame.data, lpcm, channel_counts[s]);
            if (!planar.has_value()) {
                return std::unexpected(planar.error());
            }
            if ((*planar)[0].size() != codec.num_samples_per_frame) {
                return std::unexpected(Error::kBadObu);  // the Codec Config's frame length is every frame's
            }
            for (std::size_t c = 0; c < channel_counts[s]; ++c) {
                substreams[s].channels[c].insert(substreams[s].channels[c].end(), (*planar)[c].begin(),
                                                 (*planar)[c].end());
            }
        }
    }
    return substreams;
}

}  // namespace

std::expected<DecodedElement, Error> decode_pcm(const Sequence& sequence, std::uint32_t audio_element_id) {
    return decode_pcm(sequence, audio_element_id, DecodeOptions{});
}

std::expected<DecodedElement, Error> decode_pcm(const Sequence& sequence, std::uint32_t audio_element_id,
                                                const DecodeOptions& options) {
    const AudioElement* element = nullptr;
    for (const auto& candidate : sequence.audio_elements) {
        if (candidate.audio_element_id == audio_element_id) {
            element = &candidate;
        }
    }
    if (element == nullptr) {
        return std::unexpected(Error::kBadDescriptor);
    }
    const CodecConfig* codec = nullptr;
    for (const auto& candidate : sequence.codec_configs) {
        if (candidate.codec_config_id == element->codec_config_id) {
            codec = &candidate;
        }
    }
    if (codec == nullptr) {
        return std::unexpected(Error::kBadDescriptor);
    }
    if (codec->codec_id != "ipcm" || !codec->lpcm.has_value()) {
        return std::unexpected(Error::kUnsupported);
    }
    const LpcmConfig& lpcm = *codec->lpcm;
    if (lpcm.sample_size != 16 && lpcm.sample_size != 24 && lpcm.sample_size != 32) {
        return std::unexpected(Error::kBadDescriptor);
    }

    // What each substream carries: its channel names, in the order the channels are interleaved.
    std::vector<std::vector<std::string>> substream_channels;
    switch (element->type) {
        case ElementType::kChannelBased: {
            if (element->layers.empty()) {
                return std::unexpected(Error::kUnsupported);
            }
            if (element->layers.size() > 1) {
                // Scalable: read every substream whole and let the reconstruction pick the layer.
                auto substreams = read_ipcm_substreams(sequence, *element, *codec, lpcm);
                if (!substreams.has_value()) {
                    return std::unexpected(substreams.error());
                }
                return reconstruct_channels(sequence, audio_element_id, *substreams, options);
            }
            if (options.layer.has_value() && *options.layer != 0) {
                return std::unexpected(Error::kInvalidArgument);
            }
            const ChannelLayer& layer = element->layers[0];
            const auto layout = layer.loudspeaker_layout == 15 && layer.expanded_loudspeaker_layout.has_value()
                                    ? expanded_layout_info(*layer.expanded_loudspeaker_layout)
                                    : layout_info(layer.loudspeaker_layout);
            if (!layout.has_value() || layout->substreams.size() != element->audio_substream_ids.size()) {
                return std::unexpected(Error::kUnsupported);
            }
            for (const SubstreamChannels& s : layout->substreams) {
                std::vector<std::string> names{std::string(s.first)};
                if (!s.second.empty()) {
                    names.emplace_back(s.second);
                }
                substream_channels.push_back(std::move(names));
            }
            break;
        }
        case ElementType::kObjectBased: {
            if (element->audio_substream_ids.size() != 1 || element->objects.num_objects < 1 ||
                element->objects.num_objects > 2) {
                return std::unexpected(Error::kUnsupported);
            }
            std::vector<std::string> names;
            for (unsigned i = 1; i <= element->objects.num_objects; ++i) {
                names.push_back("Object " + std::to_string(i));
            }
            substream_channels.push_back(std::move(names));
            break;
        }
        case ElementType::kSceneBased: {
            if (element->ambisonics.ambisonics_mode != 0) {
                return std::unexpected(Error::kUnsupported);
            }
            for (std::size_t i = 0; i < element->audio_substream_ids.size(); ++i) {
                substream_channels.push_back({"substream " + std::to_string(i)});
            }
            break;
        }
    }

    DecodedElement decoded;
    decoded.sample_rate = lpcm.sample_rate;
    // Per substream: planar channel buffers.
    std::vector<std::vector<std::vector<float>>> substreams(substream_channels.size());
    for (std::size_t s = 0; s < substreams.size(); ++s) {
        substreams[s].resize(substream_channels[s].size());
    }

    for (const TemporalUnit& unit : sequence.temporal_units) {
        for (const AudioFrame& frame : unit.audio_frames) {
            const auto it = std::find(element->audio_substream_ids.begin(), element->audio_substream_ids.end(),
                                      frame.audio_substream_id);
            if (it == element->audio_substream_ids.end()) {
                continue;  // another Audio Element's substream
            }
            const auto s = static_cast<std::size_t>(it - element->audio_substream_ids.begin());
            const std::size_t channels = substreams[s].size();
            auto planar = detail::decode_ipcm_frame(frame.data, lpcm, channels);
            if (!planar.has_value()) {
                return std::unexpected(planar.error());
            }
            const std::size_t samples = (*planar)[0].size();
            const std::size_t trim_start = std::min<std::size_t>(frame.num_samples_to_trim_at_start, samples);
            const std::size_t trim_end = std::min<std::size_t>(frame.num_samples_to_trim_at_end, samples - trim_start);
            for (std::size_t c = 0; c < channels; ++c) {
                substreams[s][c].insert(substreams[s][c].end(),
                                        (*planar)[c].begin() + static_cast<std::ptrdiff_t>(trim_start),
                                        (*planar)[c].end() - static_cast<std::ptrdiff_t>(trim_end));
            }
        }
    }

    if (element->type == ElementType::kSceneBased) {
        const AmbisonicsConfig& config = element->ambisonics;
        for (std::size_t c = 0; c < config.channel_mapping.size(); ++c) {
            decoded.channel_names.push_back("ACN " + std::to_string(c));
            const std::size_t source = config.channel_mapping[c];
            decoded.channels.push_back(source < substreams.size() ? substreams[source][0] : std::vector<float>{});
        }
        return decoded;
    }
    for (std::size_t s = 0; s < substreams.size(); ++s) {
        for (std::size_t c = 0; c < substreams[s].size(); ++c) {
            decoded.channel_names.push_back(substream_channels[s][c]);
            decoded.channels.push_back(std::move(substreams[s][c]));
        }
    }
    return decoded;
}

}  // namespace iclforge::containers::iamf
