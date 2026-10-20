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

namespace detail {

[[nodiscard]] inline bool plan_overlap(const PlanNode& a, const PlanNode& b, int gap) {
    return std::abs(a.x - b.x) < (a.w + b.w) / 2 + gap && std::abs(a.y - b.y) < (a.h + b.h) / 2 + gap;
}

}  // namespace detail

// One node a slot, in slot order. Nodes that would touch are pushed apart (a 9.1.6 room has a ceiling
// speaker over the listener and a front height next to the centre), and every node stays inside the area.
[[nodiscard]] inline std::vector<PlanNode> plan_nodes(std::span<const PlanSpot> spots, PlanArea area = {}) {
    constexpr double kDegrees = 3.14159265358979323846 / 180.0;
    std::vector<PlanNode> nodes(spots.size());
    const int margin_x = 170;
    const int margin_y = 62;
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
    for (std::size_t i = 0; i < spots.size(); ++i) {
        if (!spots[i].connected || spots[i].lfe) {
            continue;
        }
        const double ring = std::cos(spots[i].elevation_deg * kDegrees);
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
            nodes[i].w = nodes[i].h = height ? kPlanHeightSize : kPlanEarSize;
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
                    if (!detail::plan_overlap(first, second, 6)) {
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
                    first.x -= static_cast<int>(std::lround(dx * 4.0));
                    first.y -= static_cast<int>(std::lround(dy * 4.0));
                    second.x += static_cast<int>(std::lround(dx * 4.0));
                    second.y += static_cast<int>(std::lround(dy * 4.0));
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
        nodes[i].w = kPlanLfeWidth;
        nodes[i].h = kPlanLfeHeight;
        nodes[i].x = area.width / 2 + static_cast<int>(std::lround((lfe_seen - (lfe_total - 1) / 2.0) * 110.0));
        nodes[i].y = area.height - kPlanLfeHeight;
        ++lfe_seen;
    }
    return nodes;
}

// How an output's measured level shows: a glow in a few steps, not a fade. The board measures every
// 100 ms; a screen that followed each window would redraw every speaker five or ten times a second
// (a continuous fade redrew 1.7 Mpx a second in a 7.1.4 room, nine times the lyric screen's fill), and
// a glow that jumps with every window is the thing a dim room least wants. So the level is smoothed
// (it rises at once and falls slowly), quantised, and given a margin before it changes step, and an
// update that changes no step draws nothing.
inline constexpr int kGlowCoreSteps = 5;  // 1 (faintly lit) to 5 (as loud as it shows); 0 is silent
inline constexpr int kGlowHaloSteps = 3;  // 0 (none) to 3, from the peak
inline constexpr float kGlowSilentDb = -80.0F;
inline constexpr float kGlowRmsFloorDb = -62.0F;
inline constexpr float kGlowRmsTopDb = -14.0F;
inline constexpr float kGlowPeakFloorDb = -50.0F;
inline constexpr float kGlowPeakTopDb = -8.0F;
inline constexpr float kGlowRmsFallDbPerSecond = 14.0F;
inline constexpr float kGlowPeakFallDbPerSecond = 30.0F;

class Glow {
   public:
    // One measure of the last 100 ms, in dB of full scale (-120 and below are silence), and the seconds
    // since the last call. True when a step changed, which is when something has to be redrawn.
    bool update(float rms_db, float peak_db, float seconds) {
        seconds = std::max(seconds, 0.0F);
        rms_ = std::max(rms_db, rms_ - kGlowRmsFallDbPerSecond * seconds);
        peak_ = std::max(peak_db, peak_ - kGlowPeakFallDbPerSecond * seconds);
        const int core = rms_ > kGlowSilentDb
                             ? 1 + hold(core_ - 1, unit(rms_, kGlowRmsFloorDb, kGlowRmsTopDb) * (kGlowCoreSteps - 1))
                             : 0;
        const int halo = rms_ > kGlowSilentDb ? hold(halo_, unit(peak_, kGlowPeakFloorDb, kGlowPeakTopDb) * kGlowHaloSteps) : 0;
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
    [[nodiscard]] static float unit(float db, float floor_db, float top_db) {
        return std::clamp((db - floor_db) / (top_db - floor_db), 0.0F, 1.0F);
    }
    // The step the position is on, staying where it is until the position is a quarter of a step past
    // the edge, so that a level sitting on a boundary does not flicker between two.
    [[nodiscard]] static int hold(int current, float position) {
        constexpr float kMargin = 0.25F;
        if (current >= 0 && position > static_cast<float>(current) - 0.5F - kMargin &&
            position < static_cast<float>(current) + 0.5F + kMargin) {
            return current;
        }
        return static_cast<int>(std::lround(position));
    }

    float rms_ = -120.0F;
    float peak_ = -120.0F;
    int core_ = -1;
    int halo_ = -1;
};

}  // namespace iclforge::render
