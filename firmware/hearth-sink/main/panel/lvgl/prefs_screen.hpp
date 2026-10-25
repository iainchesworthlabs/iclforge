#pragma once

#include <cstdint>

#include "lvgl.h"

#include "iclforge/screen_metrics.hpp"

#include "dev_screen.hpp"

// The Preferences sheet (planning/p4x-preferences.md): a scrollable list of everything a person chooses once and
// leaves, grouped (Display, Sound, Network, Time, Updates, System) and drawn from the registry
// (iclforge/preferences.hpp) for the board it is on. What a control is, which choices it has and whether this board
// can do them all come from the registry and the board's capability description; this file only draws them and
// carries a tap to iclforge::prefs::set_value(). A preference the board cannot support is shown greyed with its
// reason, never hidden.
//
// Every size comes from iclforge::screen::Metrics, which is derived from the display it is given: the sheet is the
// same code at 1024 by 600, 800 by 480, 480 by 320 and 320 by 240.
//
// Like the developer view it is an object tree of its own, built when it is opened and deleted when it is closed, so
// that the lyric screen is as it was with it shut; its fonts are made the first time it opens. It owns no thread:
// lyric_screen.cpp calls prefs_update() once a frame, with LVGL locked.

namespace player::panel {

// A control that is touched carries this flag, so that the preview tool can find every touch target on a sheet and
// hold it to the fingertip size (iclforge::screen::kMinTargetPx).
inline constexpr lv_obj_flag_t kTouchTargetFlag = LV_OBJ_FLAG_USER_1;

struct PrefsStats {
    std::uint32_t builds = 0;
    std::uint32_t build_us_max = 0;
    std::uint32_t refreshes = 0;
    std::uint32_t refresh_us_max = 0;
};

// Remembers the display's size and the tokens the screen is drawn in. Builds nothing.
void prefs_init(const iclforge::screen::Metrics& metrics, const DevColours& colours);

// Asks for the sheet to open (from a tap): it is built by the next prefs_update(), outside the event.
void prefs_open();

[[nodiscard]] bool prefs_is_open();

// The sheet's root object while it is up, for the preview tool's checks; null otherwise.
[[nodiscard]] lv_obj_t* prefs_root();

[[nodiscard]] PrefsStats prefs_stats();

// Once a frame, after the lyric screen's own work: builds the sheet when it was asked for, takes it down when it was
// closed or left alone for a minute, and brings what it shows up to date with the board's settings.
void prefs_update(std::uint32_t now_ms);

}  // namespace player::panel
