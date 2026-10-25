// The sizes the sheets are drawn at (iclforge/screen_metrics.hpp): every size the product is drawn at has targets a
// finger can hit, type that fits, and room left for the content.

#include <catch2/catch_test_macros.hpp>

#include "iclforge/board_variants.hpp"
#include "iclforge/screen_metrics.hpp"

namespace screen = iclforge::screen;

TEST_CASE("every drawn size has fingertip targets, gaps and type that fit", "[screen_metrics]") {
    struct Size {
        int w;
        int h;
    };
    for (const Size size : {Size{1024, 600}, Size{800, 480}, Size{480, 320}, Size{320, 240}, Size{240, 320}, Size{1280, 800},
                            Size{240, 240}}) {
        INFO(size.w << "x" << size.h);
        const screen::Metrics m = screen::metrics_for(size.w, size.h);
        REQUIRE(m.width == size.w);
        REQUIRE(m.height == size.h);
        // A touched control is at least a fingertip on both sides.
        REQUIRE(m.target >= screen::kMinTargetPx);
        REQUIRE(m.row_target >= screen::kMinTargetPx);
        REQUIRE(m.footer_button >= screen::kMinTargetPx);
        REQUIRE(m.bar_button >= screen::kMinTargetPx);
        REQUIRE(m.footer_button_width >= screen::kMinTargetPx);
        REQUIRE(m.gap >= screen::kMinGapPx);
        // The bar and the footer hold the buttons they carry, with room above and below.
        REQUIRE(m.bar >= m.bar_button);
        REQUIRE(m.footer >= m.footer_button);
        // Type is readable and ordered.
        REQUIRE(m.text >= 16);
        REQUIRE(m.note >= 13);
        REQUIRE(m.note < m.text);
        REQUIRE(m.title > m.text);
        // The margins leave most of the width, and the header and footer most of the height.
        REQUIRE(m.margin * 2 < size.w / 4);
        REQUIRE(m.header + m.footer < size.h / 2);
        // A button's label fits the least width a footer button gets: 10 characters of type this size, less the padding.
        REQUIRE(m.footer_button_width >= 2 * m.pad + 3 * m.text);
    }
}

TEST_CASE("the sizes grow with the display and are the same for a portrait panel", "[screen_metrics]") {
    const screen::Metrics big = screen::metrics_for(1024, 600);
    const screen::Metrics mid = screen::metrics_for(800, 480);
    const screen::Metrics small = screen::metrics_for(480, 320);
    const screen::Metrics tiny = screen::metrics_for(320, 240);
    REQUIRE(big.text >= mid.text);
    REQUIRE(mid.text >= small.text);
    REQUIRE(small.text >= tiny.text);
    REQUIRE(big.margin >= mid.margin);
    REQUIRE(mid.margin >= small.margin);
    REQUIRE(small.margin >= tiny.margin);
    const screen::Metrics portrait = screen::metrics_for(320, 480);
    const screen::Metrics landscape = screen::metrics_for(480, 320);
    REQUIRE(portrait.text == landscape.text);
    REQUIRE(portrait.target == landscape.target);
}

TEST_CASE("the kit's sizes are the ones it has always been drawn at", "[screen_metrics]") {
    const screen::Metrics kit = screen::metrics_for(1024, 600);
    // planning/karaoke-p4x-panel.md: 32 px margins, a 64 px footer with 48 px buttons, 72 px targets on the sheet, 24 px type.
    REQUIRE(kit.margin == 32);
    REQUIRE(kit.footer == 64);
    REQUIRE(kit.footer_button == 48);
    REQUIRE(kit.header == 64);
    REQUIRE(kit.target == 72);
    REQUIRE(kit.text == 24);
    REQUIRE(kit.footer_button_width == 160);
}

TEST_CASE("every known board's display has sizes that work", "[screen_metrics]") {
    for (const auto& variant : iclforge::known_variants()) {
        if (!variant.caps.has_screen()) {
            continue;
        }
        INFO(variant.name);
        const screen::Metrics m = screen::metrics_for(variant.caps.display.width, variant.caps.display.height);
        REQUIRE(m.target >= screen::kMinTargetPx);
        REQUIRE(m.footer + m.header + 3 * m.target < variant.caps.display.height + m.target);
    }
}
