#pragma once

#include <cstdint>
#include <string>

#include "lvgl.h"

#include "iclforge/screen_metrics.hpp"

#include "dev_screen.hpp"

// The Preferences sheet (planning/p4x-preferences.md): a scrollable list of everything a person chooses once and
// leaves, grouped (Display, Sound, Network, Time, Updates, System) and drawn from the registry
// (iclforge/preferences.hpp) for the board it is on. What a control is, which choices it has and whether this board
// can do them all come from the registry and the board's capability description; this file only draws them and
// carries a tap to iclforge::prefs::set_value(). A preference the board cannot support is shown greyed with its
// reason, never hidden.
//
// Every size comes from iclforge::screen::Metrics, which is derived from the display it is given: the sheet is the
// same code at 1024 by 600, 800 by 480, 480 by 320 and 320 by 240.
//
// Like the developer view it is an object tree of its own, built when it is opened and deleted when it is closed, so
// that the lyric screen is as it was with it shut; its fonts are made the first time it opens. It owns no thread:
// lyric_screen.cpp calls prefs_update() once a frame, with LVGL locked.

namespace player::panel {

// A control that is touched carries this flag, so that the preview tool can find every touch target on a sheet and
// hold it to the fingertip size (iclforge::screen::kMinTargetPx).
inline constexpr lv_obj_flag_t kTouchTargetFlag = LV_OBJ_FLAG_USER_1;

struct PrefsStats {
    std::uint32_t builds = 0;
    std::uint32_t build_us_max = 0;
    std::uint32_t refreshes = 0;
    std::uint32_t refresh_us_max = 0;
};

// Remembers the display's size and the tokens the screen is drawn in. Builds nothing.
void prefs_init(const iclforge::screen::Metrics& metrics, const DevColours& colours);

// Asks for the sheet to open (from a tap): it is built by the next prefs_update(), outside the event.
void prefs_open();

[[nodiscard]] bool prefs_is_open();

// The sheet's root object while it is up, for the preview tool's checks; null otherwise.
[[nodiscard]] lv_obj_t* prefs_root();

[[nodiscard]] PrefsStats prefs_stats();

// Once a frame, after the lyric screen's own work: builds the sheet when it was asked for, takes it down when it was
// closed or left alone for a minute, and brings what it shows up to date with the board's settings.
void prefs_update(std::uint32_t now_ms);

// --- for the pages the sheet opens (prefs_pages.hpp) --------------------------------------------------------------
// The sheet's tokens and pieces, so that a page put over it is drawn in the same style and at the same size.
struct PrefsUi {
    const iclforge::screen::Metrics* m = nullptr;
    const DevColours* c = nullptr;
    const lv_font_t* body = nullptr;
    const lv_font_t* note = nullptr;
    const lv_font_t* heading = nullptr;
    const lv_font_t* title = nullptr;
};
[[nodiscard]] PrefsUi prefs_ui();
lv_obj_t* prefs_box(lv_obj_t* parent);
lv_obj_t* prefs_label(lv_obj_t* parent, const std::string& text, const lv_font_t* font, std::uint32_t rgb);
lv_obj_t* prefs_paragraph(lv_obj_t* parent, const std::string& text, const lv_font_t* font, std::uint32_t rgb, int width);
// A touch target (kTouchTargetFlag) with `text` centred in it; its label is child 0.
lv_obj_t* prefs_button(lv_obj_t* parent, const std::string& text, int width, int height, std::uint32_t bg, std::uint32_t fg,
                       lv_event_cb_t callback, std::uintptr_t user_data, lv_obj_t** label_out);
// Paints a button as the sheet's choices are: chosen (the accent), plain, or greyed.
void prefs_style_button(lv_obj_t* button, bool chosen, bool available);
[[nodiscard]] int prefs_text_width(const lv_font_t* font, const std::string& text);
// What the sheet's bar says for a few seconds (a refusal, or the result of a change), and a request to bring the rows up
// to date at the next frame.
void prefs_say(const std::string& text);
void prefs_changed();

}  // namespace player::panel
