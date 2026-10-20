#pragma once

// The IP stack and the default event loop, which every network interface needs and ESP-IDF
// lets be made once (esp_event_loop_create_default() answers ESP_ERR_INVALID_STATE the second
// time, which ESP_ERROR_CHECK makes a reboot). Called by whichever interface comes up first, and
// by the second one too: only the first call does anything. A server's first socket fails an lwIP
// assertion if nothing has initialised the stack, so a build with a network calls it whether or
// not there is anything to join.

namespace player::net {

void stack_start();

}  // namespace player::net
