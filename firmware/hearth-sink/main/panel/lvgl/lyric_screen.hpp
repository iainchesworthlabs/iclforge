#pragma once

#include <cstdint>

#include "lvgl.h"

// The lyric screen (planning/karaoke-p4x-panel.md, "The screen"): LVGL objects for the header, the
// stage where the lines are, and the footer, brought up to date once a frame from what the Sendspin
// player holds. One screen, whichever display it is drawn on (display.hpp).
//
// Nothing here owns a thread or a clock: panel.cpp calls screen_update() from its task, with LVGL
// locked, and the position of the sound comes from the player's own clock through
// sendspin_read_lyrics().

namespace player::panel {

struct ScreenStats {
    // "idle", "lyrics", "gap", "waiting", "plain" or "nolyrics".
    const char* name = "idle";
    // Times the rows were rebuilt (a line change, a gap, a new item), and the longest one took.
    std::uint32_t rebuilds = 0;
    std::uint32_t rebuild_us_max = 0;
    // The developer view (dev_screen.hpp) is open over the screen, and what its refreshes have cost.
    bool dev_view = false;
    // The Preferences sheet (prefs_screen.hpp) is open over the screen, how many times it was built, and the
    // longest a build and a refresh took, microseconds.
    bool prefs_view = false;
    std::uint32_t prefs_builds = 0;
    std::uint32_t prefs_build_us_max = 0;
    std::uint32_t prefs_refresh_us_max = 0;
    std::uint32_t dev_refreshes = 0;
    std::uint32_t dev_refresh_us_last = 0;
    std::uint32_t dev_refresh_us_max = 0;
    // The no-lyrics screen has dimmed or is dark (planning/p4x-now-playing.md): a black cover is up.
    bool dimmed = false;
    // The backlight, percent: lit, the dimmed floor, or off in the dark view.
    int backlight = 80;
};

// Builds the screen on `disp`. False, having said why, if the fonts did not come up.
bool screen_init(lv_display_t* disp, int width, int height);

// One frame. True while a word is being filled in, so that the caller draws at the fill's rate;
// false between words and everywhere else, where a tenth of that does.
bool screen_update();

[[nodiscard]] ScreenStats screen_stats();

// For the preview tool (preview/), which checks that nothing on a sheet is clipped or too small to touch: the Adjust
// sheet while it is shown, and the footer; null when there is none.
[[nodiscard]] lv_obj_t* screen_sheet_object();
[[nodiscard]] lv_obj_t* screen_footer_object();

}  // namespace player::panel
