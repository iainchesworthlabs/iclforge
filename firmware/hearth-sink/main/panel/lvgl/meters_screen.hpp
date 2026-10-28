#pragma once

#include <cstdint>

#include "lvgl.h"

#include "iclforge/panel_stats.hpp"

#include "board_info.hpp"
#include "now_screen.hpp"

// The Meters view of the no-lyrics screen (planning/p4x-meters-view.md): an instrument. A third-octave spectrum of the main mix with a dB scale,
// peak-hold ticks and the zone colours of a level meter, and under it the room's channels in the representation its size and its outputs ask for
// (iclforge/meters_view.hpp: a stereo master, the surround sum, or every output grouped in space). Drawn from what the player's tap gives
// (iclforge/meters_tap.hpp): a mono mix, analysed here twenty times a second (iclforge/dsp/spectrum.hpp), and each output's peak over 50 ms.
//
// Nothing here owns a thread or a clock: lyric_screen.cpp calls these with LVGL locked.

namespace player::panel {

// Builds the view into `parent`, an empty stage of `width` by `height`, for the room `now` describes, and asks the player for its output
// (the tap). False, building nothing, when the board does not know its layout.
bool now_build_meters(lv_obj_t* parent, int width, int height, const NowPlaying& now, const NowStyle& style);

// Brings the view up to date: at most twenty times a second, whatever the frame rate, and only a bar or a tick whose pixel moved is redrawn.
// `dimmed`: the screen is dimmed, so nothing is drawn and the player is not asked for its output. Cheap to call every frame. True while the
// view wants the panel's loop at its frame rate (it is showing and not dimmed) and not at its resting beat of ten a second, which would
// cap the view at ten updates a second.
bool now_update_meters(std::uint64_t now_us, bool dimmed);

// What the view has cost since it was last opened, and the rate it holds (iclforge/panel_stats.hpp): live while it shows, the last opening's
// figures after. With LVGL locked, as the panel's own counters are read.
[[nodiscard]] iclforge::panelstats::MetersStats now_meters_stats();

// The decoder is busy and the audio comes first (panel.cpp decides from the player's burst time): hold the view at ten updates a second, or let it
// choose its rate again. From the panel's task, between cycles, as the update is; it outlives the view being closed and opened.
void now_meters_hold_slow(bool hold);

// LVGL finished a refresh that took `took_us` (the panel's task calls this from the refresh-ready event): counted while the view shows.
void now_meters_refresh(std::uint32_t took_us);

// The stage was cleared (now_forget() calls this): the objects are gone, the player is no longer asked for its output, and what was
// kept for the analysis is given back.
void now_forget_meters();

}  // namespace player::panel
