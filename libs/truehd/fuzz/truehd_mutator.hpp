#pragma once

// A libFuzzer custom mutator that puts the checks of a TrueHD / MLP stream right again after
// mutating, shared by fuzz_truehd_parse, fuzz_truehd_decode and fuzz_differential_truehd_decode.
// Same idea as libs/ac3/fuzz/crc_mutator.hpp, and for the same reason.
//
// A stream carries five checks between the byte a mutation changes and the parser that mutation
// was aimed at, and each is a door that shuts on the input:
//
//   * the access unit's check nibble (TH spec section 3.2), which parse_access_unit and
//     ElementaryReader test before the unit is read at all;
//   * the major sync's CRC-16 (section 4.1), the same;
//   * the restart header's CRC-8 (section 7.1), which the decoder tests before it reads the
//     channel layout and the filters;
//   * a segment's parity byte and CRC-8 (section 6.2), which the decoder tests before it reads one
//     block;
//   * the extra data's check nibble and parity byte (section 16), which are reported and never an
//     error, so a mutation there is not lost, but only a repaired field reaches the evolution
//     payload's framing.
//
// A mutation that changes any byte of a unit but its own check is therefore refused at one of them,
// and none of what it did to the fields behind it is read. The repair below computes each with the
// library's own routine (iclforge::truehd::major_sync_crc, restart_header_crc, substream_parity,
// substream_crc, extra_data_check_nibble: the functions libs/truehd/tests hold to FFmpeg's) and
// writes it where the library's reader looks for it, in the order the checks nest: the restart
// header's CRC, then the segment's parity and CRC that cover it, then the nibble that covers the
// directory.
//
// What it does not repair:
//
//   * Lengths, end pointers, counts and every field the syntax gives meaning to. The goal is to
//   stop
//     losing inputs at a checksum, not to constrain the engine to valid streams.
//   * The lossless check of a restart interval (section 7.3), which is a function of the decoded
//     samples of the interval before, so repairing it takes a decode. A mutation in the audio of
//     an interval ends, in strict decode, at the next restart header; lenient decode counts it and
//     goes on, and the last interval of a stream, which no restart header follows, is never
//     checked. fuzz_truehd_decode runs both.
//   * A restart header that is not at the head of a segment's first block. The syntax does not
//     tell where a later block begins without decoding the one before.
//
// The mutator leaves checks wrong on purpose, so that the paths that refuse a bad one stay
// reachable: a quarter of mutations are not repaired at all, and of the rest each of the five
// checks is left alone one time in sixteen. Both are drawn from libFuzzer's own seed argument, so
// a mutation can be reproduced.

#include <cstddef>
#include <cstdint>
#include <span>

#include "iclforge/truehd/core/crc.hpp"
#include "iclforge/truehd/core/format.hpp"
#include "iclforge/truehd/core/syntax.hpp"
#include "iclforge/truehd/io/access_unit.hpp"
#include "iclforge/truehd/io/major_sync.hpp"

// libFuzzer's own mutation engine, callable from inside a custom mutator. Declared here rather than
// included from anywhere: libFuzzer ships no header, only this documented entry point.
extern "C" std::size_t LLVMFuzzerMutate(std::uint8_t* data, std::size_t size, std::size_t max_size);

namespace iclforge_fuzz {

// The checks the repair can put right, one bit each.
enum TruehdCheck : unsigned {
    kUnitNibble = 1U << 0U,
    kMajorSyncCrc = 1U << 1U,
    kRestartCrc = 1U << 2U,
    kSegmentChecks = 1U << 3U,  // the parity byte and the CRC byte, which are written together
    kExtraDataChecks = 1U << 4U,
    kAllTruehdChecks = (1U << 5U) - 1U,
};

namespace truehd_detail {

[[nodiscard]] inline std::size_t be16(std::span<const std::byte> bytes, std::size_t at) {
    return (std::to_integer<std::size_t>(bytes[at]) << 8U) |
           std::to_integer<std::size_t>(bytes[at + 1]);
}

// Writes `count` bits of `value`, most significant first, at bit `first` of `bytes`.
inline void put_bits(std::span<std::byte> bytes, std::size_t first, unsigned count,
                     std::uint32_t value) {
    for (unsigned i = 0; i < count; ++i) {
        const std::size_t bit = first + i;
        const unsigned mask = 0x80U >> (bit & 7U);
        const bool on = ((value >> (count - 1U - i)) & 1U) != 0U;
        auto byte = std::to_integer<unsigned>(bytes[bit >> 3U]);
        byte = on ? (byte | mask) : (byte & ~mask);
        bytes[bit >> 3U] = static_cast<std::byte>(byte);
    }
}

// The XOR of every nibble of `bytes` (the fold access_unit.cpp checks the unit's header and
// directory with).
[[nodiscard]] inline unsigned nibble_fold(std::span<const std::byte> bytes) {
    unsigned folded = 0;
    for (const std::byte b : bytes) {
        folded ^= std::to_integer<unsigned>(b);
    }
    return ((folded >> 4U) ^ folded) & 0xFU;
}

// TH spec section 7.1: the restart header of a segment, when its first block carries one. The
// block opens with two flag bits (both set), then the 14-bit sync, and max_matrix_chan, which sizes
// the header, is the 4 bits that start at bit 38 of the header. `payload` is the segment without
// its parity and CRC bytes, the span the decoder reads the header from.
inline void repair_restart_crc(std::span<std::byte> payload) {
    constexpr std::size_t kFlagBits = 2;
    constexpr std::size_t kMaxMatrixChanBit = 38;
    if (payload.size() < 6 || (std::to_integer<unsigned>(payload[0]) & 0xC0U) != 0xC0U) {
        return;
    }
    const auto sync =
        static_cast<std::uint16_t>(((std::to_integer<unsigned>(payload[0]) & 0x3FU) << 8U) |
                                   std::to_integer<unsigned>(payload[1]));
    if (sync != iclforge::truehd::kRestartSyncStandard &&
        sync != iclforge::truehd::kRestartSyncNoise &&
        sync != iclforge::truehd::kRestartSyncAtmos) {
        return;
    }
    const auto max_matrix_chan = static_cast<int>(
        (std::to_integer<unsigned>(payload[(kFlagBits + kMaxMatrixChanBit) >> 3U]) >> 4U) & 0xFU);
    const std::size_t header_bits = iclforge::truehd::restart_header_bits(max_matrix_chan);
    const std::size_t crc_bit = kFlagBits + header_bits - 8;
    if (kFlagBits + header_bits > payload.size() * 8U) {
        return;
    }
    const std::uint8_t crc = iclforge::truehd::restart_header_crc(payload, kFlagBits, crc_bit);
    put_bits(payload, crc_bit, 8, crc);
}

// TH spec section 16.1 to 16.4, as access_unit.cpp reads it: a check nibble over the 12-bit length
// in the first word, and a parity byte over the payload and its padding.
inline void repair_extra_data(std::span<std::byte> extra) {
    if (extra.size() < 2) {
        return;
    }
    const std::size_t word = be16(extra, 0);
    const unsigned nibble =
        iclforge::truehd::extra_data_check_nibble(static_cast<std::uint32_t>(word & 0xFFFU));
    extra[0] =
        static_cast<std::byte>((std::to_integer<unsigned>(extra[0]) & 0x0FU) | (nibble << 4U));
    constexpr std::size_t kFramingBytes = 4;
    if (extra.size() < kFramingBytes + 1) {
        return;
    }
    const std::size_t evo_word = be16(extra, 2);
    const std::size_t reserved = evo_word >> 12U;
    const std::size_t payload_bytes = evo_word & 0xFFFU;
    if (kFramingBytes + payload_bytes + 1U > extra.size()) {
        return;
    }
    constexpr unsigned kSeed = 0xA9;
    auto accumulator = static_cast<unsigned>(reserved ^ payload_bytes) ^ kSeed;
    for (std::size_t i = kFramingBytes; i + 1 < extra.size(); ++i) {
        accumulator ^= std::to_integer<unsigned>(extra[i]);
    }
    extra.back() = static_cast<std::byte>(((accumulator >> 8U) ^ accumulator) & 0xFFU);
}

// Puts the checks of one access unit (`unit` is exactly the unit) right, those `checks` names.
// `inherited` is the substream count of the last major sync seen, which a unit without one reads.
// Returns the substream count the unit leaves behind, 0 when the unit does not read.
inline int repair_unit(std::span<std::byte> unit, int inherited, unsigned checks) {
    using namespace iclforge::truehd;
    // Not strict: the unit's own checks are what is being put right, and the reader must still
    // give its structure when they are wrong.
    const auto parsed = parse_access_unit(unit, inherited, false);
    if (!parsed) {
        return 0;
    }
    constexpr std::size_t kHeaderBytes = 4;
    std::size_t directory_start = kHeaderBytes;
    if (parsed->major_sync) {
        const std::size_t size = parsed->major_sync->size_bytes;
        if ((checks & kMajorSyncCrc) != 0U && directory_start + size <= unit.size() && size >= 2) {
            const std::uint16_t crc =
                major_sync_crc(std::span<const std::byte>(unit).subspan(directory_start, size - 2));
            unit[directory_start + size - 2] = static_cast<std::byte>(crc >> 8U);
            unit[directory_start + size - 1] = static_cast<std::byte>(crc & 0xFFU);
        }
        directory_start += size;
    }

    const auto offset_of = [&](std::span<const std::byte> part) {
        return static_cast<std::size_t>(part.data() - unit.data());
    };
    for (int i = 0; i < parsed->num_substreams; ++i) {
        const auto index = static_cast<std::size_t>(i);
        const std::span<const std::byte> seen = parsed->segments[index];
        const std::span<std::byte> segment = unit.subspan(offset_of(seen), seen.size());
        const bool has_checks = parsed->entries[index].crc_present && segment.size() >= 3;
        const std::span<std::byte> payload =
            has_checks ? segment.first(segment.size() - 2) : segment;
        if ((checks & kRestartCrc) != 0U) {
            repair_restart_crc(payload);
        }
        if (has_checks && (checks & kSegmentChecks) != 0U) {
            const std::span<const std::byte> covered = payload;
            segment[segment.size() - 2] = static_cast<std::byte>(substream_parity(covered));
            segment[segment.size() - 1] = static_cast<std::byte>(substream_crc(covered));
        }
    }

    if (parsed->extra_data && (checks & kExtraDataChecks) != 0U) {
        const std::span<const std::byte> seen = parsed->extra_data->bytes;
        repair_extra_data(unit.subspan(offset_of(seen), seen.size()));
    }

    if ((checks & kUnitNibble) != 0U) {
        // The first segment starts where the directory ends; a unit has one substream at least.
        const std::size_t segments_start = offset_of(parsed->segments[0]);
        if (segments_start >= directory_start && segments_start <= unit.size()) {
            unit[0] = static_cast<std::byte>(std::to_integer<unsigned>(unit[0]) & 0x0FU);
            const unsigned nibble =
                0xFU ^ nibble_fold(std::span<const std::byte>(unit).first(kHeaderBytes)) ^
                nibble_fold(std::span<const std::byte>(unit).subspan(
                    directory_start, segments_start - directory_start));
            unit[0] = static_cast<std::byte>(std::to_integer<unsigned>(unit[0]) | (nibble << 4U));
        }
    }
    return parsed->num_substreams;
}

// The offset of the first major sync word at or after `from`, or `size` if there is none.
[[nodiscard]] inline std::size_t find_major_sync(std::span<const std::byte> stream,
                                                 std::size_t from) {
    for (std::size_t i = from; i + 4 <= stream.size(); ++i) {
        if (std::to_integer<unsigned>(stream[i]) == iclforge::truehd::kMajorSyncByte0 &&
            iclforge::truehd::starts_with_major_sync(stream.subspan(i))) {
            return i;
        }
    }
    return stream.size();
}

}  // namespace truehd_detail

// Walks `stream` as access units the way iclforge::truehd::ElementaryReader does (from the first
// major sync, whose unit begins four bytes before it; each unit's length is the 12-bit word count
// that opens it; after a unit that does not fit, the next major sync is the way back in) and puts
// the checks in `checks` right in each. A stream that is cut or damaged keeps every unit before the
// damage repaired.
inline void restamp_truehd_checks(std::span<std::byte> stream, unsigned checks = kAllTruehdChecks) {
    constexpr std::size_t kHeaderBytes = 4;
    std::size_t sync = truehd_detail::find_major_sync(stream, kHeaderBytes);
    if (sync == stream.size()) {
        return;
    }
    std::size_t offset = sync - kHeaderBytes;
    int substreams = 0;
    while (offset + kHeaderBytes <= stream.size()) {
        const std::size_t length = (truehd_detail::be16(stream, offset) & 0xFFFU) * 2U;
        if (length >= kHeaderBytes && offset + length <= stream.size()) {
            const int left =
                truehd_detail::repair_unit(stream.subspan(offset, length), substreams, checks);
            if (left > 0) {
                substreams = left;
                offset += length;
                continue;
            }
        }
        sync = truehd_detail::find_major_sync(stream, offset + kHeaderBytes + 1);
        if (sync == stream.size()) {
            return;
        }
        offset = sync - kHeaderBytes;
        substreams = 0;
    }
}

// Which checks a mutation repairs, from libFuzzer's seed argument alone (a small generator of its
// own, so that the draw does not depend on what the engine did, which keeps a mutation
// reproducible): none for one in four, and of the others each check is left out one time in
// sixteen.
[[nodiscard]] inline unsigned truehd_repair_mask(unsigned int seed) {
    if ((seed & 3U) == 0U) {
        return 0;
    }
    std::uint32_t state = (seed * 2654435761U + 0x9E3779B9U) | 1U;
    unsigned checks = kAllTruehdChecks;
    for (unsigned bit = 0; bit < 5; ++bit) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        if ((state & 15U) == 0U) {
            checks &= ~(1U << bit);
        }
    }
    return checks;
}

// The body of LLVMFuzzerCustomMutator: libFuzzer's own mutation, then the repair the seed asks for.
inline std::size_t truehd_repairing_mutate(std::uint8_t* data, std::size_t size,
                                           std::size_t max_size, unsigned int seed) {
    const std::size_t mutated = LLVMFuzzerMutate(data, size, max_size);
    if (const unsigned checks = truehd_repair_mask(seed); checks != 0U) {
        restamp_truehd_checks({reinterpret_cast<std::byte*>(data), mutated}, checks);
    }
    return mutated;
}

}  // namespace iclforge_fuzz
