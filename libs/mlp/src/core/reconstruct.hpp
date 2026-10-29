#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "iclforge/mlp/core/stream_info.hpp"
#include "iclforge/mlp/core/syntax.hpp"

// The arithmetic the decoder runs and the encoder runs backwards, shared so that the two cannot
// differ: prediction (9.4, 9.5), the primitive matrices (10), the output shift and the lossless
// check (7.3). Each rule is the one under which FFmpeg's streams decode to their sources exactly
// (planning/truehd.md, P14, P15, P21, P23); 64-bit accumulators throughout, so that no
// intermediate overflows whatever the coefficients.

namespace iclforge::mlp::detail {

// One channel's prediction state: the effective coefficients of filters A and B, their shared
// coeff_q, and the history each filter reads, the most recent first. Filter A reads past samples;
// filter B reads past residuals, which is what makes the pair an IIR predictor.
struct ChannelFilter {
    int fir_order = 0;
    int iir_order = 0;
    int coeff_q = 0;
    std::array<std::int32_t, kMaxFirOrder> fir_coeffs{};
    std::array<std::int32_t, kMaxIirOrder> iir_coeffs{};
    std::array<std::int32_t, kMaxFirOrder> fir_history{};
    std::array<std::int32_t, kMaxIirOrder> iir_history{};

    void clear_history() noexcept {
        fir_history.fill(0);
        iir_history.fill(0);
    }

    // The prediction for the next sample, rounded down to a multiple of 1 << quant (P23's
    // reconstruction adds it to a residual that is itself such a multiple).
    [[nodiscard]] std::int64_t predict(int quant) const noexcept {
        if (fir_order == 0 && iir_order == 0) {
            return 0;
        }
        std::int64_t acc = 0;
        for (int k = 0; k < fir_order; ++k) {
            acc += std::int64_t{fir_coeffs[static_cast<std::size_t>(k)]} *
                   fir_history[static_cast<std::size_t>(k)];
        }
        for (int k = 0; k < iir_order; ++k) {
            acc += std::int64_t{iir_coeffs[static_cast<std::size_t>(k)]} *
                   iir_history[static_cast<std::size_t>(k)];
        }
        const std::int64_t prediction = acc >> coeff_q;
        return (prediction >> quant) << quant;
    }

    // Records a sample and the residual it was coded with.
    void push(std::int32_t sample, std::int32_t residual) noexcept {
        for (std::size_t k = kMaxFirOrder - 1; k > 0; --k) {
            fir_history[k] = fir_history[k - 1];
        }
        fir_history[0] = sample;
        for (std::size_t k = kMaxIirOrder - 1; k > 0; --k) {
            iir_history[k] = iir_history[k - 1];
        }
        iir_history[0] = residual;
    }

    // Installs a filter's coded parameters (9.2): its effective coefficients, and filter B's
    // states when it sends them.
    void set_fir(const FilterParams& f) noexcept {
        fir_order = f.order;
        for (int k = 0; k < f.order; ++k) {
            fir_coeffs[static_cast<std::size_t>(k)] = f.coeffs[static_cast<std::size_t>(k)]
                                                      << f.coeff_shift;
        }
        if (f.order > 0) {
            coeff_q = f.coeff_q;
        }
    }
    void set_iir(const FilterParams& f) noexcept {
        iir_order = f.order;
        for (int k = 0; k < f.order; ++k) {
            iir_coeffs[static_cast<std::size_t>(k)] = f.coeffs[static_cast<std::size_t>(k)]
                                                      << f.coeff_shift;
        }
        if (f.order > 0) {
            coeff_q = f.coeff_q;
        }
        if (f.new_states) {
            for (int k = 0; k < f.order; ++k) {
                iir_history[static_cast<std::size_t>(k)] = f.states[static_cast<std::size_t>(k)]
                                                           << f.state_shift;
            }
        }
    }
};

// A primitive matrix in effective form: coefficients in units of 2^-14 (P21).
struct EffectiveMatrix {
    int output_channel = 0;
    bool lsb_bypass = false;
    std::array<std::int32_t, kMaxMatrixInputs> coeffs{};
};

[[nodiscard]] inline EffectiveMatrix effective(const PrimitiveMatrix& p) noexcept {
    EffectiveMatrix m;
    m.output_channel = p.output_channel;
    m.lsb_bypass = p.lsb_bypass;
    for (std::size_t ch = 0; ch < kMaxMatrixInputs; ++ch) {
        if (((p.coeff_present >> ch) & 1U) != 0) {
            m.coeffs[ch] = p.coeffs[ch] * (std::int32_t{1} << (14 - p.frac_bits));
        }
    }
    return m;
}

// The new value of a matrix's output channel for one sample row (10, P21): the weighted sum of
// the row's inputs, rounded down to the output channel's quantiser step, plus the bypassed LSB.
[[nodiscard]] inline std::int64_t matrix_output(const EffectiveMatrix& m, const std::int32_t* row,
                                                int inputs, int quant,
                                                unsigned bypass_bit) noexcept {
    std::int64_t acc = 0;
    for (int ch = 0; ch < inputs; ++ch) {
        acc += std::int64_t{m.coeffs[static_cast<std::size_t>(ch)]} * row[ch];
    }
    const std::int64_t v = acc >> 14;
    return ((v >> quant) << quant) + bypass_bit;
}

[[nodiscard]] constexpr std::int32_t apply_output_shift(std::int32_t v, int shift) noexcept {
    return shift >= 0 ? static_cast<std::int32_t>(static_cast<std::uint32_t>(v) << shift)
                      : v >> (-shift);
}

// Section 7.3 as P14 reads it: a 24-bit accumulator into which every output sample's low 24 bits
// go rotated left by its channel's index, folded to a byte.
class LosslessCheck {
   public:
    void add(std::int32_t sample, int channel) noexcept {
        const std::uint32_t v = static_cast<std::uint32_t>(sample) & 0xFFFFFFU;
        const unsigned k = static_cast<unsigned>(channel) % 24U;
        const std::uint32_t r = k == 0 ? v : ((v << k) | (v >> (24U - k))) & 0xFFFFFFU;
        acc_ ^= r;
    }
    [[nodiscard]] std::uint8_t value() const noexcept {
        return static_cast<std::uint8_t>((acc_ ^ (acc_ >> 8U) ^ (acc_ >> 16U)) & 0xFFU);
    }
    void reset() noexcept { acc_ = 0; }

   private:
    std::uint32_t acc_ = 0;
};

// The MSB bias of a Huffman-coded sample (P23): huff_type t centres its codebook by
// 1 << (lsbs_bits + 2 - t), nothing when that shift is negative.
[[nodiscard]] constexpr std::int64_t huffman_bias(int huff_type, int lsbs_bits) noexcept {
    const int shift = lsbs_bits + 2 - huff_type;
    return shift >= 0 ? std::int64_t{1} << shift : 0;
}

}  // namespace iclforge::mlp::detail
