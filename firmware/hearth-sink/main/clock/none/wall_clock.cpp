// No time of day: a build with no network (and QEMU's, which has nothing to ask). See ../../wall_clock.hpp.

#include "../../wall_clock.hpp"

namespace player {

bool wall_clock_built() { return false; }

void wall_clock_start() {}

bool wall_clock_set_zone(std::string_view /*zone*/) { return false; }

void wall_clock_set_server(std::string_view /*server*/) {}

void wall_clock_set_sync(bool /*on*/) {}

iclforge::wallclock::Reading wall_clock_read() { return {}; }

WallClockStats wall_clock_stats() { return {}; }

std::string wall_clock_servers() { return {}; }

}  // namespace player
