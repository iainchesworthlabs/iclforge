#pragma once

#include <cstdint>

#include "lvgl.h"

#include "iclforge/preferences.hpp"
#include "iclforge/screen_metrics.hpp"

#include "dev_screen.hpp"

// The editors of the Preferences sheet (planning/p4x-preferences.md): the entries whose value is not one of a few
// buttons but a pair of times or a place in a list. An entry says which it wants (iclforge::prefs::Pref::editor); the
// sheet draws its value as a button and a tap on that opens the editor, which covers the sheet whole and has a Done of
// its own. What is chosen goes to the board through iclforge::prefs::set_value(), as a tap on any row does, so a
// refusal reads the same and the registry's rules (availability, the words a route takes) are the only ones.
//
// The editors measure from the same iclforge::screen::Metrics as the sheet: the same code at 1024 by 600, 800 by 480,
// 480 by 320 and 320 by 240. Like the sheet they are an object tree of their own, made when opened and deleted when
// closed, and own no thread: prefs_screen.cpp calls editor_update() once a frame, with LVGL locked.

namespace player::panel {

// What an editor is drawn with: the sheet's own sizes, colours and type, and the object it covers.
struct EditorKit {
    iclforge::screen::Metrics m;
    DevColours c;
    const lv_font_t* body = nullptr;   // buttons and rows
    const lv_font_t* note = nullptr;   // notes and reasons
    const lv_font_t* title = nullptr;  // a title
    lv_obj_t* parent = nullptr;        // the sheet's root, which an editor covers whole
};

// Opens the editor `pref` asks for. False, and nothing drawn, for an entry that wants none or when one is open.
[[nodiscard]] bool editor_open(const EditorKit& kit, const iclforge::prefs::Pref& pref);

// Puts the editor away at once (the sheet is going), keeping anything it has not yet written.
void editor_close();

[[nodiscard]] bool editor_is_open();

// The editor's root object while it is up, for the preview tool's checks; null otherwise.
[[nodiscard]] lv_obj_t* editor_root();

// Once a frame: takes the editor down when Done was tapped, makes what a tap asked for, writes a pair of times a
// moment after the last step of them (a held button steps ten times a second and must not write ten times), and
// brings what it shows up to date with the board.
void editor_update(std::uint32_t now_ms);

}  // namespace player::panel
