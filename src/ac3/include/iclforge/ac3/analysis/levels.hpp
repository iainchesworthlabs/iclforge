#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "iclforge/ac3/core/tables.hpp"
#include "iclforge/ac3/export.hpp"
#include "iclforge/base/levels.hpp"

// The level meter is iclforge::base's (iclforge/base/levels.hpp, since
// planning/consolidation.md's C6), and its names are kept here for a release.
// What is AC-3's is the acmod: A/52 Table 5.8's channel order, names and
// directions, and a meter keyed on them.

namespace iclforge::ac3::analysis {

using base::ChannelLevel;
using base::ChannelSummary;
using base::kFloorDb;
using base::kFullScale;
using base::meter_fraction;
using base::MeterBallistics;
using base::SoundfieldVector;
using base::to_dbfs;

[[nodiscard]] constexpr int channel_count(Acmod acmod, bool lfe) {
    return fullbw_channel_count(acmod) + (lfe ? 1 : 0);
}

// A/52 Table 5.8 channel array ordering. `index` runs over the full-bandwidth
// channels in that order, with the LFE last when present; an out-of-range
// index gives an empty view.
[[nodiscard]] ICLFORGE_AC3_EXPORT std::string_view channel_name(Acmod acmod, bool lfe, int index);

// The layout in the spec's own front/rear notation, e.g. "3/2 + LFE".
[[nodiscard]] ICLFORGE_AC3_EXPORT std::string_view layout_name(Acmod acmod, bool lfe);

// Loudspeaker azimuth for a channel: degrees counterclockwise from front, on
// the same ITU-R BS.775 ring the spatial renderer pans over. Empty for the
// LFE, which carries no direction, and for 1+1 dual mono, whose two channels
// are unrelated programs rather than one soundfield.
[[nodiscard]] ICLFORGE_AC3_EXPORT std::optional<double> channel_azimuth_deg(Acmod acmod, bool lfe,
                                                                        int index);

// Meters audio in A/52 channel order.
class ICLFORGE_AC3_EXPORT LevelMeter : public base::LevelMeter {
   public:
    LevelMeter(Acmod acmod, bool lfe, std::uint32_t sample_rate,
               const MeterBallistics& ballistics = {});

    // Meters `channels` channels instead of the acmod's own count, for a
    // layout no acmod can name: E-AC-3's dependent substreams add speakers the
    // coding mode has no word for, and a 7.1.4 access unit carries fourteen
    // coded channels against a coding mode that tops out at six.
    //
    // The acmod still names and places the first channel_count(acmod, lfe) of
    // them - those are the bed, in Table 5.8 order, and they are what the
    // soundfield ring is computed from. The rest are metered but contribute no
    // direction, which is also what channel_azimuth_deg says about them.
    // `channels` below the acmod's own count is raised to it rather than
    // truncating a layout the caller has already committed to.
    LevelMeter(Acmod acmod, bool lfe, std::uint32_t sample_rate, int channels,
               const MeterBallistics& ballistics = {});

    [[nodiscard]] Acmod acmod() const { return acmod_; }
    [[nodiscard]] bool lfe() const { return lfe_; }

   private:
    Acmod acmod_;
    bool lfe_;
};

// Computed from the integrated RMS of the full-bandwidth channels only: the
// LFE has no direction to contribute, and a subwoofer's level would otherwise
// swamp the sum.
[[nodiscard]] ICLFORGE_AC3_EXPORT SoundfieldVector energy_vector(std::span<const ChannelLevel> levels,
                                                             Acmod acmod);

}  // namespace iclforge::ac3::analysis
