#pragma once

#include <cstddef>
#include <string>

#include "iclforge/dev_view.hpp"

// What the developer view reads of the board and what it asks of it (dev_screen.cpp,
// planning/karaoke-p4x-panel.md, "The developer view"). A seam of its own, like board_info.hpp, so
// that the view builds on a computer (preview/): the firmware's definitions are in dev_info.cpp
// (and panel.cpp, which adds its own counters), over the settings, the network, the Sendspin player,
// the heap and the console's log ring; the preview's are fixtures.

namespace player {
struct PanelStats;
}

namespace player::panel {

struct TouchCounters;

// Firmware only (dev_info.cpp), under panel.cpp's read_dev_facts(): what the board knows, with the
// panel's own counters, which panel.cpp holds, added to it.
[[nodiscard]] iclforge::devview::Facts gather_dev_facts(const PanelStats& panel, const TouchCounters& touch);

// Everything the view shows, about twice a second. Called from the panel's task with LVGL's lock
// held, so it takes no lock of the panel's and waits for nothing for long.
[[nodiscard]] iclforge::devview::Facts read_dev_facts();

// The console's last `limit` bytes (the ring GET /log serves), or nothing where there is no ring.
[[nodiscard]] std::string read_log_tail(std::size_t limit);

// Asks for the network preference to change, with the call PUT /network-preference makes
// (network_set_preference). The change waits for the interface it names to come up, which is
// seconds, so it runs on a task of its own and the result shows in Facts::net_change. False when
// another change is still under way or the task could not be made.
bool request_network_preference(iclforge::devview::Preference preference);

}  // namespace player::panel
