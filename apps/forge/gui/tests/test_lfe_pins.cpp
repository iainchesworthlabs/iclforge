#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <utility>
#include <vector>

#include "iclforge/ac3/core/eac3_tables.hpp"
#include "lfe_pins.hpp"

// forge-gui's object mode pins a channel to a bed position (apps/forge/gui/src/lfe_pins.hpp). A
// channel pinned to the LFE is not an object: an object reaches the LFE as lfe_send * gain, and the
// same gain pans it into the speakers, so the pin the window built for it (gain 0, lfe_send 1) was
// silent - and took one of the fifteen object slots to be so. These hold the split that keeps it
// out of the objects and the feed it rides instead; the encoder's own side of it is
// libs/ac3/tests/oba/test_atmos.cpp's [lfe] cases, and the controller's use of both is Qt's.

namespace {

using iclforge::ac3::eac3::chanmap::Location;
using Pin = std::pair<std::size_t, Location>;

}  // namespace

TEST_CASE("an LFE pin is set apart from the speaker pins", "[lfe][gui]") {
    const std::vector<Pin> pinned{
        {0, Location::kLeft}, {1, Location::kLfe}, {2, Location::kCentre}, {5, Location::kLfe2}};
    const auto split = forge_gui::split_pins(pinned);

    // Both LFE locations are the LFE, in the order the channels came.
    CHECK(split.lfes == std::vector<std::size_t>{1, 5});
    // The speaker pins keep their channels and their order.
    REQUIRE(split.speakers.size() == 2);
    CHECK(split.speakers[0] == Pin{0, Location::kLeft});
    CHECK(split.speakers[1] == Pin{2, Location::kCentre});
}

TEST_CASE("pins that are all speakers leave no LFE", "[lfe][gui]") {
    const std::vector<Pin> pinned{{0, Location::kLeft}, {1, Location::kRight}};
    const auto split = forge_gui::split_pins(pinned);
    CHECK(split.lfes.empty());
    CHECK(split.speakers.size() == 2);
}

TEST_CASE("nothing pinned splits into nothing", "[lfe][gui]") {
    const auto split = forge_gui::split_pins({});
    CHECK(split.lfes.empty());
    CHECK(split.speakers.empty());
}

TEST_CASE("an LFE pinned alone is all LFE and no object", "[lfe][gui]") {
    // The window refuses to encode this (an Atmos stream needs an object or a speaker beside the LFE);
    // what it relies on to do so is that the split leaves nothing in the speakers.
    const auto split = forge_gui::split_pins({{3, Location::kLfe}});
    CHECK(split.speakers.empty());
    CHECK(split.lfes == std::vector<std::size_t>{3});
}

TEST_CASE("the LFE feed is every LFE plane added, each holding its last sample", "[lfe][gui]") {
    const std::vector<std::vector<float>> planes{{1.0F, 2.0F, 3.0F, 4.0F}, {10.0F, 20.0F}};
    std::vector<float> out(4);
    std::vector<float> scratch(4);

    // From the start: the first plane as it is, the second to its end and then its last sample, 20.
    forge_gui::fill_lfe_frame(planes, 0, out, scratch);
    CHECK(out == std::vector<float>{11.0F, 22.0F, 23.0F, 24.0F});

    // Two samples in, the first plane runs out as well and holds its 4 beside the second's 20.
    forge_gui::fill_lfe_frame(planes, 2, out, scratch);
    CHECK(out == std::vector<float>{23.0F, 24.0F, 24.0F, 24.0F});
}

TEST_CASE("the LFE feed of no planes is silence, whatever was in the frame", "[lfe][gui]") {
    std::vector<float> out{9.0F, 9.0F, 9.0F};
    std::vector<float> scratch(3);
    forge_gui::fill_lfe_frame({}, 0, out, scratch);
    CHECK(out == std::vector<float>{0.0F, 0.0F, 0.0F});
}
