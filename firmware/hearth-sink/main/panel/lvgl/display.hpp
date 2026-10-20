#pragma once

#include <array>
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

// What the touch controller has said to the pointer since boot, for `GET :8081/panel`: a finger
// that the glass does not report (a film over it, a wrong address) leaves `reads` counting and
// `touches` still. The first run on the kit had a tap that did nothing and no way to tell which of
// the controller, the coordinates or LVGL was at fault. Only a build with a touch controller
// counts.
struct TouchCounters {
    std::atomic<std::uint32_t> reads{0};     // controller reads that answered
    std::atomic<std::uint32_t> failures{0};  // controller reads that returned an error
    std::atomic<std::uint32_t> touches{0};   // answers that held at least one point
    std::atomic<int> x{-1};                  // the last point, as the controller gave it
    std::atomic<int> y{-1};
    std::atomic<int> last_error{0};          // the last failed read's esp_err_t
    // Where the last presses began, as x << 16 | y, in a ring: `recent_next` counts them all and
    // the newest is at (recent_next - 1) % kRecent. A fingertip put on the corners and the middle
    // in turn shows whether the controller's axes are the panel's.
    static constexpr std::uint32_t kRecent = 8;
    std::array<std::atomic<std::uint32_t>, kRecent> recent{};
    std::atomic<std::uint32_t> recent_next{0};

    void record_press(int px, int py) {
        const std::uint32_t slot = recent_next.fetch_add(1) % kRecent;
        recent[slot].store((static_cast<std::uint32_t>(px) << 16) |
                           (static_cast<std::uint32_t>(py) & 0xFFFFu));
    }
};
TouchCounters& touch_counters();

}  // namespace player::panel
