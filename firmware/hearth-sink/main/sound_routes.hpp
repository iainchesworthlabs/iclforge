#pragma once

// The routes of the board's sound preferences (sound.hpp): GET and PUT /trim, GET and POST /identify. hearth_sink.cpp hands
// them to the control surface; they are the same code the Preferences and the Speakers sheet go through
// (sound_set_trim_text(), sound_identify_request()), so that a person on the glass and one on the web page are refused in
// the same words and for the same reasons.

#include <optional>
#include <string>
#include <string_view>

#include "iclforge/control.hpp"

namespace player {

// --- the routes ---------------------------------------------------------------------------------------

// GET /trim: the level and distance of each speaker the layout in force has at an output, with what is really in force
// there (a server's settings can differ) and what this build can do. Nothing on a board with no player.
[[nodiscard]] std::optional<std::string> sound_trim_json();

// PUT /trim: statements that change the table (SpeakerTrimTable::apply_text), taken whole or not at all, stored, and
// handed to the player, which puts them in force at its next chunk. Nothing on success.
[[nodiscard]] std::optional<iclforge::ControlRefusal> sound_set_trim(std::string_view body);

// GET /identify: what the test tone is doing, and the speakers it can play.
[[nodiscard]] std::optional<std::string> sound_identify_json();

// POST /identify: off, all, a speaker, or a speaker and loop. Refused over a stream, and over a play from the
// control surface. Nothing on success.
[[nodiscard]] std::optional<iclforge::ControlRefusal> sound_identify(std::string_view body);

}  // namespace player
