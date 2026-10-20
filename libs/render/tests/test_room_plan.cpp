// The room seen from above, and the glow of a level (iclforge/render/room_plan.hpp), on the host.
//
// What is held here is what a picture cannot say: that the centre is where the centre goes, that no two
// speakers of any layout the grammar can name are drawn on top of each other, and that a level sitting on
// the edge of a step does not make the screen flicker.

#include <algorithm>
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
using iclforge::render::OutputLayout;
using iclforge::render::PlanArea;
using iclforge::render::PlanNode;
using iclforge::render::PlanShape;
using iclforge::render::PlanSpot;

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
                std::string name = std::to_string(front) + "." + std::to_string(lfe);
                if (height != 0) {
                    name += "." + std::to_string(height);
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
    CHECK(checked >= 20);
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
    CHECK(glow.update(-120.0F, -120.0F, 0.2F) == true);  // the first update settles the first step
    CHECK_FALSE(glow.live());
    CHECK(glow.core() == 0);
    CHECK(glow.halo() == 0);
    CHECK(glow.update(-10.0F, -4.0F, 0.2F));
    CHECK(glow.live());
    CHECK(glow.core() == iclforge::render::kGlowCoreSteps);
    CHECK(glow.halo() == iclforge::render::kGlowHaloSteps);
}

TEST_CASE("Glow: an unchanged level changes no step, so nothing is redrawn", "[room_plan][glow]") {
    Glow glow;
    glow.update(-30.0F, -20.0F, 0.2F);
    for (int i = 0; i < 20; ++i) {
        CHECK_FALSE(glow.update(-30.0F, -20.0F, 0.2F));
    }
}

TEST_CASE("Glow: it falls slowly and goes dark only after a few seconds of silence", "[room_plan][glow]") {
    Glow glow;
    glow.update(-14.0F, -8.0F, 0.2F);
    REQUIRE(glow.core() == iclforge::render::kGlowCoreSteps);
    // A second of silence is not yet dark: 14 dB a second from -14 is -28, still well lit.
    for (int i = 0; i < 5; ++i) {
        glow.update(-120.0F, -120.0F, 0.2F);
    }
    CHECK(glow.live());
    CHECK(glow.core() < iclforge::render::kGlowCoreSteps);
    // Seven seconds is -112 dB: dark.
    for (int i = 0; i < 40; ++i) {
        glow.update(-120.0F, -120.0F, 0.2F);
    }
    CHECK_FALSE(glow.live());
    CHECK(glow.core() == 0);
}

TEST_CASE("Glow: steps are monotonic in level", "[room_plan][glow]") {
    int last = 0;
    for (float db = -70.0F; db <= -10.0F; db += 2.0F) {
        Glow glow;
        glow.update(db, db + 6.0F, 0.2F);
        CHECK(glow.core() >= last);
        last = glow.core();
    }
    CHECK(last == iclforge::render::kGlowCoreSteps);
}

TEST_CASE("Glow: a level on the edge of a step does not make it flicker", "[room_plan][glow]") {
    // The edge between the second and third lit steps is 1.5 steps up the 48 dB, 12 dB a step: -44 dB.
    // A level that is seen 1.5 dB either side of it, window after window (the output falls 2.8 dB between
    // two windows and the next one lifts it again), is a quarter of a step either way.
    Glow glow;
    for (int i = 0; i < 6; ++i) {
        glow.update(i % 2 == 0 ? -42.5F : -45.5F, -120.0F, 0.2F);
    }
    int changes = 0;
    for (int i = 0; i < 40; ++i) {
        changes += glow.update(i % 2 == 0 ? -42.5F : -45.5F, -120.0F, 0.2F) ? 1 : 0;
    }
    CHECK(changes == 0);
}
