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
    Metrics m;
    m.width = width;
    m.height = height;
    if (shorter >= 560) {  // 1024 by 600: the P4X kit
        m = {width, height, 32, 12, 72, 64, 24, 20, 32, 24, 64, 64, 48, 160, 64, 56};
    } else if (shorter >= 440) {  // 800 by 480
        m = {width, height, 24, 10, 64, 56, 22, 18, 28, 20, 56, 60, 48, 140, 60, 52};
    } else if (shorter >= 300) {  // 480 by 320
        m = {width, height, 16, 8, 56, 52, 18, 15, 24, 14, 48, 56, 48, 110, 56, 48};
    } else {  // 320 by 240 and smaller
        m = {width, height, 10, 6, 48, 48, 16, 13, 20, 10, 40, 56, 48, 84, 56, 48};
    }
    return m;
}

}  // namespace iclforge::screen
