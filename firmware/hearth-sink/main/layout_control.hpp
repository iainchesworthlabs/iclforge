#pragma once

#include <string>
#include <string_view>

#include "iclforge/control.hpp"

// The speaker layout, as the board holds it (hearth_sink.cpp): what GET /layout answers and what PUT /layout does.
// The Preferences list (preferences.cpp) goes through the same two functions, so a layout chosen on the screen is
// judged, queued, applied to a running stream and stored exactly as one sent to the route is.

namespace player {

// The layout in force, or the one the next play uses: the name or the list as it was given.
[[nodiscard]] std::string layout_text();

// What PUT /layout does with a text. It is parsed, then judged by iclforge::evaluate_layout() over the board's
// capabilities (board_caps()) - the verdict GET /layout-options and the Preferences list give - and refused with
// that verdict's reason when the board cannot carry it. An accepted layout goes to the Sendspin player (which takes
// it at its next burst boundary, without stopping the stream), to the queue app_main drains (the next POST /play)
// and to the stored settings (it survives a restart); a layout with a limit on it is accepted with the limit as
// the note. Safe from any task.
[[nodiscard]] iclforge::ControlLayoutAnswer layout_set(std::string_view text);

// Whether a stream is playing now: a server's over Sendspin, or a play started with POST /play. A layout change
// breaks the audio of one that is (the sink is opened again), which the Preferences list says before it applies one.
[[nodiscard]] bool playing();

}  // namespace player
