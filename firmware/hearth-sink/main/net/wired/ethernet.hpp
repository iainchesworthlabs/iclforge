#pragma once

#include <optional>
#include <string>

// The cable, as an engine: the chip's own Ethernet MAC through an RMII PHY, an interface for it,
// and DHCP on it (net/wired/ethernet.cpp). Like station.hpp it is not the seam:
// net/dual/network.cpp chooses between the cable and the WiFi station by what each holds.
//
// The driver sees the link come and go by itself - the PHY is asked every second whether a partner
// answers - and the interface asks for an address again each time one does, so none of this needs
// calling when a cable is pulled and put back.

namespace player::net {

struct EthernetInfo {
    // The driver is started: the interface is not switched off.
    bool running = false;
    // A cable with a partner on it.
    bool link = false;
    std::string address;
    std::string mac;
    // What the link negotiated, while there is one.
    std::optional<int> speed_mbps;
    std::optional<bool> full_duplex;
};

// Makes the MAC, the PHY, the driver and the interface the first time, and starts the driver. Does
// not wait for a cable or an address (ethernet_wait does). False, having said why on the console,
// when the PHY did not answer on the bus - a board with the pins but no PHY on them - after which
// it is not tried again until the next boot.
[[nodiscard]] bool ethernet_start();

// Stops the driver: the link goes down and the interface lets go of its address. A later
// ethernet_start() brings it back.
void ethernet_stop();

[[nodiscard]] bool ethernet_running();
[[nodiscard]] bool ethernet_link();
// The interface holds an address now.
[[nodiscard]] bool ethernet_ready();
[[nodiscard]] std::string ethernet_address();
[[nodiscard]] EthernetInfo ethernet_info();

// Waits for a link for up to `link_ms` and then, if one came, for an address for up to
// `address_ms`. True when the interface holds an address. A cable that is not there costs only
// `link_ms`.
[[nodiscard]] bool ethernet_wait(unsigned link_ms, unsigned address_ms);

}  // namespace player::net
