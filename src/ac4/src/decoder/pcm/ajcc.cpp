#include "pcm/ajcc.hpp"

#include <algorithm>
#include <initializer_list>
#include <numbers>
#include <span>

namespace iclforge::ac4::detail {
namespace {

using S = Speaker;

constexpr double kSqrt2 = std::numbers::sqrt2;

// Pseudocodes 8 and 12: every input times 2 + 1/sqrt(2).
constexpr double kInputGain = 2.0 + 1.0 / std::numbers::sqrt2;
constexpr Real kSqrt2Real = static_cast<Real>(kSqrt2);
constexpr Real kInputGainReal = static_cast<Real>(kInputGain);

// AjccFrameValues::q's parameters, in syntax order.
enum Param : std::uint8_t {
    kAlpha1,
    kAlpha2,
    kBeta1,
    kBeta2,
    kDry1,
    kDry2,
    kDry3,
    kDry4,
    kWet1,
    kWet2,
    kWet3,
    kWet4,
    kWet5,
    kWet6,
};

[[nodiscard]] std::size_t at(int index) noexcept {
    return static_cast<std::size_t>(index);
}

[[nodiscard]] acpl::Quant quant_of(int quant_mode) noexcept {
    return quant_mode == 0 ? acpl::Quant::kFine : acpl::Quant::kCoarse;
}

[[nodiscard]] acpl::Framing framing_of(const AjccFraming& framing) noexcept {
    return acpl::Framing{.steep = framing.interpolation_type == 1,
                         .num_param_sets = framing.num_param_sets,
                         .param_timeslot = {framing.param_timeslot[0], framing.param_timeslot[1]}};
}

// One module's dequantised parameters in parameter set `ps` and band `pb`
// (Pseudocodes 4 and 5, and Part 1 Tables 203 to 206 for alpha and beta).
[[nodiscard]] ajcc::ModuleParams module_params(const AjccFrameValues& v, std::size_t side,
                                               std::size_t ps, std::size_t pb) noexcept {
    const auto q = [&](Param left, Param right) {
        return static_cast<int>(v.q[side == 0 ? left : right][ps][pb]);
    };
    const acpl::AlphaValue alpha = acpl::dequantise_alpha(q(kAlpha1, kAlpha2), v.quant_ab);
    using ajcc::Kind;
    return {.alpha = alpha.alpha,
            .beta = acpl::dequantise_beta(q(kBeta1, kBeta2), alpha.ibeta, v.quant_ab),
            .dry1 = ajcc::dequantise(Kind::kDry, q(kDry1, kDry3), v.quant_dw),
            .dry2 = ajcc::dequantise(Kind::kDry, q(kDry2, kDry4), v.quant_dw),
            .wet1 = ajcc::dequantise(Kind::kWet, q(kWet1, kWet4), v.quant_dw),
            .wet2 = ajcc::dequantise(Kind::kWet, q(kWet2, kWet5), v.quant_dw),
            .wet3 = ajcc::dequantise(Kind::kWet, q(kWet3, kWet6), v.quant_dw)};
}

}  // namespace

ajcc::ModuleParams AjccFrameValues::module_params_5fronts(std::size_t module, std::size_t ps,
                                                          std::size_t pb) const noexcept {
    // dry1f to dry4f then dry1b to dry4b, then wet1f to wet6f and wet1b to wet6b: module m (lf,
    // rf, lb, rb) has dry1 and dry2 at 2m and 2m + 1, and wet1 to wet3 at 8 + 3m.
    const acpl::Quant quant = module < 2 ? quant_f : quant_b;
    const auto dry = [&](std::size_t k) {
        return ajcc::dequantise(ajcc::Kind::kDry, static_cast<int>(q[2 * module + k][ps][pb]),
                                quant);
    };
    const auto wet = [&](std::size_t k) {
        return ajcc::dequantise(ajcc::Kind::kWet, static_cast<int>(q[8 + 3 * module + k][ps][pb]),
                                quant);
    };
    return {.dry1 = dry(0), .dry2 = dry(1), .wet1 = wet(0), .wet2 = wet(1), .wet3 = wet(2)};
}

ParseResult ajcc_values(const AjccData& data, AjccQuantHistory& history, AjccFrameValues& out) {
    out = AjccFrameValues{};
    out.b_5fronts = data.b_5fronts;
    out.core_mode = data.core_mode;
    out.num_bands = ajcc::num_param_bands(data.num_param_bands_id);
    out.quant_ab = quant_of(data.qm_ab);
    out.quant_dw = quant_of(data.qm_dw);
    out.quant_f = quant_of(data.qm_f);
    out.quant_b = quant_of(data.qm_b);
    const std::size_t modules = data.b_5fronts ? kAjccModules : 2;
    for (std::size_t module = 0; module < modules; ++module) {
        out.framing[module] = framing_of(data.framing[module]);
    }
    // 6.2.6.1's order: without b_5fronts alpha1, alpha2, beta1, beta2, dry1 to dry4, wet1 to
    // wet6; with it dry1f to dry4f, dry1b to dry4b, wet1f to wet6f, wet1b to wet6b.
    std::array<const AjccParams*, kAjccParams> params{};
    std::size_t count = 0;
    if (data.b_5fronts) {
        for (std::size_t k = 0; k < kAjccMaxDry; ++k) {
            params[count++] = &data.dry[k];
        }
        for (std::size_t k = 0; k < kAjccMaxWet; ++k) {
            params[count++] = &data.wet[k];
        }
    } else {
        for (const AjccParams* param :
             {&data.alpha[0], &data.alpha[1], &data.beta[0], &data.beta[1], &data.dry[0],
              &data.dry[1], &data.dry[2], &data.dry[3], &data.wet[0], &data.wet[1], &data.wet[2],
              &data.wet[3], &data.wet[4], &data.wet[5]}) {
            params[count++] = param;
        }
    }
    for (std::size_t p = 0; p < count; ++p) {
        const AjccParams& param = *params[p];
        const acpl::Quant quant = quant_of(param.quant_mode);
        acpl::Range range{};
        switch (param.data_type) {
            case AjccDataType::kAlpha:
                range = acpl::quantised_range(acpl::Kind::kAlpha, quant);
                break;
            case AjccDataType::kBeta:
                range = acpl::quantised_range(acpl::Kind::kBeta, quant);
                break;
            case AjccDataType::kDry:
                range = ajcc::quantised_range(ajcc::Kind::kDry, quant);
                break;
            case AjccDataType::kWet:
                range = ajcc::quantised_range(ajcc::Kind::kWet, quant);
                break;
        }
        for (std::size_t ps = 0; ps < param.num_param_sets && ps < ajcc::kMaxParamSets; ++ps) {
            const AjccParamSet& set = param.sets[ps];
            const bool diff_time = set.diff_type == 1;
            const int f0_off =
                ajcc_codebook(param.data_type, param.quant_mode, AjccHcbType::kF0).cb_off;
            const int rest_off = ajcc_codebook(param.data_type, param.quant_mode,
                                               diff_time ? AjccHcbType::kDt : AjccHcbType::kDf)
                                     .cb_off;
            std::array<int, ajcc::kMaxParamBands> coded{};
            for (int i = 0; i < out.num_bands; ++i) {
                const int off = !diff_time && i == 0 ? f0_off : rest_off;
                coded[at(i)] = static_cast<int>(set.huff_index[at(i)]) - off;
            }
            std::array<int, ajcc::kMaxParamBands> decoded{};
            if (!ajcc::differential_decode(range, diff_time, out.num_bands, coded,
                                           history.params[p], decoded)) {
                return fail(DecodeError::kInvalidStream,
                            "an A-JCC parameter outside its quantisation range");
            }
            history.params[p] = decoded;
            for (int i = 0; i < out.num_bands; ++i) {
                out.q[p][ps][at(i)] = static_cast<std::int8_t>(decoded[at(i)]);
            }
        }
    }
    return {};
}

AjccStage::AjccStage()
    : decorrelators_{acpl::Decorrelator<Real>(0), acpl::Decorrelator<Real>(2),
                     acpl::Decorrelator<Real>(1), acpl::Decorrelator<Real>(0),
                     acpl::Decorrelator<Real>(2), acpl::Decorrelator<Real>(1),
                     acpl::Decorrelator<Real>(2), acpl::Decorrelator<Real>(2)} {}

void AjccStage::reset() {
    for (auto& decorrelator : decorrelators_) {
        decorrelator.reset();
    }
    for (auto& ducker : duckers_) {
        ducker.reset();
    }
    pre_.reset();
    // Each side in turn: assigning a whole `{}` builds the 25 kB array as a temporary first.
    for (auto& side : prev_) {
        side.fill(acpl::ParamPrev{});
    }
}

// Pseudocode 111 on `in`, then Pseudocode 114's ducking, as A-CPL's
// inputSignalModification() and applyTransientDucker() (clause 5.6.3.4).
void AjccStage::decorrelate(std::size_t slot, const std::vector<QmfValue>& in,
                            std::vector<QmfValue>& out, int num_ts) {
    out.resize(in.size());
    decorrelators_[slot].process(in, out, num_ts);
    duckers_[slot].process(out, num_ts);
}

// One term of a module's sums: the coefficient `k` of module `module`, interpolated (Pseudocode 6)
// with `framing` and its own ajcc_param_prev, times `in`, added to `z`; then its
// ajcc_param_prev moves on. A coefficient that is 0 now and was 0 before adds nothing.
void AjccStage::accumulate_coefficient(std::size_t module, std::size_t k,
                                       const acpl::Framing& framing, const AjccFrameValues& values,
                                       int num_ts, const std::vector<QmfValue>& in, QmfMatrix z) {
    auto& coefficients = coefficients_[module];
    auto& prev = prev_[module];
    const bool silent = std::ranges::all_of(prev[k], [](double v) { return v == 0.0; }) &&
                        std::ranges::all_of(coefficients[k], [](const auto& set) {
                            return std::ranges::all_of(set, [](double v) { return v == 0.0; });
                        });
    if (!silent) {
        interp_.resize(at(num_ts) * at(ajcc::kSubbands));
        acpl::interpolate(framing, values.num_bands, coefficients[k], prev[k], num_ts, interp_);
        // ajcc::accumulate is templated on Real, unlike acpl::interpolate
        // above (double, unconditionally): narrow once, explicitly - the
        // identity conversion at Real = double.
        interp_real_.resize(interp_.size());
        std::ranges::transform(interp_, interp_real_.begin(),
                               [](double v) { return static_cast<Real>(v); });
        ajcc::accumulate<Real>(interp_real_, in, z, num_ts);
    }
    acpl::end_frame(framing, values.num_bands, coefficients[k], prev[k]);
}

// Pseudocode 11 (full decoding) or 14 (core decoding) for one side: each
// coefficient per parameter set and band, interpolated with its own
// ajcc_param_prev and weighting the input it goes with into its output.
void AjccStage::module(DecodingMode decoding, std::size_t side, const AjccFrameValues& values,
                       int num_ts, std::array<const std::vector<QmfValue>*, 2> x,
                       std::array<const std::vector<QmfValue>*, 3> y,
                       std::array<QmfMatrix, ajcc::kModule2Outputs> z) {
    const bool full = decoding == DecodingMode::kFull;
    const std::size_t outputs = full ? ajcc::kModule2Outputs : ajcc::kModule4Outputs;
    const std::size_t count = full ? ajcc::kModule2Coefficients : ajcc::kModule4Coefficients;
    const acpl::Framing& framing = values.framing[side];
    const std::size_t sets = at(std::clamp(framing.num_param_sets, 1, ajcc::kMaxParamSets));
    auto& coefficients = coefficients_[side];
    for (auto& coefficient : coefficients) {
        coefficient = {};
    }
    for (std::size_t ps = 0; ps < sets; ++ps) {
        for (std::size_t pb = 0; pb < at(values.num_bands); ++pb) {
            const ajcc::ModuleParams p = module_params(values, side, ps, pb);
            if (full) {
                const auto c = ajcc::module_2(values.core_mode, p);
                for (std::size_t k = 0; k < count; ++k) {
                    coefficients[k][ps][pb] = c[k];
                }
            } else {
                const auto c = ajcc::module_4(values.core_mode, p);
                for (std::size_t k = 0; k < count; ++k) {
                    coefficients[k][ps][pb] = c[k];
                }
            }
        }
    }
    const std::size_t n = at(num_ts) * at(ajcc::kSubbands);
    for (std::size_t o = 0; o < outputs; ++o) {
        std::fill_n(z[o].begin(), n, QmfValue{});
    }
    for (std::size_t k = 0; k < count; ++k) {
        const ajcc::Term term = ajcc::module_term(k, outputs);
        const std::vector<QmfValue>& in = term.decorrelated ? *y[term.input] : *x[term.input];
        accumulate_coefficient(side, k, framing, values, num_ts, in, z[term.output]);
    }
}

// Pseudocode 10 for one of b_5fronts' four modules: dry and wet parameters of its own, one input
// and two decorrelated ones.
void AjccStage::module_fronts(std::size_t module, const AjccFrameValues& values, int num_ts,
                              const std::vector<QmfValue>& x, const std::vector<QmfValue>& y0,
                              const std::vector<QmfValue>& y1, std::array<QmfMatrix, 3> z) {
    const acpl::Framing& framing = values.framing[module];
    const std::size_t sets = at(std::clamp(framing.num_param_sets, 1, ajcc::kMaxParamSets));
    auto& coefficients = coefficients_[module];
    for (auto& coefficient : coefficients) {
        coefficient = {};
    }
    for (std::size_t ps = 0; ps < sets; ++ps) {
        for (std::size_t pb = 0; pb < at(values.num_bands); ++pb) {
            const auto c = ajcc::module_1(values.module_params_5fronts(module, ps, pb));
            for (std::size_t k = 0; k < ajcc::kModule1Coefficients; ++k) {
                coefficients[k][ps][pb] = c[k];
            }
        }
    }
    const std::size_t n = at(num_ts) * at(ajcc::kSubbands);
    for (QmfMatrix& output : z) {
        std::fill_n(output.begin(), n, QmfValue{});
    }
    const std::array<const std::vector<QmfValue>*, 3> inputs = {&x, &y0, &y1};
    for (std::size_t k = 0; k < ajcc::kModule1Coefficients; ++k) {
        const ajcc::Term term = ajcc::module_1_term(k);
        const std::vector<QmfValue>& in = *inputs[term.decorrelated ? 1 + term.input : 0];
        accumulate_coefficient(module, k, framing, values, num_ts, in, z[term.output]);
    }
}

// Pseudocode 13 for one side of b_5fronts' core decoding. The module is called with the front
// framing's num_pset_1 and the back framing's num_pset_2: the front module's `side` and the back
// module's 2 + `side`. Each coefficient takes the framing of the half it was calculated from.
void AjccStage::module_fronts_core(std::size_t side, const AjccFrameValues& values, int num_ts,
                                   std::array<const std::vector<QmfValue>*, 2> x,
                                   std::array<const std::vector<QmfValue>*, 2> y,
                                   std::array<QmfMatrix, 3> z) {
    const acpl::Framing& front = values.framing[side];
    const acpl::Framing& back = values.framing[2 + side];
    const std::size_t sets =
        at(std::clamp(std::max(front.num_param_sets, back.num_param_sets), 1, ajcc::kMaxParamSets));
    auto& coefficients = coefficients_[side];
    for (auto& coefficient : coefficients) {
        coefficient = {};
    }
    for (std::size_t ps = 0; ps < sets; ++ps) {
        // A framing with fewer sets takes the values of its last for the sets it has no use for.
        const std::size_t ps_front = std::min(ps, at(std::max(front.num_param_sets, 1) - 1));
        const std::size_t ps_back = std::min(ps, at(std::max(back.num_param_sets, 1) - 1));
        for (std::size_t pb = 0; pb < at(values.num_bands); ++pb) {
            const auto c = ajcc::module_3(values.module_params_5fronts(side, ps_front, pb),
                                          values.module_params_5fronts(2 + side, ps_back, pb));
            for (std::size_t k = 0; k < ajcc::kModule4Coefficients; ++k) {
                coefficients[k][ps][pb] = c[k];
            }
        }
    }
    const std::size_t n = at(num_ts) * at(ajcc::kSubbands);
    for (QmfMatrix& output : z) {
        std::fill_n(output.begin(), n, QmfValue{});
    }
    for (std::size_t k = 0; k < ajcc::kModule4Coefficients; ++k) {
        const ajcc::Term term = ajcc::module_term(k, ajcc::kModule4Outputs);
        const std::vector<QmfValue>& in = term.decorrelated ? *y[term.input] : *x[term.input];
        // d0 to d2 and w0 to w2 are the front framing's, d3 to d5 and w3 to w5 the back's.
        const bool is_front = k % 6 < 3;
        accumulate_coefficient(side, k, is_front ? front : back, values, num_ts, in,
                               z[term.output]);
    }
}

// Pseudocode 8's b_5fronts branch (full decoding) and Pseudocode 12 (core decoding) with b_5fronts.
void AjccStage::apply_fronts(DecodingMode decoding, const AjccFrameValues& values, int num_ts,
                             const AcplChannels& channels) {
    const std::size_t n = at(num_ts) * at(ajcc::kSubbands);
    const auto matrix_of = [&](Speaker speaker) -> QmfMatrix {
        for (std::size_t c = 0; c < channels.speakers.size() && c < channels.matrices.size(); ++c) {
            if (channels.speakers[c] == speaker && channels.matrices[c].size() >= n) {
                return channels.matrices[c].first(n);
            }
        }
        return {};
    };
    const bool full = decoding == DecodingMode::kFull;
    // Each module's outputs: Pseudocode 8's (z0, z9, z3), (z1, z10, z4), (z5, z7, z11) and (z6,
    // z8, z12); Pseudocode 12's (z0, z3, z5) and (z1, z4, z6).
    const std::array<std::array<S, 3>, 4> kFullOutputs = {
        {{S::kLeft, S::kTopFrontLeft, S::kLeftScreen},
         {S::kRight, S::kTopFrontRight, S::kRightScreen},
         {S::kLeftSurround, S::kLeftBack, S::kTopBackLeft},
         {S::kRightSurround, S::kRightBack, S::kTopBackRight}}};
    const std::array<std::array<S, 3>, 2> kCoreOutputs = {
        {{S::kLeft, S::kLeftSurround, S::kTopSideLeft},
         {S::kRight, S::kRightSurround, S::kTopSideRight}}};
    const std::size_t modules = full ? kAjccModules : 2;
    std::array<std::array<QmfMatrix, 3>, kAjccModules> z{};
    for (std::size_t m = 0; m < modules; ++m) {
        for (std::size_t o = 0; o < 3; ++o) {
            z[m][o] = matrix_of(full ? kFullOutputs[m][o] : kCoreOutputs[m][o]);
            if (z[m][o].empty()) {
                return;
            }
        }
    }
    const QmfMatrix centre = matrix_of(S::kCentre);
    if (centre.empty()) {
        return;
    }
    // x0, x1, x3 and x4 (L, R, Ls and Rs holding A'', B'', D'' and E''), times the input gain,
    // before any output overwrites them.
    const std::array<S, 4> kInputs = {S::kLeft, S::kRight, S::kLeftSurround, S::kRightSurround};
    for (std::size_t i = 0; i < kInputs.size(); ++i) {
        const QmfMatrix source = matrix_of(kInputs[i]);
        x_in_[i].resize(n);
        for (std::size_t k = 0; k < n; ++k) {
            x_in_[i][k] = kInputGainReal * source[k];
        }
    }
    const std::vector<QmfValue>& x0 = x_in_[0];
    const std::vector<QmfValue>& x1 = x_in_[1];
    const std::vector<QmfValue>& x3 = x_in_[2];
    const std::vector<QmfValue>& x4 = x_in_[3];
    if (full) {
        // Pseudocode 8: u0 and u1 are x0in through D0 and D2, u2 and u3 x1in through D0 and D2, u4
        // and u5 x3in through D1 and D2, u6 and u7 x4in through D1 and D2, each its own instance.
        const std::array<const std::vector<QmfValue>*, 4> inputs = {&x0, &x1, &x3, &x4};
        // The slots of each module's two decorrelators (see the declaration).
        constexpr std::array<std::array<std::size_t, 2>, 4> kSlots = {
            {{0, 1}, {3, 4}, {2, 6}, {5, 7}}};
        for (std::size_t m = 0; m < kAjccModules; ++m) {
            decorrelate(kSlots[m][0], *inputs[m], y_fronts_[0], num_ts);
            decorrelate(kSlots[m][1], *inputs[m], y_fronts_[1], num_ts);
            module_fronts(m, values, num_ts, *inputs[m], y_fronts_[0], y_fronts_[1], z[m]);
        }
        // z5 to z12, the back modules' outputs and the top front pair's, times the square root of
        // 2; z0, z1, z3 and z4 are not scaled, and neither is z2.
        for (std::size_t m = 0; m < kAjccModules; ++m) {
            for (std::size_t o = 0; o < 3; ++o) {
                const bool scaled = m >= 2 || o == 1;
                if (scaled) {
                    for (QmfValue& v : z[m][o]) {
                        v *= kSqrt2Real;
                    }
                }
            }
        }
    } else {
        // Pseudocode 12: u0 to u3 are x0in through D0, x3in through D2, x1in through D0 and x4in
        // through D2, in slots 0, 1, 3 and 4.
        decorrelate(0, x0, y_[0], num_ts);
        decorrelate(1, x3, y_[1], num_ts);
        module_fronts_core(0, values, num_ts, {&x0, &x3}, {&y_[0], &y_[1]}, z[0]);
        decorrelate(3, x1, y_[0], num_ts);
        decorrelate(4, x4, y_[1], num_ts);
        module_fronts_core(1, values, num_ts, {&x1, &x4}, {&y_[0], &y_[1]}, z[1]);
    }
    // z2 = x2in.
    for (std::size_t k = 0; k < n; ++k) {
        centre[k] *= kInputGainReal;
    }
}

void AjccStage::apply(DecodingMode decoding, const AjccFrameValues& values, int num_ts,
                      const AcplChannels& channels) {
    if (values.b_5fronts) {
        apply_fronts(decoding, values, num_ts, channels);
        return;
    }
    const std::size_t n = at(num_ts) * at(ajcc::kSubbands);
    // A channel's first n values, or nothing where the layout has no such channel.
    const auto matrix_of = [&](Speaker speaker) -> QmfMatrix {
        for (std::size_t c = 0; c < channels.speakers.size() && c < channels.matrices.size(); ++c) {
            if (channels.speakers[c] == speaker && channels.matrices[c].size() >= n) {
                return channels.matrices[c].first(n);
            }
        }
        return {};
    };
    const bool full = decoding == DecodingMode::kFull;
    const std::array<std::array<S, ajcc::kModule2Outputs>, 2> kFullOutputs = {
        {{S::kLeft, S::kLeftSurround, S::kLeftBack, S::kTopFrontLeft, S::kTopBackLeft},
         {S::kRight, S::kRightSurround, S::kRightBack, S::kTopFrontRight, S::kTopBackRight}}};
    const std::array<std::array<S, ajcc::kModule4Outputs>, 2> kCoreOutputs = {
        {{S::kLeft, S::kLeftSurround, S::kTopSideLeft},
         {S::kRight, S::kRightSurround, S::kTopSideRight}}};
    std::array<std::array<QmfMatrix, ajcc::kModule2Outputs>, 2> z{};
    for (std::size_t side = 0; side < 2; ++side) {
        const std::size_t outputs = full ? ajcc::kModule2Outputs : ajcc::kModule4Outputs;
        for (std::size_t o = 0; o < outputs; ++o) {
            z[side][o] = matrix_of(full ? kFullOutputs[side][o] : kCoreOutputs[side][o]);
            if (z[side][o].empty()) {
                return;
            }
        }
    }
    const QmfMatrix centre = matrix_of(S::kCentre);
    if (centre.empty()) {
        return;
    }
    // x0, x1, x3 and x4 (L, R, Ls and Rs holding A'', B'', D'' and E''), times
    // the input gain, before any output overwrites them.
    const std::array<S, 4> kInputs = {S::kLeft, S::kRight, S::kLeftSurround, S::kRightSurround};
    for (std::size_t i = 0; i < kInputs.size(); ++i) {
        const QmfMatrix source = matrix_of(kInputs[i]);
        x_in_[i].resize(n);
        for (std::size_t k = 0; k < n; ++k) {
            x_in_[i][k] = kInputGainReal * source[k];
        }
    }
    const std::vector<QmfValue>& x0 = x_in_[0];
    const std::vector<QmfValue>& x1 = x_in_[1];
    const std::vector<QmfValue>& x3 = x_in_[2];
    const std::vector<QmfValue>& x4 = x_in_[3];
    if (full) {
        // Pseudocode 8: (w1in, w2in) of Pseudocode 9, then D0, D2 and D1 on
        // x0in, w1in and x3in for the left module and on x1in, w2in and x4in
        // for the right.
        for (auto& w : w_in_) {
            w.resize(n);
        }
        pre_.process(values.core_mode, num_ts, x0, x3, x1, x4, w_in_[0], w_in_[1]);
        decorrelate(0, x0, y_[0], num_ts);
        decorrelate(1, w_in_[0], y_[1], num_ts);
        decorrelate(2, x3, y_[2], num_ts);
        module(decoding, 0, values, num_ts, {&x0, &x3}, {&y_[0], &y_[1], &y_[2]}, z[0]);
        decorrelate(3, x1, y_[0], num_ts);
        decorrelate(4, w_in_[1], y_[1], num_ts);
        decorrelate(5, x4, y_[2], num_ts);
        module(decoding, 1, values, num_ts, {&x1, &x4}, {&y_[0], &y_[1], &y_[2]}, z[1]);
        // z5 to z12, every output but L, R and C, times the square root of 2.
        for (std::size_t side = 0; side < 2; ++side) {
            for (std::size_t o = 1; o < ajcc::kModule2Outputs; ++o) {
                for (QmfValue& v : z[side][o]) {
                    v *= kSqrt2Real;
                }
            }
        }
    } else {
        // Pseudocode 12: D0 and D2 on x0in and x3in for the left module, and
        // on x1in and x4in for the right.
        decorrelate(0, x0, y_[0], num_ts);
        decorrelate(1, x3, y_[1], num_ts);
        module(decoding, 0, values, num_ts, {&x0, &x3}, {&y_[0], &y_[1], nullptr}, z[0]);
        decorrelate(3, x1, y_[0], num_ts);
        decorrelate(4, x4, y_[1], num_ts);
        module(decoding, 1, values, num_ts, {&x1, &x4}, {&y_[0], &y_[1], nullptr}, z[1]);
    }
    // z2 = x2in.
    for (std::size_t k = 0; k < n; ++k) {
        centre[k] *= kInputGainReal;
    }
}

}  // namespace iclforge::ac4::detail
