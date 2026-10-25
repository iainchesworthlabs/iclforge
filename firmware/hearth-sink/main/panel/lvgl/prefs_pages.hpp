#pragma once

#include <cstdint>
#include <string>

#include "lvgl.h"

#include "iclforge/preferences.hpp"

// The pages the System group of Preferences puts over the sheet (planning/p4x-preferences.md, E4): the confirming step
// before an action or a change that asks first (Restart, Reset preferences, Factory reset with its held button, the
// switch between the cable and Wi-Fi, Forget on a paired server), the keyboard that types the player's name, About,
// and the list of paired servers.
//
// The words, the keys and the rules are libs/device's and tested there (iclforge/system_actions.hpp,
// device_name.hpp, about_view.hpp); this file draws them in the sheet's own style (prefs_screen.hpp: prefs_ui()) and at
// whatever size the display is, and carries a tap to iclforge::prefs::set_value(). A page owns no thread: the sheet
// calls pages_update() once a frame, with LVGL locked, and a page is built and deleted there and never inside an
// event.

namespace player::panel {

// Opens the page `pref` wants, for the word the person chose (the choice tapped, or "run"): a confirming step for an
// action or a choice that asks first, the keyboard for a text, About for an info entry, the list for a list. Does
// nothing while a page is open.
void pages_open(const iclforge::prefs::Pref& pref, const std::string& word);

// A page is up (or about to be): the sheet under it takes no touches and does not put itself away.
[[nodiscard]] bool pages_active();

// Takes the pages down (the sheet is being closed).
void pages_close();

// The root of the page that is up, for the preview tool's checks; null when none is.
[[nodiscard]] lv_obj_t* pages_root();

// Once a frame: builds a page that was asked for, takes down one that was closed, repaints what moved, and carries the
// held button of the factory reset.
void pages_update(std::uint32_t now_ms);

}  // namespace player::panel
