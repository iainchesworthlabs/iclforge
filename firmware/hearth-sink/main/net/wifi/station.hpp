#pragma once

#include <optional>
#include <string>

// The WiFi station, as an engine: the code that joins the network the board was told to join
// (settings.hpp, or Kconfig until something stores one), keeps trying it for as long as the board
// runs, and says whether the board holds an address on it. It is not the seam (network.hpp):
// net/wifi/network.cpp makes it one for a board with a station alone, and net/dual/network.cpp
// chooses between it and a cable. What each function does is what network.hpp says of the
// function it was before there were two.

namespace player::net {

struct StationInfo {
    // A network is stored or built in, so the station has one to join.
    bool configured = false;
    // Associated with the access point, with or without an address.
    bool associated = false;
    // The station's address as text; empty while it holds none.
    std::string address;
    std::string mac;
    // The access point's SSID and the signal from it, while the board holds an address.
    std::string ssid;
    std::optional<int> rssi_dbm;
};

// Whether there is a network to join: one stored in NVS, or the image's own.
[[nodiscard]] bool station_configured();

// Makes the station's interface and starts the driver, without joining anything. The station's
// interface is made here rather than at the first join so that a board with a cable can make it
// before the cable's: lwIP routes a destination on the local network through the interface made
// last (netif_add puts a new one at the head of its list), and the cable's is the one to prefer.
// Does nothing the second time.
void station_prepare();

// Starts the station on the stored network and blocks until the board holds an address or the
// quick retries are spent (network_up()'s description in network.hpp). False means no network
// yet, and the station goes on trying by itself. Safe to call again, from any task.
[[nodiscard]] bool station_up();

// Switches the station off: it leaves its network and stops trying it, and the radio is idle,
// until the next station_up(). What a preference for the cable alone asks for.
void station_stop();

// Holds an address now.
[[nodiscard]] bool station_ready();
[[nodiscard]] std::string station_address();
[[nodiscard]] StationInfo station_info();

// "stored", "built-in" or "none" (network_source() in network.hpp).
[[nodiscard]] const char* station_source();
void station_adopt_built_in();

// Erases what the WiFi driver keeps of its own about the network it was given (network.hpp:
// network_forget_driver_state()).
void station_forget_driver_state();

}  // namespace player::net
