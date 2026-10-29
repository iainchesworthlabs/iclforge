#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "iclforge/mlp/core/syntax.hpp"

// The encoder's search for prediction filters (9, 19.1): real-valued designs from the signal,
// quantised to what filter_params() can carry. Filter A alone is linear prediction from past
// samples (autocorrelation and Levinson-Durbin, every order from 1 to 8 at once); filters A and B
// together predict from past samples and past residuals, designed by two-stage least squares (a
// long autoregression estimates the residuals, then one solve fits both sets of coefficients).
// The candidates are pooled: each is costed exactly afterwards, with the decoder's arithmetic, and
// the cheapest kept, so a design that is poor for the actual integer recursion is never chosen.

namespace iclforge::mlp::detail {

// A pair of filters as the block header carries them: filter A and filter B share coeff_q (9.3).
struct FilterDesign {
    FilterParams fir;
    FilterParams iir;
};

// Quantises real coefficients to a design: the largest coeff_q in 8 to 15 under which every
// coefficient fits 16 bits, then the narrowest coeff_bits and the coeff_shift that drops the
// common trailing zeros. nullopt when a coefficient is too large for coeff_q 8.
[[nodiscard]] std::optional<FilterDesign> quantise(std::span<const double> fir,
                                                   std::span<const double> iir);

// Linear prediction coefficients for orders 1 to `max_order` of `signal`, windowed: entry k-1
// holds the order-k predictor (k coefficients), empty where the recursion stopped.
[[nodiscard]] std::vector<std::vector<double>> lpc(std::span<const std::int32_t> signal,
                                                   int max_order);

// A joint FIR + IIR predictor of the given orders by two-stage least squares; nullopt when the
// normal equations are singular.
[[nodiscard]] std::optional<std::pair<std::vector<double>, std::vector<double>>> arma(
    std::span<const std::int32_t> signal, int fir_order, int iir_order);

// Least-squares weights predicting `target` from `inputs` (each the same length as target), for
// the inter-channel matrices; nullopt when singular.
[[nodiscard]] std::optional<std::vector<double>> least_squares(
    std::span<const std::span<const std::int32_t>> inputs, std::span<const std::int32_t> target);

}  // namespace iclforge::mlp::detail
