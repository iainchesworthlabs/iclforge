#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "iclforge/ac4/decoder/decoder.hpp"
#include "decoder/syntax/presentation.hpp"

// Part 2's channel audio renderer, ETSI TS 103 190-2 V1.3.1 clause 5.10.2, for
// the immersive element: the generalized rendering matrix (5.10.2.2) from the
// decoded channels to an output channel configuration, by Tables 38 to 43 in
// full decoding and Tables 45 and 46 in core decoding, with the custom downmix
// parameters an I-frame sends (6.2.9.2, Table 130's defaults otherwise), and
// the loudness correction of the output (4.8.5.3).
//
// The input channel configuration is the layout the element's source had,
// which the substream's presence flags give (6.3.2.7.3 to 6.3.2.7.5): 7.X with
// b_4_back_channels_present and 5.X without; .4 with top_channels_present 3,
// .2 with 1 or 2, whose Tsl and Tsr the element carries in Tfl and Tfr or in
// Tbl and Tbr, and .0 with 0 (src/ac4dec/ERRATA.md, "The renderer's input
// channel configuration"). The 9.X.4 modes' are 9.X.4, 9.X.2 and 9.X.0 by
// top_channels_present alike: the 7.X configuration with the screen pair Lscr
// and Rscr, whatever b_4_back_channels_present says, since Table A.27 has Lb and
// Rb in the 9.X layouts as channels that can be silent ("The 9.X.4 element's
// rendering"). The channels the configuration leaves out are silenced, as
// 5.10.2.2 says.

namespace iclforge::ac4::detail {

// The immersive element's content and decoding, as the renderer takes them.
struct ImmersiveLayout {
    bool backs = true;  // b_4_back_channels_present
    int tops = 3;       // top_channels_present
    bool lfe = true;    // the channel mode's LFE
    bool screen = false;  // a 9.X.4 mode: Lscr and Rscr, and Lb and Rb whatever `backs` says
    DecodingMode decoding = DecodingMode::kFull;

    friend bool operator==(const ImmersiveLayout&, const ImmersiveLayout&) = default;
};

// An output (or input) channel configuration of Tables 34 and 44: its width
// (5 or 7), its top channels (0, 2 or 4) and, for a 9.X one, the screen pair
// (the 7.X width, and Lscr and Rscr).
struct ChannelConfiguration {
    int width = 5;
    int tops = 0;
    bool screen = false;

    friend bool operator==(const ChannelConfiguration&, const ChannelConfiguration&) = default;
};

// The input channel configuration of `layout`, in full decoding.
[[nodiscard]] ChannelConfiguration input_configuration(const ImmersiveLayout& layout) noexcept;

// The custom downmix gains the tables use, linear: gain_f1 (Table 128), gain_f2,
// gain_b, gain_t1 and gain_t2a to gain_t2f (Table 129).
struct RenderGains {
    double gain_f1 = 0.0;
    double gain_f2 = 1.0;
    double gain_b = 0.0;
    double gain_t1 = 0.0;
    std::array<double, 6> gain_t2{};  // a to f
};

// Clause 6.3.10.3.10: the gains for `out_ch_config` (Table 127) from `cdmx`,
// the last custom_dmx_data() that sent custom downmix data (none: nullptr),
// Table 130's defaults for what it does not send, and from bs_ch_config 1 to
// out_ch_config 4 out_ch_config 1's tool_t4_to_t2() where out_ch_config 4
// sends none (src/ac4dec/ERRATA.md, "Custom downmix data").
[[nodiscard]] RenderGains render_gains(const CustomDmxData* cdmx, int out_ch_config) noexcept;

// The layout decode() gives `target` for an immersive element: its channels,
// and the configuration the renderer takes the input to on the way, 5.X.0 for
// the two-channel and mono targets (whose Part 1 steps follow, pcm/downmix.hpp).
// As coded, a 9.X layout's channels are in Table A.27's order, which the
// decoded channels are in; every other layout's are in Part 1's.
struct RenderPlan {
    ChannelConfiguration output{};
    std::vector<Speaker> speakers;  // the output configuration's, in decode()'s order
    bool stereo = false;            // the Part 1 steps to two channels (or one) follow
};
[[nodiscard]] RenderPlan render_plan(const ImmersiveLayout& layout, DownmixTarget target);

// The rendering matrix from the channels `decoded` names to `plan`'s output
// configuration: one row per plan.speakers channel, one column per decoded
// channel.
[[nodiscard]] std::vector<std::vector<double>> render_matrix(const ImmersiveLayout& layout,
                                                             std::span<const Speaker> decoded,
                                                             const RenderPlan& plan,
                                                             const RenderGains& gains);

// Table 127's out_ch_config of an output configuration below 7.X.4; nothing
// for 7.X.4, which takes no custom downmix parameters.
[[nodiscard]] std::optional<int> out_ch_config(const ChannelConfiguration& output) noexcept;

// Clause 4.8.5.3: which loud_corr() correction an output configuration takes,
// in full or core decoding: none where the renderer downmixes nothing, its
// output as wide and as high as the input configuration.
enum class LoudCorrOutput : std::uint8_t {
    kNone,
    k5X,
    k5X2,
    k5X4,
    k7X,
    k7X2,
    k7X4,
    kCore5X2,
    kCore5X,
};
[[nodiscard]] LoudCorrOutput loud_corr_output(const ImmersiveLayout& layout,
                                              const ChannelConfiguration& output) noexcept;

}  // namespace iclforge::ac4::detail
