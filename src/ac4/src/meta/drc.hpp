#pragma once

#include <array>

// The dynamic range control both directions compute (ETSI TS 103 190-1 V1.4.1 clause 5.7.9.3.1):
// a compression curve's gain for a level relative to dialnorm, the level detector's K-weighting,
// and the smoothing of clause 5.7.9.3.1.2 with the curve's time constants. The decoder applies the
// smoothed gain (decoder/pcm/drc.hpp); the encoder sends it as transmitted gains
// (encoder/frame/drc_gains.hpp).
//
// Levels and gains are in dB2 (Part 1 clause 3.4: 6 dB2 is a factor of 2), as the control points
// (4.3.13.4.1) and the transmitted gains (5.7.9.3.2) are; the curve's conversion that clause
// 5.7.9.3.1.2 prints as 10^(G/20) is taken as 2^(G/6) with them (src/ac4/ERRATA.md, "DRC's units").
//
// The level detector, which clause 5.7.9.3.1.1 leaves to the implementation, is ITU-R BS.1770's
// K-weighted power of the channels with its channel weights (1 for the front channels, 1.41 for
// those at the sides, none for the LFE), in LKFS relative to dialnorm: the K-weighting is
// BS.1770's two filters at 48 kHz, read at each QMF subband's centre frequency, with the QMF
// analysis's energy gain, the sum of QWIN's squared coefficients, taken out.

namespace iclforge::ac4::detail {

// A compression curve and its time constants, in dB2 and ms (Table 166, Table 162 for a default
// profile, or Table 167's defaults).
struct DrcCurve {
    double max_boost_gain = 0.0;
    double max_boost_level = 0.0;
    double section_boost_gain = 0.0;
    double section_boost_level = 0.0;
    double null_low = 0.0;
    double null_high = 0.0;
    double section_cut_gain = 0.0;
    double section_cut_level = 0.0;
    double max_cut_gain = 0.0;
    double max_cut_level = 0.0;
    double attack_ms = 100.0;
    double release_ms = 3000.0;
    double attack_fast_ms = 10.0;
    double release_fast_ms = 1000.0;
    bool adaptive = false;
    double attack_threshold = 15.0;
    double release_threshold = 20.0;

    // The curve's gain for a level relative to dialnorm (clause 5.7.9.3.1.2's G1 to G4, each a
    // line between two control points).
    [[nodiscard]] double gain(double level) const noexcept;
};

// BS.1770's offset from K-weighted mean square to LKFS.
inline constexpr double kLkfsOffset = -0.691;
// The power of silence, which no level is taken below.
inline constexpr double kDrcPowerFloor = 1e-15;
// BS.1770's channel weight at the sides (60 to 120 degrees); 1 elsewhere, none for the LFE.
inline constexpr double kDrcSideWeight = 1.41;

// ITU-R BS.1770's K-weighting at 48 kHz, the shelf and the high-pass, as power at `hz` (read at
// 23.5 kHz above it: the shelf is flat there).
[[nodiscard]] double k_weight(double hz);

// k_weight() at the centre of each of the 64 QMF subbands of a bank running at `rate_hz`.
[[nodiscard]] std::array<double, 64> k_weights(double rate_hz);

// The QMF analysis's energy gain: the sum of QWIN's squared coefficients.
[[nodiscard]] double qmf_energy_gain() noexcept;

// Clause 5.7.9.3.1.2's smoothing of the level and the gain, one QMF slot at a time.
struct DrcSmoothing {
    bool primed = false;
    double level = 0.0;  // L~, as a power
    double gain = 1.0;   // g~, linear

    // Takes one slot whose K-weighted mean square per sample at full scale 1.0 is `power` (no
    // lower than kDrcPowerFloor), against `dialnorm` (dBFS), with the curve's time constants over
    // a slot of `slot_ms`; returns the smoothed gain.
    double step(const DrcCurve& curve, double power, double dialnorm, double slot_ms) noexcept;
};

}  // namespace iclforge::ac4::detail
