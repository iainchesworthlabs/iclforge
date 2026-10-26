// The room seen from above, and the glow of a level (iclforge/render/room_plan.hpp), on the host.
//
// What is held here is what a picture cannot say: that the centre is where the centre goes, that no two
// speakers of any layout the grammar can name are drawn on top of each other, that a level sitting on
// the edge of a step does not make the screen flicker, that the show is the same at any volume, and that a
// pulsing speaker never moves much, or jumps, or flickers when it is quiet.

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "iclforge/render/layout.hpp"
#include "iclforge/render/room_plan.hpp"

namespace {

using iclforge::render::Glow;
using iclforge::render::LevelCeiling;
using iclforge::render::OutputLayout;
using iclforge::render::PlanArea;
using iclforge::render::PlanNode;
using iclforge::render::PlanShape;
using iclforge::render::PlanSpot;
using iclforge::render::Pulse;

constexpr PlanArea kArea{1024, 472};

struct Planned {
    std::vector<PlanSpot> spots;
    std::vector<PlanNode> nodes;

    [[nodiscard]] const PlanNode& at(const char* name) const {
        for (std::size_t i = 0; i < spots.size(); ++i) {
            if (spots[i].name == name) {
                return nodes[i];
            }
        }
        FAIL("no slot named " << name);
        return nodes.front();
    }
};

Planned planned(const char* text) {
    const auto layout = OutputLayout::parse(text);
    REQUIRE(layout.has_value());
    Planned out;
    out.spots = iclforge::render::plan_spots(*layout);
    out.nodes = iclforge::render::plan_nodes(out.spots, kArea);
    return out;
}

bool touches(const PlanNode& a, const PlanNode& b) {
    return std::abs(a.x - b.x) < (a.w + b.w) / 2 && std::abs(a.y - b.y) < (a.h + b.h) / 2;
}

}  // namespace

TEST_CASE("plan_spots: one spot a slot, in slot order, with the layout's own names", "[room_plan]") {
    const Planned five = planned("5.1");
    REQUIRE(five.spots.size() == 6);
    const char* const names[] = {"L", "C", "R", "Ls", "Rs", "LFE"};
    for (std::size_t i = 0; i < 6; ++i) {
        CHECK(five.spots[i].name == names[i]);
        CHECK(five.spots[i].connected);
    }
    CHECK(five.spots[5].lfe);
    CHECK_FALSE(five.spots[0].lfe);
    // Left is positive, as the layout says it.
    CHECK(five.spots[0].azimuth_deg > 0.0);
    CHECK(five.spots[2].azimuth_deg < 0.0);
    CHECK(five.spots[1].azimuth_deg == 0.0);
}

TEST_CASE("plan_spots: a slot placed by an angle is named for its place in the bus; '-' is not drawn", "[room_plan]") {
    const Planned listed = planned("30/0,-,-30/0,lfe");
    REQUIRE(listed.spots.size() == 4);
    CHECK(listed.spots[0].name == "S1");
    CHECK_FALSE(listed.spots[1].connected);
    CHECK(listed.nodes[1].shape == PlanShape::kNone);
    CHECK(listed.nodes[0].shape == PlanShape::kEar);
    CHECK(listed.nodes[3].shape == PlanShape::kLfe);
}

TEST_CASE("plan_nodes: two speakers sit side by side in the middle and are not stretched across the screen",
          "[room_plan]") {
    const Planned stereo = planned("2.0");
    const PlanNode& left = stereo.at("L");
    const PlanNode& right = stereo.at("R");
    CHECK(left.x < right.x);
    CHECK(left.y == right.y);
    // Symmetric about the middle, and about as far apart as a stereo pair, not the width of the panel.
    CHECK(left.x + right.x == kArea.width);
    CHECK(right.x - left.x < kArea.width / 2);
    CHECK(left.y > kArea.height / 4);
    CHECK(left.y < kArea.height * 3 / 4);
}

TEST_CASE("plan_nodes: a 5.1 room has the centre at the top, the surrounds behind and the LFE along the bottom",
          "[room_plan]") {
    const Planned five = planned("5.1");
    const PlanNode& c = five.at("C");
    const PlanNode& l = five.at("L");
    const PlanNode& r = five.at("R");
    const PlanNode& ls = five.at("Ls");
    const PlanNode& rs = five.at("Rs");
    const PlanNode& lfe = five.at("LFE");
    CHECK(c.x == kArea.width / 2);
    CHECK(c.y < l.y);
    CHECK(l.y == r.y);
    CHECK(l.x < c.x);
    CHECK(r.x > c.x);
    // The surrounds are behind the listener and outside the fronts.
    CHECK(ls.y > l.y);
    CHECK(rs.y == ls.y);
    CHECK(ls.x < l.x);
    CHECK(rs.x > r.x);
    // Mirrored about the middle.
    CHECK(l.x + r.x == kArea.width);
    CHECK(ls.x + rs.x == kArea.width);
    CHECK(lfe.shape == PlanShape::kLfe);
    CHECK(lfe.x == kArea.width / 2);
    CHECK(lfe.y > ls.y);
    CHECK(five.at("L").shape == PlanShape::kEar);
}

TEST_CASE("plan_nodes: height speakers are squares, drawn inside the ring they sit above", "[room_plan]") {
    const Planned atmos = planned("7.1.4");
    for (const char* name : {"Vhl", "Vhr", "Lts", "Rts"}) {
        CHECK(atmos.at(name).shape == PlanShape::kHeight);
    }
    for (const char* name : {"L", "C", "R", "Ls", "Rs", "Lrs", "Rrs"}) {
        CHECK(atmos.at(name).shape == PlanShape::kEar);
    }
    // The front height is nearer the listener than the front left it is above.
    CHECK(atmos.at("Vhl").y > atmos.at("L").y);
    CHECK(atmos.at("Vhl").x == atmos.at("L").x);
    // The rear heights are in front of the rear surrounds.
    CHECK(atmos.at("Lts").y < atmos.at("Lrs").y);
}

TEST_CASE("plan_nodes: no two speakers of any layout the grammar names are drawn on each other", "[room_plan]") {
    std::size_t checked = 0;
    for (const int front : {1, 2, 3, 4, 5, 7, 9}) {
        for (const int lfe : {0, 1, 2}) {
            for (const int height : {0, 2, 4, 6}) {
              // The same layouts with the heights in the ceiling (":top"): every height speaker is straight overhead.
              for (const bool ceiling : {false, true}) {
                if (ceiling && height == 0) {
                    continue;
                }
                std::string name = std::to_string(front) + "." + std::to_string(lfe);
                if (height != 0) {
                    name += "." + std::to_string(height);
                }
                if (ceiling) {
                    name += ":top";
                }
                const auto layout = OutputLayout::parse(name);
                if (!layout) {
                    continue;  // 9.2.6 has more slots than a bus carries
                }
                ++checked;
                const std::vector<PlanSpot> spots = iclforge::render::plan_spots(*layout);
                const std::vector<PlanNode> nodes = iclforge::render::plan_nodes(spots, kArea);
                REQUIRE(nodes.size() == spots.size());
                for (std::size_t a = 0; a < nodes.size(); ++a) {
                    INFO(name << " slot " << spots[a].name);
                    CHECK(nodes[a].x - nodes[a].w / 2 >= 0);
                    CHECK(nodes[a].x + nodes[a].w / 2 <= kArea.width);
                    CHECK(nodes[a].y - nodes[a].h / 2 >= 0);
                    CHECK(nodes[a].y + nodes[a].h / 2 <= kArea.height);
                    for (std::size_t b = a + 1; b < nodes.size(); ++b) {
                        INFO(name << ": " << spots[a].name << " and " << spots[b].name);
                        CHECK_FALSE(touches(nodes[a], nodes[b]));
                    }
                }
              }
            }
        }
    }
    CHECK(checked >= 20);
}

TEST_CASE("plan_nodes_fit: every layout the grammar names fits a small pane, inside it, with no two speakers on each other",
          "[room_plan]") {
    // The panes the layout sheet draws a room in: the kit's right-hand pane, an 800 by 480's, a 480 by 320's, and the
    // strip at the top of a 240-pixel screen.
    struct Pane {
        int width;
        int height;
    };
    std::size_t checked = 0;
    for (const Pane pane : {Pane{345, 424}, Pane{400, 360}, Pane{270, 300}, Pane{300, 200}, Pane{161, 170}, Pane{220, 100},
                            Pane{640, 560}}) {
        for (const int front : {1, 2, 3, 4, 5, 7, 9}) {
            for (const int lfe : {0, 1, 2}) {
                for (const int height : {0, 2, 4, 6}) {
                  for (const bool ceiling : {false, true}) {
                    if (ceiling && height == 0) {
                        continue;
                    }
                    std::string name = std::to_string(front) + "." + std::to_string(lfe);
                    if (height != 0) {
                        name += "." + std::to_string(height);
                    }
                    if (ceiling) {
                        name += ":top";
                    }
                    const auto layout = OutputLayout::parse(name);
                    if (!layout) {
                        continue;
                    }
                    ++checked;
                    const std::vector<PlanSpot> spots = iclforge::render::plan_spots(*layout);
                    const std::vector<PlanNode> nodes = iclforge::render::plan_nodes_fit(spots, pane.width, pane.height);
                    REQUIRE(nodes.size() == spots.size());
                    for (std::size_t a = 0; a < nodes.size(); ++a) {
                        INFO(name << " in " << pane.width << "x" << pane.height << " slot " << spots[a].name);
                        CHECK(nodes[a].x - nodes[a].w / 2 >= 0);
                        CHECK(nodes[a].x + nodes[a].w / 2 <= pane.width);
                        CHECK(nodes[a].y - nodes[a].h / 2 >= 0);
                        CHECK(nodes[a].y + nodes[a].h / 2 <= pane.height);
                        for (std::size_t b = a + 1; b < nodes.size(); ++b) {
                            INFO(name << ": " << spots[a].name << " and " << spots[b].name);
                            CHECK_FALSE(touches(nodes[a], nodes[b]));
                        }
                    }
                  }
                }
            }
        }
    }
    CHECK(checked >= 160);
}

TEST_CASE("plan_nodes_fit: a bigger pane has bigger speakers, and an empty pane is an empty room", "[room_plan]") {
    const auto layout = OutputLayout::parse("5.1");
    REQUIRE(layout.has_value());
    const std::vector<PlanSpot> spots = iclforge::render::plan_spots(*layout);
    const std::vector<PlanNode> big = iclforge::render::plan_nodes_fit(spots, 400, 360);
    const std::vector<PlanNode> small = iclforge::render::plan_nodes_fit(spots, 200, 180);
    REQUIRE(big.size() == small.size());
    for (std::size_t i = 0; i < big.size(); ++i) {
        INFO(spots[i].name);
        CHECK(big[i].w > small[i].w);
    }
    // The screen's own area and style are what they were: 58-pixel speakers.
    CHECK(planned("5.1").at("C").w == iclforge::render::kPlanEarSize);
    for (const PlanNode& node : iclforge::render::plan_nodes_fit(spots, 0, 100)) {
        CHECK(node.shape == PlanShape::kNone);
    }
}

TEST_CASE("plan_nodes: slots placed at the same angle are drawn apart, not on each other", "[room_plan]") {
    // A speaker list may put two slots on one spot (a spare, or two small speakers in a corner).
    const Planned stacked = planned("30/0,30/0,-30/0,0/0,0/45");
    REQUIRE(stacked.nodes.size() == 5);
    for (std::size_t a = 0; a < stacked.nodes.size(); ++a) {
        for (std::size_t b = a + 1; b < stacked.nodes.size(); ++b) {
            INFO("slots " << a << " and " << b);
            CHECK_FALSE(touches(stacked.nodes[a], stacked.nodes[b]));
        }
    }
}

TEST_CASE("plan_nodes: the same layout draws the same picture", "[room_plan]") {
    const Planned first = planned("9.1.6");
    const Planned second = planned("9.1.6");
    REQUIRE(first.nodes.size() == second.nodes.size());
    for (std::size_t i = 0; i < first.nodes.size(); ++i) {
        CHECK(first.nodes[i].x == second.nodes[i].x);
        CHECK(first.nodes[i].y == second.nodes[i].y);
    }
}

TEST_CASE("plan_nodes: no speakers at all, or only an LFE, is an empty room and not a crash", "[room_plan]") {
    CHECK(iclforge::render::plan_nodes({}, kArea).empty());
    const std::vector<PlanSpot> only_lfe{{.name = "LFE", .lfe = true}};
    const auto nodes = iclforge::render::plan_nodes(only_lfe, kArea);
    REQUIRE(nodes.size() == 1);
    CHECK(nodes[0].shape == PlanShape::kLfe);
}

TEST_CASE("Glow: silence is step 0 and a loud output is the top step at once", "[room_plan][glow]") {
    Glow glow;
    constexpr float kCeiling = -14.0F;
    CHECK(glow.update(-120.0F, -120.0F, 0.1F, kCeiling) == true);  // the first update settles the first step
    CHECK_FALSE(glow.live());
    CHECK(glow.core() == 0);
    CHECK(glow.halo() == 0);
    CHECK(glow.update(-14.0F, -8.0F, 0.1F, kCeiling));
    CHECK(glow.live());
    CHECK(glow.core() == iclforge::render::kGlowCoreSteps);
    CHECK(glow.halo() == iclforge::render::kGlowHaloSteps);
}

TEST_CASE("Glow: an unchanged level changes no step, so nothing is redrawn", "[room_plan][glow]") {
    Glow glow;
    glow.update(-30.0F, -20.0F, 0.1F, -14.0F);
    for (int i = 0; i < 20; ++i) {
        CHECK_FALSE(glow.update(-30.0F, -20.0F, 0.1F, -14.0F));
    }
}

TEST_CASE("Glow: it falls fast enough for a beat to show, and goes dark after a couple of seconds of silence",
          "[room_plan][glow]") {
    Glow glow;
    glow.update(-14.0F, -8.0F, 0.1F, -14.0F);
    REQUIRE(glow.core() == iclforge::render::kGlowCoreSteps);
    // Each window of silence takes 6 dB off, a step: a hit that is gone is a step down within two windows.
    glow.update(-120.0F, -120.0F, 0.1F, -14.0F);
    glow.update(-120.0F, -120.0F, 0.1F, -14.0F);
    CHECK(glow.live());
    CHECK(glow.core() < iclforge::render::kGlowCoreSteps);
    // Half a second is 30 dB: still faintly lit, not yet dark.
    for (int i = 0; i < 3; ++i) {
        glow.update(-120.0F, -120.0F, 0.1F, -14.0F);
    }
    CHECK(glow.live());
    // Two seconds is past the -80 dB where the output is silent: dark.
    for (int i = 0; i < 15; ++i) {
        glow.update(-120.0F, -120.0F, 0.1F, -14.0F);
    }
    CHECK_FALSE(glow.live());
    CHECK(glow.core() == 0);
}

TEST_CASE("Glow: steps are monotonic in level", "[room_plan][glow]") {
    int last = 0;
    int last_halo = 0;
    for (float db = -70.0F; db <= -10.0F; db += 1.0F) {
        Glow glow;
        glow.update(db, db + 6.0F, 0.1F, -14.0F);
        CHECK(glow.core() >= last);
        CHECK(glow.halo() >= last_halo);
        last = glow.core();
        last_halo = glow.halo();
    }
    CHECK(last == iclforge::render::kGlowCoreSteps);
    CHECK(last_halo == iclforge::render::kGlowHaloSteps);
}

TEST_CASE("Glow: a level on the edge of a step does not make it flicker", "[room_plan][glow]") {
    // With the ceiling at -14 dB the core's span is -46 to -16 dB, 6 dB a step: the edge between the second and
    // third lit steps is 1.5 steps up, at -37 dB. A level seen 1.5 dB either side of it, window after window (the
    // output falls 3 dB between two windows and the next one lifts it again), is a quarter of a step either way.
    Glow glow;
    for (int i = 0; i < 6; ++i) {
        glow.update(i % 2 == 0 ? -35.5F : -38.5F, -120.0F, 0.1F, -14.0F);
    }
    int changes = 0;
    for (int i = 0; i < 40; ++i) {
        changes += glow.update(i % 2 == 0 ? -35.5F : -38.5F, -120.0F, 0.1F, -14.0F) ? 1 : 0;
    }
    CHECK(changes == 0);
}

TEST_CASE("LevelCeiling: the loudest lately, rising at once and falling slowly, never below its floor", "[room_plan][glow]") {
    LevelCeiling ceiling;
    CHECK(ceiling.db() == iclforge::render::kCeilingFloorDb);
    ceiling.feed(-12.0F, 0.1F);
    CHECK(ceiling.db() == -12.0F);
    // A quieter window does not lower it at once: 2 dB a second.
    ceiling.feed(-30.0F, 0.1F);
    CHECK(ceiling.db() > -12.5F);
    for (int i = 0; i < 50; ++i) {
        ceiling.feed(-30.0F, 0.1F);
    }
    CHECK(ceiling.db() > -23.0F);  // five seconds is 10 dB from -12: still well up
    for (int i = 0; i < 60; ++i) {
        ceiling.feed(-30.0F, 0.1F);
    }
    CHECK(ceiling.db() == -30.0F);  // ten seconds is 20 dB: down to the level itself
    // Silence takes it to its floor and no further: a very quiet passage is not turned up to look loud.
    for (int i = 0; i < 2000; ++i) {
        ceiling.feed(-120.0F, 0.1F);
    }
    CHECK(ceiling.db() == iclforge::render::kCeilingFloorDb);
}

TEST_CASE("Glow: the same music shows the same at any volume", "[room_plan][glow]") {
    // Three outputs of a programme, as the board measures them, at full volume and 24 dB down: the ceiling
    // follows the loudest, so each output is on the same step at the end of the same few windows.
    const float rms[3] = {-13.0F, -21.0F, -30.0F};
    const float peak[3] = {-6.0F, -14.0F, -25.0F};
    const auto run = [&](float offset_db) {
        LevelCeiling ceiling;
        Glow glow[3];
        for (int w = 0; w < 30; ++w) {
            ceiling.feed(rms[0] + offset_db, 0.1F);
            for (int o = 0; o < 3; ++o) {
                glow[o].update(rms[o] + offset_db, peak[o] + offset_db, 0.1F, ceiling.db());
            }
        }
        return std::vector<int>{glow[0].core(), glow[1].core(), glow[2].core(), glow[0].halo(), glow[1].halo(), glow[2].halo()};
    };
    const std::vector<int> loud = run(0.0F);
    CHECK(run(-12.0F) == loud);
    CHECK(run(-24.0F) == loud);
    // And the room still has its balance: the loudest output is brighter than the quietest.
    CHECK(loud[0] > loud[2]);
}

TEST_CASE("Pulse: a silent speaker is at its smallest and never moves", "[room_plan][pulse]") {
    Pulse pulse;
    for (int i = 0; i < 50; ++i) {
        CHECK_FALSE(pulse.update(-120.0F, 0.1F, -14.0F));
    }
    CHECK(pulse.step() == 0);
    CHECK(pulse.scale() == Catch::Approx(1.0F - iclforge::render::kPulseReach));
}

TEST_CASE("Pulse: a speaker next to nothing on it stays at its smallest and does not flicker", "[room_plan][pulse]") {
    // 40 dB under the ceiling, in and out of the noise: below the quiet level, so no size change at all.
    Pulse pulse;
    int changes = 0;
    for (int i = 0; i < 100; ++i) {
        changes += pulse.update(i % 2 == 0 ? -52.0F : -58.0F, 0.1F, -14.0F) ? 1 : 0;
    }
    CHECK(changes == 0);
    CHECK(pulse.step() == 0);
}

TEST_CASE("Pulse: a loud speaker grows to its largest, by no more than a few sizes a window", "[room_plan][pulse]") {
    Pulse pulse;
    int largest_jump = 0;
    int last = pulse.step();
    for (int i = 0; i < 30; ++i) {
        pulse.update(-14.0F, 0.1F, -14.0F);
        largest_jump = std::max(largest_jump, std::abs(pulse.step() - last));
        last = pulse.step();
    }
    CHECK(largest_jump <= iclforge::render::kPulseMaxStepsPerWindow);
    CHECK(pulse.step() == iclforge::render::kPulseSteps - 1);
    CHECK(pulse.scale() == Catch::Approx(1.0F + iclforge::render::kPulseReach));
    // And then it falls back, more slowly than it rose.
    int windows_to_rise = 0;
    {
        Pulse again;
        while (again.step() < iclforge::render::kPulseSteps - 1 && windows_to_rise < 100) {
            again.update(-14.0F, 0.1F, -14.0F);
            ++windows_to_rise;
        }
    }
    int windows_to_fall = 0;
    while (pulse.step() > 0 && windows_to_fall < 100) {
        pulse.update(-120.0F, 0.1F, -14.0F);
        ++windows_to_fall;
    }
    CHECK(pulse.step() == 0);
    CHECK(windows_to_fall > windows_to_rise);
}

TEST_CASE("Pulse: steady music moves it only a little, and an unchanged level changes nothing", "[room_plan][pulse]") {
    Pulse pulse;
    for (int i = 0; i < 30; ++i) {
        pulse.update(-22.0F, 0.1F, -14.0F);
    }
    for (int i = 0; i < 40; ++i) {
        CHECK_FALSE(pulse.update(-22.0F, 0.1F, -14.0F));
    }
    // A beat of 6 dB every half second: the size follows it by a size or two, not the whole range.
    Pulse beat;
    int low = iclforge::render::kPulseSteps;
    int high = 0;
    for (int i = 0; i < 100; ++i) {
        beat.update(i % 5 == 0 ? -16.0F : -24.0F, 0.1F, -14.0F);
        if (i > 30) {
            low = std::min(low, beat.step());
            high = std::max(high, beat.step());
        }
    }
    CHECK(high - low <= 3);
    CHECK(high - low >= 1);
}

TEST_CASE("Pulse: sizes are monotonic in the level", "[room_plan][pulse]") {
    int last = 0;
    for (float db = -60.0F; db <= -10.0F; db += 1.0F) {
        Pulse pulse;
        for (int i = 0; i < 40; ++i) {
            pulse.update(db, 0.1F, -14.0F);
        }
        CHECK(pulse.step() >= last);
        last = pulse.step();
    }
    CHECK(last == iclforge::render::kPulseSteps - 1);
}
