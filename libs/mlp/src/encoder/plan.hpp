#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <vector>

#include "iclforge/mlp/core/error.hpp"
#include "iclforge/mlp/core/syntax.hpp"
#include "iclforge/mlp/encoder/encoder.hpp"

// The encoder's channel and matrix plan.
//
// Substreams and presentations (2.2, 2.3): channels 0 and 1 are substream 0 (the 2-channel
// presentation), channels 2 to 5 substream 1 (the 6-channel one), 6 and 7 substream 2 (the
// 8-channel one) and 8 to 15 substream 3 (the 16-channel one). Each substream's presentation is
// its matrix stack applied to the channels of every substream up to it (P29).
//
// The downmix (10.5): a presentation below the top is a fold-down of the one above it. Stage k
// folds presentation k's extra channels into presentation k-1's: each target channel t of the
// lower presentation becomes
//
//     d_t = x_t - floor(-sum_s C_s x_s / 2^14)
//
// which the primitive matrix with 1.0 on t and -C_s on each source undoes exactly (10.2). A
// presentation's stack therefore undoes stages 1 to k in that order. The limiter scales a stage's
// gains by 1, 1/2, 1/4 or 0 for a restart interval, the largest under which no folded channel
// leaves the sample range (primary, secondary, then the unfolded fallback of 10.5).
//
// Decorrelation: a channel j may be coded as its difference from a least-squares prediction from
// channels 0 to j-1, undone by a primitive matrix with 1.0 on j. Every presentation that holds j
// undoes it, before its downmix stages; each substream's budget of primitive matrices (10.3) caps
// how many are used.

namespace iclforge::mlp::detail {

struct SubstreamPlan {
    int min_chan = 0;
    int max_chan = 0;
    int max_matrix_chan = 0;  // the presentation's channels, less one
    std::uint16_t sync_word = kRestartSyncA;
    int max_matrices = 8;
};

// One fold of a downmix stage: the nominal gain of each source on the target.
struct FoldStep {
    int target = 0;
    std::array<double, kMaxChannels> gains{};
};

struct Plan {
    int channels = 0;
    int substreams = 1;
    std::array<SubstreamPlan, kMaxSubstreams> sub{};
    std::uint16_t assignment = 0;
    std::uint8_t sixch_assignment = 0;
    std::uint16_t eightch_assignment = 0;
    std::uint8_t substream_info = 0;
    // stages[k] folds presentation k into presentation k - 1 (k from 1).
    std::array<std::vector<FoldStep>, kMaxSubstreams> stages{};

    [[nodiscard]] int substream_of(int channel) const noexcept {
        for (int s = 0; s < substreams; ++s) {
            if (channel <= sub[static_cast<std::size_t>(s)].max_chan) {
                return s;
            }
        }
        return substreams - 1;
    }
};

[[nodiscard]] std::expected<Plan, Error> make_plan(int channels, std::uint16_t assignment,
                                                   bool downmix);

// One restart interval's analysis.
struct IntervalPlan {
    // The source's trailing zeros, carried by the output shift (a 4-bit signed field, so at most
    // 7) and the quantiser step for the rest: a 16-bit source in 24-bit words is shift 7 and
    // quant 1, as FFmpeg codes one.
    int shift = 0;
    int quant = 0;
    int max_bits = 0;  // the restart header's max_bits: the widest output sample's bits
    bool limited = false;
    int decorrelation_count = 0;
    // The channels as coded (shifted, folded and decorrelated), which prediction then codes.
    std::vector<std::vector<std::int32_t>> coded;
    // Each substream's presentation, shifted: its channels 0 to max_matrix_chan.
    std::array<std::vector<std::vector<std::int32_t>>, kMaxSubstreams> presentations{};
    std::array<MatrixParams, kMaxSubstreams> stacks{};
};

// `input` is per channel, at 24-bit scale. `gains` overrides stage 3's nominal fold gains (the
// Atmos objects' render into the bed), per source and target, when not empty.
[[nodiscard]] std::expected<void, Error> plan_interval(
    const Plan& plan, const EncoderConfig& config,
    const std::vector<std::vector<std::int32_t>>& input, IntervalPlan& out);

}  // namespace iclforge::mlp::detail
