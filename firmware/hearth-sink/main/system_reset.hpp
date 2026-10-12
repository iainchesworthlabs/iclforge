// How this firmware restarts a board whose Wi-Fi is behind esp_hosted. See system_reset.cpp.
#pragma once

namespace player {

// Once, early in app_main, before anything else registers a shutdown handler (this one has to run
// last): on a board whose Wi-Fi is a companion chip over SDIO (the ESP32-P4's C6), every
// esp_restart() - this application's own, the update's, esp_hosted's when its link to the chip
// fails - ends in a reset of the whole system rather than of the CPU alone. Nothing on any other
// board.
void restart_through_system_reset();

}  // namespace player
