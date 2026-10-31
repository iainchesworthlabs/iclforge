#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "iclforge/board_caps.hpp"
#include "iclforge/dev_view.hpp"
#include "iclforge/now_view.hpp"
#include "iclforge/wall_clock.hpp"

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
    // The no-lyrics screen has dimmed, or is dark (planning/p4x-now-playing.md): a black cover is up.
    bool dimmed = false;
    // The backlight the glass is given, percent: the person's brightness (80 unless they set it) while lit, the dimmed
    // floor (4, and never above the brightness) or off (0) in the dark view.
    int backlight = 80;
    // Updates of the picture that LVGL made, and the pixels they covered.
    std::uint32_t refreshes = 0;
    std::uint64_t pixels = 0;
    // Microseconds LVGL took to render and flush one refresh: the last, the mean and the longest.
    std::uint32_t render_us_last = 0;
    std::uint32_t render_us_mean = 0;
    std::uint32_t render_us_max = 0;
    // And all of them added, which a mean between two readings is worked out from.
    std::uint64_t render_us_total = 0;
    // The longest the screen took to rebuild its rows (a line change), microseconds, and how many
    // times it did.
    std::uint32_t rebuild_us_max = 0;
    std::uint32_t rebuilds = 0;
    // The developer view (dev_screen.hpp) is open over the screen, and what its refreshes (reading
    // the board, setting the text) have cost the task, microseconds.
    bool dev_view = false;
    // The Preferences sheet (lvgl/prefs_screen.hpp) is open, how many times it was built and the longest a build
    // took, microseconds.
    bool prefs_view = false;
    std::uint32_t prefs_builds = 0;
    std::uint32_t prefs_build_us_max = 0;
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
// How the speakers view shows a level (iclforge::nowview::SpeakersStyle, planning/p4x-speakers-options.md).
void panel_set_speakers_style(iclforge::nowview::SpeakersStyle style);

// What a person chose for the screen (planning/p4x-preferences.md), followed at once: the backlight's lit level,
// percent from iclforge::nowview::kBrightnessMin to kBrightnessMax (the dimmed floor stays under it), and how the
// clock view writes the time. Stored by the caller (settings.hpp). Without a panel, or with a glass whose
// backlight cannot be dimmed (panel_display_caps().backlight), the brightness does nothing.
void panel_set_brightness(int percent);
void panel_set_clock_format(iclforge::wallclock::ClockFormat format);
// The night schedule: go dark between two times by the board's own time, as the dark view does. Stored by the caller;
// the screen follows at once. Without a panel it does nothing.
void panel_set_night(const iclforge::nowview::NightSchedule& night);

// The display the panel opened, as the board's capability description has it (iclforge/board_caps.hpp): its kind,
// size, whether a touch controller is reading and whether the backlight can be set. Fixed once the panel is up,
// read without the panel's lock; a build with no panel, or one whose display did not come up, has none.
[[nodiscard]] iclforge::DisplayCaps panel_display_caps();

// The board as About and the network preference's confirming step read it: the network, the configuration and the
// pairing count (iclforge::devview::Facts, read the way the developer view reads them: panel/lvgl/dev_info.cpp). Safe
// from the panel's task and the control surface's; the network part is a call across SDIO on a board whose radio is a
// companion chip, cached for two seconds. Empty in a build with no panel, which has no screen to show it on.
[[nodiscard]] iclforge::devview::Facts panel_board_facts();

// Starts the panel's task: once, from app_main, after the network and the Sendspin player. True when
// a panel is running; false (having said why) when there is none or it did not come up.
bool panel_start();

// What the panel has done so far; all zero without one.
[[nodiscard]] PanelStats panel_stats();

// GET /panel-stats (iclforge/panel_stats.hpp): what the panel and the Meters view cost, as JSON text, for a person or a script on port 80
// (the capture port is off in a release image). Safe from the control surface's task. Nothing without a panel running.
[[nodiscard]] std::optional<std::string> panel_stats_text();

}  // namespace player
