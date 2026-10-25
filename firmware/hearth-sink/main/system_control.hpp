#pragma once

#include <optional>
#include <string>

#include "iclforge/system_actions.hpp"

// What the System group of the board's Preferences does to the board (planning/p4x-preferences.md, E4),
// as one set of calls that the screen and the REST routes both make, so that a restart or a reset from
// the screen is the restart or the reset a route makes, with the same refusals and the same words. The
// words themselves - the confirming steps, the reasons - are libs/device's (iclforge/system_actions.hpp).
//
// Defined in hearth_sink.cpp, which owns the command queue, the firmware and the player. Safe from any
// task. Each returns nothing when it is done (for the restart and the factory reset: when it is under
// way, the restart itself following a moment later, after whoever asked has had its answer), and
// otherwise why it was not done, in words a person reads.

namespace player {

// The board as the System group needs it: whether a stream plays, what the firmware is doing, what
// networks the board has and whether the cable is plugged in, how many servers it is paired with. The
// network part is a call across SDIO on a board whose radio is a companion chip (about 40 ms): asked
// when a confirming step opens, not every refresh.
[[nodiscard]] iclforge::sysactions::Situation system_situation();

// Restarts the board through the firmware, as POST /restart does: servers hear it is going, and a P4
// resets the whole system (system_reset.cpp). Refused while an update is under way and while the running
// image is still on trial, when a restart would go back to the image before it.
[[nodiscard]] std::optional<std::string> system_restart();

// Reset preferences: the preferences go back to the image's defaults and the screen, the clock and the
// rest follow at once. The name, the network, the wiring, the layout and the pairings stay.
[[nodiscard]] std::optional<std::string> system_reset_preferences();

// Factory reset: the pairings, then every setting, then the WiFi driver's own copy of the network, and
// the board restarts as a new one. On a board with no cable that has nothing to join afterwards: the
// confirming step (iclforge::sysactions::factory_reset_confirm) says so, and what brings it back.
[[nodiscard]] std::optional<std::string> system_factory_reset();

}  // namespace player
