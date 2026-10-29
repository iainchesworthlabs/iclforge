#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "iclforge/base/bitwriter.hpp"
#include "iclforge/truehd/core/syntax.hpp"

// The pieces of the encoder that decide how a channel is coded: the residual coder of TH spec §11
// (the codebook, the raw width and the offset a block uses, and their bit count) and the FIR
// filter of TH spec §9 (analysis and quantisation). They are pure functions of the samples; the
// stream layer (encoder.cpp) decides where their results go.

namespace iclforge::truehd::detail {

// What a channel's block data is coded with (TH spec §8.3): the values of huff_type, huff_lsbs and
// huff_offset. `lsbs` counts the raw bits plus the channel's quantiser step.
struct ChannelCoding {
    std::uint8_t type = 0;
    std::uint8_t lsbs = 24;  // the value a restart leaves (ERRATA.md, entry 19)
    std::int32_t offset = 0;
    friend bool operator==(const ChannelCoding&, const ChannelCoding&) = default;
};

inline constexpr int kInfeasible = 1 << 30;
inline constexpr int kMaxSampleBits = 29;  // TH spec §11.4: a Huffman coded sample's width limit

// The bits a block of residuals (already shifted right by the channel's quantiser step) takes with
// `coding`, or kInfeasible if a residual cannot be coded with it.
[[nodiscard]] int coded_bits(std::span<const std::int32_t> residuals, int quant,
                             const ChannelCoding& coding) noexcept;

struct CodingChoice {
    ChannelCoding coding;
    int data_bits = kInfeasible;  // the block's sample data
    int header_bits = 0;          // what the block header spends on this channel
    [[nodiscard]] int total() const noexcept { return data_bits + header_bits; }
};

// The bits a channel's entry in a block header takes when it moves from `before` to `after`, with
// `fir_bits` more if the entry also carries a filter (TH spec §8.2, §8.3).
[[nodiscard]] int channel_header_bits(const ChannelCoding& before, const ChannelCoding& after,
                                      int fir_bits) noexcept;

// The coding of `residuals` that costs the fewest bits in all, header included, starting from
// `before`; data_bits is kInfeasible if none can code them.
[[nodiscard]] CodingChoice choose_coding(std::span<const std::int32_t> residuals, int quant,
                                         const ChannelCoding& before, int fir_bits) noexcept;

// Writes one residual with `coding` (TH spec §11.3's inverse). `lb` is the raw width,
// coding.lsbs - quant.
void put_residual(BitWriter& writer, std::int32_t residual, int quant,
                  const ChannelCoding& coding) noexcept;

// --- TH spec §9: the FIR filter ---

// A rough bit count for a block of residuals, for choosing between filters without searching
// every coding: n * (log2(mean |e|) + 1.6) as a Rice code would take, 0 for an all-zero block.
[[nodiscard]] double estimated_bits(std::span<const std::int32_t> residuals) noexcept;

// Prediction coefficients of orders 1..max_order from the autocorrelation r[0..max_order] by the
// Levinson-Durbin recursion: coeff[order][k - 1] predicts x[t] from x[t - k]; error[order] is the
// prediction error power. r[0] must be positive.
struct Predictors {
    static constexpr int kMaxOrder = kMaxFirOrder;
    std::array<std::array<double, kMaxOrder>, kMaxOrder + 1> coeff{};
    std::array<double, kMaxOrder + 1> error{};
    int max_order = 0;
};
[[nodiscard]] Predictors levinson(std::span<const double> autocorrelation, int max_order) noexcept;

// The windowed autocorrelation of `samples` at lags 0..max_lag.
[[nodiscard]] std::vector<double> autocorrelation(std::span<const std::int32_t> samples,
                                                  int max_lag);

// A FilterSpec for `order` real coefficients under TH spec §9.3's limits: coeff_q (the shift) from
// 8 to 15, the coefficients within 16 bits once shifted down by coeff_shift. Order 0 gives a
// filter with no coefficients.
[[nodiscard]] FilterSpec quantise_fir(std::span<const double> coefficients);

// The bits a filter takes in a block header (TH spec §9.2), 0 for an empty one, which is not sent.
[[nodiscard]] int filter_header_bits(const FilterSpec& filter) noexcept;

// The prediction of the FIR filter for sample `t` of `history`: sum(coeff[k] * history[t - 1 - k])
// >> shift, with history before the start taken as zero (TH spec §9.4; the decoder's history is
// zero after a restart).
[[nodiscard]] std::int64_t fir_prediction(const FilterSpec& filter,
                                          std::span<const std::int32_t> history,
                                          std::size_t t) noexcept;

}  // namespace iclforge::truehd::detail
