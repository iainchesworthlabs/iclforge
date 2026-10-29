#include "iclforge/truehd/io/access_unit.hpp"

#include "core/bit_io.hpp"
#include "iclforge/base/bitreader.hpp"
#include "iclforge/base/bitwriter.hpp"
#include "iclforge/truehd/core/crc.hpp"

namespace iclforge::truehd {

namespace {

using detail::put_field;
using detail::put_signed;
using detail::read_signed;

constexpr unsigned kCheckNibbleTarget = 0xF;  // TH spec §3.2
constexpr std::size_t kHeaderBytes = 4;
constexpr unsigned kExtraDataSeed = 0xA9;  // TH spec §16.4

[[nodiscard]] unsigned be16(std::span<const std::byte> data, std::size_t at) noexcept {
    return (std::to_integer<unsigned>(data[at]) << 8U) | std::to_integer<unsigned>(data[at + 1]);
}

// The XOR of every nibble of `bytes`.
[[nodiscard]] unsigned nibble_fold(std::span<const std::byte> bytes) noexcept {
    unsigned folded = 0;
    for (const std::byte b : bytes) {
        folded ^= std::to_integer<unsigned>(b);
    }
    return ((folded >> 4U) ^ folded) & 0xFU;
}

[[nodiscard]] ExtraData parse_extra_data(std::span<const std::byte> bytes) {
    ExtraData extra;
    extra.bytes = bytes;
    const unsigned word = be16(bytes, 0);
    extra.header_check_nibble = static_cast<std::uint8_t>(word >> 12U);
    extra.length = static_cast<std::uint16_t>(word & 0xFFFU);
    extra.check_ok = extra.header_check_nibble == extra_data_check_nibble(extra.length);
    extra.parity = std::to_integer<std::uint8_t>(bytes.back());
    // evo_reserved and evo_frame_byte_length share the next 16 bits; the payload starts on the byte
    // after.
    constexpr std::size_t kFramingBytes = 4;
    if (bytes.size() < kFramingBytes + 1) {
        return extra;
    }
    const unsigned evo_word = be16(bytes, 2);
    extra.evo_reserved = static_cast<std::uint8_t>(evo_word >> 12U);
    extra.evo_frame_byte_length = static_cast<std::uint16_t>(evo_word & 0xFFFU);
    if (kFramingBytes + extra.evo_frame_byte_length + 1U > bytes.size()) {
        return extra;
    }
    extra.framing_ok = true;
    extra.evolution_payload = bytes.subspan(kFramingBytes, extra.evo_frame_byte_length);
    // TH spec §16.4: the payload and the padding are the bytes between the framing and the parity
    // byte.
    unsigned accumulator = extra.evo_reserved ^ extra.evo_frame_byte_length ^ kExtraDataSeed;
    for (std::size_t i = kFramingBytes; i + 1 < bytes.size(); ++i) {
        accumulator ^= std::to_integer<unsigned>(bytes[i]);
    }
    extra.parity_ok = (((accumulator >> 8U) ^ accumulator) & 0xFFU) == extra.parity;
    return extra;
}

}  // namespace

std::optional<ExtraData> read_extra_data(std::span<const std::byte> bytes) {
    if (bytes.size() < 2) {
        return std::nullopt;
    }
    return parse_extra_data(bytes);
}

std::size_t access_unit_length_bytes(std::span<const std::byte> data) noexcept {
    return data.size() < 2 ? 0 : std::size_t{be16(data, 0) & 0xFFFU} * 2U;
}

std::expected<AccessUnit, Error> parse_access_unit(std::span<const std::byte> data,
                                                   int inherited_substreams, bool strict) {
    if (data.size() < kHeaderBytes) {
        return std::unexpected(Error{ErrorKind::kTruncated, "access unit header"});
    }
    AccessUnit unit;
    const unsigned first_word = be16(data, 0);
    unit.check_nibble = static_cast<std::uint8_t>(first_word >> 12U);
    unit.length_words = static_cast<std::uint16_t>(first_word & 0xFFFU);
    unit.input_timing = static_cast<std::uint16_t>(be16(data, 2));
    if (unit.length_words < kHeaderBytes / 2) {
        return std::unexpected(Error{ErrorKind::kBadLength, "access unit shorter than its header"});
    }
    if (data.size() < unit.length_bytes()) {
        return std::unexpected(Error{ErrorKind::kTruncated, "access unit"});
    }
    const std::span<const std::byte> whole = data.first(unit.length_bytes());
    unit.bytes = whole;

    std::size_t offset = kHeaderBytes;
    int num_substreams = inherited_substreams;
    if (starts_with_major_sync(whole.subspan(offset))) {
        auto major_sync = parse_major_sync(whole.subspan(offset), strict);
        if (!major_sync) {
            return std::unexpected(major_sync.error());
        }
        offset += major_sync->size_bytes;
        num_substreams = major_sync->num_substreams;
        unit.major_sync = *major_sync;
    }
    if (num_substreams < 1) {
        return std::unexpected(Error{ErrorKind::kNeedsRestart, "no major sync seen yet"});
    }
    if (num_substreams > kMaxSubstreams) {
        // Only a caller's `inherited_substreams` can be above 4; a major sync's count is read from
        // 4 bits, and parse_major_sync refuses what a stream cannot have.
        return std::unexpected(Error{ErrorKind::kBadParameter, "more than 4 substreams"});
    }
    unit.num_substreams = num_substreams;

    // TH spec §6.1: the directory. An entry that sends its extra word is two words long.
    const std::size_t directory_start = offset;
    for (int i = 0; i < num_substreams; ++i) {
        if (offset + 2 > whole.size()) {
            return std::unexpected(Error{ErrorKind::kTruncated, "substream directory"});
        }
        BitReader reader(whole.subspan(offset, std::min<std::size_t>(4, whole.size() - offset)));
        SubstreamEntry& entry = unit.entries[static_cast<std::size_t>(i)];
        entry.extra_substream_word = reader.read(1) != 0;
        entry.restart_nonexistent = reader.read(1) != 0;
        entry.crc_present = reader.read(1) != 0;
        entry.reserved_bit = reader.read(1) != 0;
        entry.end_ptr = static_cast<std::uint16_t>(reader.read(12));
        offset += 2;
        if (entry.extra_substream_word) {
            if (offset + 2 > whole.size()) {
                return std::unexpected(
                    Error{ErrorKind::kTruncated, "substream directory extra word"});
            }
            entry.drc_gain_update = static_cast<std::int16_t>(read_signed(reader, 9));
            entry.drc_time_update = static_cast<std::uint8_t>(reader.read(3));
            entry.extra_word_padding = static_cast<std::uint8_t>(reader.read(4));
            offset += 2;
        }
    }
    const std::size_t segments_start = offset;

    // TH spec §3.2: the nibbles of the header and the directory XOR to 0xF.
    const unsigned folded =
        nibble_fold(whole.first(kHeaderBytes)) ^
        nibble_fold(whole.subspan(directory_start, segments_start - directory_start));
    if (strict && folded != kCheckNibbleTarget) {
        return std::unexpected(Error{ErrorKind::kBadCheckNibble, "access unit check nibble"});
    }

    // TH spec §6.3: end_ptr is cumulative, in 16-bit words from the start of the first segment.
    std::size_t previous_end = 0;
    for (int i = 0; i < num_substreams; ++i) {
        const SubstreamEntry& entry = unit.entries[static_cast<std::size_t>(i)];
        if (entry.end_ptr < previous_end ||
            segments_start + std::size_t{entry.end_ptr} * 2U > whole.size()) {
            return std::unexpected(Error{ErrorKind::kBadLength, "substream end pointer"});
        }
        unit.segments[static_cast<std::size_t>(i)] = whole.subspan(
            segments_start + previous_end * 2U, (std::size_t{entry.end_ptr} - previous_end) * 2U);
        previous_end = entry.end_ptr;
    }

    // TH spec §16.1: what follows the last segment is extra data.
    const std::size_t extra_start = segments_start + previous_end * 2U;
    if (extra_start + 2 <= whole.size()) {
        unit.extra_data = parse_extra_data(whole.subspan(extra_start));
    }
    return unit;
}

std::expected<std::vector<std::byte>, Error> write_access_unit_prefix(
    const std::optional<MajorSync>& major_sync, std::span<const SubstreamEntry> entries,
    std::uint16_t input_timing, std::size_t total_bytes) {
    if (entries.empty() || entries.size() > static_cast<std::size_t>(kMaxSubstreams)) {
        return std::unexpected(Error{ErrorKind::kBadParameter, "substream count outside 1..4"});
    }
    if (total_bytes % 2 != 0 || total_bytes < kHeaderBytes || total_bytes / 2 > 0xFFFU) {
        return std::unexpected(
            Error{ErrorKind::kBadLength, "access unit size is not 4 to 8190 bytes in words"});
    }
    if (major_sync && major_sync->num_substreams != entries.size()) {
        return std::unexpected(
            Error{ErrorKind::kBadParameter, "major sync and directory disagree on substreams"});
    }

    BitWriter directory;
    for (const SubstreamEntry& entry : entries) {
        put_field(directory, entry.extra_substream_word ? 1U : 0U, 1);
        put_field(directory, entry.restart_nonexistent ? 1U : 0U, 1);
        put_field(directory, entry.crc_present ? 1U : 0U, 1);
        put_field(directory, entry.reserved_bit ? 1U : 0U, 1);
        put_field(directory, entry.end_ptr, 12);
        if (entry.extra_substream_word) {
            put_signed(directory, entry.drc_gain_update, 9);
            put_field(directory, entry.drc_time_update, 3);
            put_field(directory, entry.extra_word_padding, 4);
        }
    }
    const std::vector<std::byte> directory_bytes = directory.take();

    const auto words = static_cast<unsigned>(total_bytes / 2);
    std::array<std::byte, kHeaderBytes> header{
        static_cast<std::byte>((words >> 8U) & 0xFU), static_cast<std::byte>(words & 0xFFU),
        static_cast<std::byte>(input_timing >> 8U), static_cast<std::byte>(input_timing & 0xFFU)};
    const unsigned nibble = kCheckNibbleTarget ^ nibble_fold(header) ^ nibble_fold(directory_bytes);
    header[0] = static_cast<std::byte>((nibble << 4U) | std::to_integer<unsigned>(header[0]));

    std::vector<std::byte> prefix(header.begin(), header.end());
    if (major_sync) {
        auto sync_bytes = write_major_sync(*major_sync);
        if (!sync_bytes) {
            return std::unexpected(sync_bytes.error());
        }
        prefix.insert(prefix.end(), sync_bytes->begin(), sync_bytes->end());
    }
    prefix.insert(prefix.end(), directory_bytes.begin(), directory_bytes.end());
    return prefix;
}

}  // namespace iclforge::truehd
