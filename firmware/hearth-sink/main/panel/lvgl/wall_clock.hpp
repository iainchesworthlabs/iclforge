#pragma once

#include <cstdint>
#include <string_view>

#include "iclforge/now_view.hpp"
#include "iclforge/wall_clock.hpp"

// The wall clock of the clock view (planning/p4x-now-playing.md, stage 3): the board has no time of its own,
// so this asks for it, and only while the clock view is the one chosen. A time zone is the string the owner
// gave (iclforge/wall_clock.hpp), applied to the C library's TZ so that localtime() answers for it.
//
// Safe from any task: the zone and the time server's state are guarded by one lock, and the SNTP client's
// own calls are lwIP's, which marshal themselves onto its thread.

namespace player::panel {

struct WallClockStats {
    // The clock view is the one chosen, so a time server is wanted.
    bool wanted = false;
    // The SNTP client is started: the board is asking, or about to.
    bool running = false;
    // A time server has answered since the client was last started, and the time is one that can be shown.
    bool synced = false;
    // Answers heard since boot, and how many times the client was started.
    std::uint32_t syncs = 0;
    std::uint32_t starts = 0;
};

// At boot: applies the stored zone (an empty string is none, and the clock says UTC) and listens for an
// address, so that a clock chosen before the network is up starts asking when it is. Does not ask anyone.
void wall_clock_start(std::string_view zone);

// A view was chosen (a tap on the sheet, PUT /now-playing, or the stored one at boot): the clock view starts
// the SNTP client if it is not running, and every other view stops it and forgets the time it had.
void wall_clock_follow_view(iclforge::nowview::View view);

// Reads `zone` ("" for none) the way the C library does and applies it, or refuses it, changing nothing.
// A zone the library reads differently from iclforge::wallclock::parse_time_zone is refused.
[[nodiscard]] bool wall_clock_set_zone(std::string_view zone);

// What the clock says now: no time until a time server has answered, and none before 2026.
[[nodiscard]] iclforge::wallclock::Reading wall_clock_read();

[[nodiscard]] WallClockStats wall_clock_stats();

}  // namespace player::panel
