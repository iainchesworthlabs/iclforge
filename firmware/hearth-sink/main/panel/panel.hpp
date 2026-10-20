#pragma once

#include <cstdint>
#include <string_view>

#include "iclforge/now_view.hpp"

// The board's screen (planning/karaoke-p4x-panel.md): the lyrics of the item playing, drawn with the
// colour moving as the sound does, and, when nothing is playing, what the board is and what it waits
// for. A seam CMake resolves, like the sink and the source:
//
//   panel/none/   no screen; every function here does nothing. Every image but a P4's with a panel;
//   panel/lvgl/   LVGL drawing the lyric screen (lyric_screen.cpp) onto a MIPI DSI panel with touch
//                 (display_dsi.cpp), or into a frame buffer in PSRAM with no panel behind it
//                 (display_null.cpp), which is what a board without a screen runs to see what the
//                 screen looks like and what drawing it costs.
//
// The panel reads the board and never writes to it: the lyrics from the Sendspin player
// (sendspin.hpp, sendspin_read_lyrics), the name and the address from the settings and the network.
// A panel that does not come up says so once and the board plays as it would without one.

namespace player {

struct PanelStats {
    // The panel's task is up and drawing.
    bool running = false;
    // "none", "null" (a frame buffer) or "dsi".
    const char* kind = "none";
    int width = 0;
    int height = 0;
    // A touch controller (or the injected taps of a build with none) is reading.
    bool touch = false;
    // Which screen is showing: "idle", "lyrics", "gap", "waiting", "plain", "nolyrics".
    const char* screen = "";
    // The no-lyrics screen has dimmed (planning/p4x-now-playing.md): the backlight is at its floor.
    bool dimmed = false;
    // Updates of the picture that LVGL made, and the pixels they covered.
    std::uint32_t refreshes = 0;
    std::uint64_t pixels = 0;
    // Microseconds LVGL took to render and flush one refresh: the last, the mean and the longest.
    std::uint32_t render_us_last = 0;
    std::uint32_t render_us_mean = 0;
    std::uint32_t render_us_max = 0;
    // The longest the screen took to rebuild its rows (a line change), microseconds, and how many
    // times it did.
    std::uint32_t rebuild_us_max = 0;
    std::uint32_t rebuilds = 0;
    // The developer view (dev_screen.hpp) is open over the screen, and what its refreshes (reading
    // the board, setting the text) have cost the task, microseconds.
    bool dev_view = false;
    std::uint32_t dev_refreshes = 0;
    std::uint32_t dev_refresh_us_last = 0;
    std::uint32_t dev_refresh_us_max = 0;
    // The task's own cycles that ran later than its period, and the slowest cycle, microseconds.
    std::uint32_t late_cycles = 0;
    std::uint32_t cycle_us_max = 0;
    // The task's stack left at its least, bytes.
    std::uint32_t stack_free = 0;
};

// Whether this build has a screen at all.
[[nodiscard]] bool panel_built();

// The screen for a song with no lyrics (planning/p4x-now-playing.md): which of the four views this build
// can draw, which one to show, and the minutes of nothing touched after which it dims (0 is never). The
// board's stored settings (settings.hpp) are the caller's to keep; these change what the screen does, at
// once and without a restart. Without a panel every one does nothing.
[[nodiscard]] bool panel_has_view(iclforge::nowview::View view);
void panel_set_view(iclforge::nowview::View view);
void panel_set_dim_after(int minutes);

// The time zone the clock view shows, as a POSIX string, or "" for none (iclforge/wall_clock.hpp): read
// the way the board's C library reads it, and refused (false, nothing changed) where that differs from
// the grammar. Not stored: the board's settings are the caller's to keep. Without a panel does nothing.
[[nodiscard]] bool panel_set_time_zone(std::string_view zone);

// Starts the panel's task: once, from app_main, after the network and the Sendspin player. True when
// a panel is running; false (having said why) when there is none or it did not come up.
bool panel_start();

// What the panel has done so far; all zero without one.
[[nodiscard]] PanelStats panel_stats();

}  // namespace player
