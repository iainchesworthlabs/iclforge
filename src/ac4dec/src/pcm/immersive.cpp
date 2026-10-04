#include "pcm/immersive.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <numbers>

#include "iclforge/ac4core/dsp/qmf.hpp"
#include "syntax/channel_elements.hpp"

namespace iclforge::ac4::detail {
namespace {

using S = Speaker;

constexpr double kSqrt2 = std::numbers::sqrt2;

// Pseudocode 1: -1.5 dB, as printed.
constexpr double kPostProcessing = 0.841395;
constexpr Real kSqrt2Real = static_cast<Real>(kSqrt2);

// Table 23's coupled pairs: the channel holding D'', E'', F'' or G'' and the
// one holding H'', I'', J'' or K''.
constexpr std::array<std::array<S, 2>, 4> kCoupled = {{{S::kLeftSurround, S::kLeftBack},
                                                       {S::kRightSurround, S::kRightBack},
                                                       {S::kTopFrontLeft, S::kTopBackLeft},
                                                       {S::kTopFrontRight, S::kTopBackRight}}};

[[nodiscard]] std::vector<Real>* channel(std::span<const Speaker> speakers,
                                         std::span<std::vector<Real>> time,
                                         Speaker speaker) noexcept {
    for (std::size_t c = 0; c < speakers.size() && c < time.size(); ++c) {
        if (speakers[c] == speaker) {
            return &time[c];
        }
    }
    return nullptr;
}

void scale(std::vector<Real>& samples, Real gain) noexcept {
    for (Real& x : samples) {
        x *= gain;
    }
}

}  // namespace

void apply_scpl(int codec_mode, DecodingMode decoding, bool fronts,
                std::span<const Speaker> speakers, std::span<std::vector<Real>> time) {
    if (codec_mode != immersive_mode::kScpl && codec_mode != immersive_mode::kAspxScpl) {
        return;
    }
    const bool scpl = codec_mode == immersive_mode::kScpl;
    const Real c_gain = scpl ? Real{2} : Real{1};
    if (decoding == DecodingMode::kCore) {
        for (std::size_t c = 0; c < speakers.size() && c < time.size(); ++c) {
            if (speakers[c] != S::kLfe) {
                scale(time[c], c_gain);
            }
        }
        return;
    }
    // Table 23, b_5fronts 0: L and R are c_gain times A'' and B''; with b_5fronts only C is c_gain
    // times its signal, and L, Lscr, R and Rscr come of the pairs (A'', L'') and (B'', M'')
    // below, which carry no c_gain.
    constexpr std::array<S, 3> kFronts = {S::kLeft, S::kRight, S::kCentre};
    for (std::size_t k = fronts ? 2 : 0; k < kFronts.size(); ++k) {
        if (std::vector<Real>* samples = channel(speakers, time, kFronts[k])) {
            scale(*samples, c_gain);
        }
    }
    // gain x 2 x (1/2, 1/2; 1/2, -1/2).
    const auto couple = [&](Speaker first, Speaker second, Real gain) {
        std::vector<Real>* x = channel(speakers, time, first);
        std::vector<Real>* y = channel(speakers, time, second);
        if (x == nullptr || y == nullptr) {
            return;
        }
        const std::size_t n = std::min(x->size(), y->size());
        for (std::size_t i = 0; i < n; ++i) {
            const Real sum = (*x)[i] + (*y)[i];
            const Real difference = (*x)[i] - (*y)[i];
            (*x)[i] = gain * sum;
            (*y)[i] = gain * difference;
        }
    };
    if (fronts) {
        couple(S::kLeft, S::kLeftScreen, Real{1});
        couple(S::kRight, S::kRightScreen, Real{1});
    }
    const Real m_gain = scpl ? kSqrt2Real : Real{1};
    for (const auto& [first, second] : kCoupled) {
        couple(first, second, m_gain);
    }
}

BandGains immersive_gains(int codec_mode, DecodingMode decoding, bool fronts,
                          Speaker speaker) noexcept {
    if (speaker == S::kLfe) {
        return {};
    }
    const bool core = decoding == DecodingMode::kCore;
    switch (codec_mode) {
        case immersive_mode::kAspxScpl: {
            if (core) {
                // Table 9: Ls, Rs, Tfl and Tfr (here Tsl and Tsr), and with b_5fronts L and R.
                const bool processed = speaker == S::kLeftSurround ||
                                       speaker == S::kRightSurround || speaker == S::kTopSideLeft ||
                                       speaker == S::kTopSideRight ||
                                       (fronts && (speaker == S::kLeft || speaker == S::kRight));
                return {.low = 2.0, .high = processed ? 2.0 * kPostProcessing : 2.0};
            }
            // Tables 10 and 11: 2 for C (and for L and R without b_5fronts), 1 for L, Lscr, R and
            // Rscr with it, and the square root of 2 for the coupled pairs.
            double g = kSqrt2;
            if (speaker == S::kCentre) {
                g = 2.0;
            } else if (speaker == S::kLeft || speaker == S::kRight) {
                g = fronts ? 1.0 : 2.0;
            } else if (speaker == S::kLeftScreen || speaker == S::kRightScreen) {
                g = 1.0;
            }
            return {.low = g, .high = g};
        }
        case immersive_mode::kAspxAcpl1:
        case immersive_mode::kAspxAcpl2:
            if (core) {
                return {.low = 2.0, .high = 2.0};
            }
            return {};
        default:
            return {};
    }
}

void apply_band_gains(std::span<QmfValue> matrix, int num_ts, int sbx, BandGains gains) noexcept {
    constexpr auto kSubbands = static_cast<std::size_t>(dsp::kQmfSubbands);
    const auto split = static_cast<std::size_t>(std::clamp(sbx, 0, dsp::kQmfSubbands));
    const std::size_t slots =
        std::min(static_cast<std::size_t>(std::max(num_ts, 0)), matrix.size() / kSubbands);
    // BandGains stays double: two values set once per element per frame, not
    // per QMF sample, the same shape a downmix or DRC gain matrix keeps.
    const auto low = static_cast<Real>(gains.low);
    const auto high = static_cast<Real>(gains.high);
    for (std::size_t ts = 0; ts < slots; ++ts) {
        const std::span<QmfValue> slot = matrix.subspan(ts * kSubbands, kSubbands);
        for (std::size_t sb = 0; sb < kSubbands; ++sb) {
            slot[sb] *= sb < split ? low : high;
        }
    }
}

}  // namespace iclforge::ac4::detail
