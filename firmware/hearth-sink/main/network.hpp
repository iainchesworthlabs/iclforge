#pragma once

#include <optional>
#include <string>

#include "settings.hpp"

// The board's network, as a seam CMake resolves - the same rule as
// byte_source.hpp and audio_sink.hpp beside it. Four implementations, one
// chosen per build in main/CMakeLists.txt from main/Kconfig.projbuild:
//
//   net/wifi/     a station on the access point the board was told about
//                 (settings.hpp, or Kconfig until something stores one). What
//                 a board has.
//   net/dual/     that station and a cable: the chip's Ethernet MAC through an
//                 RMII PHY (net/wired/), where the board has both. It chooses
//                 between them by the board's network preference
//                 (settings.hpp) and by which has an address. Every function
//                 below answers for the pair; the station alone is the same
//                 code as net/wifi/ (net/wifi/station.hpp).
//   net/openeth/  the OpenCores Ethernet MAC qemu-system-xtensa emulates,
//                 which `idf.py qemu` attaches to the host's network. What CI
//                 has - it is how the HTTP source runs end to end with no
//                 board and no access point, the host serving at 10.0.2.2.
//   net/none/     a build that plays from flash, FAT or an SD card and joins
//                 nothing.
//
// IT COMES UP AT BOOT, not at the first play. A sink is found before it is
// played to: mDNS has to be answering and the control surface reachable while
// the board sits idle, which is the whole of B2
// (planning/hearth-reference-player.md). The HTTP source still calls
// network_up() when it opens, and gets the network that is already up. A
// board with no network at boot gets one when Improv gives it one
// (provision.hpp), and app_main then starts what boot would have.

namespace player {

// Brings the network up and blocks until it holds an address, or returns
// straight away if it holds one now. False means there is no network yet - no
// build with one, nothing stored to join, or the station's quick retries
// failed to join it - and the implementation has already said which on the
// console.
//
// The WiFi station does not give up there. After a false return it goes on
// trying the same network with a growing wait, up to 15 s between tries, and
// it does the same for a network it has joined and then lost, such as an
// access point that restarts. network_ready() says when it is back.
//
// Safe to call again after it returns false, from any task, and while a lost
// network is being retried: the next call starts over with whatever is
// stored by then. That is how Improv moves a board that is not on a network
// onto the one it has just been given, without a restart. A board that holds
// an address keeps its network, and one stored meanwhile (PUT /network) is
// the one it joins at its next boot. Whatever it returns, a build with a
// network has its IP stack running once it has been called, so a server can
// listen before there is an address to reach it at.
[[nodiscard]] bool network_up();

// Whether the board holds an address on its network now - on either interface,
// on a board with a cable and a station. The WiFi station clears it when it
// loses its network or its address and sets it again when it rejoins, and so
// does the cable; QEMU's Ethernet keeps it once set. Asked by anything that wants
// to know whether to bother - mDNS, the Improv reply's device URL and state,
// app_main watching for a network that comes up after boot - rather than to
// bring the network up itself.
[[nodiscard]] bool network_ready();

// The address the board holds, as text ("192.168.1.45"), or empty when it
// holds none: the interface in use, on a board with two (the cable's while it
// has one, then the station's). For the URL an Improv client sends the user
// to, and for the console line that says where the page is.
[[nodiscard]] std::string network_address();

// One network interface of a board, for GET /status. Never the passphrase.
struct InterfaceStatus {
    // The build drives this interface at all.
    bool present = false;
    // The preference has not switched it off.
    bool enabled = false;
    // WiFi: associated with the access point. Ethernet: a cable with a link partner on it.
    bool link = false;
    // Its IPv4 address as text; empty while it holds none.
    std::string address;
    // The interface's own MAC address as text ("e8:f6:0a:e7:53:fb"), for a DHCP reservation.
    std::string mac;
    // WiFi only: a network is stored or built in, so the station has one to join.
    bool configured = false;
    // WiFi only, while associated: the access point's SSID and the signal from it.
    std::string ssid;
    std::optional<int> rssi_dbm;
    // Ethernet only, while there is a link: what was negotiated.
    std::optional<int> speed_mbps;
    std::optional<bool> full_duplex;
};

// What the board's network is, for GET /status (iclforge::ControlNetwork):
// "wifi" or "ethernet" - the interface the board is using, or while none holds
// an address the one it is trying - and, on WiFi, while the station is
// associated, the access point's SSID and the signal from it. `kind` is null in
// a build with no network. Safe from any task.
//
// On a build with both interfaces `preference` names the setting ("auto",
// "ethernet" or "wifi") and `wifi` and `ethernet` say how each stands; on every
// other build `preference` is null and they are left empty.
struct NetworkLink {
    const char* kind = nullptr;
    std::string ssid;
    std::optional<int> rssi_dbm;
    const char* preference = nullptr;
    InterfaceStatus wifi;
    InterfaceStatus ethernet;
};
[[nodiscard]] NetworkLink network_link();

// A board with both a cable and a station chooses between them (settings.hpp,
// NetworkPreference). False on every other build, which has one network or
// none, and where there is nothing to choose.
[[nodiscard]] bool network_has_choice();

// Stores the preference and acts on it at once, with no restart: the interface
// the preference leaves out is switched off, and the one it names (or both,
// for auto) is brought up. False, having said why on the console, when the board
// has no choice, when NVS refused the value, or when the choice would cut the
// board off from the network it is being asked over: `wifi` with no network
// stored, or a preference for an interface that holds no address now. The
// reply of PUT /network-preference is sent before the interface that carried
// it can go away.
[[nodiscard]] bool network_set_preference(NetworkPreference preference);

// Whether the WiFi station alone holds an address now, and the call that
// brings it up whatever the cable is doing. What Improv is about: a client hands
// the board a WiFi network, and the answer is whether the station joined it. On
// a build with the station alone these are network_ready() and network_up().
[[nodiscard]] bool network_wifi_ready();
[[nodiscard]] bool network_wifi_up();

// Where the network this board joins comes from, for GET /firmware
// (planning/esp32-ota.md): "stored" in NVS, "built-in" to this image alone
// (CONFIG_ICLFORGE_EXAMPLE_WIFI_SSID), "wired" for a network that needs
// nothing stored (QEMU's Ethernet), or "none". An image with no network
// built in - every image CI publishes - cannot rejoin a "built-in" one.
[[nodiscard]] const char* network_source();

// At boot, before the network comes up: a network built into this image and
// none stored is stored, so the board keeps it through an update to an image
// built without one. Does nothing when one is stored already, or with no
// network built in.
void network_adopt_built_in();

// For the factory reset (settings_forget()): erases what the WiFi driver keeps of its own about the
// network the board was given - ESP-IDF stores the SSID and passphrase in NVS each time it is handed
// them - so that a board handed on does not carry the passphrase in a namespace nothing of ours clears.
// Best effort, and a no-op on a build with no station. Does not drop the connection the board is on.
void network_forget_driver_state();

}  // namespace player
