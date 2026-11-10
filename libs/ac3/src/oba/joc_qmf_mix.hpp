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

}  // namespace iclforge::ac3::oba::joc::qmf_mix
