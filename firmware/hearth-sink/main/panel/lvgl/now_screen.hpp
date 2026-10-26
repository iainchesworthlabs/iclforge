#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "lvgl.h"

#include "iclforge/title_card.hpp"
#include "iclforge/wall_clock.hpp"

#include "board_info.hpp"

// The no-lyrics screens (planning/p4x-now-playing.md): what the panel shows while a song with no
// lyrics plays. The speakers view, the title card and the clock view live here, each as an object tree the
// lyric screen puts on its stage and (the speakers and the clock) brings up to date once a frame, the way
// dev_screen.cpp is a screen of its own; the numbers (where each speaker goes, how a level makes it glow,
// when the screen dims, what a title store keeps of what a server says, what the clock says) are
// libs/render's room_plan.hpp and libs/device's now_view.hpp, title_card.hpp and wall_clock.hpp, tested on
// the host.
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
    // The face a speaker's name is drawn in, which is also the clock view's note and the title card's third
    // line (Archivo Regular, 24 px).
    const lv_font_t* label = nullptr;
    // The face the clock view's date is drawn in (Archivo Regular, 36 px).
    const lv_font_t* date = nullptr;
    // The title card's faces: the title's (Archivo SemiBold, at the size its length asks for, which the
    // caller has set) and the artist's (Archivo Regular, 36 px).
    const lv_font_t* title = nullptr;
    const lv_font_t* artist = nullptr;
    // The screen's margin, pixels.
    int margin = 32;
};

// The views this build draws: the speakers, the title card, the clock, "dark" (nothing lit until a touch
// lights the speakers) and "off" (the sentence the screen has always had). The title card is drawn only once
// a server has said a title: until then the screen shows the speakers (or the sentence), never an empty card.
[[nodiscard]] constexpr bool now_view_drawn(iclforge::nowview::View view) {
    return view == iclforge::nowview::View::kSpeakers || view == iclforge::nowview::View::kTitle ||
           view == iclforge::nowview::View::kClock ||
           view == iclforge::nowview::View::kMeters ||
           view == iclforge::nowview::View::kDark ||
           view == iclforge::nowview::View::kOff;
}

// Builds the speakers view into `parent`, an empty stage of `width` by `height`, for the room `now`
// describes. False, building nothing, when the board does not know its layout.
bool now_build_speakers(lv_obj_t* parent, int width, int height, const NowPlaying& now, const NowStyle& style);

// Brings the glow up to date from the levels in `now`. Cheap to call every frame: it does its work at
// most five times a second, and only an output whose step changed is redrawn.
void now_update_speakers(const NowPlaying& now, std::uint64_t now_us);

// Builds the title card into `parent`, an empty stage of `width` by `height`: the title in two rows at
// most, the artist and the third line in one each, the block in the middle of the stage. The three are as
// the screen can draw them (drawable_title()); an empty one is left out. False, building nothing, when
// there is no title.
bool now_build_title(lv_obj_t* parent, int width, int height, std::string_view title, std::string_view artist,
                     std::string_view detail, const NowStyle& style);

// Copies what the player's store holds into `out`: the strings only when the store's revision moved from
// the one `out` has, the song's position and length every time. `held` is null while nothing is known.
void now_copy_title(NowTitle& out, const iclforge::nowview::TitleStore* held, std::optional<std::int64_t> position_ms);

// The third line of the card: the album, with the year after it when there is one.
[[nodiscard]] LvString now_title_detail(const iclforge::nowview::TitleStore& held);
// Builds the clock view into `parent`, an empty stage of `width` by `height`: the time in the semibold face at
// 200 px, the date under it and, for a board with no time zone, the sentence that says it is UTC. Nothing is
// drawn until now_update_clock() is given a time. False if the face did not come up.
bool now_build_clock(lv_obj_t* parent, int width, int height, const NowStyle& style);

// Brings the clock up to date from the board's reading: the time when the minute changes, the date when the
// day does, and nothing at all otherwise (a reading is a count of seconds; the view shows no seconds, so
// this redraws one rectangle once a minute). A reading with no time, before a time server has answered,
// clears what was drawn and draws nothing in its place. Cheap to call every frame.
void now_update_clock(const iclforge::wallclock::Reading& reading);

// The stage was cleared: forget the objects that were on it.
void now_forget();

// A key that says whether the room changed (which slots, what they are called and where), so that the
// view is rebuilt for a new layout and left alone while only the levels move.
[[nodiscard]] std::uint32_t now_room_key(const NowPlaying& now);

}  // namespace player::panel
