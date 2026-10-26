#pragma once

#include <cstdint>

#include "lvgl.h"

// The Speaker layout sheet (planning/p4x-layout-builder.md): the page the Preferences' "Speaker layout" row opens. Two ways
// in to one choice. QUICK PICKS are the layouts a home has, as chips in two groups (around you, with heights); BUILD
// YOURS is how many speakers are around you, how many subwoofers, how many heights and how the heights are mounted, as
// buttons. Both set the same working layout, drawn as the room it makes beside the buttons (iclforge/render/room_plan.hpp)
// and named in the footer, where Apply is. A layout this board cannot carry is greyed with the board's reason; a tap on
// it still shows the room it would be and says why it cannot be applied.
//
// What a layout is, which buttons are greyed and the sentence that says why are libs/device's
// (iclforge/layout_builder.hpp, over the one verdict in layout_options.hpp, tested on the host for every known board).
// This file measures the controls, draws them and carries a tap to those. It owns no thread: the Preferences sheet calls
// layout_sheet_update() once a frame, with LVGL locked.
//
// THE CONTROLS THAT DECIDE ARE NEVER OFF THE GLASS. The bar (Cancel) and the footer (the layout's name, Apply) are fixed;
// only the buttons between them scroll, and the room is drawn beside them where the display is wide enough for it and at
// the top of the scrolling part where it is not.

namespace player::panel {

// Asks for the sheet to open (from a tap): it is built by the next layout_sheet_update(), outside the event.
void layout_sheet_open();

// The sheet is up or about to be.
[[nodiscard]] bool layout_sheet_active();

// Takes the sheet down now (the Preferences sheet is being closed or has put itself away). Safe to call when it is not up.
void layout_sheet_close();

// The sheet's root object while it is up, for the preview tool's checks; null otherwise.
[[nodiscard]] lv_obj_t* layout_sheet_root();

// Once a frame: builds the sheet when it was asked for, takes it down when it was closed, and repaints what moved.
void layout_sheet_update(std::uint32_t now_ms);

}  // namespace player::panel
