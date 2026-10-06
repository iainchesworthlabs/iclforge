#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "iclforge/ac4dec/decoder.hpp"
#include "iclforge/ac4core/acpl/acpl.hpp"
#include "iclforge/ac4core/ajcc/ajcc.hpp"
#include "pcm/acpl.hpp"
#include "pcm/aspx.hpp"
#include "syntax/ajcc.hpp"

// A-JCC's QMF-domain step, ETSI TS 103 190-2 V1.3.1 clause 5.6, for the
// immersive element in ASPX_AJCC, after A-SPX (4.8.3.12): the five core
// channels, held in L, R, C, Ls and Rs as A'' to E'' (pcm/routing.hpp), made
// into the 7.X.4 layout's eleven channels in full decoding (5.6.3.5.2,
// Pseudocode 8) or the 5.X.2 core's seven in core decoding (5.6.3.5.3,
// Pseudocode 12); with b_5fronts (the 9.X.4 modes) the 9.X.4 layout's thirteen
// in full decoding, by four ajcc_module_1() (Pseudocode 10), or the same core's
// seven by two ajcc_module_3() (Pseudocode 13). A frame's parameters are differentially decoded
// when the frame is read (5.6.3.2), so that a value outside its range refuses the frame before
// anything moves on, and applied d_ctrl frames later with the signal they belong to, as A-CPL's are
// (pcm/acpl.hpp). The core (ajcc/ajcc.hpp) holds the dequantisation and the modules' coefficients;
// A-CPL's core the decorrelators, the transient ducker and the interpolation.

namespace iclforge::ac4::detail {

// ajcc_data()'s fourteen parameters without b_5fronts, in syntax order: alpha1,
// alpha2, beta1, beta2, dry1 to dry4, wet1 to wet6. The left module takes alpha1,
// beta1, dry1, dry2 and wet1 to wet3; the right alpha2, beta2, dry3, dry4 and wet4
// to wet6. With b_5fronts its twenty: dry1f to dry4f, dry1b to dry4b, wet1f to
// wet6f and wet1b to wet6b; the modules lf, rf, lb and rb take dry1 and dry2,
// dry3 and dry4, and wet1 to wet3 or wet4 to wet6, of the front or the back.
inline constexpr std::size_t kAjccParams = 20;

// One frame's A-JCC data, differentially decoded: each parameter's quantised
// values, [parameter][set][band], in syntax order.
struct AjccFrameValues {
    bool b_5fronts = false;
    int core_mode = 0;  // ajcc_core_mode
    int num_bands = ajcc::kMaxParamBands;
    acpl::Quant quant_ab = acpl::Quant::kFine;  // ajcc_qm_ab
    acpl::Quant quant_dw = acpl::Quant::kFine;  // ajcc_qm_dw
    acpl::Quant quant_f = acpl::Quant::kFine;   // ajcc_qm_f, with b_5fronts
    acpl::Quant quant_b = acpl::Quant::kFine;   // ajcc_qm_b
    // The left and the right module's; with b_5fronts the left front, right front, left back and
    // right back module's.
    std::array<acpl::Framing, 4> framing{};
    std::array<std::array<std::array<std::int8_t, ajcc::kMaxParamBands>, ajcc::kMaxParamSets>,
               kAjccParams>
        q{};

    // With b_5fronts, a module's (0 to 3: lf, rf, lb, rb) parameters in set `ps` and band `pb`:
    // Pseudocodes 4 and 5 on its dry1, dry2 and wet1 to wet3.
    [[nodiscard]] ajcc::ModuleParams module_params_5fronts(std::size_t module, std::size_t ps,
                                                           std::size_t pb) const noexcept;
};

// Pseudocode 3's ajcc_SET_q_prev: each parameter's quantised values in the
// last parameter set decoded.
struct AjccQuantHistory {
    std::array<std::array<int, ajcc::kMaxParamBands>, kAjccParams> params{};
};

// Which of an A-JCC frame's modules `values` has: four with b_5fronts, two otherwise.
inline constexpr std::size_t kAjccModules = 4;

// Clause 5.6.3.2 for `data`, moving `history` on. Fails when a value leaves its
// range: alpha's and beta's A-CPL tables, dry's and wet's F0 codebook.
// `history` is then partly moved on, so the caller passes a copy and keeps it
// only when the frame is kept.
[[nodiscard]] ParseResult ajcc_values(const AjccData& data, AjccQuantHistory& history,
                                      AjccFrameValues& out);

// What A-JCC carries from frame to frame: its decorrelators with their
// transient duckers, the pre-modification's ajcc_core_mode_prev, and
// ajcc_param_prev of every coefficient its modules interpolate.
class AjccStage {
   public:
    AjccStage();

    // The first frame's state: silence in the decorrelators and duckers, every
    // ajcc_param_prev 0 (5.6.3.3), and ajcc_core_mode_prev to be the next
    // frame's ajcc_core_mode.
    void reset();

    // Applies one frame's values to `channels`, which name L, R, C, Ls and Rs
    // and the channels `decoding` makes of them.
    void apply(DecodingMode decoding, const AjccFrameValues& values, int num_ts,
               const AcplChannels& channels);

   private:
    // One module: `side` 0 left, 1 right; `x` its two inputs, `y` its
    // decorrelated ones (three in full decoding, two in core); `z` its
    // outputs, which it writes.
    void module(DecodingMode decoding, std::size_t side, const AjccFrameValues& values, int num_ts,
                std::array<const std::vector<QmfValue>*, 2> x,
                std::array<const std::vector<QmfValue>*, 3> y,
                std::array<QmfMatrix, ajcc::kModule2Outputs> z);
    // b_5fronts: Pseudocode 10's module `module` (0 to 3: lf, rf, lb, rb) of full decoding, on
    // the input `x` and the decorrelated `y0` and `y1`, to `z`; or Pseudocode 13's module of core
    // decoding for `side` 0 or 1, which takes the front module `side` and the back module 2 +
    // `side` and writes its three outputs.
    void module_fronts(std::size_t module, const AjccFrameValues& values, int num_ts,
                       const std::vector<QmfValue>& x, const std::vector<QmfValue>& y0,
                       const std::vector<QmfValue>& y1, std::array<QmfMatrix, 3> z);
    void module_fronts_core(std::size_t side, const AjccFrameValues& values, int num_ts,
                            std::array<const std::vector<QmfValue>*, 2> x,
                            std::array<const std::vector<QmfValue>*, 2> y,
                            std::array<QmfMatrix, 3> z);
    // One coefficient of module `module`, interpolated with `framing` and added, weighted by
    // `term`'s input, to `z`, and its ajcc_param_prev moved on.
    void accumulate_coefficient(std::size_t module, std::size_t k, const acpl::Framing& framing,
                                const AjccFrameValues& values, int num_ts,
                                const std::vector<QmfValue>& in, QmfMatrix z);
    void decorrelate(std::size_t slot, const std::vector<QmfValue>& in, std::vector<QmfValue>& out,
                     int num_ts);
    void apply_fronts(DecodingMode decoding, const AjccFrameValues& values, int num_ts,
                      const AcplChannels& channels);

    // Pseudocode 8's decorrelators, D0, D2, D1, D0, D2 and D1, each its own
    // instance; core decoding takes the first two and the fourth and fifth. With b_5fronts
    // (Pseudocode 8's eight) the sixth and seventh are D2 and D2 as well; its modules take slots
    // 0 and 1 (lf: D0, D2), 3 and 4 (rf: D0, D2), 2 and 6 (lb: D1, D2) and 5 and 7 (rb: D1, D2).
    static constexpr std::size_t kDecorrelatorSlots = 8;
    std::array<acpl::Decorrelator<Real>, kDecorrelatorSlots> decorrelators_;
    std::array<acpl::TransientDucker<Real>, kDecorrelatorSlots> duckers_{};
    ajcc::PreModification<Real> pre_;
    // ajcc_param_prev and this frame's values of each module's coefficients,
    // [module][coefficient]: Pseudocode 11's 25 in full decoding, 14's 12 in core;
    // b_5fronts' modules take the first nine and twelve. The modules are the two sides (left and
    // right), or the four of b_5fronts' full decoding; its core's two take the front and back
    // framings' coefficients of one side, and the other two entries are not used there.
    // ac4core's acpl::ParamPrev/ParamSets stay double (see pcm/acpl.hpp's own
    // comment on acpl::interpolate() not being retemplated); ajcc::accumulate,
    // unlike acpl::interpolate, is templated on Real, so interp_ (its double
    // output) is narrowed once into interp_real_ before that call.
    std::array<std::array<acpl::ParamPrev, ajcc::kModule2Coefficients>, kAjccModules> prev_{};
    std::array<std::array<acpl::ParamSets, ajcc::kModule2Coefficients>, kAjccModules>
        coefficients_{};

    // Scratch, sized once: the five inputs times (2 + 1/sqrt 2) but C's, the
    // two pre-modified ones, a module's decorrelated inputs, and one
    // interpolated coefficient in ac4core's double and narrowed to Real.
    std::array<std::vector<QmfValue>, 4> x_in_{};
    std::array<std::vector<QmfValue>, 2> w_in_{};
    std::array<std::vector<QmfValue>, 3> y_{};
    std::array<std::vector<QmfValue>, 2> y_fronts_{};
    std::vector<double> interp_;
    std::vector<Real> interp_real_;
};

}  // namespace iclforge::ac4::detail
