// What an object's metadata says about HOW it may be rendered - its zone
// constraints, channel lock and extent - and the layout renderer's answer.
//
// TS 103 420 §4.3 leaves the algorithm to the renderer but §5.2 fixes what
// each property MEANS, and Table A.7 says which zones each speaker is in. So
// what is checked here is the meaning: a locked object is one speaker, an
// excluded zone gets none of the object, a sized object is more speakers at
// the same level. The numbers are the panner's (test_spatial.cpp has its
// geometry); an object that says nothing renders as it always did, which
// test_layout.cpp's placement tests keep pinning.

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>

#include "iclforge/objects/oamd.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/render/render.hpp"
#include "iclforge/render/spatial.hpp"

namespace {

using iclforge::objects::oba::DisplayObject;
using iclforge::objects::oba::ZoneConstraint;
using iclforge::render::LayoutRenderer;
using iclforge::render::OutputLayout;
using Catch::Approx;
using Location = iclforge::base::Location;

DisplayObject object_at(double x, double y, double z) {
    DisplayObject object;
    object.position = {.x = x, .y = y, .z = z};
    return object;
}

// The gains the renderer settles on for one object, every slot of the layout.
std::array<float, OutputLayout::kMaxSlots> gains_of(const OutputLayout& layout,
                                                    const DisplayObject& object) {
    LayoutRenderer renderer{layout};
    renderer.set_objects(std::array<DisplayObject, 1>{object});
    std::array<float, OutputLayout::kMaxSlots> out{};
    for (std::size_t slot = 0; slot < layout.slots(); ++slot) {
        out[slot] = renderer.object_gain(0, slot);
    }
    return out;
}

double power(const std::array<float, OutputLayout::kMaxSlots>& gains) {
    double sum = 0.0;
    for (const float g : gains) {
        sum += static_cast<double>(g) * static_cast<double>(g);
    }
    return sum;
}

int slot_of(const OutputLayout& layout, Location location) {
    return layout.index_of(location);
}

// A room-anchored position behind the listener: the back wall's centre.
constexpr double kBackX = 0.5;
constexpr double kBackY = 1.0;

}  // namespace

TEST_CASE("an object with no constraint and no extent renders as it always did",
          "[render][object][constraints]") {
    const auto layout = OutputLayout::parse("5.1.4");
    REQUIRE(layout.has_value());
    // Spelled out: the defaults are no zone, elevation allowed, a point, no snap.
    DisplayObject plain = object_at(0.2, 0.3, 0.4);
    DisplayObject explicit_defaults = plain;
    explicit_defaults.zone = ZoneConstraint::kNone;
    explicit_defaults.enable_elevation = true;
    explicit_defaults.snap = false;
    explicit_defaults.size = {};
    CHECK(gains_of(*layout, plain) == gains_of(*layout, explicit_defaults));
    CHECK(power(gains_of(*layout, plain)) == Approx(1.0).margin(1e-6));
}

TEST_CASE("channel lock puts the object on the one nearest speaker", "[render][object][constraints]") {
    const auto layout = OutputLayout::parse("5.1");
    REQUIRE(layout.has_value());

    SECTION("between L and C it goes to whichever is nearer, at unit gain") {
        // Left of centre, towards the left speaker: x 0.3 on the front wall.
        DisplayObject object = object_at(0.3, 0.0, 0.0);
        const auto panned = gains_of(*layout, object);
        // Panned it is shared between L and C.
        CHECK(panned[static_cast<std::size_t>(slot_of(*layout, Location::kLeft))] > 0.1F);
        CHECK(panned[static_cast<std::size_t>(slot_of(*layout, Location::kCentre))] > 0.1F);

        object.snap = true;
        const auto locked = gains_of(*layout, object);
        std::size_t nonzero = 0;
        for (const float g : locked) {
            if (g > 0.0F) {
                ++nonzero;
            }
        }
        CHECK(nonzero == 1);
        CHECK(power(locked) == Approx(1.0));
        const float left = locked[static_cast<std::size_t>(slot_of(*layout, Location::kLeft))];
        const float centre = locked[static_cast<std::size_t>(slot_of(*layout, Location::kCentre))];
        // x 0.3 is nearer L (at 30 degrees) than C: the direction is about 22 degrees left.
        CHECK(left == Approx(1.0F));
        CHECK(centre == 0.0F);
    }

    SECTION("an object on a speaker's own position snaps to that speaker") {
        DisplayObject object = object_at(0.5, 0.0, 0.0);  // front centre
        object.snap = true;
        const auto locked = gains_of(*layout, object);
        CHECK(locked[static_cast<std::size_t>(slot_of(*layout, Location::kCentre))] == Approx(1.0F));
        CHECK(power(locked) == Approx(1.0));
    }

    SECTION("channel lock wins over extent: a wide locked object is still one speaker") {
        DisplayObject object = object_at(0.5, 0.0, 0.0);
        object.size = {.width = 1.0, .depth = 0.0, .height = 0.0};
        object.snap = true;
        const auto locked = gains_of(*layout, object);
        std::size_t nonzero = 0;
        for (const float g : locked) {
            if (g > 0.0F) {
                ++nonzero;
            }
        }
        CHECK(nonzero == 1);
    }

    SECTION("the LFE is never the nearest speaker") {
        DisplayObject object = object_at(0.5, 0.0, 0.0);
        object.snap = true;
        const auto locked = gains_of(*layout, object);
        CHECK(locked[static_cast<std::size_t>(slot_of(*layout, Location::kLfe))] == 0.0F);
    }
}

TEST_CASE("zone constraints take the excluded zones' speakers out of the object's panning",
          "[render][object][constraints]") {
    const auto layout = OutputLayout::parse("5.1");
    REQUIRE(layout.has_value());
    const auto at = [&](const std::array<float, OutputLayout::kMaxSlots>& g, Location location) {
        return g[static_cast<std::size_t>(slot_of(*layout, location))];
    };

    SECTION("screen only: an object behind the listener is played from the front") {
        DisplayObject object = object_at(kBackX, kBackY, 0.0);
        const auto free = gains_of(*layout, object);
        // Free, it is in the surrounds (Table A.7 puts Ls/Rs in the back zone of a 5.1).
        CHECK(at(free, Location::kLeftSurround) > 0.1F);

        object.zone = ZoneConstraint::kScreenOnly;
        const auto held = gains_of(*layout, object);
        CHECK(at(held, Location::kLeftSurround) == 0.0F);
        CHECK(at(held, Location::kRightSurround) == 0.0F);
        // Not quieter - moved: still unit power, over the screen speakers.
        CHECK(power(held) == Approx(1.0).margin(1e-6));
        CHECK(at(held, Location::kLeft) + at(held, Location::kCentre) + at(held, Location::kRight) >
              0.0F);
    }

    SECTION("surround only: an object at the front is played from the surrounds") {
        DisplayObject object = object_at(0.5, 0.0, 0.0);
        object.zone = ZoneConstraint::kSurroundOnly;
        const auto held = gains_of(*layout, object);
        CHECK(at(held, Location::kCentre) == 0.0F);
        CHECK(at(held, Location::kLeft) == 0.0F);
        CHECK(at(held, Location::kLeftSurround) + at(held, Location::kRightSurround) > 0.0F);
        CHECK(power(held) == Approx(1.0).margin(1e-6));
    }

    SECTION("back excluded keeps the object off a 5.1's surrounds, which are its back zone") {
        DisplayObject object = object_at(kBackX, kBackY, 0.0);
        object.zone = ZoneConstraint::kBackExcluded;
        const auto held = gains_of(*layout, object);
        CHECK(at(held, Location::kLeftSurround) == 0.0F);
        CHECK(at(held, Location::kRightSurround) == 0.0F);
        CHECK(power(held) == Approx(1.0).margin(1e-6));
    }

    SECTION("centre-and-back only: the centre joins the back, the front pair is out") {
        DisplayObject object = object_at(0.3, 0.0, 0.0);  // front left
        object.zone = ZoneConstraint::kCentreAndBackOnly;
        const auto held = gains_of(*layout, object);
        CHECK(at(held, Location::kLeft) == 0.0F);
        CHECK(at(held, Location::kRight) == 0.0F);
        CHECK(at(held, Location::kCentre) > 0.0F);
        CHECK(power(held) == Approx(1.0).margin(1e-6));
    }
}

TEST_CASE("side excluded and a surround pair's zone depend on the layout it is in",
          "[render][object][constraints]") {
    // Table A.7 note 2: Ls/Rs are the back zone in a 5.X layout and the side
    // zone in a larger one. So the same constraint takes them out in the
    // 7.1 and leaves them in the 5.1.
    const auto five = OutputLayout::parse("5.1");
    const auto seven = OutputLayout::parse("7.1");
    REQUIRE(five.has_value());
    REQUIRE(seven.has_value());

    DisplayObject object = object_at(0.0, 0.5, 0.0);  // the left wall's middle
    object.zone = ZoneConstraint::kSideExcluded;

    const auto in_five = gains_of(*five, object);
    CHECK(in_five[static_cast<std::size_t>(slot_of(*five, Location::kLeftSurround))] > 0.0F);

    const auto in_seven = gains_of(*seven, object);
    CHECK(in_seven[static_cast<std::size_t>(slot_of(*seven, Location::kLeftSurround))] == 0.0F);
    CHECK(in_seven[static_cast<std::size_t>(slot_of(*seven, Location::kRightSurround))] == 0.0F);
    // The rear pair is the back zone, which is not excluded.
    CHECK(in_seven[static_cast<std::size_t>(slot_of(*seven, Location::kLrs))] > 0.0F);
    CHECK(power(in_seven) == Approx(1.0).margin(1e-6));
}

TEST_CASE("disabling elevation keeps an object off the height speakers",
          "[render][object][constraints]") {
    const auto layout = OutputLayout::parse("5.1.4");
    REQUIRE(layout.has_value());
    const auto heights = [&](const std::array<float, OutputLayout::kMaxSlots>& g) {
        float sum = 0.0F;
        for (const Location location :
             {Location::kVhl, Location::kVhr, Location::kLts, Location::kRts}) {
            sum += g[static_cast<std::size_t>(slot_of(*layout, location))];
        }
        return sum;
    };

    DisplayObject object = object_at(0.5, 0.5, 1.0);  // the ceiling's centre
    CHECK(heights(gains_of(*layout, object)) > 0.5F);

    object.enable_elevation = false;
    const auto flat = gains_of(*layout, object);
    CHECK(heights(flat) == 0.0F);
    // And the object is somewhere: on the ring, at full power.
    CHECK(power(flat) == Approx(1.0).margin(1e-6));
}

TEST_CASE("a constraint the layout cannot honour plays the object unconstrained",
          "[render][object][constraints]") {
    // Stereo has no surround speaker: "surround only" would otherwise be silence.
    const auto layout = OutputLayout::parse("2.0");
    REQUIRE(layout.has_value());
    DisplayObject object = object_at(0.0, 0.0, 0.0);
    const auto free = gains_of(*layout, object);
    object.zone = ZoneConstraint::kSurroundOnly;
    const auto held = gains_of(*layout, object);
    CHECK(held == free);
    CHECK(power(held) == Approx(1.0).margin(1e-6));
}

TEST_CASE("a sized object is spread over more speakers at the same level",
          "[render][object][constraints]") {
    const auto layout = OutputLayout::parse("5.1");
    REQUIRE(layout.has_value());
    const auto at = [&](const std::array<float, OutputLayout::kMaxSlots>& g, Location location) {
        return g[static_cast<std::size_t>(slot_of(*layout, location))];
    };

    DisplayObject point = object_at(0.5, 0.0, 0.0);  // the front wall's centre
    const auto pointed = gains_of(*layout, point);
    CHECK(at(pointed, Location::kCentre) == Approx(1.0F));
    CHECK(at(pointed, Location::kLeft) == 0.0F);

    DisplayObject wide = point;
    wide.size = {.width = 1.0, .depth = 0.0, .height = 0.0};
    const auto spread = gains_of(*layout, wide);
    // Spread: the centre gives some up to the pair either side...
    CHECK(at(spread, Location::kCentre) < 0.95F);
    CHECK(at(spread, Location::kLeft) > 0.1F);
    CHECK(at(spread, Location::kRight) > 0.1F);
    // ...symmetrically, since it is centred...
    CHECK(at(spread, Location::kLeft) == Approx(at(spread, Location::kRight)).margin(1e-5));
    // ...and at the same total level, not louder.
    CHECK(power(spread) == Approx(1.0).margin(1e-5));

    SECTION("a larger object is more spread than a smaller one") {
        DisplayObject small = point;
        small.size = {.width = 0.3, .depth = 0.0, .height = 0.0};
        CHECK(at(gains_of(*layout, small), Location::kCentre) >
              at(spread, Location::kCentre));
    }

    SECTION("an extent along every axis is still one unit of power") {
        DisplayObject cube = object_at(0.5, 0.5, 0.0);
        cube.size = {.width = 0.8, .depth = 0.8, .height = 0.8};
        CHECK(power(gains_of(*layout, cube)) == Approx(1.0).margin(1e-5));
    }

    SECTION("extent and a zone constraint together stay inside the zone") {
        DisplayObject object = wide;
        object.zone = ZoneConstraint::kSurroundOnly;
        const auto held = gains_of(*layout, object);
        CHECK(at(held, Location::kLeft) == 0.0F);
        CHECK(at(held, Location::kCentre) == 0.0F);
        CHECK(at(held, Location::kRight) == 0.0F);
        CHECK(power(held) == Approx(1.0).margin(1e-5));
    }
}

TEST_CASE("Table A.7's zones of each speaker, and Tables 20 and 21's admissions",
          "[render][object][constraints][zones]") {
    namespace sp = iclforge::spatial;
    using sp::zone_bit::kBack;
    using sp::zone_bit::kCentre;
    using sp::zone_bit::kScreen;
    using sp::zone_bit::kSide;
    using sp::zone_bit::kSurround;
    using sp::zone_bit::kTopBottom;

    CHECK(sp::speaker_zones(Location::kLeft, false) == kScreen);
    CHECK(sp::speaker_zones(Location::kCentre, true) == (kScreen | kCentre));
    CHECK(sp::speaker_zones(Location::kLeftSurround, false) == (kSurround | kBack));
    CHECK(sp::speaker_zones(Location::kLeftSurround, true) == (kSurround | kSide));
    CHECK(sp::speaker_zones(Location::kLrs, true) == kBack);
    CHECK(sp::speaker_zones(Location::kCs, false) == (kSurround | kBack));
    CHECK(sp::speaker_zones(Location::kLw, true) == kSide);
    CHECK(sp::speaker_zones(Location::kVhl, false) == kTopBottom);
    CHECK(sp::speaker_zones(Location::kRts, true) == kTopBottom);
    CHECK(sp::speaker_zones(Location::kLfe, false) == 0);

    // Table 20, one speaker at a time.
    const unsigned centre = kScreen | kCentre;
    const unsigned surround_51 = kSurround | kBack;
    const unsigned side_71 = kSurround | kSide;
    const unsigned rear = kBack;
    CHECK(sp::zone_admits(centre, 0, true));
    CHECK_FALSE(sp::zone_admits(surround_51, 1, true));  // back excluded
    CHECK(sp::zone_admits(side_71, 1, true));
    CHECK_FALSE(sp::zone_admits(side_71, 2, true));      // side excluded
    CHECK(sp::zone_admits(rear, 2, true));
    CHECK(sp::zone_admits(rear, 3, true));               // centre-and-back
    CHECK(sp::zone_admits(centre, 3, true));             // ... which takes the centre in
    CHECK_FALSE(sp::zone_admits(kScreen, 3, true));      // but not the front pair
    CHECK(sp::zone_admits(kScreen, 4, true));            // screen only
    CHECK_FALSE(sp::zone_admits(rear, 4, true));
    CHECK(sp::zone_admits(surround_51, 5, true));        // surround only
    CHECK_FALSE(sp::zone_admits(kScreen, 5, true));
    // Reserved values constrain nothing.
    CHECK(sp::zone_admits(rear, 6, true));
    CHECK(sp::zone_admits(rear, 7, true));
    // Table 21: the Top-Bottom zone answers to b_enable_elevation alone.
    CHECK(sp::zone_admits(kTopBottom, 4, true));
    CHECK_FALSE(sp::zone_admits(kTopBottom, 0, false));
}
