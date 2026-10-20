#pragma once

#include <string>

// What the panel's developer view (panel/lvgl/dev_screen.cpp) asks of the application that no seam
// has: the output layout in force, the profile this image was built as, and the figures of the last
// play from the control surface (POST /play) that GET /status reports as us_per_frame and the rest.
// Defined in hearth_sink.cpp, which holds all of it, and read only by a build with a panel.

namespace player {

struct BoardReport {
    // The layout the next play will use, as GET /layout has it.
    std::string layout;
    // Which locked profile this image was built as, and a hash of what it locks (GET /hardware).
    std::string profile;
    std::string profile_hash;
    // A play from the control surface has run, or is running: the figures below mean something.
    bool played = false;
    unsigned long long us_per_frame = 0;
    unsigned long long worst_frame_us = 0;
    unsigned long long sink_us_per_frame = 0;
    unsigned long long render_us_per_frame = 0;
};

// Called from the panel's task with its lock held: waits a few milliseconds for the player's lock and
// leaves the layout and the figures empty if it is not free, rather than hold the screen up.
[[nodiscard]] BoardReport board_report();

}  // namespace player
