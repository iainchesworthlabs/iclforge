#include "core/access_unit.hpp"

#include <algorithm>

#include "core/crc.hpp"
#include "core/syntax.hpp"
#include "iclforge/base/bitreader.hpp"
#include "iclforge/base/bitwriter.hpp"

namespace iclforge::mlp::detail {

namespace {

constexpr std::size_t kHeaderBytes = 4;
constexpr std::size_t kSyncBytes = 4;
// Sync word, format_info through the channel meaning (24 bytes after the sync), and the CRC:
// 28 bytes without the extra channel meaning (4.1).
constexpr std::size_t kMajorSyncBaseBytes = 28;

std::uint32_t be32(std::span<const std::byte> d) noexcept {
    return std::to_integer<std::uint32_t>(d[0]) << 24U |
           std::to_integer<std::uint32_t>(d[1]) << 16U |
           std::to_integer<std::uint32_t>(d[2]) << 8U | std::to_integer<std::uint32_t>(d[3]);
}

}  // namespace

bool starts_with_major_sync(std::span<const std::byte> data) noexcept {
    if (data.size() < kSyncBytes) {
        return false;
    }
    const std::uint32_t word = be32(data);
    return word == kMajorSyncTrueHd || word == kMajorSyncMlp;
}

std::expected<MajorSyncRead, Error> read_major_sync(std::span<const std::byte> data) {
    if (data.size() < kMajorSyncBaseBytes) {
        return std::unexpected(Error::kTruncated);
    }
    const std::uint32_t word = be32(data);
    if (word != kMajorSyncTrueHd && word != kMajorSyncMlp) {
        return std::unexpected(Error::kNoMajorSync);
    }
    MajorSyncRead out;
    out.sync.stream_type = word == kMajorSyncTrueHd ? StreamType::kTrueHd : StreamType::kMlp;
    // Section 5.2's MLP format_info is laid out differently; a 0xBB stream is recognised and
    // refused by the decoder, so only its length matters here, which the same fields give.
    BitReader bits(data.subspan(kSyncBytes));
    SyntaxReader io(bits);
    if (auto r = transfer(io, out.sync); !r) {
        return std::unexpected(r.error());
    }
    const std::size_t body = bits.bit_position() / 8U;  // whole bytes: the fields fill 16-bit words
    const std::size_t crc_at = kSyncBytes + body;
    if (data.size() < crc_at + 2U) {
        return std::unexpected(Error::kTruncated);
    }
    // P12: the unaugmented remainder over everything before the CRC field, sync word included.
    const std::uint16_t computed = crc16_bytes(data.first(crc_at));
    const auto stored = static_cast<std::uint16_t>(std::to_integer<unsigned>(data[crc_at]) << 8U |
                                                   std::to_integer<unsigned>(data[crc_at + 1U]));
    if (computed != stored) {
        return std::unexpected(Error::kMajorSyncCrc);
    }
    if (out.sync.signature != kSignatureMlp && out.sync.signature != kSignatureTrueHd) {
        return std::unexpected(Error::kMajorSyncSignature);
    }
    out.size = crc_at + 2U;
    return out;
}

std::expected<std::vector<std::byte>, Error> write_major_sync(const MajorSync& sync) {
    BitWriter bits;
    bits.write(32, sync.stream_type == StreamType::kTrueHd ? kMajorSyncTrueHd : kMajorSyncMlp,
               "format_sync");
    SyntaxWriter io(bits);
    if (auto r = transfer(io, sync); !r) {
        return std::unexpected(r.error());
    }
    if (io.overflowed()) {
        return std::unexpected(Error::kInvalidConfiguration);
    }
    std::vector<std::byte> out = bits.take();
    const std::uint16_t crc = crc16_bytes(out);
    out.push_back(static_cast<std::byte>(crc >> 8U));
    out.push_back(static_cast<std::byte>(crc & 0xFFU));
    return out;
}

std::expected<AccessUnitLayout, Error> parse_access_unit(std::span<const std::byte> data,
                                                         const MajorSync* current) {
    if (data.size() < kHeaderBytes) {
        return std::unexpected(Error::kTruncated);
    }
    AccessUnitLayout au;
    {
        BitReader bits(data.first(kHeaderBytes));
        SyntaxReader io(bits);
        (void)transfer(io, au.header);
    }
    const std::size_t length = static_cast<std::size_t>(au.header.length_words) * 2U;
    if (length < kHeaderBytes) {
        return std::unexpected(Error::kBadAccessUnitLength);
    }
    if (length > data.size()) {
        return std::unexpected(Error::kTruncated);
    }
    au.bytes = data.first(length);
    std::size_t at = kHeaderBytes;
    const MajorSync* sync = current;
    if (starts_with_major_sync(au.bytes.subspan(at))) {
        auto read = read_major_sync(au.bytes.subspan(at));
        if (!read) {
            return std::unexpected(read.error());
        }
        au.major_sync = read->sync;
        sync = &*au.major_sync;
        at += read->size;
    }
    if (sync == nullptr) {
        return std::unexpected(Error::kNoMajorSync);
    }
    au.num_substreams = sync->num_substreams;
    if (au.num_substreams < 1 || au.num_substreams > kMaxSubstreams) {
        return std::unexpected(Error::kBadSubstreamCount);
    }
    // The directory, then the check nibble over the header and the directory (3.2).
    const std::size_t directory_start = at;
    {
        BitReader bits(au.bytes.subspan(at));
        SyntaxReader io(bits);
        for (int i = 0; i < au.num_substreams; ++i) {
            if (auto r = transfer(io, au.segments[static_cast<std::size_t>(i)].entry); !r) {
                return std::unexpected(r.error());
            }
        }
        at += bits.bit_position() / 8U;
    }
    const std::uint8_t nibbles = static_cast<std::uint8_t>(
        nibble_xor(au.bytes.first(kHeaderBytes)) ^
        nibble_xor(au.bytes.subspan(directory_start, at - directory_start)));
    if (nibbles != 0x0F) {
        return std::unexpected(Error::kCheckNibble);
    }
    // P13: end pointers count 16-bit words from the end of the directory.
    std::size_t start = at;
    for (int i = 0; i < au.num_substreams; ++i) {
        auto& seg = au.segments[static_cast<std::size_t>(i)];
        if (au.major_sync && seg.entry.restart_nonexistent) {
            return std::unexpected(Error::kRestartInMajorSync);
        }
        const std::size_t end = at + static_cast<std::size_t>(seg.entry.end_ptr) * 2U;
        if (end < start || end > length) {
            return std::unexpected(Error::kSubstreamEndPointer);
        }
        seg.bytes = au.bytes.subspan(start, end - start);
        start = end;
    }
    au.extra_data = au.bytes.subspan(start);
    return au;
}

std::expected<std::vector<std::byte>, Error> assemble_access_unit(const AccessUnitParts& parts) {
    if (parts.num_substreams < 1 || parts.num_substreams > kMaxSubstreams) {
        return std::unexpected(Error::kInvalidConfiguration);
    }
    std::vector<std::byte> sync_bytes;
    if (parts.major_sync != nullptr) {
        auto ms = write_major_sync(*parts.major_sync);
        if (!ms) {
            return std::unexpected(ms.error());
        }
        sync_bytes = std::move(*ms);
    }
    BitWriter directory;
    SyntaxWriter dio(directory);
    std::size_t words = 0;
    for (int i = 0; i < parts.num_substreams; ++i) {
        const auto& seg = parts.segments[static_cast<std::size_t>(i)];
        if (seg.size() % 2U != 0) {
            return std::unexpected(Error::kInvalidConfiguration);
        }
        words += seg.size() / 2U;
        SubstreamDirectoryEntry entry = parts.entries[static_cast<std::size_t>(i)];
        entry.end_ptr = static_cast<std::uint16_t>(words);
        if (words > 0x0FFFU) {
            return std::unexpected(Error::kInvalidConfiguration);
        }
        if (auto r = transfer(dio, entry); !r) {
            return std::unexpected(r.error());
        }
    }
    const std::vector<std::byte> dir = directory.take();
    if (parts.extra_data.size() % 2U != 0) {
        return std::unexpected(Error::kInvalidConfiguration);
    }
    std::size_t total =
        kHeaderBytes + sync_bytes.size() + dir.size() + words * 2U + parts.extra_data.size();
    if (total / 2U > 0x0FFFU) {
        return std::unexpected(Error::kInvalidConfiguration);
    }
    std::vector<std::byte> out;
    out.reserve(total);
    AccessUnitHeader header;
    header.length_words = static_cast<std::uint16_t>(total / 2U);
    header.input_timing = parts.input_timing;
    {
        BitWriter hw;
        SyntaxWriter io(hw);
        (void)transfer(io, header);
        const std::vector<std::byte> first = hw.take();
        const std::uint8_t x = static_cast<std::uint8_t>(nibble_xor(first) ^ nibble_xor(dir));
        // The check nibble is the header's first nibble: choose it so that the XOR gives 0xF.
        header.check_nibble = static_cast<std::uint8_t>(x ^ 0x0FU);
    }
    BitWriter hw;
    SyntaxWriter io(hw);
    (void)transfer(io, header);
    const std::vector<std::byte> head = hw.take();
    out.insert(out.end(), head.begin(), head.end());
    out.insert(out.end(), sync_bytes.begin(), sync_bytes.end());
    out.insert(out.end(), dir.begin(), dir.end());
    for (int i = 0; i < parts.num_substreams; ++i) {
        const auto& seg = parts.segments[static_cast<std::size_t>(i)];
        out.insert(out.end(), seg.begin(), seg.end());
    }
    out.insert(out.end(), parts.extra_data.begin(), parts.extra_data.end());
    return out;
}

}  // namespace iclforge::mlp::detail
