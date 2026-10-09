#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "iclforge/base/bitreader.hpp"
#include "decoder/pcm/snf_random.hpp"
#include "decoder/syntax/context.hpp"

// The speech spectral frontend: ssf_data() (ETSI TS 103 190-1 V1.4.1 clause 4.2.9, Tables 43 to
// 46, semantics 4.3.7) and its decoding to spectral lines (clause 5.2, Pseudocodes 4a to 58 and
// Annex C).
//
// The syntax and the decoding are one function. ssf_ac_data() is arithmetic coded and has no
// length field: where it ends is known only when the arithmetic decoder has finished
// (Pseudocode 47), and what it codes depends on tables the decoding builds (the allocation of
// bits to each band, from the envelope, the gain and the predictor gain - clause 5.2.5). So a
// granule is read, dequantised, predicted and scaled in one pass, block by block, and the
// frame's lines are what the sf_data() element holds (SsfData), already in window order.
//
// The text is defective in several places, whose readings libs/ac4/ERRATA.md gives under
// "The speech spectral frontend". No stream here uses the tool.

namespace iclforge::ac4::detail {

inline constexpr int kSsfMaxBands = 19;    // Pseudocode 7's MAX_NUM_BANDS
inline constexpr int kSsfMaxBlocks = 4;    // SHORT_STRIDE's num_blocks
inline constexpr int kSsfSpecBuffers = 5;  // clause 5.2.6's NUM_SPEC_BUF
inline constexpr int kSsfEnvBuffers = 4;   // and NUM_ENV_BUF
inline constexpr int kSsfMaxGranules = 2;  // Table 112: one or two per frame

// What one ssf_granule() decoded to: Table 111's stride_flag, the helper elements of 4.3.7.5.
struct SsfGranule {
    int stride_flag = 0;  // 0 LONG_STRIDE, 1 SHORT_STRIDE
    int num_bands = 0;
    int n_mdct = 0;  // the block length, granule_length / num_blocks
    int num_blocks = 1;
    int num_bins = 0;
};

// One sf_data() element of an SSF track in one frame: its granules and the lines they decode to,
// every block of every granule in order, n_mdct lines each, the lines above num_bins zero.
struct SsfData {
    int granule_count = 0;
    std::array<SsfGranule, kSsfMaxGranules> granules{};
    std::vector<double> lines;
};

// What an SSF track carries from granule to granule and frame to frame: the envelope and the
// predictor lag index of the last block, the two random number generators (clause 5.2.8.3) and
// the predictor's buffers (clause 5.2.6). All of it starts afresh at an SSF-I-frame.
struct SsfState {
    bool started = false;  // an SSF-I-frame has been decoded since the last failure
    int num_bands = 0;     // num_bands_minus12 + 12, sent in an I-frame granule only
    std::array<int, kSsfMaxBands> env_prev{};  // env[band] of the last granule
    int prev_pred_lag_idx = 0;                 // i_prev_pred_lag_idx
    RandGenState dither_gen{};
    RandGenState noise_gen{};
    std::vector<double> last_spec;  // the last block's output, f_spec[bin]
    std::array<std::vector<double>, kSsfSpecBuffers> spec_buffer;
    std::array<std::array<double, kSsfMaxBands>, kSsfEnvBuffers> env_buffer{};
};

// ssf_data(b_iframe) with b_iframe = ctx.b_iframe, over frame_len_base lines, into `out`. The
// reader is left after the last granule's arithmetic coded data. Fails with kMissingIFrame
// where no SSF-I-frame has come, kInvalidStream for the values 5.2 and Annex C do not define,
// and kTruncated where the data ends inside the element.
[[nodiscard]] ParseResult parse_ssf_data(BitReader& r, const SubstreamContext& ctx, SsfState& state,
                                         SsfData& out);

// The lengths of the blocks an SSF track's frame is cut into, in order, for the inverse
// transform (clause 5.5.3, Table 187: 768|768, 4*192|768 and so on).
void ssf_window_lengths(const SsfData& data, std::vector<int>& lengths);

// The fixed point helpers of Pseudocodes 29 and 30, exposed for the tests that hold them to the
// functions they approximate. Input and result Qx.10; false where the input leaves their range.
[[nodiscard]] bool ssf_map_db_to_lin(std::int32_t input, std::int32_t& result) noexcept;
[[nodiscard]] bool ssf_map_lin_to_db(std::int32_t input, std::int32_t& result) noexcept;

}  // namespace iclforge::ac4::detail
