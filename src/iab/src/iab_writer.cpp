#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bitwriter.hpp"
#include "iclforge/iab/writer.hpp"

// The IAB element grammar written out: §7 (IABitstream), §9 (the element syntax tables) and §10
// (the field definitions) of SMPTE ST 2098-2:2022. Each put_* function below is the inverse of
// the parse_* function of the same name in iab_reader.cpp and writes the fields of one syntax
// table in the table's order.

namespace iclforge::iab {

namespace {

using detail::BitWriter;
using Bytes = std::vector<std::byte>;

// §10.1.1 Table 14 - ElementID values.
namespace element_id {
inline constexpr std::uint32_t kIaFrame = 0x08;
inline constexpr std::uint32_t kBedDefinition = 0x10;
inline constexpr std::uint32_t kBedRemap = 0x20;
inline constexpr std::uint32_t kObjectDefinition = 0x40;
inline constexpr std::uint32_t kObjectZoneDefinition19 = 0x80;
inline constexpr std::uint32_t kAuthoringToolInfo = 0x100;
inline constexpr std::uint32_t kUserData = 0x101;
inline constexpr std::uint32_t kAudioDataDlc = 0x200;
inline constexpr std::uint32_t kAudioDataPcm = 0x400;
}  // namespace element_id

// §5.2: the largest symbol Plex can carry.
constexpr std::uint64_t kMaxPlexSymbol = 0xFFFFFFFEULL;

// §9 Table 3: ElementID and ElementSize, then the payload.
[[nodiscard]] std::expected<Bytes, WriteError> wrap_element(std::uint32_t id, std::span<const std::byte> payload) {
    if (payload.size() > kMaxPlexSymbol) {
        return std::unexpected(WriteError::kBadElement);
    }
    BitWriter bw;
    bw.put_plex(id, 8);
    bw.put_plex(payload.size(), 8);
    bw.put_bytes(payload);
    return bw.take();
}

// §5.5 inverse: the 10-bit gain code for a linear gain in (0, 1).
[[nodiscard]] std::uint32_t gain_code(double gain) {
    const double code = std::round(-64.0 * std::log2(gain));
    return static_cast<std::uint32_t>(std::clamp(code, 0.0, 1023.0));
}

// Inverse of read_unity_gain(): Table 20, prefix 0 is unity, 1 is mute, otherwise the 10-bit code.
void put_unity_gain(BitWriter& bw, double gain) {
    if (!(gain < 1.0)) {
        bw.put_bits(0, 2);  // unity; also what a gain above 1 becomes
        return;
    }
    if (!(gain > 0.0)) {
        bw.put_bits(1, 2);  // mute
        return;
    }
    const std::uint32_t code = gain_code(gain);
    if (code >= 0x3FF) {
        bw.put_bits(1, 2);  // 0x3FF means multiply by zero, the same as the mute prefix
        return;
    }
    bw.put_bits(2, 2);
    bw.put_bits(code, 10);
}

// Inverse of read_zone_gain(): Table 25, prefix 0 is mute, 1 is unity, otherwise ZoneGain =
// gain * (2^10 - 1) (§10.5.14, §10.6.3).
void put_zone_gain(BitWriter& bw, double gain) {
    if (!(gain > 0.0)) {
        bw.put_bits(0, 2);
        return;
    }
    if (!(gain < 1.0)) {
        bw.put_bits(1, 2);
        return;
    }
    bw.put_bits(2, 2);
    bw.put_bits(static_cast<std::uint64_t>(std::round(gain * 1023.0)), 10);
}

// Inverse of read_decor(): Table 21 and Table 27, prefix 0 is none, 1 is maximum, otherwise
// code / 255.
void put_decor(BitWriter& bw, double decor) {
    if (!(decor > 0.0)) {
        bw.put_bits(0, 2);
        return;
    }
    if (!(decor < 1.0)) {
        bw.put_bits(1, 2);
        return;
    }
    bw.put_bits(2, 2);
    bw.put_bits(static_cast<std::uint64_t>(std::round(decor * 255.0)), 8);
}

// §5.4 DistanceXY inverse for an n-bit field. Valid codes run 2^(n-1) - 1 to 2^n - 1.
[[nodiscard]] std::uint64_t encode_distance_xy(double value, unsigned n) {
    const double half = static_cast<double>(std::uint64_t{1} << (n - 1));
    const double code = std::round(value * half + (half - 1.0));
    return static_cast<std::uint64_t>(std::clamp(code, half - 1.0, 2.0 * half - 1.0));
}

// §5.4 DistanceZ inverse for an n-bit field.
[[nodiscard]] std::uint64_t encode_distance_z(double value, unsigned n) {
    const double max = static_cast<double>((std::uint64_t{1} << n) - 1);
    return static_cast<std::uint64_t>(std::clamp(std::round(value * max), 0.0, max));
}

// §10.3.12 / Table 22 and the AudioDescriptionText that follows when bit 7 is set.
[[nodiscard]] std::expected<void, WriteError> put_audio_description(BitWriter& bw, const AudioDescription& desc) {
    unsigned byte = 0;
    byte |= desc.not_indicated ? 0x01U : 0U;
    byte |= desc.dialog ? 0x02U : 0U;
    byte |= desc.music ? 0x04U : 0U;
    byte |= desc.effects ? 0x08U : 0U;
    byte |= desc.foley ? 0x10U : 0U;
    byte |= desc.ambience ? 0x20U : 0U;
    if (desc.text.has_value()) {
        if (desc.text->find('\0') != std::string::npos) {
            return std::unexpected(WriteError::kBadElement);
        }
        byte |= 0x80U;
    }
    bw.put_bits(byte, 8);
    if (desc.text.has_value()) {
        for (const char c : *desc.text) {
            bw.put_bits(static_cast<std::uint8_t>(c), 8);
        }
        bw.put_bits(0, 8);
    }
    return {};
}

[[nodiscard]] std::expected<Bytes, WriteError> write_bed_definition(const BedDefinition& bed,
                                                                     std::uint8_t frame_rate_code);
[[nodiscard]] std::expected<Bytes, WriteError> write_object_definition(const ObjectDefinition& object,
                                                                        std::uint8_t frame_rate_code);

// §9.3 Table 7.
[[nodiscard]] std::expected<Bytes, WriteError> write_bed_remap(const BedRemap& remap, unsigned num_sub_blocks) {
    if (remap.sub_blocks.size() != num_sub_blocks) {
        return std::unexpected(WriteError::kSubBlockCount);
    }
    BitWriter bw;
    bw.put_plex(remap.meta_id, 8);
    bw.put_bits(remap.use_case, 8);
    bw.put_plex(remap.source_channels, 4);
    bw.put_plex(remap.destination_channels, 4);

    for (unsigned sb = 0; sb < num_sub_blocks; ++sb) {
        const auto& block = remap.sub_blocks[sb];
        if (sb != 0) {
            bw.put_bits(block.has_remap_info ? 1U : 0U, 1);
        }
        const bool present = sb == 0 || block.has_remap_info;
        if (!present) {
            continue;
        }
        if (block.destination_channel_ids.size() != remap.destination_channels ||
            block.gains.size() != remap.destination_channels) {
            return std::unexpected(WriteError::kBadElement);
        }
        for (std::uint32_t o_chan = 0; o_chan < remap.destination_channels; ++o_chan) {
            bw.put_plex(block.destination_channel_ids[o_chan], 4);
            if (block.gains[o_chan].size() != remap.source_channels) {
                return std::unexpected(WriteError::kBadElement);
            }
            for (const double gain : block.gains[o_chan]) {
                put_unity_gain(bw, gain);
            }
        }
    }
    bw.align();
    bw.put_plex(0, 8);  // Reserved, set to 0
    return bw.take();
}

// §9.2 Table 6.
std::expected<Bytes, WriteError> write_bed_definition(const BedDefinition& bed, std::uint8_t frame_rate_code) {
    const auto sub_blocks = num_pan_sub_blocks(frame_rate_code);
    if (!sub_blocks.has_value()) {
        return std::unexpected(WriteError::kReservedFrameRate);
    }

    BitWriter bw;
    bw.put_plex(bed.meta_id, 8);
    bw.put_bits(bed.activation.conditional ? 1U : 0U, 1);
    if (bed.activation.conditional) {
        bw.put_bits(bed.activation.use_case, 8);
    }

    bw.put_plex(bed.channels.size(), 4);
    for (const auto& channel : bed.channels) {
        bw.put_plex(channel.channel_id, 4);
        bw.put_plex(channel.audio_data_id, 8);
        put_unity_gain(bw, channel.gain);
        bw.put_bits(channel.decorrelation.has_value() ? 1U : 0U, 1);
        if (channel.decorrelation.has_value()) {
            bw.put_bits(0, 4);  // Reserved, set to 0
            put_decor(bw, *channel.decorrelation);
        }
    }
    bw.put_bits(0x180, 10);  // Reserved, set to 0x180
    bw.align();

    if (auto status = put_audio_description(bw, bed.description); !status.has_value()) {
        return std::unexpected(status.error());
    }

    std::vector<Bytes> children;
    for (const auto& child : bed.beds) {
        auto payload = write_bed_definition(child, frame_rate_code);
        if (!payload.has_value()) {
            return std::unexpected(payload.error());
        }
        auto element = wrap_element(element_id::kBedDefinition, *payload);
        if (!element.has_value()) {
            return std::unexpected(element.error());
        }
        children.push_back(std::move(*element));
    }
    for (const auto& remap : bed.remaps) {
        auto payload = write_bed_remap(remap, *sub_blocks);
        if (!payload.has_value()) {
            return std::unexpected(payload.error());
        }
        auto element = wrap_element(element_id::kBedRemap, *payload);
        if (!element.has_value()) {
            return std::unexpected(element.error());
        }
        children.push_back(std::move(*element));
    }
    bw.put_plex(children.size(), 8);
    for (const auto& child : children) {
        bw.put_bytes(child);
    }
    return bw.take();
}

// §9.5 Table 9.
[[nodiscard]] std::expected<Bytes, WriteError> write_zone19(const ObjectZoneDefinition19& zone19,
                                                             unsigned num_sub_blocks) {
    if (zone19.sub_blocks.size() != num_sub_blocks) {
        return std::unexpected(WriteError::kSubBlockCount);
    }
    BitWriter bw;
    for (unsigned sb = 0; sb < num_sub_blocks; ++sb) {
        const auto& block = zone19.sub_blocks[sb];
        if (sb != 0) {
            bw.put_bits(block.has_zone_info ? 1U : 0U, 1);
        }
        if (sb == 0 || block.has_zone_info) {
            for (const double gain : block.zone_gains) {
                put_zone_gain(bw, gain);
            }
        }
    }
    return bw.take();
}

// §9.4 Table 8, one sub block's pan information.
void put_pan_info(BitWriter& bw, const ObjectPanSubBlock& block) {
    put_unity_gain(bw, block.gain);
    bw.put_bits(0x1, 3);  // Reserved, set to 0b001

    bw.put_bits(encode_distance_xy(block.position.x, 16), 16);
    bw.put_bits(encode_distance_xy(block.position.y, 16), 16);
    bw.put_bits(encode_distance_z(block.position.z, 16), 16);

    bw.put_bits(block.snap ? 1U : 0U, 1);
    if (block.snap) {
        bw.put_bits(block.snap_tolerance.has_value() ? 1U : 0U, 1);
        if (block.snap_tolerance.has_value()) {
            bw.put_bits(encode_distance_z(*block.snap_tolerance, 12), 12);
        }
        bw.put_bits(0, 1);  // Res2, set to 0
    }

    bw.put_bits(block.zone_gains.has_value() ? 1U : 0U, 1);
    if (block.zone_gains.has_value()) {
        for (const double gain : *block.zone_gains) {
            put_zone_gain(bw, gain);
        }
    }

    bw.put_bits(static_cast<unsigned>(block.spread.mode), 2);
    switch (block.spread.mode) {
        case ObjectSpreadMode::kLowRez:
            bw.put_bits(encode_distance_z(block.spread.x, 8), 8);
            break;
        case ObjectSpreadMode::kNone:
            break;
        case ObjectSpreadMode::kOneD:
            bw.put_bits(encode_distance_z(block.spread.x, 12), 12);
            break;
        case ObjectSpreadMode::kThreeD:
            bw.put_bits(encode_distance_z(block.spread.x, 12), 12);
            bw.put_bits(encode_distance_z(block.spread.y, 12), 12);
            bw.put_bits(encode_distance_z(block.spread.z, 12), 12);
            break;
    }

    bw.put_bits(0, 4);  // Reserved, set to 0
    put_decor(bw, block.decorrelation);
}

// §9.4 Table 8.
std::expected<Bytes, WriteError> write_object_definition(const ObjectDefinition& object,
                                                          std::uint8_t frame_rate_code) {
    const auto sub_blocks = num_pan_sub_blocks(frame_rate_code);
    if (!sub_blocks.has_value()) {
        return std::unexpected(WriteError::kReservedFrameRate);
    }
    if (object.sub_blocks.size() != *sub_blocks) {
        return std::unexpected(WriteError::kSubBlockCount);
    }

    BitWriter bw;
    bw.put_plex(object.meta_id, 8);
    bw.put_plex(object.audio_data_id, 8);
    bw.put_bits(object.activation.conditional ? 1U : 0U, 1);
    if (object.activation.conditional) {
        bw.put_bits(1, 1);  // Reserved, set to 1
        bw.put_bits(object.activation.use_case, 8);
    }
    bw.put_bits(0, 1);  // Reserved, set to 0

    for (unsigned sb = 0; sb < *sub_blocks; ++sb) {
        const auto& block = object.sub_blocks[sb];
        if (sb != 0) {
            bw.put_bits(block.has_pan_info ? 1U : 0U, 1);
        }
        if (sb == 0 || block.has_pan_info) {
            put_pan_info(bw, block);
        }
    }
    bw.align();

    if (auto status = put_audio_description(bw, object.description); !status.has_value()) {
        return std::unexpected(status.error());
    }

    std::vector<Bytes> children;
    for (const auto& child : object.objects) {
        auto payload = write_object_definition(child, frame_rate_code);
        if (!payload.has_value()) {
            return std::unexpected(payload.error());
        }
        auto element = wrap_element(element_id::kObjectDefinition, *payload);
        if (!element.has_value()) {
            return std::unexpected(element.error());
        }
        children.push_back(std::move(*element));
    }
    if (object.zone19.has_value()) {
        auto payload = write_zone19(*object.zone19, *sub_blocks);
        if (!payload.has_value()) {
            return std::unexpected(payload.error());
        }
        auto element = wrap_element(element_id::kObjectZoneDefinition19, *payload);
        if (!element.has_value()) {
            return std::unexpected(element.error());
        }
        children.push_back(std::move(*element));
    }
    bw.put_plex(children.size(), 8);
    for (const auto& child : children) {
        bw.put_bytes(child);
    }
    return bw.take();
}

// §9.6 Table 10's envelope: AudioDataID, DLCSize, then the coded bytes.
[[nodiscard]] std::expected<Bytes, WriteError> write_audio_data_dlc(const AudioDataDlc& dlc) {
    if (dlc.coded.size() > 0xFFFFU) {
        return std::unexpected(WriteError::kDlcTooLarge);
    }
    BitWriter bw;
    bw.put_plex(dlc.audio_data_id, 8);
    bw.put_bits(dlc.coded.size(), 16);
    bw.put_bytes(dlc.coded);
    return bw.take();
}

// §9.7 Table 11 / §10.8.1: little-endian PCMData at the frame's bit depth.
[[nodiscard]] std::expected<Bytes, WriteError> write_audio_data_pcm(const AudioDataPcm& pcm, const IaFrame& frame,
                                                                     std::uint32_t expected_samples) {
    if (pcm.samples.size() != expected_samples) {
        return std::unexpected(WriteError::kSampleCount);
    }
    BitWriter bw;
    bw.put_plex(pcm.audio_data_id, 8);
    Bytes bytes = bw.take();

    const auto full_scale = static_cast<double>(std::uint32_t{1} << (frame.bit_depth - 1));
    for (const float sample : pcm.samples) {
        const double scaled = std::round(static_cast<double>(sample) * full_scale);
        const auto raw = static_cast<std::int32_t>(std::clamp(scaled, -full_scale, full_scale - 1.0));
        const auto bits = static_cast<std::uint32_t>(raw);
        for (unsigned b = 0; b < frame.bit_depth / 8; ++b) {
            bytes.push_back(static_cast<std::byte>((bits >> (8 * b)) & 0xFFU));
        }
    }
    return bytes;
}

void put_be32(Bytes& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
    }
}

}  // namespace

std::string_view describe(WriteError error) {
    switch (error) {
        case WriteError::kCannotOpen:
            return "could not open the output";
        case WriteError::kBadSampleRate:
            return "IaFrame::sample_rate is not 48000 or 96000";
        case WriteError::kBadBitDepth:
            return "IaFrame::bit_depth is not 16 or 24";
        case WriteError::kReservedFrameRate:
            return "frame_rate_code is Reserved (0xA-0xF)";
        case WriteError::kSubBlockCount:
            return "a sub block list is not NumPanSubBlocks long for the frame rate";
        case WriteError::kSampleCount:
            return "an audio element does not hold SampleCount samples for the frame rate and sample rate";
        case WriteError::kBadElement:
            return "a field is outside what its syntax can carry";
        case WriteError::kDlcTooLarge:
            return "an AudioDataDLC element is larger than its 16-bit DLCSize can describe";
        case WriteError::kDlcFrameRate:
            return "AudioDataDLC cannot be used at this frame rate";
    }
    return "unknown iab writer error";
}

std::expected<Bytes, WriteError> write_iaframe(const IaFrame& frame) {
    unsigned sample_rate_code = 0;
    if (frame.sample_rate == 48000) {
        sample_rate_code = 0;
    } else if (frame.sample_rate == 96000) {
        sample_rate_code = 1;
    } else {
        return std::unexpected(WriteError::kBadSampleRate);
    }
    unsigned bit_depth_code = 0;
    if (frame.bit_depth == 16) {
        bit_depth_code = 0;
    } else if (frame.bit_depth == 24) {
        bit_depth_code = 1;
    } else {
        return std::unexpected(WriteError::kBadBitDepth);
    }
    const auto samples_per_frame = sample_count(frame.frame_rate_code, frame.sample_rate == 96000);
    if (!samples_per_frame.has_value()) {
        return std::unexpected(WriteError::kReservedFrameRate);
    }

    std::vector<Bytes> children;
    const auto add_child = [&children](std::uint32_t id, const std::expected<Bytes, WriteError>& payload)
        -> std::expected<void, WriteError> {
        if (!payload.has_value()) {
            return std::unexpected(payload.error());
        }
        auto element = wrap_element(id, *payload);
        if (!element.has_value()) {
            return std::unexpected(element.error());
        }
        children.push_back(std::move(*element));
        return {};
    };

    for (const auto& bed : frame.beds) {
        if (auto status = add_child(element_id::kBedDefinition, write_bed_definition(bed, frame.frame_rate_code));
            !status.has_value()) {
            return std::unexpected(status.error());
        }
    }
    for (const auto& object : frame.objects) {
        if (auto status =
                add_child(element_id::kObjectDefinition, write_object_definition(object, frame.frame_rate_code));
            !status.has_value()) {
            return std::unexpected(status.error());
        }
    }
    for (const auto& dlc : frame.audio_dlc) {
        if (auto status = add_child(element_id::kAudioDataDlc, write_audio_data_dlc(dlc)); !status.has_value()) {
            return std::unexpected(status.error());
        }
    }
    for (const auto& pcm : frame.audio_pcm) {
        if (auto status = add_child(element_id::kAudioDataPcm, write_audio_data_pcm(pcm, frame, *samples_per_frame));
            !status.has_value()) {
            return std::unexpected(status.error());
        }
    }
    if (frame.authoring_tool.has_value()) {
        Bytes uri;
        for (const char c : frame.authoring_tool->uri) {
            uri.push_back(static_cast<std::byte>(c));
        }
        uri.push_back(std::byte{0});
        if (auto status = add_child(element_id::kAuthoringToolInfo, uri); !status.has_value()) {
            return std::unexpected(status.error());
        }
    }
    for (const auto& user : frame.user_data) {
        Bytes payload(user.user_id.begin(), user.user_id.end());
        payload.insert(payload.end(), user.data.begin(), user.data.end());
        if (auto status = add_child(element_id::kUserData, payload); !status.has_value()) {
            return std::unexpected(status.error());
        }
    }

    BitWriter bw;
    bw.put_bits(1, 8);  // Version (§10.2.1)
    bw.put_bits(sample_rate_code, 2);
    bw.put_bits(bit_depth_code, 2);
    bw.put_bits(frame.frame_rate_code, 4);
    bw.put_plex(frame.max_rendered, 8);
    bw.align();
    bw.put_plex(children.size(), 8);
    for (const auto& child : children) {
        bw.put_bytes(child);
    }
    return bw.take();
}

std::expected<Bytes, WriteError> write_iabitstream(std::span<const IABitstreamFrame> frames) {
    Bytes out;
    for (const auto& entry : frames) {
        auto payload = write_iaframe(entry.frame);
        if (!payload.has_value()) {
            return std::unexpected(payload.error());
        }
        auto element = wrap_element(element_id::kIaFrame, *payload);
        if (!element.has_value()) {
            return std::unexpected(element.error());
        }
        if (entry.preamble.size() > 0xFFFFFFFFULL || element->size() > 0xFFFFFFFFULL) {
            return std::unexpected(WriteError::kBadElement);
        }

        out.push_back(std::byte{0x01});  // PreambleTag (§8.1.1)
        put_be32(out, static_cast<std::uint32_t>(entry.preamble.size()));
        out.insert(out.end(), entry.preamble.begin(), entry.preamble.end());
        out.push_back(std::byte{0x02});  // IAFrameTag (§8.1.4)
        put_be32(out, static_cast<std::uint32_t>(element->size()));
        out.insert(out.end(), element->begin(), element->end());
    }
    return out;
}

std::expected<void, WriteError> write_iabitstream(const std::string& path, std::span<const IABitstreamFrame> frames) {
    auto bytes = write_iabitstream(frames);
    if (!bytes.has_value()) {
        return std::unexpected(bytes.error());
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return std::unexpected(WriteError::kCannotOpen);
    }
    out.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
    if (!out) {
        return std::unexpected(WriteError::kCannotOpen);
    }
    return {};
}

}  // namespace iclforge::iab
