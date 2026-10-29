#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "iclforge/base/bitwriter.hpp"

// The entropy coding of a block of prediction residuals (11): the choice of huff_type, huff_lsbs
// and huff_offset that codes them in the fewest bits, and the writing of each sample. The
// reconstruction the choice must invert is P23's: for huff_type t and lsbs_bits n,
//
//     residual = (msb << n) + raw_lsbs - bias(t, n) + huff_offset,  bias = 1 << (n + 2 - t)
//
// and for huff_type 0, residual = raw_lsbs - (1 << (n - 1)) + huff_offset. The encoder never
// uses a quantiser step (its output shift carries a source's trailing zeros instead), so
// huff_lsbs is lsbs_bits.

namespace iclforge::mlp::detail {

struct CodingParams {
    int huff_type = 0;  // 0 raw, 1 to 3 the codebooks of Appendix C
    int lsbs = 0;       // huff_lsbs (= lsbs_bits)
    int offset = 0;     // huff_offset, 15 bits signed
};

inline constexpr std::int64_t kInfiniteCost = std::numeric_limits<std::int64_t>::max() / 4;

// The bits `residuals` take under `params`, or kInfiniteCost when some residual cannot be coded
// with them (an MSB outside the codebook, a raw value outside its width).
[[nodiscard]] std::int64_t coding_cost(std::span<const std::int32_t> residuals,
                                       const CodingParams& params) noexcept;

struct CodingChoice {
    CodingParams params;
    std::int64_t bits = kInfiniteCost;
};

// The cheapest parameters for `residuals`. `current` is what the decoder holds now: keeping its
// offset saves the 16 bits of a new one, and keeping its type and width saves the channel's
// parameter bits, which `header_bits` returns. Searches every huff_type, the widths around the
// residuals' spread and the offsets that centre them.
// `max_lsbs` is the widest lsbs_bits the channel can take: 24 less its quantiser step, since
// huff_lsbs, which is lsbs_bits plus the step, is at most 24 (8.3).
[[nodiscard]] CodingChoice choose_coding(std::span<const std::int32_t> residuals,
                                         const CodingParams& current, int max_lsbs = 24);

// The block header bits a channel's coding parameters cost against the decoder's current ones:
// the huff_type and huff_lsbs fields (7) when the channel's parameters are sent at all, and a new
// offset's 16. `filter_changed` says the channel's parameters are sent anyway.
[[nodiscard]] int header_bits(const CodingParams& next, const CodingParams& current,
                              bool filter_changed) noexcept;

// Writes one sample's code: its MSB codeword and raw LSBs, or its raw LSBs.
void write_sample(BitWriter& out, std::int32_t residual, const CodingParams& params);

}  // namespace iclforge::mlp::detail
