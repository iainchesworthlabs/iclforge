#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "iclforge/mlp/core/stream_info.hpp"

// The syntax elements of a TrueHD (MLP) access unit below the major sync, as values: the access
// unit header (3.1), the substream directory (6.1), the restart header (7.1) and a block's
// decoding parameters (8.2, 8.3, 9.2, 10.2). Coefficients are held as they are coded, so that a
// structure read and written again gives the same bits; the decoder derives the effective values.
//
// One description of each structure serves both directions: src/core/syntax.hpp transfers each
// through a reader or a writer with the same function, so the two cannot drift apart.

namespace iclforge::mlp {

// Section 3.1, the first 32 bits of every access unit.
struct AccessUnitHeader {
    std::uint8_t check_nibble = 0;
    std::uint16_t length_words = 0;  // access_unit_length, in 16-bit words
    std::uint16_t input_timing = 0;
};

// Section 6.1. substream_end_ptr counts 16-bit words from the end of the directory (P13).
struct SubstreamDirectoryEntry {
    bool extra_substream_word = false;
    bool restart_nonexistent = false;
    bool crc_present = false;
    bool reserved = false;
    std::uint16_t end_ptr = 0;
    // When extra_substream_word: 12.1's drc_gain_update (9 bits, signed) and drc_time_update.
    std::int16_t drc_gain_update = 0;
    std::uint8_t drc_time_update = 0;
    std::uint8_t drc_padding = 0;
};

// Restart sync words (7.2, P5, P22). 0x31EA and 0x31EB serve substreams 0 to 2, 0x31EC the
// 16-channel substream.
inline constexpr std::uint16_t kRestartSyncA = 0x31EA;
inline constexpr std::uint16_t kRestartSyncB = 0x31EB;
inline constexpr std::uint16_t kRestartSyncC = 0x31EC;

// Section 7.1 as FFmpeg's streams hold it (P6): no separate noise_type bit, and the 19
// "reserved" bits are max_shift, max_lsbs, max_bits and max_bits_repeat.
struct RestartHeader {
    std::uint16_t sync_word = kRestartSyncA;  // 14 bits
    std::uint16_t output_timing = 0;
    std::uint8_t min_chan = 0;
    std::uint8_t max_chan = 0;
    std::uint8_t max_matrix_chan = 0;
    std::uint8_t noise_shift = 0;
    std::uint32_t noisegen_seed = 0;  // 23 bits
    std::uint8_t max_shift = 0;
    std::uint8_t max_lsbs = 0;
    std::uint8_t max_bits = 0;
    std::uint8_t max_bits_repeat = 0;
    bool data_check_present = false;
    // The check of the restart interval that ends at this header (P14); 0 in the first.
    std::uint8_t lossless_check = 0;
    std::uint16_t reserved = 0;
    std::array<std::uint8_t, kMaxChannels> ch_assign{};  // [0, max_matrix_chan]
    std::uint8_t crc = 0;  // as read; the writer computes it

    // P22: the sync word's last bit is the noise type; 0x31EC's substream has none.
    [[nodiscard]] bool noise_type() const noexcept { return sync_word == kRestartSyncB; }
};

// Section 9.2. Coefficients and states as coded; the effective coefficient is coeff << coeff_shift
// and the effective state state << state_shift.
struct FilterParams {
    std::uint8_t order = 0;
    std::uint8_t coeff_q = 0;
    std::uint8_t coeff_bits = 0;
    std::uint8_t coeff_shift = 0;
    std::array<std::int32_t, kMaxFirOrder> coeffs{};
    bool new_states = false;  // read for both filters, set only for filter B (P19)
    std::uint8_t state_bits = 0;
    std::uint8_t state_shift = 0;
    std::array<std::int32_t, kMaxIirOrder> states{};
};

// Section 10.2, one primitive matrix. coeff[ch] is coded in frac_bits + 2 bits, signed; its
// effective value in units of 2^-14 is coeff[ch] << (14 - frac_bits). The loop runs over
// max_chan + 3 inputs, the last two the noise channels (P21).
struct PrimitiveMatrix {
    std::uint8_t output_channel = 0;
    std::uint8_t frac_bits = 0;
    bool lsb_bypass = false;
    std::uint32_t coeff_present = 0;  // bit ch
    std::array<std::int32_t, kMaxMatrixInputs> coeffs{};
    std::uint8_t noise_shift = 0;  // read when the restart header's noise type is 1
};

struct MatrixParams {
    std::uint8_t count = 0;
    std::array<PrimitiveMatrix, kMaxPrimitiveMatrices> matrices{};
};

// Section 8.3 for one channel. An absent optional is a new_* bit of 0.
struct ChannelParams {
    std::optional<FilterParams> fir;  // filter A
    std::optional<FilterParams> iir;  // filter B
    std::optional<std::int16_t> huff_offset;  // 15 bits, signed
    std::uint8_t huff_type = 0;
    std::uint8_t huff_lsbs = 0;
};

// Section 8.2's parameter presence flags (P11): which new_* bits a block header carries.
inline constexpr std::uint8_t kPresenceBlockSize = 0x80;
inline constexpr std::uint8_t kPresenceMatrix = 0x40;
inline constexpr std::uint8_t kPresenceOutputShift = 0x20;
inline constexpr std::uint8_t kPresenceQuantStep = 0x10;
inline constexpr std::uint8_t kPresenceFir = 0x08;
inline constexpr std::uint8_t kPresenceIir = 0x04;
inline constexpr std::uint8_t kPresenceHuffOffset = 0x02;
inline constexpr std::uint8_t kPresenceAll = 0xFF;

// Section 8.2, the parameters one block header changes. An absent optional is a new_* bit of 0.
struct BlockHeader {
    std::optional<std::uint8_t> param_presence;  // new_guards
    std::optional<std::uint16_t> block_size;
    std::optional<MatrixParams> matrix;
    std::optional<std::array<std::int8_t, kMaxChannels>> output_shift;  // [0, max_matrix_chan]
    std::optional<std::array<std::uint8_t, kMaxChannels>> quant_step;   // [0, max_chan]
    std::array<std::optional<ChannelParams>, kMaxChannels> channels{};   // [min_chan, max_chan]
};

}  // namespace iclforge::mlp
