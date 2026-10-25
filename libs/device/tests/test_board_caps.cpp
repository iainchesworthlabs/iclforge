// What a board can do, as data (iclforge/board_caps.hpp), checked for every board this product knows
// (iclforge/board_variants.hpp): the slot arithmetic agrees with GET /hardware's own ceiling, a screen is a screen
// whichever way it is attached, and no variant is described twice.

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

#include "iclforge/board_caps.hpp"
#include "iclforge/board_variants.hpp"

using iclforge::BoardCaps;
using iclforge::DisplayKind;
using iclforge::known_variants;
namespace variants = iclforge::variants;

TEST_CASE("every known variant has a name of its own and a profile of its own", "[board_caps]") {
    std::set<std::string> names;
    std::set<std::string> profiles;
    for (const auto& variant : known_variants()) {
        INFO(variant.name);
        REQUIRE(names.insert(variant.name).second);
        REQUIRE_FALSE(variant.caps.profile.empty());
        REQUIRE(profiles.insert(variant.caps.profile).second);
    }
    REQUIRE(names.size() >= 8);
}

TEST_CASE("a display is present when it has a kind and a size, and not otherwise", "[board_caps]") {
    for (const auto& variant : known_variants()) {
        INFO(variant.name);
        const auto& display = variant.caps.display;
        if (display.kind == DisplayKind::kNone) {
            REQUIRE_FALSE(display.present());
            REQUIRE_FALSE(variant.caps.has_screen());
            REQUIRE(display.width == 0);
            REQUIRE(display.height == 0);
            // A board with no screen has no touch and no backlight to speak of.
            REQUIRE_FALSE(display.touch);
            REQUIRE_FALSE(display.backlight);
        } else {
            REQUIRE(display.present());
            REQUIRE(display.width >= 240);
            REQUIRE(display.height >= 240);
        }
    }
    const iclforge::DisplayCaps no_width{.kind = DisplayKind::kDsi, .width = 0, .height = 600};
    const iclforge::DisplayCaps no_kind{.kind = DisplayKind::kNone, .width = 1024, .height = 600};
    REQUIRE_FALSE(no_width.present());
    REQUIRE_FALSE(no_kind.present());
}

TEST_CASE("the display kinds have the words GET :8081/panel uses", "[board_caps]") {
    REQUIRE(std::string(iclforge::display_kind_name(DisplayKind::kNone)) == "none");
    REQUIRE(std::string(iclforge::display_kind_name(DisplayKind::kFrameBuffer)) == "null");
    REQUIRE(std::string(iclforge::display_kind_name(DisplayKind::kDsi)) == "dsi");
    REQUIRE(std::string(iclforge::display_kind_name(DisplayKind::kSpi)) == "spi");
    REQUIRE(std::string(iclforge::display_kind_name(DisplayKind::kParallel)) == "parallel");
}

TEST_CASE("the sink's slots follow sink_plan.hpp's ceiling at every setting", "[board_caps]") {
    // An ESP32-S3: a 128-bit frame a line, a second controller. 4 or 8 slots at 32 bits, 8 or 16 at 16.
    const iclforge::SinkCaps s3 = variants::s3_n16r8().sink;
    REQUIRE(s3.slots_at(32, false) == 4);
    REQUIRE(s3.slots_at(32, true) == 8);
    REQUIRE(s3.slots_at(16, false) == 8);
    REQUIRE(s3.slots_at(16, true) == 16);
    REQUIRE(s3.slots() == 4);  // 32-bit slots, no second line wired
    REQUIRE(s3.slots_most() == 16);

    // An ESP32-C6 has one controller: a second line adds nothing.
    const iclforge::SinkCaps c6 = variants::c6_16mb().sink;
    REQUIRE(c6.slots_at(16, true) == 8);
    REQUIRE(c6.slots_at(32, true) == 4);
    REQUIRE(c6.slots_most() == 8);

    // The P4's wide sink: a 512-bit frame, sixteen 32-bit slots or thirty-two 16-bit ones.
    const iclforge::SinkCaps p4 = variants::p4_rev3_kit().sink;
    REQUIRE(p4.slots() == 16);
    REQUIRE(p4.slots_at(16, false) == 32);
    REQUIRE(p4.slots_most() == 32);
}

TEST_CASE("a chip that cannot open a TDM frame carries two channels at most, whatever the frame says",
          "[board_caps]") {
    const iclforge::SinkCaps rev1 = variants::p4_rev1().sink;
    REQUIRE_FALSE(rev1.tdm_usable);
    REQUIRE(rev1.slots() == 2);
    REQUIRE(rev1.slots_at(16, false) == 2);
    REQUIRE(rev1.slots_most() == 2);
}

TEST_CASE("a sink with no TDM frame carries its fixed slot count", "[board_caps]") {
    iclforge::SinkCaps capture;
    capture.fixed_slots = 8;
    REQUIRE(capture.slots() == 8);
    REQUIRE(capture.slots_at(16, true) == 8);
    REQUIRE(capture.slots_most() == 8);
    REQUIRE(iclforge::SinkCaps{}.slots() == 0);
}

TEST_CASE("an unknown slot width has no slots", "[board_caps]") {
    REQUIRE(variants::s3_n16r8().sink.slots_at(24, false) == 0);
}

TEST_CASE("the sink's most slots is the figure GET /hardware reports, on every board", "[board_caps]") {
    // One truth: HardwareFacts::sink_max_slots comes from the sink, and so does this. A chip that cannot clock a wide
    // frame reports the two it reaches, not the thirty-two the frame holds (the sink knows the revision).
    for (const auto& variant : known_variants()) {
        INFO(variant.name);
        const BoardCaps& caps = variant.caps;
        if (caps.sink.frame_bit_max == 0 && caps.sink.fixed_slots == 0) {
            // A board with no audio output (a JetKVM Mini): no slots at any setting, and GET /hardware says none.
            REQUIRE(caps.sink.slots() == 0);
            REQUIRE(caps.sink.slots_most() == 0);
            REQUIRE(caps.hardware.sink_max_slots == 0);
            continue;
        }
        REQUIRE(caps.hardware.sink_max_slots_bits == 16);
        REQUIRE(caps.sink.slots_most() == caps.hardware.sink_max_slots);
        if (!caps.sink.tdm_usable) {
            REQUIRE(caps.hardware.revision_hard_limit_below > caps.hardware.revision_major * 100 + caps.hardware.revision_minor);
            REQUIRE(caps.sink.slots_most() == 2);
        }
        // The slots in force never exceed the best the hardware takes.
        REQUIRE(caps.sink.slots() <= caps.sink.slots_most());
        REQUIRE(caps.sink.slots() > 0);
    }
}

TEST_CASE("the networks a board has, and whether it has a choice between them", "[board_caps]") {
    REQUIRE(variants::p4_rev3_kit().network.choice());
    REQUIRE(variants::p4_rev3_kit().network.any());
    REQUIRE_FALSE(variants::s3_n16r8().network.choice());
    REQUIRE_FALSE(variants::c6_16mb().network.choice());
    const iclforge::NetworkCaps cable_only{.ethernet = true};
    REQUIRE_FALSE(iclforge::NetworkCaps{}.any());
    REQUIRE_FALSE(cable_only.choice());
    for (const auto& variant : known_variants()) {
        INFO(variant.name);
        // Every board of the fleet has a way onto a network, and so a clock to keep.
        REQUIRE(variant.caps.network.any());
        REQUIRE(variant.caps.wall_clock);
    }
}

TEST_CASE("the P4X kit is the one board with a real screen today", "[board_caps]") {
    int real_screens = 0;
    for (const auto& variant : known_variants()) {
        if (variant.caps.display.kind == DisplayKind::kDsi && variant.caps.profile == "p4-rev3") {
            ++real_screens;
            REQUIRE(variant.caps.display.width == 1024);
            REQUIRE(variant.caps.display.height == 600);
            REQUIRE(variant.caps.display.touch);
            REQUIRE(variant.caps.display.backlight);
        }
    }
    REQUIRE(real_screens == 1);
}

TEST_CASE("a JetKVM Mini is a square screen with no touch, side buttons, no audio output and one network", "[board_caps]") {
    for (const BoardCaps& mini : {variants::p4x_jetkvm_mini_240(), variants::p4x_jetkvm_mini_w_240()}) {
        INFO(mini.profile);
        REQUIRE(mini.display.present());
        REQUIRE(mini.display.width == 240);
        REQUIRE(mini.display.height == 240);
        REQUIRE_FALSE(mini.display.touch);
        REQUIRE(mini.display.keys == 2);
        // Something to press, so a person can drive it, though not by touch.
        REQUIRE(mini.display.operable());
        REQUIRE(mini.sink.slots_most() == 0);
        REQUIRE_FALSE(mini.network.choice());
        REQUIRE(mini.network.any());
    }
    REQUIRE(variants::p4x_jetkvm_mini_240().network.ethernet);
    REQUIRE_FALSE(variants::p4x_jetkvm_mini_240().network.wifi);
    REQUIRE(variants::p4x_jetkvm_mini_w_240().network.wifi);
    REQUIRE_FALSE(variants::p4x_jetkvm_mini_w_240().network.ethernet);
    // The kit's panel can be touched; a screen with neither touch nor keys cannot be driven at all.
    REQUIRE(variants::p4_rev3_kit().display.operable());
    REQUIRE_FALSE(variants::c3_spi_320x240_no_touch().display.operable());
    REQUIRE_FALSE(variants::s3_n16r8().display.operable());
}

TEST_CASE("the object cap is the build's, and the P4 places a dozen", "[board_caps]") {
    REQUIRE(variants::p4_rev3_kit().max_objects == 12);
    REQUIRE(variants::s3_n16r8().max_objects == 0);
    REQUIRE_FALSE(BoardCaps{}.max_objects.has_value());
}
