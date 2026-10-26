#pragma once

// The room a layout describes, seen from above, for a screen: where each speaker is drawn, and how a
// level makes one glow (planning/p4x-now-playing.md). The ESP32-P4's panel draws it while a song with no
// lyrics plays; the numbers are here, standard library only and tested on the host
// (libs/render/tests/test_room_plan.cpp), because a wrong subscript here puts the centre in the
// subwoofer's place and the picture is the only thing that would say so.
//
// A layout gives every slot a direction (azimuth counterclockwise from the front, left positive, and an
// elevation above the listener's plane). Seen from above the front is at the top and left is on the
// left; a height speaker is pulled in towards the listener by the cosine of its elevation, which is what
// the view from above does to it, and a ceiling speaker (90 degrees) lands over the listener. The LFE
// has no place in the room and goes along the bottom. The result is fitted to an area: a room with
// surrounds fills it, a room with two speakers does not stretch them across it.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "iclforge/base/layout.hpp"
#include "iclforge/render/layout.hpp"

namespace iclforge::render {

// One output slot of a layout, as the plan needs it.
struct PlanSpot {
    // The location's name ("L", "Ls", "Vhl", "LFE"), or "S7" for a slot placed by an angle.
    std::string name;
    double azimuth_deg = 0.0;
    double elevation_deg = 0.0;
    bool lfe = false;
    // False for a slot nothing is connected to: it is not drawn.
    bool connected = true;
};

// The slots of `layout`, in slot order.
[[nodiscard]] inline std::vector<PlanSpot> plan_spots(const OutputLayout& layout) {
    std::vector<PlanSpot> out;
    out.reserve(layout.slots());
    for (std::size_t i = 0; i < layout.slots(); ++i) {
        const Speaker& slot = layout.slot(i);
        PlanSpot spot;
        spot.lfe = slot.kind == Speaker::Kind::kLfe;
        spot.connected = slot.kind != Speaker::Kind::kEmpty;
        spot.azimuth_deg = slot.direction.azimuth_deg;
        spot.elevation_deg = slot.direction.elevation_deg;
        spot.name = slot.location ? std::string(base::name(*slot.location)) : "S" + std::to_string(i + 1);
        out.push_back(std::move(spot));
    }
    return out;
}

enum class PlanShape : std::uint8_t {
    kNone,    // a slot nothing is connected to
    kEar,     // a full-bandwidth speaker at ear level: a circle
    kHeight,  // a height speaker: a rounded square
    kLfe,     // a low-frequency feed: a wide rounded box
};

// Where a slot is drawn: the centre and the size, in pixels of the area it was fitted to.
struct PlanNode {
    PlanShape shape = PlanShape::kNone;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

struct PlanArea {
    int width = 1024;
    int height = 472;
};

inline constexpr int kPlanEarSize = 58;
inline constexpr int kPlanHeightSize = 48;
inline constexpr int kPlanLfeWidth = 84;
inline constexpr int kPlanLfeHeight = 50;
// A speaker at or above this elevation is overhead, and where there are several they are fanned on a ring of this size
// (in units of the room's radius) instead of piled on the listener.
inline constexpr double kPlanOverheadDeg = 80.0;
inline constexpr double kPlanOverheadRing = 0.3;

// The sizes plan_nodes() lays a room out in: the margins the speakers keep from the edge of the area, the speakers' own
// sizes, how far apart two must be and the step by which two that touch are pushed apart. The defaults are the screen's
// (1024 pixels wide, speakers of 58); plan_style_for() is the same room in a pane.
struct PlanStyle {
    int margin_x = 170;
    int margin_y = 62;
    int ear = kPlanEarSize;
    int height = kPlanHeightSize;
    int lfe_width = kPlanLfeWidth;
    int lfe_height = kPlanLfeHeight;
    int lfe_pitch = 110;
    int gap = 6;
    int step = 4;
};

namespace detail {

[[nodiscard]] inline bool plan_overlap(const PlanNode& a, const PlanNode& b, int gap) {
    return std::abs(a.x - b.x) < (a.w + b.w) / 2 + gap && std::abs(a.y - b.y) < (a.h + b.h) / 2 + gap;
}

}  // namespace detail

// One node a slot, in slot order. Nodes that would touch are pushed apart (a 9.1.6 room has a ceiling
// speaker over the listener and a front height next to the centre), and every node stays inside the area.
[[nodiscard]] inline std::vector<PlanNode> plan_nodes(std::span<const PlanSpot> spots, PlanArea area = {},
                                                       PlanStyle style = {}) {
    constexpr double kDegrees = 3.14159265358979323846 / 180.0;
    std::vector<PlanNode> nodes(spots.size());
    const int margin_x = style.margin_x;
    const int margin_y = style.margin_y;
    const int left = margin_x;
    const int right = std::max(left + 1, area.width - margin_x);
    const int top = margin_y;
    const int bottom = std::max(top + 1, area.height - margin_y);

    std::vector<std::size_t> placed;
    std::vector<double> u(spots.size(), 0.0);
    std::vector<double> v(spots.size(), 0.0);
    double min_u = 1e9;
    double max_u = -1e9;
    double min_v = 1e9;
    double max_v = -1e9;
    // Two or more speakers straight overhead (a layout with its heights in the ceiling) have no direction from above: all
    // would land on the listener. They are drawn on a small ring of their own, each in the direction its azimuth names. One
    // overhead speaker (the top surround of a 9.1.6) stays over the listener, where it always was.
    int overhead = 0;
    for (const PlanSpot& spot : spots) {
        overhead += spot.connected && !spot.lfe && spot.elevation_deg >= kPlanOverheadDeg ? 1 : 0;
    }
    for (std::size_t i = 0; i < spots.size(); ++i) {
        if (!spots[i].connected || spots[i].lfe) {
            continue;
        }
        double ring = std::cos(spots[i].elevation_deg * kDegrees);
        if (overhead >= 2 && spots[i].elevation_deg >= kPlanOverheadDeg) {
            ring = std::max(ring, kPlanOverheadRing);
        }
        u[i] = -std::sin(spots[i].azimuth_deg * kDegrees) * ring;
        v[i] = -std::cos(spots[i].azimuth_deg * kDegrees) * ring;
        placed.push_back(i);
        min_u = std::min(min_u, u[i]);
        max_u = std::max(max_u, u[i]);
        min_v = std::min(min_v, v[i]);
        max_v = std::max(max_v, v[i]);
    }
    if (!placed.empty()) {
        const double du = max_u - min_u;
        const double dv = max_v - min_v;
        // A room with two speakers is not stretched across the screen, and one with none behind the
        // listener is not stretched down it.
        const double cap_x = 0.35 * area.width;
        const double cap_y = 0.42 * area.height;
        const double sx = std::min(du < 1e-6 ? cap_x : (right - left) / du, cap_x);
        const double sy = dv < 1e-6 ? 0.0 : std::min((bottom - top) / dv, cap_y);
        const double mid_x = (left + right) / 2.0;
        const double mid_y = (top + bottom) / 2.0;
        const double centre_u = (min_u + max_u) / 2.0;
        const double centre_v = (min_v + max_v) / 2.0;
        for (const std::size_t i : placed) {
            const bool height = spots[i].elevation_deg >= 30.0;
            nodes[i].shape = height ? PlanShape::kHeight : PlanShape::kEar;
            nodes[i].w = nodes[i].h = height ? style.height : style.ear;
            nodes[i].x = static_cast<int>(std::lround(mid_x + (u[i] - centre_u) * sx));
            nodes[i].y = static_cast<int>(std::lround(mid_y + (v[i] - centre_v) * sy));
        }
        // Pushed apart along the line between the two centres (straight down for a pair on the same
        // spot), a little at a time, so that the same layout always draws the same picture.
        for (int pass = 0; pass < 40; ++pass) {
            bool moved = false;
            for (std::size_t a = 0; a + 1 < placed.size(); ++a) {
                for (std::size_t b = a + 1; b < placed.size(); ++b) {
                    PlanNode& first = nodes[placed[a]];
                    PlanNode& second = nodes[placed[b]];
                    if (!detail::plan_overlap(first, second, style.gap)) {
                        continue;
                    }
                    double dx = second.x - first.x;
                    double dy = second.y - first.y;
                    const double length = std::hypot(dx, dy);
                    if (length < 1e-6) {
                        dx = 0.0;
                        dy = 1.0;
                    } else {
                        dx /= length;
                        dy /= length;
                    }
                    first.x -= static_cast<int>(std::lround(dx * style.step));
                    first.y -= static_cast<int>(std::lround(dy * style.step));
                    second.x += static_cast<int>(std::lround(dx * style.step));
                    second.y += static_cast<int>(std::lround(dy * style.step));
                    moved = true;
                }
            }
            if (!moved) {
                break;
            }
        }
        for (const std::size_t i : placed) {
            nodes[i].x = std::clamp(nodes[i].x, nodes[i].w / 2, area.width - nodes[i].w / 2);
            nodes[i].y = std::clamp(nodes[i].y, nodes[i].h / 2, area.height - nodes[i].h / 2);
        }
    }

    int lfe_total = 0;
    for (const PlanSpot& spot : spots) {
        lfe_total += spot.lfe && spot.connected ? 1 : 0;
    }
    int lfe_seen = 0;
    for (std::size_t i = 0; i < spots.size(); ++i) {
        if (!spots[i].lfe || !spots[i].connected) {
            continue;
        }
        nodes[i].shape = PlanShape::kLfe;
        nodes[i].w = style.lfe_width;
        nodes[i].h = style.lfe_height;
        nodes[i].x = area.width / 2 +
                     static_cast<int>(std::lround((lfe_seen - (lfe_total - 1) / 2.0) * style.lfe_pitch));
        nodes[i].y = area.height - style.lfe_height;
        ++lfe_seen;
    }
    return nodes;
}

// The same room in a pane of `width` by `height` pixels: speakers of a size that suits the pane (the screen's 58 pixels in a
// pane the screen's size, a third of that in a quarter of it), the margins and gaps to match, and the low-frequency feeds in
// a strip of their own under the room, so that the rear surrounds of a 7.2 or 9.2 layout never sit on them. Every node stays
// inside the pane. A speaker under about 24 pixels is too small to carry its name, which is the caller's to leave off
// (PlanNode::w says).
[[nodiscard]] inline PlanStyle plan_style_for(int width, int height) {
    PlanStyle style;
    const int ear = std::clamp(std::min(width / 8, height / 9), 8, kPlanEarSize);
    style.ear = ear;
    style.height = std::max(6, ear * 5 / 6);
    style.lfe_width = ear * 3 / 2;
    style.lfe_height = std::max(6, ear * 5 / 6);
    style.margin_x = ear / 2 + 3;
    style.margin_y = ear / 2 + 3;
    style.lfe_pitch = style.lfe_width + ear / 3 + 2;
    style.gap = std::max(1, ear / 10);
    style.step = std::max(1, ear / 14);
    return style;
}

[[nodiscard]] inline std::vector<PlanNode> plan_nodes_fit(std::span<const PlanSpot> spots, int width, int height) {
    if (width <= 0 || height <= 0) {
        return std::vector<PlanNode>(spots.size());
    }
    const PlanStyle style = plan_style_for(width, height);
    std::vector<PlanSpot> ring(spots.begin(), spots.end());
    int feeds = 0;
    for (PlanSpot& spot : ring) {
        if (spot.lfe && spot.connected) {
            spot.connected = false;
            ++feeds;
        }
    }
    const int strip = feeds > 0 ? style.lfe_height + style.gap + 2 : 0;
    std::vector<PlanNode> nodes = plan_nodes(ring, PlanArea{width, std::max(1, height - strip)}, style);
    int placed = 0;
    for (std::size_t i = 0; i < spots.size(); ++i) {
        if (!(spots[i].lfe && spots[i].connected)) {
            continue;
        }
        nodes[i].shape = PlanShape::kLfe;
        nodes[i].w = style.lfe_width;
        nodes[i].h = style.lfe_height;
        nodes[i].x = std::clamp(width / 2 + static_cast<int>(std::lround((placed - (feeds - 1) / 2.0) * style.lfe_pitch)),
                                nodes[i].w / 2, width - nodes[i].w / 2);
        nodes[i].y = height - style.lfe_height / 2 - 1;
        ++placed;
    }
    return nodes;
}

// How an output's measured level shows: a glow in a few steps, not a fade. The board measures every
// 100 ms; a screen that followed each window with a continuous fade redrew 1.7 Mpx a second in a 7.1.4 room
// (nine times the lyric screen's fill), and a glow that jumps with every window is the thing a dim room least
// wants. So the level is smoothed (it rises at once and falls at a rate that lets a beat show), put on a few
// steps, and given a margin before it changes step, and an update that changes no step draws nothing.
//
// A level is judged against the loudest the room has lately been (LevelCeiling), not against full scale. The
// player's volume is applied before the meter, so against full scale the glow shrinks with the volume knob;
// against the programme's own loudest the same music shows the same at any volume
// (planning/p4x-speakers-options.md). Silence is still absolute: an output with nothing on it is hollow.
inline constexpr int kGlowCoreSteps = 6;  // 1 (faintly lit) to 6 (as loud as it shows); 0 is silent
inline constexpr int kGlowHaloSteps = 2;  // 0 (none) to 2, from the peak
inline constexpr float kGlowSilentDb = -80.0F;
// The core's steps span this many dB under 2 dB below the ceiling (a step is 6 dB); the halo's span the same, a
// peak being about 6 dB over its RMS.
inline constexpr float kGlowSpanDb = 30.0F;
inline constexpr float kGlowTopBelowCeilingDb = 2.0F;
inline constexpr float kGlowPeakAboveRmsDb = 6.0F;
// How fast the smoothed level falls: 60 dB a second is 6 dB a window, so a hit that is gone by the next window
// is a step down in the next but one, where the 14 dB a second it had before held the glow up between beats. The
// halo, the peak, falls more slowly and changes less readily, being the bigger and so the dearer redraw.
inline constexpr float kGlowRmsFallDbPerSecond = 60.0F;
inline constexpr float kGlowPeakFallDbPerSecond = 20.0F;
// How far past the edge of a step a level has to go (in steps) before the step changes. Chosen with
// firmware/hearth-sink/preview/speakers_options/tune.cpp over a synthetic groove in 2.0, 5.1 and 7.1.4: 72% of
// onsets of 5 dB or more showed as a step up in the same window (the 5 Hz glow before: 3 to 26%), 3 to 5% of steady
// windows moved a step, and a 5.1 room flushed about 70,000 pixels a second (the lyric screen's song filling: 192,000).
inline constexpr float kGlowCoreMargin = 0.40F;
inline constexpr float kGlowHaloMargin = 0.70F;

// The loudest the room has lately been, in dB of full scale: the loudest RMS of any output, held, falling at
// kCeilingFallDbPerSecond, and never below kCeilingFloorDb (a very quiet passage is not turned up to look loud).
inline constexpr float kCeilingFloorDb = -48.0F;
inline constexpr float kCeilingFallDbPerSecond = 2.0F;

class LevelCeiling {
   public:
    // The loudest RMS of any output in the window that closed `seconds` after the last one.
    void feed(float loudest_db, float seconds) {
        seconds = std::max(seconds, 0.0F);
        ceiling_ = std::max({loudest_db, ceiling_ - kCeilingFallDbPerSecond * seconds, kCeilingFloorDb});
    }
    [[nodiscard]] float db() const { return ceiling_; }
    void reset() { ceiling_ = kCeilingFloorDb; }

   private:
    float ceiling_ = kCeilingFloorDb;
};

namespace detail {

[[nodiscard]] inline float level_unit(float db, float floor_db, float top_db) {
    return std::clamp((db - floor_db) / (top_db - floor_db), 0.0F, 1.0F);
}

// The step the position is on, staying where it is until the position is `margin` of a step past the edge,
// so that a level sitting on a boundary does not flicker between two.
[[nodiscard]] inline int level_hold(int current, float position, float margin = 0.25F) {
    if (current >= 0 && position > static_cast<float>(current) - 0.5F - margin &&
        position < static_cast<float>(current) + 0.5F + margin) {
        return current;
    }
    return static_cast<int>(std::lround(position));
}

}  // namespace detail

class Glow {
   public:
    // One measure of the last 100 ms, in dB of full scale (-120 and below are silence), the seconds since the
    // last call, and the room's ceiling (LevelCeiling::db()). True when a step changed, which is when something
    // has to be redrawn.
    bool update(float rms_db, float peak_db, float seconds, float ceiling_db) {
        seconds = std::max(seconds, 0.0F);
        rms_ = std::max(rms_db, rms_ - kGlowRmsFallDbPerSecond * seconds);
        peak_ = std::max(peak_db, peak_ - kGlowPeakFallDbPerSecond * seconds);
        const float rms_top = ceiling_db - kGlowTopBelowCeilingDb;
        const float peak_top = rms_top + kGlowPeakAboveRmsDb;
        const int core = rms_ > kGlowSilentDb
                             ? 1 + detail::level_hold(core_ - 1,
                                                      detail::level_unit(rms_, rms_top - kGlowSpanDb, rms_top) * (kGlowCoreSteps - 1),
                                                      kGlowCoreMargin)
                             : 0;
        const int halo = rms_ > kGlowSilentDb
                             ? detail::level_hold(halo_, detail::level_unit(peak_, peak_top - kGlowSpanDb, peak_top) * kGlowHaloSteps,
                                                  kGlowHaloMargin)
                             : 0;
        const bool changed = core != core_ || halo != halo_;
        core_ = core;
        halo_ = halo;
        return changed;
    }

    // 0 while the output carries nothing, else 1 to kGlowCoreSteps.
    [[nodiscard]] int core() const { return std::max(core_, 0); }
    // 0 to kGlowHaloSteps.
    [[nodiscard]] int halo() const { return std::max(halo_, 0); }
    [[nodiscard]] bool live() const { return core_ > 0; }
    // What the level the screen is following is, for a test.
    [[nodiscard]] float rms_db() const { return rms_; }

   private:
    float rms_ = -120.0F;
    float peak_ = -120.0F;
    int core_ = -1;
    int halo_ = -1;
};

// The "pulse" style: the filled circle of a speaker grows and shrinks with its level, a little. At its quietest
// a speaker is kPulseReach smaller than the plan draws it and at its loudest kPulseReach larger, in
// kPulseSteps sizes. Never jumpy: the level it follows is smoothed (it rises over a few windows and falls over
// more), a speaker never moves more than kPulseMaxStepsPerWindow sizes in a window, a size is held until the
// level is a quarter of a step past its edge, and a speaker with next to nothing on it stays at its smallest
// and does not move at all.
inline constexpr int kPulseSteps = 8;  // sizes 0 (smallest) to 7 (largest)
inline constexpr float kPulseReach = 0.12F;
inline constexpr int kPulseMaxStepsPerWindow = 3;
// The size follows the level over this many dB under the glow's top (narrower than the glow's, so that a beat of
// 8 dB is half the range and shows).
inline constexpr float kPulseSpanDb = 18.0F;
// The level (0 to 1 over that span) below which a speaker is at its smallest and stays.
inline constexpr float kPulseQuiet = 0.12F;
// Time constants of the smoothed level, seconds: it takes a few windows to rise and about a quarter of a
// second to fall.
inline constexpr float kPulseRiseSeconds = 0.08F;
inline constexpr float kPulseFallSeconds = 0.25F;

class Pulse {
   public:
    // As Glow::update: the RMS of the last window, the seconds since the last call and the room's ceiling.
    // True when the size changed.
    bool update(float rms_db, float seconds, float ceiling_db) {
        seconds = std::max(seconds, 0.0F);
        const float top = ceiling_db - kGlowTopBelowCeilingDb;
        float target = rms_db > kGlowSilentDb ? detail::level_unit(rms_db, top - kPulseSpanDb, top) : 0.0F;
        if (target < kPulseQuiet) {
            target = 0.0F;
        }
        const float tau = target > level_ ? kPulseRiseSeconds : kPulseFallSeconds;
        level_ += (target - level_) * (1.0F - std::exp(-seconds / tau));
        const float wanted = level_ < kPulseQuiet ? 0.0F : level_ * static_cast<float>(kPulseSteps - 1);
        int next = detail::level_hold(step_, wanted);
        next = std::clamp(next, step_ - kPulseMaxStepsPerWindow, step_ + kPulseMaxStepsPerWindow);
        next = std::clamp(next, 0, kPulseSteps - 1);
        const bool changed = next != step_;
        step_ = next;
        return changed;
    }

    // 0 (smallest) to kPulseSteps - 1.
    [[nodiscard]] int step() const { return step_; }
    // What to multiply the plan's size by: 1 - kPulseReach to 1 + kPulseReach.
    [[nodiscard]] float scale() const {
        return 1.0F - kPulseReach + 2.0F * kPulseReach * static_cast<float>(step_) / static_cast<float>(kPulseSteps - 1);
    }
    [[nodiscard]] float level() const { return level_; }

   private:
    float level_ = 0.0F;
    int step_ = 0;
};

}  // namespace iclforge::render
