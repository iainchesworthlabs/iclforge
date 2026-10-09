#pragma once

#include <array>
#include <optional>
#include <span>
#include <vector>

#include "iclforge/ac4/decoder/decoder.hpp"
#include "decoder/pcm/aspx.hpp"
#include "decoder/syntax/metadata.hpp"
#include "meta/drc.hpp"

// The dynamic range control tool of ETSI TS 103 190-1 V1.4.1 clause 5.7.9, with
// the output level gain it applies (5.7.9.3.3): in the QMF domain, before
// synthesis, each channel's matrix is scaled by 2^((Lout - dialnorm) / 6) and,
// in the DRC decoder mode clause 5.7.9.2 selects, by the gain a compression
// curve gives the signal's level or by the gains the stream transmits.
//
// Levels and gains are in dB2 (Part 1 clause 3.4: 6 dB2 is a factor of 2), as
// the control points (4.3.13.4.1), the transmitted gains (5.7.9.3.2) and the
// output level gain are; the curve's conversion that clause 5.7.9.3.1.2 prints
// as 10^(G/20) is taken as 2^(G/6) with them (libs/ac4/ERRATA.md, "DRC's
// units").
//
// The level detector, which clause 5.7.9.3.1.1 leaves to the implementation,
// is meta/drc.hpp's: one wideband value per QMF time slot, so a steady 997 Hz
// sine at full scale in one channel reads -3.01 LKFS, as BS.1770 has it. That
// level and the gain are smoothed as clause 5.7.9.3.1.2 gives, with the time
// constants of the mode (meta/drc.hpp's DrcSmoothing).

namespace iclforge::ac4::detail {

// Table 166: a transmitted curve's control points and time constants.
[[nodiscard]] DrcCurve drc_curve(const DrcCompressionCurve& transmitted) noexcept;

// Table 162: the (E-)AC-3 profile drc_eac3_profile names (Table 160); nullopt
// for a reserved one.
[[nodiscard]] std::optional<DrcCurve> drc_default_curve(int drc_eac3_profile) noexcept;

// Clause 5.7.9.2: the DRC decoder mode the stream's configuration gives for an
// output level, or nullopt where the stream configures none that applies.
// Table 161's ranges are read as whole dB and inclusive, so a fractional
// output level is taken to its nearest whole dB (ERRATA, "Choosing a DRC
// decoder mode").
[[nodiscard]] std::optional<int> drc_mode_for(const DrcConfig& config, DrcMode wanted,
                                              double output_level, bool headphones) noexcept;

// One frame's DRC, as the mode chosen for it compresses: by a curve, by the
// gains of this frame's drc_frame(), or not at all.
struct DrcFrameValues {
    std::optional<double> dialnorm;  // dBFS
    std::optional<DrcCurve> curve;
    std::optional<DrcGainset> gains;
    bool reset = false;  // drc_reset_flag
};

// What a frame's metadata gives the tool: the configuration in force and this
// frame's drc_frame(), when there are ones.
[[nodiscard]] DrcFrameValues drc_frame_values(const OutputConfig& output,
                                              std::optional<double> dialnorm, const DrcState* state,
                                              const DrcFrame* frame);

class DrcStage {
   public:
    // `rate_hz` is the rate the QMF banks run at, the internal rate; `slots`
    // num_qmf_timeslots; `speakers` the channels, in the order process() gets
    // their matrices; `add_ch_base` groups a 7.X element's channels for the
    // transmitted gains (Table 168), and `immersive` the immersive element's or
    // the 22.2 element's by Part 2's Table 69.
    void configure(double rate_hz, int slots, std::span<const Speaker> speakers, bool add_ch_base,
                   bool immersive = false);

    // Forgets the smoothing and the last dialnorm.
    void reset() noexcept;

    // Applies one frame to `matrices`, one per channel in configure()'s order,
    // `slots` slots of 64 subbands each, in place. `side` are the same channels
    // before dialogue enhancement, the side chain the level is measured on
    // (Part 1 clause 6.2.13); they may be `matrices` themselves.
    void process(const OutputConfig& output, const DrcFrameValues& values,
                 std::span<const QmfMatrix> matrices,
                 std::span<const QmfMatrix> side);

    // The linear gain the last slot of the last frame had, output level gain
    // included, for tests.
    [[nodiscard]] double last_gain() const noexcept { return last_gain_; }

   private:
    // The K-weighted mean square per sample of `side` in one slot, at full
    // scale 1.0; read only.
    [[nodiscard]] double slot_level(std::span<const QmfMatrix> side, int slot) const;

    double rate_hz_ = 48000.0;
    int slots_ = 32;
    std::vector<Speaker> speakers_;
    std::vector<double> loudness_weight_;  // BS.1770's channel weights
    std::vector<int> group_;               // Table 168's (or 69's) channel group, per channel
    std::array<double, 64> k_weight_{};    // |K(f)|^2 at each subband's centre
    double qmf_gain_ = 1.0;                // sum of QWIN^2
    std::optional<double> dialnorm_;       // the last one carried
    DrcSmoothing smoothing_;
    double last_gain_ = 1.0;
};

}  // namespace iclforge::ac4::detail
