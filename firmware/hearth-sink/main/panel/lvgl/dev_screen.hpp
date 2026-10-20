#pragma once

#include <cstdint>

#include "lvgl.h"

// The developer view (planning/karaoke-p4x-panel.md, "The developer view"): a screen of what the
// board is and how it is doing, opened by three taps on the board's name in the footer and closed by
// a tap. It sits on top of the lyric screen as an object tree of its own, built when it is opened
// and deleted when it is closed, so that the lyric screen and the idle screen are as they were with
// it shut; its fonts are made the first time it opens, and its facts are read once a second
// (dev_info.hpp) and not at all while it is closed.
//
// Like the lyric screen it owns no thread: lyric_screen.cpp calls dev_update() once a frame, with
// LVGL locked.

namespace player::panel {

// The tokens the lyric screen is drawn in (docs/hearth/design/sink-screen-tokens.json), passed in
// rather than written twice: the check that holds them to the spec reads lyric_screen.cpp.
struct DevColours {
    std::uint32_t bg = 0;
    std::uint32_t surface = 0;
    std::uint32_t surface_up = 0;
    std::uint32_t text = 0;
    std::uint32_t accent = 0;
    std::uint32_t muted = 0;
    std::uint32_t faint = 0;
};

// What the view's refreshes (reading the board, setting the labels' text, the console) have cost
// the panel's task, microseconds, over the life of the program: LVGL's drawing of what they changed
// is the panel's render time and is not in these.
struct DevStats {
    std::uint32_t refreshes = 0;
    std::uint32_t refresh_us_last = 0;
    std::uint32_t refresh_us_max = 0;
};

// Remembers the screen's size and tokens. Builds nothing.
void dev_init(int width, int height, const DevColours& colours);

// Asks for the view to open (from a tap): it is built by the next dev_update(), outside the event.
void dev_open();

[[nodiscard]] DevStats dev_stats();

// Whether the view is shown, or is about to be.
[[nodiscard]] bool dev_is_open();

// Once a frame, after the lyric screen's own work: builds the view when it was asked for, takes it
// down when it was closed or left alone for five minutes, and brings what it shows up to date.
void dev_update(std::uint32_t now_ms);

}  // namespace player::panel
