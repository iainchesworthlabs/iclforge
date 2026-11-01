#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

#include "iclforge/base/bitreader.hpp"
#include "iclforge/base/bitwriter.hpp"
#include "iclforge/truehd/core/error.hpp"
#include "iclforge/truehd/core/format.hpp"
#include "iclforge/truehd/export.hpp"

// The syntax of a substream segment: the restart header (TH spec §7), the block header (§8.2), the
// channel parameters (§8.3), the prediction filters (§9.2) and the primitive matrices (§10.2), each
// as a value a reader returns and a writer takes. The decoder reads through these and an encoder
// writes through them, so that the two directions share one description of the bits. Where the
// streams disagree with the specification the layout here is the streams' (libs/truehd/ERRATA.md
// says where).

namespace iclforge::truehd {

// TH spec §8.2: param_presence_flags. A block header sends a group of parameters only when its flag
// is set; the flags are 0xFF after a restart and a block header can replace them. The bit for each
// group is the order the syntax lists the groups in, the first group on bit 7. Streams all send
// 0xFF; no stream here shows another value, so the assignment is provisional (ERRATA.md).
namespace presence {
inline constexpr std::uint8_t kBlockSize = 0x80;
inline constexpr std::uint8_t kMatrix = 0x40;
inline constexpr std::uint8_t kOutputShift = 0x20;
inline constexpr std::uint8_t kQuantStep = 0x10;
inline constexpr std::uint8_t kFir = 0x08;
inline constexpr std::uint8_t kIir = 0x04;
inline constexpr std::uint8_t kHuffOffset = 0x02;
inline constexpr std::uint8_t kChannels = 0x01;  // the loop over the channels' parameters
inline constexpr std::uint8_t kAll = 0xFF;
}  // namespace presence

// TH spec §7.1. Fields in the order of the stream.
struct RestartHeader {
    std::uint16_t sync = kRestartSyncStandard;  // 14 bits; bit 0 is noise_type for 0x31EA / 0x31EB
    std::uint16_t output_timing = 0;
    std::uint8_t min_chan = 0;
    std::uint8_t max_chan = 0;
    std::uint8_t max_matrix_chan = 0;
    std::uint8_t noise_shift = 0;
    std::uint32_t noisegen_seed = 0;  // 23 bits
    // The 19 bits that follow the seed carry four fields (max_shift 4, max_lsbs 5, max_bits 5,
    // max_bits_repeat 5), which TH spec §7.1 lists after the lossless check and calls reserved
    // here. They describe the interval's largest shift, residual width and coded width, and a
    // decoder needs none of them; ERRATA.md has the evidence for the split.
    std::uint8_t max_shift = 0;
    std::uint8_t max_lsbs = 0;
    std::uint8_t max_bits = 0;
    std::uint8_t max_bits_repeat = 0;
    bool data_check_present = false;
    // TH spec §7.3: the check of the PREVIOUS restart interval's samples (the first restart of a
    // stream carries a value nothing checks). ERRATA.md has the fold.
    std::uint8_t lossless_check = 0;
    // The 16 bits that end the fixed part; TH spec §7.1 lists several fields in them that cannot
    // all fit. Zero in every stream checked; carried as read.
    std::uint16_t tail = 0;
    // 6 bits each, max_matrix_chan + 1 of them: ch_assign[m] is the output channel matrix channel m
    // goes to (TH spec §7.4; ERRATA.md entry 42). The decoder requires a permutation.
    std::array<std::uint8_t, kMaxChannels> ch_assign{};
    std::uint8_t crc = 0;  // as read; the writer computes it

    // TH spec §7.2: the low bit of the first two syncs; 0x31EC has none.
    [[nodiscard]] bool noise_type() const noexcept { return sync == kRestartSyncNoise; }
    [[nodiscard]] int matrix_channels() const noexcept { return max_matrix_chan + 1; }
    friend bool operator==(const RestartHeader&, const RestartHeader&) = default;
};

// The fixed part of a restart header in bits, from the first bit of the sync to the first of
// ch_assign, and the size of the whole for a given max_matrix_chan.
inline constexpr std::size_t kRestartFixedBits = 113;
[[nodiscard]] constexpr std::size_t restart_header_bits(int max_matrix_chan) noexcept {
    return kRestartFixedBits + 6U * static_cast<std::size_t>(max_matrix_chan + 1) + 8U;
}

// Reads a restart header from `reader`, positioned at the first bit of its 14-bit sync (after the
// two flag bits of the block that carries it). `data` is the bytes the reader reads, for the CRC.
// With `verify_crc` a wrong CRC is kBadRestartCrc.
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT std::expected<RestartHeader, Error> read_restart_header(
    BitReader& reader, std::span<const std::byte> data, bool verify_crc = true);

// Writes the header from its sync on, computing the CRC. `writer` is at the bit after the block's
// flag bits.
ICLFORGE_TRUEHD_EXPORT void write_restart_header(BitWriter& writer, const RestartHeader& header);

// TH spec §9.2: a prediction filter. `coeff` holds the values after the left shift by coeff_shift.
struct FilterSpec {
    std::uint8_t order = 0;
    std::uint8_t shift = 0;        // coeff_q: the right shift of the prediction
    std::uint8_t coeff_bits = 0;   // 1..16
    std::uint8_t coeff_shift = 0;  // 0..7
    std::array<std::int32_t, kMaxFirOrder> coeff{};
    // TH spec §9.2 sends the state flag for an IIR filter alone; the streams send it for a FIR
    // filter too (which must leave it clear). state[i] is the filter's history at lag i + 1.
    bool has_state = false;
    std::uint8_t state_bits = 0;   // 0..15
    std::uint8_t state_shift = 0;  // 0..15
    std::array<std::int32_t, kMaxFirOrder> state{};
    friend bool operator==(const FilterSpec&, const FilterSpec&) = default;
};

// TH spec §8.3: what a block header changes for one channel. A channel that is not `present`
// changes nothing.
struct ChannelUpdate {
    bool present = false;
    std::optional<FilterSpec> fir;
    std::optional<FilterSpec> iir;
    std::optional<std::int32_t> huff_offset;  // 15 bits, signed
    std::uint8_t huff_type = 0;               // 0 sends the low bits raw, 1..3 pick a codebook
    std::uint8_t huff_lsbs = 0;               // 5 bits
    friend bool operator==(const ChannelUpdate&, const ChannelUpdate&) = default;
};

// TH spec §10.2: one primitive matrix. coeff[c] is the value as sent (frac_bits + 2 bits, signed)
// for channel c; the channels are the matrix channels 0..max_matrix_chan, then the two noise
// channels.
struct MatrixSpec {
    std::uint8_t out_ch = 0;
    std::uint8_t frac_bits = 0;
    bool lsb_bypass = false;
    std::array<bool, kMaxChannels + kNoiseChannels> present{};
    std::array<std::int32_t, kMaxChannels + kNoiseChannels> coeff{};
    std::uint8_t noise_shift = 0;  // 4 bits, sent when the restart's noise_type is set
    // The matrices of restart sync 0x31EC (TH spec §10.2.1a) also send these three. f_shift widens
    // the range of the coefficients: the real value is coeff / 2^(frac_bits - f_shift), -1 to 6.
    // field_2b and field_m4 are reserved and carried as read.
    std::int8_t f_shift = 0;
    std::uint8_t field_2b = 0;  // 2 bits
    std::uint8_t field_m4 = 0;  // 4 bits
    // The second precision term of the object-audio matrices (TH spec §10.2.1a; ERRATA.md entry
    // 69): a width, two more bits and one value for each present coefficient. Parsed and carried; the
    // owner's DEE streams set it where the objects move, and what it does to the samples is open.
    std::uint8_t ext_width = 0;   // 4 bits
    std::uint8_t ext_field2 = 0;  // 2 bits
    std::array<std::int32_t, kMaxChannels> ext_value{};
    friend bool operator==(const MatrixSpec&, const MatrixSpec&) = default;
};

// What write_object_matrices sends of an object-audio section (ERRATA.md entry 69). The reader
// does not fill it in. The default is a full section: A and B set, every header and coefficient, no
// second precision term.
struct ObjectSectionForm {
    // A: the matrices change; clear, the last set and its deltas stand
    bool change = true;
    // B (with A): the headers follow; clear, coefficients alone on the headers that stand
    bool headers = true;
    // A2 and B2: a delta for each coefficient of a matrix with a width follows
    bool deltas = false;
    // C2 (with deltas): each matrix's width and reserved field follow
    bool widths = false;
    friend bool operator==(const ObjectSectionForm&, const ObjectSectionForm&) = default;
};

// The matrices a block header sends: `count` of `items` are used. A fixed array, so that reading a
// header does not allocate.
struct MatrixSet {
    std::array<MatrixSpec, kMaxObjectMatrices> items{};
    std::uint8_t count = 0;
    bool extension = false;  // object-audio matrices: a second precision term followed
    ObjectSectionForm form;  // object-audio matrices: how the writer sends the set; not read back
    friend bool operator==(const MatrixSet&, const MatrixSet&) = default;
};

// TH spec §8.2: what a block header changes. The fields that are empty are left as they were.
struct BlockHeaderSpec {
    std::optional<std::uint8_t> presence_flags;
    std::optional<std::uint16_t> block_size;
    std::optional<MatrixSet> matrices;
    std::optional<std::array<std::int8_t, kMaxChannels>> output_shift;  // matrix_channels() of them
    std::optional<std::array<std::uint8_t, kMaxChannels>> quant_step;   // max_chan + 1 of them
    std::array<ChannelUpdate, kMaxChannels> channels{};                 // min_chan..max_chan
    friend bool operator==(const BlockHeaderSpec&, const BlockHeaderSpec&) = default;
};

// What a block header's syntax depends on, from the restart header and the headers before it.
struct BlockContext {
    int min_chan = 0;
    int max_chan = 0;
    int max_matrix_chan = 0;
    bool noise_type = false;
    // Restart sync 0x31EC: the matrix section is the object-audio variant of TH spec §10.2.1a.
    bool object_matrices = false;
    // The object-audio matrices in force before this header: its headers are the ones a section
    // that sends none reuses (TH spec §10.2.1a). Not owned; null when there are none.
    const MatrixSet* object_previous = nullptr;
    // The open grammar of the second precision term, for the owner's trials (ERRATA.md entry 69):
    // bit 0 reads a value for every matrix channel and not for the present ones, bit 1 shares one
    // width and one reserved field among the matrices.
    std::uint8_t presence_flags = presence::kAll;  // in effect before this header
    friend bool operator==(const BlockContext&, const BlockContext&) = default;
};

// Reads the block header that follows a block's flag bits (and its restart header, if it has one).
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT std::expected<BlockHeaderSpec, Error> read_block_header(
    BitReader& reader, const BlockContext& context);

ICLFORGE_TRUEHD_EXPORT void write_block_header(BitWriter& writer, const BlockContext& context,
                                               const BlockHeaderSpec& header);

// TH spec App. C: the three Huffman codebooks. A symbol is −7..+10 (type 1), −7..+8 (type 2) or
// −7..+7 (type 3). `bits` is 0 for a symbol outside the codebook's range.
struct HuffmanCode {
    std::uint16_t code = 0;
    std::uint8_t bits = 0;
};
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT HuffmanCode huffman_code(int huff_type, int symbol) noexcept;
inline constexpr int kHuffmanMaxBits = 9;

}  // namespace iclforge::truehd
