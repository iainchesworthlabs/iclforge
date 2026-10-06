#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "iclforge/ac4/detail/real.hpp"
#include "core/acpl/acpl.hpp"
#include "core/dsp/complex.hpp"

// Advanced joint channel coding's signal processing, ETSI TS 103 190-2 V1.3.1
// clause 5.6: differential decoding and dequantisation (5.6.3.2, Pseudocodes 3
// to 5), the pre-modification of two decorrelator inputs (Pseudocode 9), and the
// coefficients of the modules with the sums they weight: the 7.X.4 channel
// modes' (b_5fronts 0) full and core decoding modules (Pseudocodes 11 and 14),
// and the 9.X.4 modes' (b_5fronts 1) ajcc_module_1() and ajcc_module_3()
// (Pseudocodes 10 and 13). The
// parameter bands, the interpolation, the decorrelators and the transient
// ducker are A-CPL's (acpl/acpl.hpp): clauses 5.6.3.1, 5.6.3.3 and 5.6.3.4
// define them by Part 1's, and Pseudocode 6 is Part 1's Pseudocode 109.
//
// The decoder runs these on its QMF matrices after A-SPX; a matrix of slots is
// laid out as the QMF banks' (dsp/qmf.hpp): value [ts * 64 + sb].

namespace iclforge::ac4::detail::ajcc {

inline constexpr int kSubbands = acpl::kSubbands;
inline constexpr int kMaxParamBands = acpl::kMaxParamBands;
inline constexpr int kMaxParamSets = acpl::kMaxParamSets;

// Table 83: the parameter bands ajcc_num_param_bands_id gives, 15, 12, 9 or 7.
[[nodiscard]] int num_param_bands(int num_param_bands_id) noexcept;

// The dry and wet parameters, which dequantise by a step (Pseudocodes 4 and
// 5); alpha and beta dequantise by Part 1's A-CPL tables (acpl/acpl.hpp).
enum class Kind : std::uint8_t { kDry, kWet };

// The quantised values a dry or wet parameter takes: its F0 codebook's (Part 2
// Annex A.1.2), dry 0 to 22 (11 coarse) and wet 0 to 40 (20).
[[nodiscard]] acpl::Range quantised_range(Kind kind, acpl::Quant quant) noexcept;

// Pseudocodes 4 and 5: q times 0.1 (0.2 coarse), less 0.6 for dry and 2.0 for
// wet.
[[nodiscard]] double dequantise(Kind kind, int q, acpl::Quant quant) noexcept;

// Pseudocode 3 for one parameter set: `coded` holds huff_decode_diff()'s
// values for bands 0 to num_bands - 1, each codebook index less its cb_off.
// DIFF_FREQ takes the first as it is and adds each next one to the band below;
// DIFF_TIME adds each to `previous`, the same band's value in the parameter set
// before. False, with `out` undefined, when a value leaves `range`.
[[nodiscard]] bool differential_decode(acpl::Range range, bool diff_time, int num_bands,
                                       std::span<const int, kMaxParamBands> coded,
                                       std::span<const int, kMaxParamBands> previous,
                                       std::span<int, kMaxParamBands> out) noexcept;

// One module's dequantised parameters in one parameter band of one parameter
// set: ajcc_alpha, ajcc_beta, ajcc_dry1 and 2, ajcc_wet1 to 3 of its side (the
// left module's 1 of each, the right's 2, 3 and 4, and 4 to 6).
struct ModuleParams {
    double alpha = 0.0;
    double beta = 0.0;
    double dry1 = 0.0;
    double dry2 = 0.0;
    double wet1 = 0.0;
    double wet2 = 0.0;
    double wet3 = 0.0;
};

// Pseudocode 11, ajcc_module_2() of full decoding: d0 to d9, then w0 to w14.
// Output k of the module's five (z0 to z4) is the sum of d_k and d_(k+5) times
// the inputs x0 and x1, and of w_k, w_(k+5) and w_(k+10) times the decorrelated
// y0, y1 and y2 (module_2_term()).
inline constexpr std::size_t kModule2Outputs = 5;
inline constexpr std::size_t kModule2Coefficients = 25;
[[nodiscard]] std::array<double, kModule2Coefficients> module_2(int core_mode,
                                                                const ModuleParams& p) noexcept;

// Pseudocode 14, ajcc_module_4() of core decoding: d0 to d5, then w0 to w5.
// Output k of three is d_k x0 + d_(k+3) x1 + w_k y0 + w_(k+3) y1.
inline constexpr std::size_t kModule4Outputs = 3;
inline constexpr std::size_t kModule4Coefficients = 12;
[[nodiscard]] std::array<double, kModule4Coefficients> module_4(int core_mode,
                                                                const ModuleParams& p) noexcept;

// Pseudocode 10, ajcc_module_1() of b_5fronts' full decoding: one input x and
// the decorrelated y0 and y1 make three outputs, z0 = d0 x + p0 y0 + p1 y1, z1 =
// d1 x + p2 y0 + p3 y1 and z2 = d2 x + p4 y0 + p5 y1. The coefficients are
// d0 to d2, then y0's weights p0, p2 and p4 on z0 to z2, then y1's p1, p3 and
// p5; ajcc_dry3 of the pseudocode is d2, 1 - dry1 - dry2. Only dry1, dry2 and
// wet1 to wet3 of `p` are read.
inline constexpr std::size_t kModule1Outputs = 3;
inline constexpr std::size_t kModule1Coefficients = 9;
[[nodiscard]] std::array<double, kModule1Coefficients> module_1(const ModuleParams& p) noexcept;

// Pseudocode 13, ajcc_module_3() of b_5fronts' core decoding: the front
// parameters `front` (dry2f and wet2f and wet3f of the pseudocode's arguments:
// dry2, wet2 and wet3 of this module's side) and the back parameters `back`
// (dry1b, dry2b, wet1b and wet3b: dry1, dry2, wet1 and wet3) make d0 to d5 and w0
// to w5 laid out as ajcc_module_4()'s, output k of three being d_k x0 + d_(k+3)
// x1 + w_k y0 + w_(k+3) y1 (module_term() with 3 outputs). The coefficients 0 to 2
// and 6 to 8 follow the front framing, 3 to 5 and 9 to 11 the back.
[[nodiscard]] std::array<double, kModule4Coefficients> module_3(const ModuleParams& front,
                                                                const ModuleParams& back) noexcept;

// Where a module's coefficient goes: the output it adds to, and whether it
// weights an input x (d) or a decorrelated y (w), and which.
struct Term {
    std::size_t output = 0;
    bool decorrelated = false;
    std::size_t input = 0;
};
[[nodiscard]] Term module_term(std::size_t coefficient, std::size_t outputs) noexcept;

// Where module_1()'s coefficient goes: the output it adds to, and whether it
// weights x (the first three) or y0 or y1 (input 0 or 1).
[[nodiscard]] Term module_1_term(std::size_t coefficient) noexcept;

// out += weight * in, value by value over `num_ts` slots: one term of
// Pseudocodes 11 and 14's sums, `weight` the interpolated coefficient.
template <typename Real>
void accumulate(std::span<const Real> weight, std::span<const dsp::Complex<Real>> in,
                std::span<dsp::Complex<Real>> out, int num_ts) noexcept;

// Pseudocode 9, input_sig_pre_modification(): out1 = g in2 + (1 - g) in1 and
// out2 = g in4 + (1 - g) in3, g 1 while ajcc_core_mode stays 0 and 0 while it
// stays 1, and ramping across the frame where it changes.
template <typename Real>
class PreModification {
   public:
    using Complex = dsp::Complex<Real>;

    // ajcc_core_mode_prev takes the next frame's ajcc_core_mode.
    void reset() noexcept { primed_ = false; }

    void process(int core_mode, int num_ts, std::span<const Complex> in1,
                 std::span<const Complex> in2, std::span<const Complex> in3,
                 std::span<const Complex> in4, std::span<Complex> out1,
                 std::span<Complex> out2) noexcept;

   private:
    bool primed_ = false;
    int core_mode_prev_ = 0;
};

extern template void accumulate<Real>(std::span<const Real>,
                                      std::span<const dsp::Complex<Real>>,
                                      std::span<dsp::Complex<Real>>, int) noexcept;
extern template class PreModification<Real>;

}  // namespace iclforge::ac4::detail::ajcc
