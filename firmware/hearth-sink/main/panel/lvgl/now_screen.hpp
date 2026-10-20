#pragma once

#include <cstdint>

#include "lvgl.h"

#include "board_info.hpp"

// The no-lyrics screens (planning/p4x-now-playing.md): what the panel shows while a song with no
// lyrics plays. The speakers view lives here, as an object tree the lyric screen puts on its stage and
// brings up to date once a frame, the way dev_screen.cpp is a screen of its own; the numbers (where each
// speaker goes, how a level makes it glow, when the screen dims) are libs/render's room_plan.hpp and
// libs/device's now_view.hpp, tested on the host.
//
// Nothing here owns a thread or a clock: lyric_screen.cpp calls these with LVGL locked.

namespace player::panel {

// The tokens and the face the view is drawn in, handed over by lyric_screen.cpp, which owns them (the
// check that holds the tokens to the shared sink-screen spec reads lyric_screen.cpp).
struct NowStyle {
    std::uint32_t text = 0;
    std::uint32_t accent = 0;
    std::uint32_t muted = 0;
    std::uint32_t faint = 0;
    // The face a speaker's name is drawn in (Archivo Regular, 24 px).
    const lv_font_t* label = nullptr;
};

// The views this build draws: the speakers, "dark" (nothing lit until a touch lights the speakers) and
// "off" (the sentence the screen has always had). The title card and the clock come with what they need
// (planning/p4x-now-playing.md); until then the sheet does not offer them and PUT /now-playing refuses
// them.
[[nodiscard]] constexpr bool now_view_drawn(iclforge::nowview::View view) {
    return view == iclforge::nowview::View::kSpeakers || view == iclforge::nowview::View::kDark ||
           view == iclforge::nowview::View::kOff;
}

// Builds the speakers view into `parent`, an empty stage of `width` by `height`, for the room `now`
// describes. False, building nothing, when the board does not know its layout.
bool now_build_speakers(lv_obj_t* parent, int width, int height, const NowPlaying& now, const NowStyle& style);

// Brings the glow up to date from the levels in `now`. Cheap to call every frame: it does its work at
// most five times a second, and only an output whose step changed is redrawn.
void now_update_speakers(const NowPlaying& now, std::uint64_t now_us);

// The stage was cleared: forget the objects that were on it.
void now_forget();

// A key that says whether the room changed (which slots, what they are called and where), so that the
// view is rebuilt for a new layout and left alone while only the levels move.
[[nodiscard]] std::uint32_t now_room_key(const NowPlaying& now);

}  // namespace player::panel
