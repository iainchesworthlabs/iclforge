#pragma once

#include <array>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "iclforge/ac4/decoder/decoder.hpp"
#include "decoder/pcm/aspx.hpp"
#include "decoder/pcm/renderer.hpp"
#include "decoder/syntax/metadata.hpp"
#include "decoder/syntax/presentation.hpp"

// Rendering the decoded channels to fewer (ETSI TS 103 190-1 V1.4.1 clause
// 6.2.17), in the QMF domain after DRC, whose curve gain is the same in every
// channel and so passes through a downmix unchanged, and before synthesis, so
// only the channels that come out are synthesised.
//
// The downmixes cascade: a 7.X element's channels to 5.X by Table 219, which
// its channel mode and add_ch_base choose; 5.X (or 3.0) to two channels by
// Table 218 (217), Lo/Ro, Lt/Rt or Lt/Rt in its Pro Logic II form, with the
// stream's centre and surround mix gains (Tables 149 and 149a, -3 dB where it
// has sent none), the LFE at lfe_mixgain, and the Lo/Ro or Lt/Rt loudness
// correction; and two channels to one as L + R. A listener's choice of Lo/Ro or
// Lt/Rt overrides preferred_dmx_method, and Lt/Rt takes its Pro Logic II form
// where the stream prefers that. A mono stream comes out in stereo at 0.707 in
// each channel (6.2.17.6). The folds to 5.X take the loudness correction Part
// 2 clause 4.8.5.3 gives them.
//
// The immersive element takes Part 2's channel renderer (clause 5.10.2,
// pcm/renderer.hpp) instead of step 1: to the layout asked for, with the
// correction 4.8.5.3 gives that output, and to 5.X.0 on the way to two
// channels or one, the steps to which follow as for 5.X with the Lo/Ro or
// Lt/Rt correction alone, the core's in core decoding (libs/ac4/ERRATA.md,
// "The renderer's two-channel output").
//
// The mix values, the custom downmix data and the loudness corrections
// persist from the frame that sends them until another does (6.2.17.0, Part
// 2 clause 4.8.5.3). The gains are in dB, their linear values 10^(dB/20)
// (libs/ac4/ERRATA.md, "The downmix gains").
//
// Two more corrections scale every channel that comes out, as coded or not,
// and are the frame's own: an alternative presentation's target loudness
// correction, by the category of the device the output plays on (Part 2
// clause 4.8.5.4, Tables 17 and 67), and the real-time loudness correction
// (clause 4.8.5.5). They are made in this stage, with the downmix's own
// loudness correction (4.8.5.3), which is after DRC here. Part 2 puts loudness
// correction before DRC, and the two orders differ only in what a compression
// curve's level detector measures (libs/ac4/ERRATA.md, "Alternative and
// real-time loudness correction").

namespace iclforge::ac4::detail {

// Part 2 clause 4.8.5.4: for each of Table 67's categories (TargetDevice's order), the
// loud_corr_target code an alternative presentation gives it, a category no target specifies
// taking the code of Table 17's first fallback that has one.
using TargetCorrections = std::array<std::optional<int>, 4>;

// What a frame's metadata gives the downmix, where it sends them.
struct DownmixValues {
    std::optional<StereoDmxCoeff> coeff;
    std::optional<int> loro_loud_corr;
    std::optional<int> ltrt_loud_corr;
    std::optional<int> loud_corr_5x;  // Part 2's loud_corr_5_X
    // The rest of Part 2's loud_corr() (clause 6.2.9.1), for the immersive
    // element's outputs, and core decoding's.
    std::optional<int> loud_corr_5x2;
    std::optional<int> loud_corr_5x4;
    std::optional<int> loud_corr_7x;
    std::optional<int> loud_corr_7x2;
    std::optional<int> loud_corr_7x4;
    std::optional<int> loud_corr_core_5x2;
    std::optional<int> loud_corr_core_5x;
    std::optional<int> loud_corr_core_loro;
    std::optional<int> loud_corr_core_ltrt;
    // custom_dmx_data() where it sends custom downmix data (b_cdmx_data_present).
    std::optional<CustomDmxData> cdmx;
    // The next three belong to the frame that sends them and do not persist: the targets of an
    // alternative presentation are in each frame's presentation substream, and the real-time
    // loudness correction is real-time data.
    //
    // Part 2 clause 4.8.5.5: rtll_comp, where the frame sends one.
    std::optional<int> rtll_comp;
    // Clause 4.8.5.4: an alternative presentation's target corrections (none for any other).
    TargetCorrections target_corr{};
    // OutputConfig::target_device: the category the output plays on, where the system says.
    std::optional<TargetDevice> device;
};

// The values from the presentation substream (bitstream version 2), or else
// the audio substream's basic_metadata().
[[nodiscard]] DownmixValues downmix_values(const PresentationSubstream* presentation,
                                           const Metadata& metadata);

// Tables 149 and 149a: a mix gain code's linear gain; a surround code the
// table reserves reads as the -3 dB of no code at all.
[[nodiscard]] double centre_mix_gain(int code) noexcept;
[[nodiscard]] double surround_mix_gain(int code) noexcept;

class DownmixStage {
   public:
    // The channels `speakers` names, in that order, to `target`'s layout; an
    // immersive element's, which `immersive` describes, by Part 2's renderer.
    void configure(std::span<const Speaker> speakers, bool add_ch_base, DownmixTarget target,
                   bool mix_lfe, const std::optional<ImmersiveLayout>& immersive = std::nullopt);

    // Whether the channels come out as coded: as the layout asks for, and at no correction of
    // clause 4.8.5.4 or 4.8.5.5, which scale every channel.
    [[nodiscard]] bool passes_through() const noexcept {
        return pass_through_ && correction_gain_ == 1.0;
    }

    // The layout that comes out.
    [[nodiscard]] std::span<const Speaker> speakers() const noexcept { return out_speakers_; }

    // Back to the values no stream has sent: -3 dB mix gains, no LFE, no
    // loudness correction, Table 130's custom downmix parameters.
    void reset();

    // Takes this frame's values, which then stay in force: the matrix() they give. process()
    // does this first; a caller that applies the matrix to something other than QMF values (a
    // substream at 96 or 192 kHz has none) calls this and reads matrix().
    void update(const DownmixValues& values);

    // Takes this frame's values and writes out[o] = sum_c M[o][c] in[c] for
    // every QMF value, out resized to speakers().
    // Each output channel is its own sum, and runs through `executor` where there is one
    // (iclforge/ac4/decoder/executor.hpp).
    void process(const DownmixValues& values, std::span<const QmfMatrix> in,
                 std::vector<std::vector<QmfValue>>& out, Executor* executor = nullptr);

    // The matrix in force, one row per channel out, one column per channel in.
    [[nodiscard]] const std::vector<std::vector<double>>& matrix() const noexcept {
        return matrix_;
    }

   private:
    // A combination of the input channels, one weight per channel.
    using Mix = std::vector<double>;

    // The matrix of the layout, then the corrections of clauses 4.8.5.4 and 4.8.5.5 on every row.
    void rebuild();
    void rebuild_matrix();
    // The immersive element's matrix, by render_matrix().
    void rebuild_immersive();
    // The device category the output plays on: the system's, else Table 17's by the layout.
    [[nodiscard]] std::optional<TargetDevice> playback_device() const;
    // The scalar the corrections of clauses 4.8.5.4 and 4.8.5.5 make together, 1 for none.
    [[nodiscard]] double corrections() const;
    // Step 2: Lo and Ro from the 5.X channels, each a combination of the
    // input channels, with the Lo/Ro or Lt/Rt correction given.
    [[nodiscard]] std::pair<Mix, Mix> two_channels(const Mix& l, const Mix& r, const Mix& c,
                                                   const Mix& lfe, const Mix& ls, const Mix& rs,
                                                   const std::optional<int>& loro_correction,
                                                   const std::optional<int>& ltrt_correction) const;
    // Lo and Ro into the matrix, or for mono their sum (6.2.17.2).
    void push_two(const Mix& lo, const Mix& ro);
    // The loudness correction in force for `output`.
    [[nodiscard]] std::optional<int> correction(LoudCorrOutput output) const noexcept;

    std::vector<Speaker> in_speakers_;
    std::vector<Speaker> out_speakers_;
    bool add_ch_base_ = false;
    DownmixTarget target_ = DownmixTarget::kAsCoded;
    bool mix_lfe_ = true;
    bool pass_through_ = true;
    // This frame's rtll_comp, targets and device (DownmixValues), and the scalar they make.
    std::optional<int> rtll_comp_;
    TargetCorrections target_corr_{};
    std::optional<TargetDevice> device_;
    double correction_gain_ = 1.0;
    std::optional<ImmersiveLayout> immersive_;
    RenderPlan plan_;  // the immersive element's
    // The values in force.
    std::optional<StereoDmxCoeff> coeff_;
    std::optional<int> loro_loud_corr_;
    std::optional<int> ltrt_loud_corr_;
    std::optional<int> loud_corr_5x_;
    std::optional<int> loud_corr_5x2_;
    std::optional<int> loud_corr_5x4_;
    std::optional<int> loud_corr_7x_;
    std::optional<int> loud_corr_7x2_;
    std::optional<int> loud_corr_7x4_;
    std::optional<int> loud_corr_core_5x2_;
    std::optional<int> loud_corr_core_5x_;
    std::optional<int> loud_corr_core_loro_;
    std::optional<int> loud_corr_core_ltrt_;
    std::optional<CustomDmxData> cdmx_;
    std::vector<std::vector<double>> matrix_;
};

}  // namespace iclforge::ac4::detail
