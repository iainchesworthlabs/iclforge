#pragma once

// The Meters view's rules, as pure functions of what the board knows (planning/p4x-meters-view.md): which representation of the channels
// appears under the spectrum, from the display's width and the number of outputs of the room; how the room's outputs fold into the six
// of a surround sum and into the stereo downmix; how many bands make a bar of the spectrum; the ranges, zones and ballistics the meters
// share. The drawing (src/panel/lvgl/meters_screen.cpp) and the analysis (iclforge/dsp/spectrum.hpp) are elsewhere; none of this
// draws or allocates, and every input is in the capabilities, never the name of a board.
//
// The rule (the owner's, 2026-10-26, from the pictures of planning/p4x-meters-view.md):
//
//                   2.0     5.1     7.1.4   9.1.6
//   width >= 800    pair    grid    grid    grid        every output a bar, grouped in space; the spectrum on top
//   400 to 799      pair    sum     sum     sum         the six masters L C R LFE Ls Rs, heights and rears folded into their sides
//   under 400       pair    pair    pair    pair        two thin bars: the room's own L and R, or its stereo downmix

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace iclforge::meters {

// --- the rule ---------------------------------------------------------------------------------------------------------

inline constexpr int kMediumWidth = 400;
inline constexpr int kLargeWidth = 800;

// The most outputs a room has (Playout::kMaxOutputs).
inline constexpr std::size_t kMaxOutputs = 16;

enum class Representation : std::uint8_t {
    kPair,  // a stereo master: L and R of a room of two, the stereo downmix of a bigger one
    kSum,   // the surround sum: six bars, L C R LFE Ls Rs
    kGrid,  // a bar for every output, grouped in space
};

[[nodiscard]] constexpr Representation representation_for(int display_width, int outputs) {
    if (outputs <= 2 || display_width < kMediumWidth) {
        return Representation::kPair;
    }
    return display_width < kLargeWidth ? Representation::kSum : Representation::kGrid;
}

// --- the spectrum's bars ----------------------------------------------------------------------------------------------------

// The spectrum has 31 third-octave bands (iclforge::dsp::spectrum::kBands); a bar is one band on the large screen, two on the middle one (15
// bars, 20 Hz dropped) and three on the small one (10 bars, an octave each).
inline constexpr int kBands = 31;

[[nodiscard]] constexpr int bands_per_bar(int display_width) {
    return display_width >= kLargeWidth ? 1 : display_width >= kMediumWidth ? 2 : 3;
}

[[nodiscard]] constexpr int spectrum_bars(int per_bar) { return per_bar == 1 ? kBands : (kBands - 1) / per_bar; }

[[nodiscard]] constexpr int first_band_of_bar(int bar, int per_bar) { return per_bar == 1 ? bar : 1 + bar * per_bar; }

// The level of a bar of `per_bar` bands: their powers added (levels are in dB below a full-scale sine, so the sum of the 10^(dB/10)).
inline constexpr float kSilentDb = -120.0F;

// 10^(dB / 10) and 10 log10(p) in single precision: the P4 has no double-precision unit, and a double pow or log10 there is several microseconds
// (planning/p4x-meters-view.md, "Kit results"). A float keeps 1e-6 dB of what a bar is read to.
[[nodiscard]] inline float db_to_power(float db) { return std::exp2(db * 0.33219281F); }
[[nodiscard]] inline float power_to_db(float power) { return power <= 1e-12F ? kSilentDb : 3.01029996F * std::log2(power); }

[[nodiscard]] inline float bar_db(std::span<const float, kBands> bands, int bar, int per_bar) {
    if (per_bar == 1) {
        return bands[static_cast<std::size_t>(bar)];
    }
    float sum = 0.0F;
    for (int b = first_band_of_bar(bar, per_bar); b < first_band_of_bar(bar, per_bar) + per_bar && b < kBands; ++b) {
        const float db = bands[static_cast<std::size_t>(b)];
        if (db > kSilentDb) {
            sum += db_to_power(db);
        }
    }
    return power_to_db(sum);
}

// The names under the bands, as many as fit (the screen draws every fourth on a row or two).
inline constexpr std::array<std::string_view, kBands> kBandLabels = {"20",  "25",  "31",  "40",  "50",  "63",  "80",  "100", "125", "160", "200",
                                                                     "250", "315", "400", "500", "630", "800", "1k",  "1.2k", "1.6k", "2k",  "2.5k",
                                                                     "3.1k", "4k",  "5k",  "6.3k", "8k",  "10k", "12k", "16k", "20k"};

// --- ranges, zones, ballistics ----------------------------------------------------------------------------------------------

// What a bar or a meter spans, in dB below full scale.
inline constexpr float kFloorDb = -60.0F;
inline constexpr float kTopDb = 0.0F;

// A level meter's zones: green below, amber from, red from.
inline constexpr float kMeterAmberDb = -18.0F;
inline constexpr float kMeterRedDb = -6.0F;
// The spectrum's bars, in the same three colours (the owner's choice, 2026-10-26).
inline constexpr float kSpectrumAmberDb = -30.0F;
inline constexpr float kSpectrumRedDb = -15.0F;

// How the bars move: fast attack (a rise is at once), a fall in dB a second, a peak held and then falling.
inline constexpr float kSpectrumFallDbPerS = 30.0F;
inline constexpr float kSpectrumHoldS = 1.0F;
inline constexpr float kSpectrumPeakFallDbPerS = 15.0F;
inline constexpr float kMeterFallDbPerS = 24.0F;
inline constexpr float kMeterHoldS = 1.5F;
inline constexpr float kMeterPeakFallDbPerS = 10.0F;
// The analyser's time average, in the power domain.
inline constexpr float kSpectrumAverageS = 0.08F;

// --- folding the room ---------------------------------------------------------------------------------------------------------

// What the board knows of an output: where its speaker is (azimuth positive to the left, elevation above the listener's ears), whether it
// carries the low-frequency effects, and whether anything is connected to it.
struct Spot {
    float azimuth_deg = 0.0F;
    float elevation_deg = 0.0F;
    bool lfe = false;
    bool connected = true;
};

// The six masters of the surround sum.
enum class Master : std::uint8_t { kL, kC, kR, kLfe, kLs, kRs };
inline constexpr std::size_t kMasters = 6;
inline constexpr std::array<std::string_view, kMasters> kMasterNames = {"L", "C", "R", "LFE", "Ls", "Rs"};

// The master an output is folded into: the one nearest its azimuth (a height or a rear goes to the side it is on); the LFE keeps its own.
[[nodiscard]] constexpr Master master_of(const Spot& spot) {
    if (spot.lfe) {
        return Master::kLfe;
    }
    const float m = spot.azimuth_deg < 0.0F ? -spot.azimuth_deg : spot.azimuth_deg;
    if (m < 15.0F) {
        return Master::kC;
    }
    if (m < 75.0F) {
        return spot.azimuth_deg > 0.0F ? Master::kL : Master::kR;
    }
    return spot.azimuth_deg > 0.0F ? Master::kLs : Master::kRs;
}

// The spatial groups of the grid, left to right.
enum class Group : std::uint8_t { kFront, kSurround, kRear, kHeight, kLfe };
inline constexpr std::size_t kGroups = 5;
inline constexpr std::array<std::string_view, kGroups> kGroupNames = {"Front", "Surround", "Rear", "Height", "LFE"};

[[nodiscard]] constexpr Group group_of(const Spot& spot) {
    if (spot.lfe) {
        return Group::kLfe;
    }
    if (spot.elevation_deg > 20.0F) {
        return Group::kHeight;
    }
    const float m = spot.azimuth_deg < 0.0F ? -spot.azimuth_deg : spot.azimuth_deg;
    if (m <= 70.0F) {
        return Group::kFront;
    }
    return m <= 125.0F ? Group::kSurround : Group::kRear;
}

// A list of outputs.
struct Members {
    std::array<std::uint8_t, kMaxOutputs> output{};
    std::uint8_t count = 0;

    void add(std::size_t o) { output[count++] = static_cast<std::uint8_t>(o); }
    [[nodiscard]] std::span<const std::uint8_t> span() const { return std::span<const std::uint8_t>(output.data(), count); }
};

// The room folded two ways, from its spots: which outputs make each master, and each group; the connected outputs in order.
struct Fold {
    std::array<Members, kMasters> master;
    std::array<Members, kGroups> group;
    Members connected;

    [[nodiscard]] static Fold of(std::span<const Spot> spots) {
        Fold f;
        for (std::size_t i = 0; i < spots.size() && i < kMaxOutputs; ++i) {
            if (!spots[i].connected) {
                continue;
            }
            f.master[static_cast<std::size_t>(master_of(spots[i]))].add(i);
            f.group[static_cast<std::size_t>(group_of(spots[i]))].add(i);
            f.connected.add(i);
        }
        return f;
    }
};

// Levels of channels that are not known to agree add as powers: the dB of the sum of the 10^(dB/10) of `members` (kSilentDb when there are none or all are silent).
[[nodiscard]] inline float power_sum_db(std::span<const float> peak_db, std::span<const std::uint8_t> members) {
    float p = 0.0F;
    for (const std::uint8_t o : members) {
        if (o < peak_db.size() && peak_db[o] > kSilentDb) {
            p += db_to_power(peak_db[o]);
        }
    }
    return power_to_db(p);
}

// The stereo downmix of the room, in the same terms (ITU: the centre and a surround at -3 dB into the side it goes to; the LFE not at all).
inline void downmix_db(std::span<const float> peak_db, const Fold& fold, float& left, float& right) {
    std::array<float, kMasters> p{};
    for (std::size_t m = 0; m < kMasters; ++m) {
        const float db = power_sum_db(peak_db, fold.master[m].span());
        p[m] = db > kSilentDb ? db_to_power(db) : 0.0F;
    }
    const float l = p[static_cast<std::size_t>(Master::kL)] + 0.5F * p[static_cast<std::size_t>(Master::kC)] + 0.5F * p[static_cast<std::size_t>(Master::kLs)];
    const float r = p[static_cast<std::size_t>(Master::kR)] + 0.5F * p[static_cast<std::size_t>(Master::kC)] + 0.5F * p[static_cast<std::size_t>(Master::kRs)];
    left = power_to_db(l);
    right = power_to_db(r);
}

}  // namespace iclforge::meters
