#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "iclforge/wall_clock.hpp"

// The board's time of day (planning/p4x-now-playing.md, stage 3). A board has no clock of its own, so on
// every ESP board that has a network (net/wifi/ and net/dual/: all the hardware) this keeps one with a time
// server, over lwIP's SNTP client, from the moment the board holds an address, and says what the time is in
// the zone the board was given. By default it is on. The server is the one a person named (PUT /ntp-server),
// else the one the router gave over DHCP (option 42), and pool.ntp.org behind it when nobody named one; a
// board that must contact none is told `off` (PUT /time-sync). The settings are settings.hpp's (time_zone,
// ntp_server, time_sync), read once here at boot and followed at once when they change.
//
// A seam CMake resolves, like the network beside it:
//
//   clock/sntp/   the client above: a build with a Wi-Fi station and/or a cable.
//   clock/none/   a build with no network, and QEMU's, which has no time server to ask and must not depend
//                 on one: every function here does nothing, and the routes answer 404.
//
// Safe from any task: the zone and the client's state are guarded by one lock, and the SNTP calls are lwIP's,
// which marshal themselves onto its thread.

namespace player {

struct WallClockStats {
    // The board keeps its time with a time server (time_sync is on).
    bool enabled = false;
    // The SNTP client is started: the board is asking, or about to.
    bool asking = false;
    // A time server has answered since the client was last started, and the time is one that can be shown.
    bool synced = false;
    // Answers heard since boot, and how many times the client was started.
    std::uint32_t syncs = 0;
    std::uint32_t starts = 0;
};

// Whether this build keeps time at all (clock/sntp/) or not (clock/none/).
[[nodiscard]] bool wall_clock_built();

// At boot, after the network has been asked to come up: applies the stored settings, listens for an address
// (so a network that comes up later, or a new address, is followed), and, with sync on and an address held,
// starts asking. Once; asks nobody until the board has an address.
void wall_clock_start();

// Reads `zone` ("" for none: UTC) the way the C library does and applies it, or refuses it, changing nothing.
// A zone the library reads differently from iclforge::wallclock::parse_time_zone is refused. Not stored: the
// settings are the caller's to keep.
[[nodiscard]] bool wall_clock_set_zone(std::string_view zone);

// The time server a person named ("" for none), and whether to ask one at all; followed at once. Not stored.
void wall_clock_set_server(std::string_view server);
void wall_clock_set_sync(bool on);

// What the time is now: no time until a time server has answered, and none before 2026.
[[nodiscard]] iclforge::wallclock::Reading wall_clock_read();

[[nodiscard]] WallClockStats wall_clock_stats();

// The servers the client is set to ask, as text for the console and the page: "192.168.1.5 (from DHCP),
// pool.ntp.org", with the address a name resolved to in brackets once it has.
[[nodiscard]] std::string wall_clock_servers();

}  // namespace player
