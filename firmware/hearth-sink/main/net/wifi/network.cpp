// A WiFi station alone: the network a board without a cable has. See ../../network.hpp; the station
// itself is station.hpp's.

#include "network.hpp"

#include "station.hpp"

namespace player {

bool network_up() {
    return net::station_up();
}

bool network_ready() {
    return net::station_ready();
}

std::string network_address() {
    return net::station_address();
}

NetworkLink network_link() {
    NetworkLink link;
    link.kind = "wifi";
    const net::StationInfo station = net::station_info();
    link.ssid = station.ssid;
    link.rssi_dbm = station.rssi_dbm;
    return link;
}

const char* network_source() {
    return net::station_source();
}

void network_adopt_built_in() {
    net::station_adopt_built_in();
}

void network_forget_driver_state() {
    net::station_forget_driver_state();
}

// One network: nothing to choose between, and the station is the network.
bool network_has_choice() {
    return false;
}

bool network_set_preference(NetworkPreference) {
    return false;
}

bool network_wifi_ready() {
    return net::station_ready();
}

bool network_wifi_up() {
    return net::station_up();
}

}  // namespace player
