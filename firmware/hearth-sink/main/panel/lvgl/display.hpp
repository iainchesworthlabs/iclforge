#pragma once

#include <atomic>
#include <cstdint>

#include "lvgl.h"

// What differs between a panel and none (planning/karaoke-p4x-panel.md, D1): the display LVGL draws
// on, and the pointer it is touched with. CMake builds display_dsi.cpp (a MIPI DSI panel and its
// touch controller) or display_null.cpp (a frame buffer in PSRAM), and the rest of the panel is the
// same code for both, which is what makes what a board with no screen measures a measure of the one
// that has one.

namespace player::panel {

struct Display {
    lv_display_t* lv = nullptr;
    int width = 0;
    int height = 0;
    // The picture as it is on the glass, or in the frame buffer with none: RGB565, width * height
    // pixels, row after row with nothing between. Valid for the life of the program.
    const std::uint16_t* frame = nullptr;
    // A touch controller is reading.
    bool touch = false;
    // "null" or "dsi".
    const char* kind = "";
};

// Opens the display on LVGL (which is initialised) and the pointer with it. False, having said why,
// when it could not: the board then has no screen.
bool display_open(Display& out);

// The backlight's brightness, percent. Does nothing where the display has none.
void display_set_backlight(int percent);

// A pointer that something other than the glass moved (a tap that POST /panel/tap made): read in
// preference to the touch controller while it is down. A board with no touch controller has only
// this.
struct InjectedPointer {
    std::atomic<int> x{0};
    std::atomic<int> y{0};
    std::atomic<bool> down{false};
};
InjectedPointer& injected_pointer();

}  // namespace player::panel
