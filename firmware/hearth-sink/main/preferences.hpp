#pragma once

#include "iclforge/preferences.hpp"

// The preferences' side of this board (iclforge/preferences.hpp): the one Backend, over the settings_* functions
// (settings.hpp) and whatever acts on a change at once (the panel's view and dim time). The screen's Preferences
// sheet, GET /preferences and the routes that set the same values all go through it, so a value is stored and
// followed in one place.
//
// A new preference adds its getter and setter to iclforge::prefs::Backend and its implementation here.

namespace player {

// Lives for the life of the program.
[[nodiscard]] iclforge::prefs::Backend& preferences();

}  // namespace player
