#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "iclforge/base/bitwriter.hpp"
#include "iclforge/truehd/core/crc.hpp"
#include "iclforge/truehd/core/syntax.hpp"
#include "iclforge/truehd/io/access_unit.hpp"
#include "iclforge/truehd/io/major_sync.hpp"

// Streams the tests build from the library's own writers: small enough to hold in the head, and
// exact about what the decoder has to give back. They are not a check of the syntax against
// anything but itself; the FFmpeg fixtures (testdata/truehd/) and tools/references/truehd_syntax.py
// are.

namespace iclforge::truehd::testing {

constexpr int kBuiltBlockSize = 8;
constexpr int kBuiltLsbs =
    4;  // huff_lsbs of the built streams: raw four-bit residuals, no Huffman codes

// The residual, as the stream holds it, of channel `ch` at sample `i` of a built unit: the decoder
// gives back raw - 8 (TH spec §11.3: raw LSBs less half their range). The second channel's values
// are not symmetric, so that the unit's lossless check is not zero.
constexpr std::array<std::uint32_t, kBuiltBlockSize> kSecondChannelRaw{5, 9, 2, 14, 7, 7, 1, 12};
constexpr std::uint32_t raw_residual(int ch, int i) {
    return ch == 0 ? static_cast<std::uint32_t>(i) : kSecondChannelRaw[static_cast<std::size_t>(i)];
}
// The bit a built stream with a bypass matrix sends for sample i, before that sample's residuals.
constexpr std::array<std::uint32_t, kBuiltBlockSize> kBuiltBypassBit{1, 0, 0, 1, 1, 0, 1, 1};
constexpr std::int32_t expected_sample(int ch, int i) {
    return static_cast<std::int32_t>(raw_residual(ch, i)) - 8;
}

// A stereo TrueHD major sync at 48 kHz with one substream.
inline MajorSync stereo_major_sync() {
    MajorSync major_sync;
    major_sync.kind = StreamKind::kTrueHd;
    major_sync.format_info = 0x00000001;  // 48 kHz; the eight-channel assignment names the L R pair
    major_sync.num_substreams = 1;
    major_sync.is_vbr = true;
    major_sync.peak_data_rate = 0x0C7F;
    major_sync.substream_info = 0x14;
    return major_sync;
}

// The bytes of one substream segment of the built stereo stream, with its parity and CRC bytes.
// With `restart` the first block carries a restart header; without, a block with no header at all.
inline std::vector<std::byte> stereo_segment(bool restart, bool with_checks = true,
                                             std::uint16_t sync = kRestartSyncStandard,
                                             bool bypass_matrix = false) {
    BitWriter writer;
    if (restart) {
        writer.put(1, 1);  // block_header_exists
        writer.put(1, 1);  // restart_header_exists
        RestartHeader header;
        header.sync = sync;
        header.min_chan = 0;
        header.max_chan = 1;
        header.max_matrix_chan = 1;
        header.ch_assign[0] = 0;
        header.ch_assign[1] = 1;
        write_restart_header(writer, header);
        BlockContext context;
        context.min_chan = 0;
        context.max_chan = 1;
        context.max_matrix_chan = 1;
        context.noise_type = sync == kRestartSyncNoise;  // the matrices carry a noise shift then
        BlockHeaderSpec block;
        block.block_size = kBuiltBlockSize;
        if (bypass_matrix) {
            MatrixSet set;
            set.count = 1;
            set.items[0].out_ch = 0;
            set.items[0].frac_bits = 0;
            set.items[0].lsb_bypass = true;
            set.items[0].present[0] = true;
            set.items[0].coeff[0] = 1;
            block.matrices = set;
        }
        for (int ch = 0; ch < 2; ++ch) {
            block.channels[static_cast<std::size_t>(ch)].present = true;
            block.channels[static_cast<std::size_t>(ch)].huff_type = 0;
            block.channels[static_cast<std::size_t>(ch)].huff_lsbs = kBuiltLsbs;
        }
        write_block_header(writer, context, block);
    } else {
        writer.put(0, 1);  // no block header: the parameters carry on
    }
    for (int i = 0; i < kBuiltBlockSize; ++i) {
        if (bypass_matrix) {
            writer.put(kBuiltBypassBit[static_cast<std::size_t>(i)], 1);  // TH spec §10.2.1
        }
        for (int ch = 0; ch < 2; ++ch) {
            writer.put(raw_residual(ch, i), kBuiltLsbs);
        }
    }
    writer.put(1, 1);  // last_block_in_segment
    std::vector<std::byte> bytes = writer.take();
    if (with_checks) {
        if (bytes.size() % 2 != 0) {
            bytes.push_back(std::byte{0});
        }
        const std::span<const std::byte> payload(bytes);
        const std::uint8_t parity = substream_parity(payload);
        const std::uint8_t crc = substream_crc(payload);
        bytes.push_back(static_cast<std::byte>(parity));
        bytes.push_back(static_cast<std::byte>(crc));
    } else if (bytes.size() % 2 != 0) {
        bytes.push_back(std::byte{0});
    }
    return bytes;
}

// One access unit of the built stereo stream.
inline std::vector<std::byte> stereo_unit(bool with_major_sync, std::uint16_t input_timing = 0,
                                          std::uint16_t sync = kRestartSyncStandard,
                                          bool bypass_matrix = false) {
    const std::vector<std::byte> segment =
        stereo_segment(with_major_sync, true, sync, bypass_matrix);
    SubstreamEntry entry;
    entry.restart_nonexistent = !with_major_sync;
    entry.crc_present = true;
    entry.end_ptr = static_cast<std::uint16_t>(segment.size() / 2);
    const std::array<SubstreamEntry, 1> entries{entry};
    const std::optional<MajorSync> major_sync =
        with_major_sync ? std::optional<MajorSync>(stereo_major_sync()) : std::nullopt;
    const std::size_t prefix_bytes = 4 + (with_major_sync ? kMajorSyncBaseBytes : 0U) + 2U;
    auto prefix =
        write_access_unit_prefix(major_sync, entries, input_timing, prefix_bytes + segment.size());
    std::vector<std::byte> unit = *prefix;
    unit.insert(unit.end(), segment.begin(), segment.end());
    return unit;
}

}  // namespace iclforge::truehd::testing
