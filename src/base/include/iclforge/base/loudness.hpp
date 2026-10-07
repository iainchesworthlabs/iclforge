#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include "iclforge/base/export.hpp"
#include "iclforge/base/speaker.hpp"

// Programme loudness to ITU-R BS.1770-4, and the rest of an R128-style meter
// built on the same K-weighted, channel-summed signal: momentary/short-term
// loudness (BS.1770-4 §2's un-gated block power at two window sizes), Loudness
// Range (EBU Tech 3342's own gated-percentile statistic) and true-peak level
// (BS.1770-4 Annex 2's oversampled peak, independent of the loudness path
// entirely). Roadmap item C1.
//
// The meter is codec-blind: a weight per channel and a rate. AC-3's forms of
// it - Annex 1 keyed on the acmod, Annex 3 on E-AC-3's channel-map locations -
// and the dialnorm that follows from it are iclforge::ac3::meta's
// (iclforge/ac3/meta/loudness.hpp).
//
// The measurement is inherently two-pass: the relative gate needs the whole
// programme before any block's contribution is known. A streaming encoder
// therefore cannot derive dialnorm from the frame it is encoding; the caller
// measures first and configures the encoder second, which is exactly what
// real encoders do with an analysis pass.

namespace iclforge::base {

// The one non-unity weight either algorithm uses, and the same number in
// both: Annex 1's Table 3 gives it to Ls and Rs by name, and Annex 3's
// Table 4 gives it to whatever sits at 60..120 degrees azimuth below
// 30 degrees elevation - which is where Ls and Rs are. The LFE participates
// in neither.
inline constexpr double kSurroundWeight = 1.41;

// ITU-R BS.1770-5 (11/2023) Annex 3, "Extended loudness measurement algorithm
// for loudspeaker configurations of advanced sound systems", Table 4: the
// weighting coefficient Gi of a channel depends only on where that channel
// sits. Gi is 1.41 (+1.5 dB) when |phi| < 30 degrees AND
// 60 <= |theta| <= 120 degrees, and 1.00 everywhere else - including every
// upper- and bottom-layer position, which is outside the |phi| < 30 row.
// Table 5 tabulates the same weights per BS.2051 configuration.
//
// Each speaker is placed at the BS.2051 label it stands for: the side
// surrounds (M±090..110) and the wides (M±060, on the sector's included edge)
// at 1.41; the fronts, the screen pair inboard of them, the backs (M±135..150),
// the rear centre (M+180) and every top and bottom speaker at 1.00.
//
// std::nullopt for the two LFEs. That is not a zero weight: Annex 3 weights
// "each channel except the LFE channels", so an LFE is not a term in the sum
// at all - which is also why true peak, which does measure it, reads it from a
// separate path.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::optional<double> position_weight(Speaker speaker);

class ICLFORGE_BASE_EXPORT LoudnessMeter {
   public:
    // One entry per pushed channel, in push()'s order: the channel's BS.1770
    // weight, or std::nullopt for a channel that is not a term in the loudness
    // sum (an LFE), which true peak still reads. BS.1770 tabulates its
    // K-weighting at 48 kHz only; any other rate is designed from the
    // standard's own prototypes.
    LoudnessMeter(std::uint32_t sample_rate, std::span<const std::optional<double>> weights);

    // BS.1770-5 Annex 3's extended algorithm over a list of speakers, one
    // pushed channel each, weighted by position_weight().
    LoudnessMeter(std::uint32_t sample_rate, std::span<const Speaker> speakers);
    // Declared (and defined in loudness.cpp, where Impl below is complete)
    // rather than implicit: a dllexport class generates every implicit
    // special member whether or not called, and the unique_ptr member makes
    // the implicit copy deleted - which is fine - but move-assignment's
    // implicit reset() needs Impl complete, so it cannot stay implicit once
    // Impl is only forward-declared here.
    ~LoudnessMeter();
    LoudnessMeter(const LoudnessMeter&) = delete;
    LoudnessMeter& operator=(const LoudnessMeter&) = delete;
    LoudnessMeter(LoudnessMeter&&) noexcept;
    LoudnessMeter& operator=(LoudnessMeter&&) noexcept;

    // Any number of samples; one span per channel, in the constructor's order.
    void push(std::span<const std::span<const float>> channels);

    // std::nullopt until at least one 400 ms block has passed the absolute
    // gate — silence has no meaningful loudness, and inventing one would put
    // a wrong dialnorm on the stream.
    [[nodiscard]] std::optional<double> integrated_lkfs() const;

    // BS.1770-4 §2's un-gated block loudness: the same 400 ms/75%-overlap
    // window integrated_lkfs() gates internally, reported directly instead.
    // Reflects the most recently completed 400 ms block; std::nullopt until
    // one has elapsed. A block whose power is exactly zero is also
    // std::nullopt rather than -inf LKFS, matching integrated_lkfs()'s own
    // "no meaningful loudness" stance on silence.
    [[nodiscard]] std::optional<double> momentary_lkfs() const;

    // The same, over a 3 s window instead of 400 ms, still un-gated.
    // std::nullopt until 3 s have elapsed.
    [[nodiscard]] std::optional<double> short_term_lkfs() const;

    // EBU Tech 3342 §3.1 Loudness Range: the 95th minus the 10th percentile
    // of short-term loudness values, themselves passed through Tech 3342's
    // own cascaded gate — an absolute threshold at −70 LUFS then a relative
    // one at −20 LU below the mean of what survives it. This is NOT the same
    // relative gate integrated_lkfs() uses (−10 LU): Tech 3342 §3.1 specifies
    // −20 LU for LRA specifically, and the population being gated is
    // short-term (3 s) blocks rather than integrated-loudness (400 ms) ones.
    // std::nullopt until at least one short-term value survives both gates.
    [[nodiscard]] std::optional<double> loudness_range() const;

    // ITU-R BS.1770-4 Annex 2: the highest absolute sample value found in a
    // 4x-oversampled reconstruction of every pushed channel, LFE included —
    // true peak is about physical overload headroom, not perceived
    // loudness, so unlike every measure above it does not exclude LFE or
    // apply the surround weighting. In dBTP (decibels relative to 100% full
    // scale, true-peak measurement). std::nullopt until at least one sample
    // has been pushed.
    [[nodiscard]] std::optional<double> true_peak_dbtp() const;

    [[nodiscard]] int channel_count() const;

   private:
    void push_block();
    void push_true_peak(int channel, float sample);

    // Every private data member - the K-weighting filter state, the
    // loudness/true-peak accumulators, all of it - lives behind this one
    // pimpl, following the same pattern as iclforge::base::WavStreamReader/Writer.
    // Impl is defined in loudness.cpp.
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace iclforge::base
