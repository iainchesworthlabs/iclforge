#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "iclforge/ac3/oba/joc.hpp"
#include "iclforge/base/detail/simd.hpp"
#include "iclforge/dsp/qmf.hpp"

// reconstruct_qmf's mixing coefficients and their sum, in the seam's two-lane double vector.
// Kept in a header of its own so libs/ac3/tests can hold each loop to the per-element
// expression it stands for, bit for bit on the doubles, which the decoded float samples
// cannot do: a last-bit difference in a coefficient seldom survives the rounding to float.

namespace iclforge::ac3::oba::joc::qmf_mix {

// §6.6.5's rule for ONE object at ONE timeslot of the ordinary six-block frame. Which formula
// applies depends only on the object's shape, on whether there is a previous frame to ramp
// from, and on the timeslot - never on the channel or the subband - so reconstruct_qmf picks it
// once per (object, timeslot) and the row loops below carry no branch on it.
enum class MixRule : std::uint8_t {
    kTailBlend,      // the delayed tail: blend two stored snapshots
    kFlat,           // no history to ramp from: this frame outright
    kSmoothWhole,    // one data point: one ramp across the window
    kSmoothFirst,    // two data points, first half
    kSmoothSecond,   // two data points, second half
    kSteepPrevious,  // steep, before the step
    kSteepFirst,     // steep, at or after the first step
    kSteepSecond,    // steep, at or after the second
};

// The mixing coefficient rows of reconstruct_qmf, one channel's 64 subbands a call, in the
// seam's two-lane double vector (libs/base/variants: f64x2). Each loop body performs, in each
// lane, the IEEE operations the scalar expression it stands for performs, in the same order:
// no reciprocal in place of a division, no reassociation, no fused multiply-add (the tree
// builds with contraction off). The coefficients, and so every decoded sample, are therefore
// bit for bit what the loop with a switch inside it produced.
using Lane2 = iclforge::internal::arch::f64x2;
constexpr std::size_t kSubbands = static_cast<std::size_t>(dsp::kQmfSubbands);
// kNumBands' largest entry (Table 50) is 23.
constexpr std::size_t kMaxBands = 24;
// One row of subbands for each of the five bed channels.
using Rows = std::array<std::array<double, dsp::kQmfSubbands>, kNumChannels5X>;

// Two stored coefficients (float) as two doubles, exactly as static_cast<double> converts them.
[[nodiscard]] inline Lane2 widen(const recon_scalar_t* p) noexcept {
    return Lane2::set(static_cast<double>(p[0]), static_cast<double>(p[1]));
}

// The coefficients of the parameter bands two subbands belong to.
[[nodiscard]] inline Lane2 by_band(const double* band_row, const std::uint8_t* map) noexcept {
    return Lane2::set(band_row[map[0]], band_row[map[1]]);
}

// x / divisor in each lane: a division, not a multiplication by the reciprocal, which rounds
// differently.
[[nodiscard]] inline Lane2 divide(Lane2 x, double divisor) noexcept {
    return x / Lane2::broadcast(divisor);
}

// The delayed tail: older + tail_frac * (previous - older). With no stored snapshot the
// missing one is this frame's first data point, and with no older one it is the previous.
template <bool HasPrevious, bool HasOlder>
inline void tail_blend(double* m, const double* dq0, const std::uint8_t* map,
                       const recon_scalar_t* previous, const recon_scalar_t* older,
                       double tail_frac) noexcept {
    const Lane2 fraction = Lane2::broadcast(tail_frac);
    for (std::size_t k = 0; k < kSubbands; k += 2) {
        Lane2 previous_val;
        if constexpr (HasPrevious) {
            previous_val = widen(previous + k);
        } else {
            previous_val = by_band(dq0, map + k);
        }
        Lane2 older_val = previous_val;
        if constexpr (HasOlder) {
            older_val = widen(older + k);
        }
        (older_val + fraction * (previous_val - older_val)).store(m + k);
    }
}

// previous + step * (dq0 - previous) / divisor, with step = ts + 1.
inline void smooth_from_previous(double* m, const double* dq0, const std::uint8_t* map,
                                 const recon_scalar_t* previous, double step,
                                 double divisor) noexcept {
    const Lane2 factor = Lane2::broadcast(step);
    for (std::size_t k = 0; k < kSubbands; k += 2) {
        const Lane2 previous_val = widen(previous + k);
        const Lane2 delta = factor * (by_band(dq0, map + k) - previous_val);
        (previous_val + divide(delta, divisor)).store(m + k);
    }
}

// previous itself, one value per subband.
inline void copy_previous(double* m, const recon_scalar_t* previous) noexcept {
    for (std::size_t k = 0; k < kSubbands; ++k) {
        m[k] = static_cast<double>(previous[k]);
    }
}

// A value per parameter band spread over its subbands.
inline void spread(double* m, const double* band_row, const std::uint8_t* map) noexcept {
    for (std::size_t k = 0; k < kSubbands; ++k) {
        m[k] = band_row[map[k]];
    }
}

// dq0 + step * (dq1 - dq0) / divisor, for the bands: it depends on the band and not on the
// subband, so it is worked out once for each band and spread, and every subband of a band
// gets the value the per-subband expression would have given it.
inline void smooth_between(double* m, const double* dq0, const double* dq1, int bands,
                           const std::uint8_t* map, double step, double divisor) noexcept {
    std::array<double, kMaxBands> per_band;
    for (std::size_t b = 0; b < static_cast<std::size_t>(bands); ++b) {
        per_band[b] = dq0[b] + step * (dq1[b] - dq0[b]) / divisor;
    }
    spread(m, per_band.data(), map);
}

// §6.6.6: for each subband, the sum over the five bed channels of the coefficient times the
// bed's real and imaginary parts, from zero and in channel order, as the loop it replaces.
inline void combine(const Rows& mix, const Rows& bed_real, const Rows& bed_imag,
                    double* object_real, double* object_imag) noexcept {
    for (std::size_t k = 0; k < kSubbands; k += 2) {
        Lane2 real = Lane2::broadcast(0.0);
        Lane2 imag = Lane2::broadcast(0.0);
        for (std::size_t c = 0; c < static_cast<std::size_t>(kNumChannels5X); ++c) {
            const Lane2 m = Lane2::load(mix[c].data() + k);
            real = real + m * Lane2::load(bed_real[c].data() + k);
            imag = imag + m * Lane2::load(bed_imag[c].data() + k);
        }
        real.store(object_real + k);
        imag.store(object_imag + k);
    }
}

// --- Short syncframes (reconstruct_qmf_short) -----------------------------------------------
//
// The same rows, the same arithmetic in double, over a frame of `slots` timeslots (4, 8 or 12)
// instead of the ordinary 24: the delayed tail does not exist there (the schedule is a FIFO of
// whole timeslots), so five of the eight rules remain, and `step` and `divisor` are passed in
// because the ramp's window is the frame's own.

// The rule of one object at one timeslot of a short frame, with the numerator and denominator
// its ramp uses. §6.6.5 as `interpolate` (joc.cpp) states it: nothing depends on the channel or
// the subband.
struct ShortRamp {
    MixRule rule;
    double step;
    double divisor;
};

[[nodiscard]] inline ShortRamp short_ramp(bool has_ramp, const ObjectShape& shape, int ts,
                                          int slots) noexcept {
    if (!has_ramp) {
        return {MixRule::kFlat, 0.0, 1.0};
    }
    if (!shape.steep) {
        if (shape.data_points == 1) {
            return {MixRule::kSmoothWhole, static_cast<double>(ts + 1),
                    static_cast<double>(slots)};
        }
        const int half = slots / 2;
        if (ts < half) {
            return {MixRule::kSmoothFirst, static_cast<double>(ts + 1),
                    static_cast<double>(half)};
        }
        return {MixRule::kSmoothSecond, static_cast<double>(ts - half + 1),
                static_cast<double>(slots - half)};
    }
    if (ts < shape.offset_ts[0]) {
        return {MixRule::kSteepPrevious, 0.0, 1.0};
    }
    if (shape.data_points == 1 || ts < shape.offset_ts[1]) {
        return {MixRule::kSteepFirst, 0.0, 1.0};
    }
    return {MixRule::kSteepSecond, 0.0, 1.0};
}

// One channel's 64 coefficients for `ramp`. `dq1` is the second data point's row for this
// channel, null where the object has only one; the rules that read it (kSmoothSecond and
// kSteepSecond) are only chosen for an object that has it. `previous` is read only by the
// rules that ramp from, or hold, the previous frame's coefficients.
inline void short_row(const ShortRamp& ramp, double* m, const double* dq0, const double* dq1,
                      int bands, const std::uint8_t* map, const recon_scalar_t* previous) noexcept {
    switch (ramp.rule) {
        case MixRule::kFlat:
            spread(m, dq1 != nullptr ? dq1 : dq0, map);
            break;
        case MixRule::kSmoothWhole:
        case MixRule::kSmoothFirst:
            smooth_from_previous(m, dq0, map, previous, ramp.step, ramp.divisor);
            break;
        case MixRule::kSmoothSecond:
            smooth_between(m, dq0, dq1, bands, map, ramp.step, ramp.divisor);
            break;
        case MixRule::kSteepPrevious:
            copy_previous(m, previous);
            break;
        case MixRule::kSteepFirst:
            spread(m, dq0, map);
            break;
        case MixRule::kSteepSecond:
            spread(m, dq1, map);
            break;
        case MixRule::kTailBlend:
            break;  // the 24-slot frame's; a short frame never asks for it
    }
}

// combine() over coefficient rows that are not a Rows: the five channels' 64 coefficients,
// one pointer a channel.
inline void combine_rows(const std::array<const double*, kNumChannels5X>& mix,
                         const Rows& bed_real, const Rows& bed_imag, double* object_real,
                         double* object_imag) noexcept {
    for (std::size_t k = 0; k < kSubbands; k += 2) {
        Lane2 real = Lane2::broadcast(0.0);
        Lane2 imag = Lane2::broadcast(0.0);
        for (std::size_t c = 0; c < static_cast<std::size_t>(kNumChannels5X); ++c) {
            const Lane2 m = Lane2::load(mix[c] + k);
            real = real + m * Lane2::load(bed_real[c].data() + k);
            imag = imag + m * Lane2::load(bed_imag[c].data() + k);
        }
        real.store(object_real + k);
        imag.store(object_imag + k);
    }
}

// --- The single-precision path (reconstruct_qmf_single) -------------------------------------
//
// The mixing coefficients of the decoder built with a float decode scalar, in the seam's
// four-lane float vector: every operand and every operation is a float, as the loops these
// replace had them (the coefficient of a parameter band is worked out once for the band and
// then spread over its subbands). The ramp's fractions are formed once for a timeslot by the
// caller - a division apiece, in float - so no loop here divides.
namespace single {

using Lane = iclforge::internal::arch::f32x4;
constexpr std::size_t kLaneWidth = 4;
// The coefficient of each parameter band, for each of the five bed channels.
using BandRows = std::array<std::array<float, kMaxBands>, kNumChannels5X>;

// out[b] for b below `bands`: four bands a step through `vec` (the lanes' expression), and the
// remainder one at a time through `scalar` (the same expression on floats).
template <typename Vec, typename Scalar>
inline void per_band(float* out, std::size_t bands, Vec&& vec, Scalar&& scalar) noexcept {
    std::size_t b = 0;
    for (; b + kLaneWidth <= bands; b += kLaneWidth) {
        vec(b).store(out + b);
    }
    for (; b < bands; ++b) {
        out[b] = scalar(b);
    }
}

// The delayed tail: older + tail_frac * (previous - older). With no stored snapshot the missing
// one is this frame's first data point, and with no older one it is the previous. `previous`
// and `older` are by band (the caller reads them at each band's first subband).
template <bool HasPrevious, bool HasOlder>
inline void tail_blend(float* out, const float* dq0, const float* previous, const float* older,
                       float tail_frac, std::size_t bands) noexcept {
    const Lane fraction = Lane::broadcast(tail_frac);
    per_band(
        out, bands,
        [&](std::size_t b) {
            Lane previous_val;
            if constexpr (HasPrevious) {
                previous_val = Lane::load(previous + b);
            } else {
                previous_val = Lane::load(dq0 + b);
            }
            Lane older_val = previous_val;
            if constexpr (HasOlder) {
                older_val = Lane::load(older + b);
            }
            return older_val + fraction * (previous_val - older_val);
        },
        [&](std::size_t b) {
            float previous_val = 0.0F;
            if constexpr (HasPrevious) {
                previous_val = previous[b];
            } else {
                previous_val = dq0[b];
            }
            float older_val = previous_val;
            if constexpr (HasOlder) {
                older_val = older[b];
            }
            return older_val + tail_frac * (previous_val - older_val);
        });
}

// previous + fraction * (dq0 - previous), for the smooth ramps that start from the previous
// frame (one data point, and the first half of two).
inline void smooth_from_previous(float* out, const float* dq0, const float* previous,
                                 float fraction, std::size_t bands) noexcept {
    const Lane factor = Lane::broadcast(fraction);
    per_band(
        out, bands,
        [&](std::size_t b) {
            const Lane previous_val = Lane::load(previous + b);
            return previous_val + factor * (Lane::load(dq0 + b) - previous_val);
        },
        [&](std::size_t b) { return previous[b] + fraction * (dq0[b] - previous[b]); });
}

// dq0 + fraction * (dq1 - dq0), the second half of a two-data-point smooth ramp.
inline void smooth_between(float* out, const float* dq0, const float* dq1, float fraction,
                           std::size_t bands) noexcept {
    const Lane factor = Lane::broadcast(fraction);
    per_band(
        out, bands,
        [&](std::size_t b) {
            const Lane first = Lane::load(dq0 + b);
            return first + factor * (Lane::load(dq1 + b) - first);
        },
        [&](std::size_t b) { return dq0[b] + fraction * (dq1[b] - dq0[b]); });
}

// §6.6.6 in float: for each subband, the sum over the five bed channels of the coefficient times
// the bed's real and imaginary parts, from zero and in channel order. A channel's coefficient for
// subband k is the value of its parameter band, band_rows[c][map[k]] (the value a copy of the band
// over its subbands would hold). slot_spectra is one timeslot of the bed's subband values,
// [channel][real, imag][subband].
inline void combine_banded(const std::array<const float*, kNumChannels5X>& band_rows,
                           const std::uint8_t* map, const float* slot_spectra,
                           float* object_real, float* object_imag) noexcept {
    for (std::size_t k = 0; k < kSubbands; k += kLaneWidth) {
        Lane real = Lane::broadcast(0.0F);
        Lane imag = Lane::broadcast(0.0F);
        for (std::size_t c = 0; c < static_cast<std::size_t>(kNumChannels5X); ++c) {
            const float* const row = band_rows[c];
            const Lane m = Lane::set(row[map[k]], row[map[k + 1]], row[map[k + 2]],
                                     row[map[k + 3]]);
            const float* const channel = slot_spectra + (c * 2U * kSubbands);
            real = real + m * Lane::load(channel + k);
            imag = imag + m * Lane::load(channel + kSubbands + k);
        }
        real.store(object_real + k);
        imag.store(object_imag + k);
    }
}

}  // namespace single

}  // namespace iclforge::ac3::oba::joc::qmf_mix
