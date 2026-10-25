#pragma once

#include "iclforge/board_caps.hpp"

// What this board can do (iclforge/board_caps.hpp), filled in from the build and the hardware: the screen the
// panel opened, the sink's frame and slots, the networks, the clock, the object cap, and the chip. The one place
// that reads all of it, so that the screen, GET /preferences and the web page ask a single description and never
// "is this the P4X".
//
// A value, built afresh from the settings on every call (the slot width and the second line are the board's
// configuration and change with a PUT); the hardware part is gathered once. Safe from any task, and from the
// panel's task while it holds LVGL: it reads the panel's display facts, which are fixed once the panel is up,
// and never takes the panel's lock.

namespace player {

[[nodiscard]] iclforge::BoardCaps board_caps();

}  // namespace player
