#include "iclforge/truehd/io/major_sync.hpp"

#include <algorithm>
#include <array>

#include "core/bit_io.hpp"
#include "iclforge/base/bitreader.hpp"
#include "iclforge/base/bitwriter.hpp"
#include "iclforge/truehd/core/crc.hpp"
#include "io/channel_meaning_fields.hpp"

namespace iclforge::truehd {

namespace {

using detail::put_field;
using detail::put_signed;
using detail::read_signed;

// TH spec §14.2 (v3.9): the extension's body is (extra_channel_meaning_length + 1) 16-bit words, at
// most 32 bytes; the CRC search goes this far past the base form (the nibble in front of the body
// takes one more word, and the CRC two bytes of their own).
constexpr std::size_t kMaxExtensionBytes = 36;

[[nodiscard]] bool has_extended_substream(const MajorSync& major_sync) noexcept {
    return major_sync.extended_substream_info != 0;
}

[[nodiscard]] ChannelMeaning read_channel_meaning(BitReader& reader) {
    ChannelMeaning meaning;
    meaning.heavy_drc_start_up_gain = static_cast<std::int8_t>(read_signed(reader, 6));
    meaning.twoch_control_enabled = reader.read(1) != 0;
    meaning.sixch_control_enabled = reader.read(1) != 0;
    meaning.eightch_control_enabled = reader.read(1) != 0;
    meaning.reserved_after_controls = reader.read(1) != 0;
    meaning.drc_start_up_gain = static_cast<std::int8_t>(read_signed(reader, 7));
    meaning.twoch_dialogue_norm = static_cast<std::uint8_t>(reader.read(6));
    meaning.twoch_mix_level = static_cast<std::uint8_t>(reader.read(6));
    meaning.sixch_dialogue_norm = static_cast<std::uint8_t>(reader.read(5));
    meaning.sixch_mix_level = static_cast<std::uint8_t>(reader.read(6));
    meaning.sixch_source_format = static_cast<std::uint8_t>(reader.read(5));
    meaning.eightch_dialogue_norm = static_cast<std::uint8_t>(reader.read(5));
    meaning.eightch_mix_level = static_cast<std::uint8_t>(reader.read(6));
    meaning.eightch_source_format = static_cast<std::uint8_t>(reader.read(6));
    meaning.reserved_before_extra = reader.read(1) != 0;
    meaning.extra_present = reader.read(1) != 0;
    return meaning;
}

void write_channel_meaning(BitWriter& writer, const ChannelMeaning& meaning) {
    put_signed(writer, meaning.heavy_drc_start_up_gain, 6);
    put_field(writer, meaning.twoch_control_enabled ? 1U : 0U, 1);
    put_field(writer, meaning.sixch_control_enabled ? 1U : 0U, 1);
    put_field(writer, meaning.eightch_control_enabled ? 1U : 0U, 1);
    put_field(writer, meaning.reserved_after_controls ? 1U : 0U, 1);
    put_signed(writer, meaning.drc_start_up_gain, 7);
    put_field(writer, meaning.twoch_dialogue_norm, 6);
    put_field(writer, meaning.twoch_mix_level, 6);
    put_field(writer, meaning.sixch_dialogue_norm, 5);
    put_field(writer, meaning.sixch_mix_level, 6);
    put_field(writer, meaning.sixch_source_format, 5);
    put_field(writer, meaning.eightch_dialogue_norm, 5);
    put_field(writer, meaning.eightch_mix_level, 6);
    put_field(writer, meaning.eightch_source_format, 6);
    put_field(writer, meaning.reserved_before_extra ? 1U : 0U, 1);
    put_field(writer, meaning.extra_present ? 1U : 0U, 1);
}

// TH spec §14.2. The fields that follow extra_channel_meaning_length exist only when the stream has
// an extended substream.
[[nodiscard]] ExtraChannelMeaning read_extra(BitReader& reader, bool extended_substream) {
    ExtraChannelMeaning extra;
    extra.length = static_cast<std::uint8_t>(reader.read(4));
    if (!extended_substream) {
        return extra;
    }
    extra.sixteench_dialogue_norm = static_cast<std::uint8_t>(reader.read(5));
    extra.sixteench_mix_level = static_cast<std::uint8_t>(reader.read(6));
    extra.sixteench_channel_count = static_cast<std::uint8_t>(reader.read(5));
    extra.dyn_object_only = reader.read(1) != 0;
    if (extra.dyn_object_only) {
        extra.lfe_present = reader.read(1) != 0;
        return extra;
    }
    extra.content_description = static_cast<std::uint8_t>(reader.read(4));
    if ((extra.content_description & 1U) != 0) {
        extra.chan_distribute = reader.read(1) != 0;
        extra.lfe_only = reader.read(1) != 0;
        if (!extra.lfe_only) {
            extra.channel_assignment = static_cast<std::uint16_t>(reader.read(10));
        }
    }
    if ((extra.content_description & 2U) != 0) {
        extra.isf = static_cast<std::uint8_t>(reader.read(3));
    }
    if ((extra.content_description & 4U) != 0) {
        extra.dynamic_object_count = static_cast<std::uint8_t>(reader.read(5));
    }
    return extra;
}

[[nodiscard]] std::uint16_t stored_crc(std::span<const std::byte> data, std::size_t size) noexcept {
    return static_cast<std::uint16_t>((std::to_integer<unsigned>(data[size - 2]) << 8U) |
                                      std::to_integer<unsigned>(data[size - 1]));
}

}  // namespace

std::string_view channel_meaning_problem(const ChannelMeaning& meaning) noexcept {
    using namespace detail;
    const struct {
        const MeaningField& field;
        int value;
    } fields[] = {
        {kHeavyDrcStartUpGain, meaning.heavy_drc_start_up_gain},
        {kDrcStartUpGain, meaning.drc_start_up_gain},
        {kTwochDialogueNorm, meaning.twoch_dialogue_norm},
        {kTwochMixLevel, meaning.twoch_mix_level},
        {kSixchDialogueNorm, meaning.sixch_dialogue_norm},
        {kSixchMixLevel, meaning.sixch_mix_level},
        {kSixchSourceFormat, meaning.sixch_source_format},
        {kEightchDialogueNorm, meaning.eightch_dialogue_norm},
        {kEightchMixLevel, meaning.eightch_mix_level},
        {kEightchSourceFormat, meaning.eightch_source_format},
    };
    for (const auto& entry : fields) {
        if (!entry.field.holds(entry.value)) {
            return entry.field.problem;
        }
    }
    return {};
}

std::array<std::uint8_t, 8> pack_channel_meaning(const ChannelMeaning& meaning) {
    BitWriter writer;
    write_channel_meaning(writer, meaning);
    const std::vector<std::byte> bytes = writer.take();
    std::array<std::uint8_t, 8> packed{};
    for (std::size_t i = 0; i < packed.size() && i < bytes.size(); ++i) {
        packed[i] = std::to_integer<std::uint8_t>(bytes[i]);
    }
    return packed;
}

bool starts_with_major_sync(std::span<const std::byte> data) noexcept {
    return data.size() >= 4 && std::to_integer<std::uint8_t>(data[0]) == kMajorSyncByte0 &&
           std::to_integer<std::uint8_t>(data[1]) == kMajorSyncByte1 &&
           std::to_integer<std::uint8_t>(data[2]) == kMajorSyncByte2 &&
           (std::to_integer<std::uint8_t>(data[3]) == kTrueHdStreamByte ||
            std::to_integer<std::uint8_t>(data[3]) == kMlpStreamByte);
}

std::expected<MajorSync, Error> parse_major_sync(std::span<const std::byte> data, bool verify_crc) {
    if (data.size() < kMajorSyncBaseBytes) {
        return std::unexpected(Error{ErrorKind::kTruncated, "major sync"});
    }
    if (!starts_with_major_sync(data)) {
        return std::unexpected(Error{ErrorKind::kBadSync, "major sync word"});
    }
    MajorSync major_sync;
    major_sync.kind = std::to_integer<std::uint8_t>(data[3]) == kTrueHdStreamByte
                          ? StreamKind::kTrueHd
                          : StreamKind::kMlp;

    BitReader reader(data);
    reader.skip(32);
    major_sync.format_info = reader.read(32);
    major_sync.signature = static_cast<std::uint16_t>(reader.read(16));
    major_sync.flags = static_cast<std::uint16_t>(reader.read(16));
    major_sync.reserved = static_cast<std::uint16_t>(reader.read(16));
    major_sync.is_vbr = reader.read(1) != 0;
    major_sync.peak_data_rate = static_cast<std::uint16_t>(reader.read(15));
    major_sync.num_substreams = static_cast<std::uint8_t>(reader.read(4));
    // TH spec §4.1: "2+2 bits (low 2 bits used, upper 2 reserved)": the reserved pair comes first
    // and extended_substream_info is the low pair (ERRATA.md entry 47, the owner's Blu-ray: 0x43
    // for four substreams with the 16-channel extension).
    major_sync.extended_reserved = static_cast<std::uint8_t>(reader.read(2));
    major_sync.extended_substream_info = static_cast<std::uint8_t>(reader.read(2));
    major_sync.substream_info = static_cast<std::uint8_t>(reader.read(8));
    if (major_sync.signature != kSignature) {
        return std::unexpected(
            Error{ErrorKind::kBadSignature, "major sync signature is not 0xB752"});
    }
    if (major_sync.num_substreams < 1 || major_sync.num_substreams > kMaxSubstreams) {
        return std::unexpected(
            Error{ErrorKind::kBadParameter, "num_substreams outside 1..4 (TH spec §4.1)"});
    }

    // The 8 bytes that follow: a TrueHD stream's channel meaning, bytes of unknown meaning in an
    // MLP stream's.
    for (std::uint8_t& byte : major_sync.channel_meaning_bytes) {
        byte = static_cast<std::uint8_t>(reader.read(8));
    }
    if (major_sync.kind == StreamKind::kTrueHd) {
        BitReader meaning_reader(data.subspan(18, 8));
        major_sync.channel_meaning = read_channel_meaning(meaning_reader);
    }

    // The size: 28 bytes, or more when the channel meaning goes on with the 16-channel fields.
    major_sync.size_bytes = kMajorSyncBaseBytes;
    const bool extended =
        major_sync.kind == StreamKind::kTrueHd && major_sync.channel_meaning.extra_present;
    if (extended) {
        std::size_t chosen = 0;
        std::optional<ExtraChannelMeaning> chosen_extra;
        std::size_t fallback = 0;
        std::optional<ExtraChannelMeaning> fallback_extra;
        // TH spec §14.2 (v3.9): the body is extra_channel_meaning_length + 1 words. The extension
        // starts at byte 26 with the 4-bit length; the body is either those words with the length
        // nibble inside the first, or the words after the nibble, which takes one more word.
        const std::size_t length_words =
            data.size() > 26 ? (std::to_integer<std::size_t>(data[26]) >> 4U) + 1U : 0;
        // The extra fields start at byte 26, where the base form has its CRC. The CRC follows them,
        // on a 16-bit boundary: try the two sizes the length gives, then each size from the base
        // form's 28 bytes (an extension of nothing) upward.
        std::array<std::size_t, 2 + (kMaxExtensionBytes / 2 + 1)> candidates{};
        std::size_t candidate_count = 0;
        candidates[candidate_count++] = 26 + 2 * length_words + 2;
        candidates[candidate_count++] = 26 + 2 * (length_words + 1) + 2;
        for (std::size_t size = kMajorSyncBaseBytes;
             size <= kMajorSyncBaseBytes + kMaxExtensionBytes; size += 2) {
            candidates[candidate_count++] = size;
        }
        for (std::size_t candidate = 0; candidate < candidate_count; ++candidate) {
            const std::size_t size = candidates[candidate];
            if (size < kMajorSyncBaseBytes || data.size() < size) {
                continue;
            }
            BitReader extra_reader(data.subspan(26, size - 28 + 2));
            const ExtraChannelMeaning extra =
                read_extra(extra_reader, has_extended_substream(major_sync));
            const std::size_t extra_bytes = (extra_reader.bit_position() + 7U) / 8U;
            if (extra_bytes + 2U > size - 26U) {
                continue;  // the fields do not fit before the CRC
            }
            if (fallback == 0 || size < fallback) {
                fallback = size;
                fallback_extra = extra;
            }
            if (major_sync_crc(data.first(size - 2)) == stored_crc(data, size)) {
                chosen = size;
                chosen_extra = extra;
                if (chosen_extra) {
                    chosen_extra->nibble_in_body = size == 26 + 2 * length_words + 2;
                    chosen_extra->size_from_length =
                        chosen_extra->nibble_in_body || size == 26 + 2 * (length_words + 1) + 2;
                }
                break;
            }
        }
        if (chosen == 0) {
            if (verify_crc || fallback == 0) {
                return std::unexpected(
                    Error{ErrorKind::kBadMajorSyncCrc, "major sync CRC (with the extension)"});
            }
            chosen = fallback;
            chosen_extra = fallback_extra;
        }
        major_sync.size_bytes = chosen;
        major_sync.extra = chosen_extra;
    } else if (verify_crc && major_sync_crc(data.first(kMajorSyncBaseBytes - 2)) !=
                                 stored_crc(data, kMajorSyncBaseBytes)) {
        return std::unexpected(Error{ErrorKind::kBadMajorSyncCrc, "major sync CRC"});
    }
    major_sync.crc = stored_crc(data, major_sync.size_bytes);
    return major_sync;
}

std::string_view extra_channel_meaning_problem(const ExtraChannelMeaning& extra) noexcept {
    if (extra.sixteench_dialogue_norm > 31) {
        return "sixteench_dialogue_norm is 5 bits: 0 to 31 (TH spec §14.2)";
    }
    if (extra.sixteench_mix_level > 63) {
        return "sixteench_mix_level is 6 bits: 0 to 63 (TH spec §14.2)";
    }
    if (extra.sixteench_channel_count > 31) {
        return "sixteench_channel_count is 5 bits: 0 to 31 (TH spec §14.2)";
    }
    if (extra.dyn_object_only) {
        return {};
    }
    if (extra.content_description > 15) {
        return "sixteench_content_description is 4 bits: 0 to 15 (TH spec §14.2)";
    }
    if (extra.channel_assignment > 1023) {
        return "sixteench_channel_assignment is 10 bits: 0 to 1023 (TH spec §14.2)";
    }
    if (extra.isf > 7) {
        return "sixteench_isf is 3 bits: 0 to 7 (TH spec §14.2)";
    }
    if (extra.dynamic_object_count > 31) {
        return "sixteench_dynamic_object_count is 5 bits: 0 to 31 (TH spec §14.2)";
    }
    return {};
}

namespace {

// TH spec §14.2, as read_extra reads it: the 4 bit length, the fields, and zero bits up to the
// (length + 1) 16-bit words the length names, the length counting the nibble itself. The owner's
// discs have length 1 with the 18 bits of a dynamic-object-only programme and 10 zero bits after
// them (planning/truehd-disc-findings.md, question 2).
void write_extra(BitWriter& writer, const ExtraChannelMeaning& extra) {
    BitWriter body;
    put_field(body, extra.sixteench_dialogue_norm, 5);
    put_field(body, extra.sixteench_mix_level, 6);
    put_field(body, extra.sixteench_channel_count, 5);
    put_field(body, extra.dyn_object_only ? 1U : 0U, 1);
    if (extra.dyn_object_only) {
        put_field(body, extra.lfe_present ? 1U : 0U, 1);
    } else {
        put_field(body, extra.content_description, 4);
        if ((extra.content_description & 1U) != 0) {
            put_field(body, extra.chan_distribute ? 1U : 0U, 1);
            put_field(body, extra.lfe_only ? 1U : 0U, 1);
            if (!extra.lfe_only) {
                put_field(body, extra.channel_assignment, 10);
            }
        }
        if ((extra.content_description & 2U) != 0) {
            put_field(body, extra.isf, 3);
        }
        if ((extra.content_description & 4U) != 0) {
            put_field(body, extra.dynamic_object_count, 5);
        }
    }
    const std::size_t bits = 4 + body.bit_count();
    const std::size_t words = (bits + 15) / 16;
    put_field(writer, static_cast<unsigned>(words - 1), 4);
    writer.append(body);
    std::size_t fill = words * 16 - bits;
    while (fill > 0) {
        const std::size_t step = std::min<std::size_t>(fill, 16);
        put_field(writer, 0, static_cast<int>(step));
        fill -= step;
    }
}

}  // namespace

std::expected<std::vector<std::byte>, Error> write_major_sync(const MajorSync& major_sync) {
    if (major_sync.extra.has_value()) {
        if (major_sync.kind != StreamKind::kTrueHd || !major_sync.channel_meaning.extra_present) {
            return std::unexpected(
                Error{ErrorKind::kBadParameter,
                      "the 16-channel extension needs a TrueHD major sync with extra_present"});
        }
        if (const std::string_view field = extra_channel_meaning_problem(*major_sync.extra);
            !field.empty()) {
            return std::unexpected(Error{ErrorKind::kBadParameter, field});
        }
    }
    if (major_sync.kind == StreamKind::kTrueHd) {
        if (const std::string_view field = channel_meaning_problem(major_sync.channel_meaning);
            !field.empty()) {
            return std::unexpected(Error{ErrorKind::kBadParameter, field});
        }
    }
    BitWriter writer;
    writer.put(kMajorSyncByte0, 8);
    writer.put(kMajorSyncByte1, 8);
    writer.put(kMajorSyncByte2, 8);
    writer.put(major_sync.kind == StreamKind::kTrueHd ? kTrueHdStreamByte : kMlpStreamByte, 8);
    writer.put(major_sync.format_info, 32);
    put_field(writer, major_sync.signature, 16);
    put_field(writer, major_sync.flags, 16);
    put_field(writer, major_sync.reserved, 16);
    put_field(writer, major_sync.is_vbr ? 1U : 0U, 1);
    put_field(writer, major_sync.peak_data_rate, 15);
    put_field(writer, major_sync.num_substreams, 4);
    put_field(writer, major_sync.extended_reserved, 2);
    put_field(writer, major_sync.extended_substream_info, 2);
    put_field(writer, major_sync.substream_info, 8);
    if (major_sync.kind == StreamKind::kTrueHd) {
        write_channel_meaning(writer, major_sync.channel_meaning);
    } else {
        for (const std::uint8_t byte : major_sync.channel_meaning_bytes) {
            put_field(writer, byte, 8);
        }
    }
    if (major_sync.extra.has_value()) {
        write_extra(writer, *major_sync.extra);
    }
    std::vector<std::byte> bytes = writer.take();
    const std::uint16_t crc = major_sync_crc(bytes);
    bytes.push_back(static_cast<std::byte>(crc >> 8U));
    bytes.push_back(static_cast<std::byte>(crc & 0xFFU));
    return bytes;
}

}  // namespace iclforge::truehd
