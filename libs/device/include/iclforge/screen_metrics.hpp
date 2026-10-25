#pragma once

// The sizes a board's sheets are drawn at, derived from the display they are drawn on and not written for one panel
// (planning/p4x-preferences.md): margins, gaps, the height of a button, the size of the type, the footer's height.
// A screen that was 1024 by 600 pixels of the P4X kit's panel is now also 800 by 480, 480 by 320 and 320 by 240, and a
// board with a different panel has its sizes from this without a line of its own.
//
// Pure arithmetic and standard library only, so that the host tests it (libs/device/tests/test_screen_metrics.cpp) for
// every size the product is drawn at. The firmware's screens (firmware/hearth-sink/main/panel/lvgl) and the preview
// tool that draws them on a computer both call metrics_for().
//
// THE FINGERTIP TARGET. A control that is touched is never smaller than kMinTargetPx on either side, with a gap of
// at least kMinGapPx to the next one. 48 px is 7.2 mm on the P4X kit's 7-inch 1024 by 600 panel (about 6.7 px per
// millimetre) and 7.4 mm on a 2.4-inch 320 by 240 module (about 6.5 px per millimetre): over the 7 mm that Apple and
// Google give as the least a finger can hit reliably (44 pt, 48 dp), and the same on every size, because the pixel
// density of the small panels this product could meet is about the kit's. A panel denser than that has smaller targets
// for the same pixels; metrics_for() takes the pixel size only, and a board with such a panel passes a smaller
// display to it (or a scale) when it has one.

#include <algorithm>

namespace iclforge::screen {

inline constexpr int kMinTargetPx = 48;
inline constexpr int kMinGapPx = 6;

struct Metrics {
    int width = 0;
    int height = 0;
    // The space at the sides of a sheet, and between controls.
    int margin = 0;
    int gap = 0;
    // The height of a button on the Adjust sheet, and of a control in the Preferences list.
    int target = 0;
    int row_target = 0;
    // The size of the type, in pixels: body text and buttons, notes and reasons, a sheet's title.
    int text = 0;
    int note = 0;
    int title = 0;
    // The space either side of a button's label.
    int pad = 0;
    // The lyric screen's header and footer, and the footer's buttons (their height, and the least width).
    int header = 0;
    int footer = 0;
    int footer_button = 0;
    int footer_button_width = 0;
    // The Preferences sheet's top bar, and the height of the Close button in it.
    int bar = 0;
    int bar_button = 0;
};

// By the display's shorter side, so that a portrait panel is drawn like its landscape twin.
[[nodiscard]] constexpr Metrics metrics_for(int width, int height) {
    const int shorter = std::min(width, height);
    // The kit's 1024 by 600 panel: the sizes of docs/hearth/design/sink-screen-tokens.json, which
    // tools/checks/sink_screen_tokens.py holds this block to (margin, header, footer, type, and the 72 px target).
    if (shorter >= 560) {
        return {.width = width, .height = height, .margin = 32, .gap = 12, .target = 72, .row_target = 64, .text = 24,
                .note = 20, .title = 32, .pad = 24, .header = 64, .footer = 64, .footer_button = 48,
                .footer_button_width = 160, .bar = 64, .bar_button = 56};
    }
    if (shorter >= 440) {  // 800 by 480
        return {.width = width, .height = height, .margin = 24, .gap = 10, .target = 64, .row_target = 56, .text = 22,
                .note = 18, .title = 28, .pad = 20, .header = 56, .footer = 60, .footer_button = 48,
                .footer_button_width = 140, .bar = 60, .bar_button = 52};
    }
    if (shorter >= 300) {  // 480 by 320
        return {.width = width, .height = height, .margin = 16, .gap = 8, .target = 56, .row_target = 52, .text = 18,
                .note = 15, .title = 24, .pad = 14, .header = 48, .footer = 56, .footer_button = 48,
                .footer_button_width = 110, .bar = 56, .bar_button = 48};
    }
    // 320 by 240 and smaller.
    return {.width = width, .height = height, .margin = 10, .gap = 6, .target = 48, .row_target = 48, .text = 16,
            .note = 13, .title = 20, .pad = 10, .header = 40, .footer = 56, .footer_button = 48,
            .footer_button_width = 84, .bar = 56, .bar_button = 48};
}

}  // namespace iclforge::screen
