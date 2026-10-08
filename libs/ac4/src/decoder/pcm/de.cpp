#include "decoder/pcm/de.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace iclforge::ac4::detail {
namespace {

constexpr int kSubbands = 64;

}  // namespace

std::array<double, kDeFront> de_rendering(int nr_channels, double coef1, double coef2) noexcept {
    switch (nr_channels) {
        case 2:
            return {coef1, std::sqrt(std::max(0.0, 1.0 - coef1 * coef1)), 0.0};
        case 3:
            return {coef1, coef2, std::sqrt(std::max(0.0, 1.0 - coef1 * coef1 - coef2 * coef2))};
        default:
            return {1.0, 0.0, 0.0};
    }
}

DeFrameValues de_frame_values(const DialogEnhancement& de, bool core) {
    DeFrameValues values;
    if (!de.b_de_data_present || de.de_nr_channels <= 0) {
        return values;
    }
    values.active = true;
    values.method = de.config.de_method;
    values.max_gain_db = 3.0 * static_cast<double>(de.config.de_max_gain + 1);
    const int config = de.config.de_channel_config;
    values.processed = {(config & 4) != 0, (config & 2) != 0, (config & 1) != 0};
    const bool cross = values.method == 1 || values.method == 3;
    // Part 2 clause 4.8.3.15: "If b_de_simulcast is true, the decoder shall use the second
    // de_data() in dialog_enhancement() for the core decoding mode."
    const DeData& data = core && de.b_de_simulcast ? de.core_data : de.data;
    values.ms = data.de_ms_proc_flag;
    for (int i = 0; i < de.de_nr_channels && i < kDeFront; ++i) {
        for (int band = 0; band < kDeNrBands; ++band) {
            values.p[static_cast<std::size_t>(i)][static_cast<std::size_t>(band)] = de_parameter(
                data.de_par[static_cast<std::size_t>(i)][static_cast<std::size_t>(band)], cross);
        }
    }
    values.r = de_rendering(de.de_nr_channels, de_mix_coefficient(data.de_mix_coef1_idx),
                            de_mix_coefficient(data.de_mix_coef2_idx));
    values.alpha_c = static_cast<double>(data.de_signal_contribution) / 31.0;
    return values;
}

void DeStage::configure(int slots, std::span<const Speaker> speakers) {
    slots_ = slots;
    // Part 2 Table 15: the dialogue enhancement channels of 9.X.4 are Lscr, Rscr and C, those of
    // the other layouts L, R and C (src/ac4/ERRATA.md, "Dialogue enhancement's channels for
    // 9.X.4"). A layout with the screen pair is a full decoding of a 9.X.4 mode; core decoding has
    // none, and its core channels are L, R and C.
    const bool screen = std::ranges::find(speakers, Speaker::kLeftScreen) != speakers.end();
    const std::array<Speaker, kDeFront> kFront = {screen ? Speaker::kLeftScreen : Speaker::kLeft,
                                                  screen ? Speaker::kRightScreen : Speaker::kRight,
                                                  Speaker::kCentre};
    for (std::size_t f = 0; f < kFront.size(); ++f) {
        const auto it = std::ranges::find(speakers, kFront[f]);
        channel_[f] = it == speakers.end() ? -1 : static_cast<int>(it - speakers.begin());
    }
    reset();
}

void DeStage::reset() noexcept {
    previous_.h.fill(identity());
    previous_.w.fill(Matrix{});
    previous_identity_ = true;
}

DeStage::Matrix DeStage::identity() noexcept {
    Matrix m{};
    for (std::size_t i = 0; i < kDeFront; ++i) {
        m[i][i] = 1.0;
    }
    return m;
}

DeStage::Frame DeStage::frame_matrices(double gain_db, const DeFrameValues& values,
                                       std::size_t waveform_channels) {
    Frame out;
    out.h.fill(identity());
    if (!values.active || gain_db <= 0.0 || values.max_gain_db <= 0.0) {
        return out;
    }
    // Clause 5.7.8.7: g = 10^(G/20) - 1, with G capped at G_max.
    const double g = std::pow(10.0, std::min(gain_db, values.max_gain_db) / 20.0) - 1.0;
    // The processed channels in L, R, C order; p[i] is the i-th's.
    std::array<std::size_t, kDeFront> front{};
    std::size_t count = 0;
    for (std::size_t c = 0; c < kDeFront; ++c) {
        if (values.processed[c]) {
            front[count++] = c;
        }
    }
    const bool cross = values.method == 1 || values.method == 3;
    const bool ms = !cross && values.ms && count == 2;
    // Clause 5.7.8.9: a hybrid method splits g, (1 - alpha_c) g to the
    // parameters and g_s = alpha_c g to the waveform, where the waveform has
    // the channels its method takes: one per processed channel with the
    // channel independent method, one for the Mid, or one rendered by r
    // (ERRATA, "The hybrid dialogue enhancement's waveform").
    const std::size_t needed = cross || ms ? 1 : count;
    const bool hybrid = values.method >= 2 && waveform_channels >= needed && needed > 0;
    const double gp = hybrid ? (1.0 - values.alpha_c) * g : g;
    const double gs = hybrid ? values.alpha_c * g : 0.0;
    for (std::size_t band = 0; band < kDeNrBands; ++band) {
        Matrix& h = out.h[band];
        Matrix& w = out.w[band];
        if (cross) {
            // Clause 5.7.8.8: Y = (I + g r p^T) m; the hybrid adds r g_s d.
            for (std::size_t i = 0; i < count; ++i) {
                for (std::size_t j = 0; j < count; ++j) {
                    h[front[i]][front[j]] += gp * values.r[i] * values.p[j][band];
                }
                w[front[i]][0] = values.r[i] * gs;
            }
        } else if (ms) {
            // Clause 5.7.8.7 with de_ms_proc_flag: the Mid alone,
            // 1/2 [1 1; 1 -1] diag(1 + g p0, 1) [1 1; 1 -1]; the hybrid adds
            // 1/2 g_s d to both.
            const double half = 0.5 * gp * values.p[0][band];
            const std::size_t a = front[0];
            const std::size_t b = front[1];
            h[a][a] = 1.0 + half;
            h[a][b] = half;
            h[b][a] = half;
            h[b][b] = 1.0 + half;
            w[a][0] = 0.5 * gs;
            w[b][0] = 0.5 * gs;
        } else {
            // Clause 5.7.8.7: Y_i = m_i + g p_i m_i; the hybrid adds g_s d_i.
            for (std::size_t i = 0; i < count; ++i) {
                h[front[i]][front[i]] = 1.0 + gp * values.p[i][band];
                w[front[i]][i] = gs;
            }
        }
    }
    return out;
}

std::array<DeMatrix, kDeNrBands> DeStage::parametric_increment(double gain_db,
                                                               const DeFrameValues& values) {
    std::array<DeMatrix, kDeNrBands> out = frame_matrices(gain_db, values, 0).h;
    for (DeMatrix& h : out) {
        for (std::size_t i = 0; i < kDeFront; ++i) {
            h[i][i] -= 1.0;
        }
    }
    return out;
}

bool DeStage::active(double gain_db, const DeFrameValues& values) const noexcept {
    return !previous_identity_ || (values.active && gain_db > 0.0 && values.max_gain_db > 0.0);
}

void DeStage::process(double gain_db, const DeFrameValues& values,
                      std::span<const QmfMatrix> matrices,
                      std::span<const QmfMatrix> waveform) {
    const Frame current = frame_matrices(gain_db, values, waveform.size());
    const Matrix unit = identity();
    const Matrix zero{};
    const bool current_identity = std::ranges::all_of(current.h, [&unit](const Matrix& m) { return m == unit; }) &&
                                  std::ranges::all_of(current.w, [&zero](const Matrix& m) { return m == zero; });
    if (current_identity && previous_identity_) {
        return;  // the tool bypassed, exactly
    }
    const auto at = [&](std::size_t c, int slot, int k) -> QmfValue* {
        const int channel = channel_[c];
        if (channel < 0 || static_cast<std::size_t>(channel) >= matrices.size()) {
            return nullptr;
        }
        return matrices[static_cast<std::size_t>(channel)].data() +
               static_cast<std::size_t>(slot * kSubbands + k);
    };
    const std::size_t waves = std::min<std::size_t>(waveform.size(), kDeFront);
    for (int n = 0; n < slots_; ++n) {
        // Clause 5.7.8.6: from the previous frame's matrix to this one's.
        const double w = (static_cast<double>(n) + 0.5) / static_cast<double>(slots_);
        for (std::size_t band = 0; band < kDeNrBands; ++band) {
            Matrix h{};
            Matrix hw{};
            for (std::size_t i = 0; i < kDeFront; ++i) {
                for (std::size_t j = 0; j < kDeFront; ++j) {
                    h[i][j] = (1.0 - w) * previous_.h[band][i][j] + w * current.h[band][i][j];
                    hw[i][j] = (1.0 - w) * previous_.w[band][i][j] + w * current.w[band][i][j];
                }
            }
            for (int k = kDeBandStart[band]; k < kDeBandStart[band + 1]; ++k) {
                std::array<QmfValue, kDeFront> m{};
                std::array<QmfValue, kDeFront> d{};
                std::array<QmfValue*, kDeFront> where{};
                for (std::size_t c = 0; c < kDeFront; ++c) {
                    where[c] = at(c, n, k);
                    m[c] = where[c] != nullptr ? *where[c] : QmfValue{};
                }
                const auto index = static_cast<std::size_t>(n * kSubbands + k);
                for (std::size_t j = 0; j < waves; ++j) {
                    d[j] = index < waveform[j].size() ? waveform[j][index] : QmfValue{};
                }
                for (std::size_t i = 0; i < kDeFront; ++i) {
                    if (where[i] == nullptr) {
                        continue;
                    }
                    // h and hw stay double (this frame's slot-by-slot
                    // interpolation of a handful of 3x3 matrices, not a
                    // per-sample QMF value): narrowed once per multiply, as a
                    // downmix or DRC gain matrix is.
                    QmfValue y{};
                    for (std::size_t j = 0; j < kDeFront; ++j) {
                        y += static_cast<Real>(h[i][j]) * m[j] + static_cast<Real>(hw[i][j]) * d[j];
                    }
                    *where[i] = y;
                }
            }
        }
    }
    previous_ = current;
    previous_identity_ = current_identity;
}

void DeCoreStage::configure(int slots) {
    slots_ = slots;
    reset();
}

void DeCoreStage::reset() noexcept {
    m_prev_ = {};
    de_prev_ = {};
    coeff_prev_ = {};
    previous_zero_ = true;
}

bool DeCoreStage::active(double gain_db, const DeFrameValues& values) const noexcept {
    return !previous_zero_ || (values.active && gain_db > 0.0 && values.max_gain_db > 0.0);
}

void DeCoreStage::process(double gain_db, const DeFrameValues& values,
                          const DeCoreCoefficients& coefficients,
                          std::span<const QmfMatrix, kDeFront> m,
                          std::span<const std::span<QmfValue>, kDeFront> delta) {
    const auto n = static_cast<std::size_t>(slots_);
    for (const std::span<QmfValue>& d : delta) {
        std::fill_n(d.begin(), std::min(d.size(), n * kSubbands), QmfValue{});
    }
    // de_param: the enhancement matrix less the identity, by band; all zero where the frame has no
    // parameters or the gain is 0, which ramps the matrices down to 0.
    const std::array<DeMatrix, kDeNrBands> increment =
        DeStage::parametric_increment(gain_db, values);
    // Pseudocode 20's three inputs: A'' with C_L, B'' with C_R, and C'' with the constant 1, one
    // smooth parameter set (int_type[2] = 0, num_ps[2] = 1).
    const std::array<acpl::Framing, kDeFront> framing = {coefficients.framing[0],
                                                         coefficients.framing[1], acpl::Framing{}};
    const double slots = static_cast<double>(slots_);
    const double half = static_cast<double>(slots_ / 2);
    bool all_zero = true;
    std::vector<double> matrix(n);
    for (int sb = 0; sb < kDeSubbands; ++sb) {
        const auto sbi = static_cast<std::size_t>(sb);
        const int ab = std::max(acpl::sb_to_pb(coefficients.num_bands, sb), 0);
        std::size_t db = 0;
        while (db + 1 < kDeNrBands && sb >= kDeBandStart[db + 1]) {
            ++db;
        }
        for (std::size_t ch2 = 0; ch2 < kDeFront; ++ch2) {
            const acpl::Framing& f = framing[ch2];
            const int sets = ch2 == 2 ? 1 : std::clamp(f.num_param_sets, 1, acpl::kMaxParamSets);
            const auto coeff = [&](int set) {
                return ch2 == 2 ? 1.0
                                : coefficients.values[ch2][static_cast<std::size_t>(set)]
                                                     [static_cast<std::size_t>(ab)];
            };
            for (std::size_t ch1 = 0; ch1 < kDeFront; ++ch1) {
                const double de_now = increment[db][ch1][ch2];
                const double de_before = de_prev_[sbi][ch1][ch2];
                const double m_before = m_prev_[sbi][ch1][ch2];
                const double delta_de = (de_now - de_before) / slots;
                // The enhancement matrix at slot t, interpolated across the frame (the value at the
                // frame's last slot is the frame's own).
                const auto de_at = [&](double t) { return de_before + (t + 1.0) * delta_de; };
                const double target = de_now * coeff(sets - 1);  // Mtgt at the frame's end
                if (!f.steep || ch2 == 2) {
                    if (sets == 1) {
                        // Linear from Mprev to Mtgt, reaching it at the last slot.
                        for (std::size_t ts = 0; ts < n; ++ts) {
                            matrix[ts] = m_before + (static_cast<double>(ts) + 1.0) *
                                                        (target - m_before) / slots;
                        }
                    } else {
                        // Two sets, smooth: to the first set's coefficient at the enhancement
                        // matrix of the last slot of the first half, floor(N / 2) - 1, then on to
                        // the frame's end.
                        const double mid = de_at(half - 1.0) * coeff(0);
                        for (std::size_t ts = 0; ts < n; ++ts) {
                            const auto t = static_cast<double>(ts);
                            matrix[ts] =
                                t < half ? m_before + (t + 1.0) * (mid - m_before) / half
                                         : mid + (t - half + 1.0) * (target - mid) / (slots - half);
                        }
                    }
                } else {
                    // Steep: the coefficient in force changes at each parameter timeslot, which
                    // takes the new coefficient's matrix at the enhancement matrix it has there;
                    // between them the matrix ramps, from the one before to the value the slot
                    // before the next timeslot has (the frame's last slot for the last segment).
                    double from = m_before;
                    double in_force = coeff_prev_[ch2][sbi];
                    double t = 0.0;
                    const auto ramp = [&](double stop) {
                        const double steps = stop - t;
                        if (steps <= 0.0) {
                            return;
                        }
                        const double end = de_at(stop - 1.0) * in_force;
                        for (double s = t; s < stop; s += 1.0) {
                            matrix[static_cast<std::size_t>(s)] =
                                from + (s - t + 1.0) * (end - from) / steps;
                        }
                        from = end;
                        t = stop;
                    };
                    for (int k = 0; k < sets; ++k) {
                        const double timeslot = std::clamp(
                            static_cast<double>(f.param_timeslot[static_cast<std::size_t>(k)]), t,
                            slots - 1.0);
                        ramp(timeslot);
                        in_force = coeff(k);
                        from = de_at(timeslot) * in_force;
                        matrix[static_cast<std::size_t>(timeslot)] = from;
                        t = timeslot + 1.0;
                    }
                    ramp(slots);
                }
                for (std::size_t ts = 0; ts < n; ++ts) {
                    if (matrix[ts] == 0.0) {
                        continue;
                    }
                    delta[ch1][ts * kSubbands + sbi] +=
                        static_cast<Real>(matrix[ts]) * m[ch2][ts * kSubbands + sbi];
                }
                m_prev_[sbi][ch1][ch2] = target;
                de_prev_[sbi][ch1][ch2] = de_now;
                all_zero = all_zero && target == 0.0 && de_now == 0.0;
            }
            coeff_prev_[ch2][sbi] = coeff(sets - 1);
        }
    }
    // Nothing to interpolate from next frame when this one ended on zero everywhere.
    previous_zero_ = all_zero;
}

}  // namespace iclforge::ac4::detail
