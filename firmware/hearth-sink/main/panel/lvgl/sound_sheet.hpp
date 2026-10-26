#pragma once

#include <cstdint>

#include "lvgl.h"

#include "iclforge/preferences.hpp"

// The Speakers sheet (planning/hearth-speaker-trim-identify.md, planning/p4x-preferences.md): the page the Preferences'
// "Speaker levels and distances" row opens. The room as the layout in force draws it (iclforge/render/room_plan.hpp,
// read-only here) with the speaker the test tone is on lit, a tap on a speaker playing the tone through that speaker
// and choosing it; and for the chosen speaker its level (-12 to +12 dB, a half decibel a step) and its distance from the
// listener (0.1 to 10 m, a tenth or a metre a step), a button that plays it, one that plays them all in turn, and one
// that puts the speaker back. On a display too narrow for a room the speakers are a row of buttons instead.
//
// What a speaker's level and distance are, the words for them and the steps a finger takes are libs/render's
// (iclforge/render/speaker_trim.hpp, tested on the host); what the board does about them - the tone, the trim, the
// reasons it refuses - is main/sound.hpp's, the same code the web page's routes go through. This file draws and carries a
// tap to those. It owns no thread: the Preferences sheet calls sound_sheet_update() once a frame, with LVGL locked.
//
// THE TONE STOPS WHEN THE SHEET IS LEFT. Back, the Preferences sheet being closed, and the Preferences putting
// themselves away after a minute all send "off"; and the player ends the tone of its own accord at once when a stream
// starts and after a minute whatever is asked.

namespace player::panel {

// Asks for the sheet to open (from a tap): it is built by the next sound_sheet_update(), outside the event.
void sound_sheet_open();

// Whether the sheet is where this entry of the Preferences goes: Identify speakers (which plays the tone and shows it) and
// Speaker levels and distances. One view for both.
[[nodiscard]] bool sound_sheet_serves(const iclforge::prefs::Pref& pref);

// The sheet is up or about to be: the Preferences sheet under it takes no touches of its own.
[[nodiscard]] bool sound_sheet_active();

// Takes the sheet down now (the Preferences sheet is being closed or has put itself away) and stops the tone. Safe to
// call when the sheet is not up: it still sends "off", for the Identify speakers button of the Preferences list.
void sound_sheet_close();

// The sheet's root object while it is up, for the preview tool's checks; null otherwise.
[[nodiscard]] lv_obj_t* sound_sheet_root();

// Once a frame: builds the sheet when it was asked for, takes it down when it was closed, rebuilds it when the layout
// changed under it, and repaints what moved (the lit speaker follows the tone within a fifth of a second).
void sound_sheet_update(std::uint32_t now_ms);

}  // namespace player::panel
