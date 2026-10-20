// A cable and a WiFi station: the network a board with an Ethernet PHY has. See ../../network.hpp;
// the station is net/wifi/station.hpp and the cable net/wired/ethernet.hpp.
//
// THE POLICY, in one place. A board holds an address on the cable, on the station, on both or on
// neither, and the network preference (settings.hpp) says which of them it uses:
//
//   auto       both are brought up, the cable first. The board uses the cable while it holds an
//              address, and the station when only that does. A cable pulled out falls back to
//              the station at once - the station has been joined all along - and a cable put
//              back returns the board to it, with no restart: ESP-IDF's default route follows
//              whichever interface with an address has the higher route priority, and the cable's
//              is above the station's (ethernet.cpp, kRoutePriority).
//   ethernet   the cable alone. The station is switched off - no radio, nothing joined.
//   wifi       the station alone. The cable's driver is stopped.
//
// A preference for one interface leaves the board with that one and nothing to fall back to,
// which is what pinning is. So a change made while the board runs is checked before it is kept:
// the interface it names is brought up first, and the other is switched off only when the
// named one holds an address. A change that cannot be verified is undone, and the preference is
// what it was (apply()). A board that boots with a pin it cannot meet comes up with no network
// and keeps trying: it was told to.

#include "network.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net/stack.hpp"
#include "net/wifi/station.hpp"
#include "net/wired/ethernet.hpp"
#include "settings.hpp"

namespace player {
namespace {

// How long boot waits for a cable to show a link before it goes on with the station: a PHY
// with a partner on it has one in about two seconds (autonegotiation).
constexpr unsigned kLinkWaitMs = 4000;
// How long a cable with a link gets to take an address. DHCP on a quiet network answers inside a
// second; a slow switch that is still spanning-tree blocking its port takes up to thirty.
constexpr unsigned kAddressWaitMs = 30000;
// How long a change of preference waits before it acts, so that the reply to the request that
// made it, which may have come over the interface being switched off, leaves first.
constexpr TickType_t kReplyGrace = pdMS_TO_TICKS(750);

// network_up() at boot and the work a change of preference does take turns. Held for the whole of
// either, and so for as long as a join takes.
std::mutex g_mutex;

// The preference before the last change a person made, which an unverified change goes back to.
std::atomic<NetworkPreference> g_previous{NetworkPreference::automatic};
std::atomic<bool> g_apply_pending{false};
std::atomic<bool> g_applier_running{false};
std::atomic<bool> g_verify{false};

NetworkPreference preference() {
    return settings().network_preference;
}

bool cable_allowed(NetworkPreference p) {
    return p != NetworkPreference::wifi;
}

bool station_allowed(NetworkPreference p) {
    return p != NetworkPreference::ethernet;
}

enum class Active { none, ethernet, wifi };

// The interface the board uses: the cable when it is allowed and holds an address, else the
// station on the same terms.
Active active() {
    const NetworkPreference p = preference();
    if (cable_allowed(p) && net::ethernet_ready()) {
        return Active::ethernet;
    }
    if (station_allowed(p) && net::station_ready()) {
        return Active::wifi;
    }
    return Active::none;
}

// A task for work that must not hold the caller: joining the station can take a quarter of a
// minute, and the cable's address is wanted before that.
void spawn(const char* name, TaskFunction_t body) {
    if (xTaskCreate(body, name, 4096, nullptr, 3, nullptr) != pdPASS) {
        std::printf("warning: no room for the %s task\n", name);
    }
}

[[noreturn]] void join_task(void*) {
    // The station keeps trying a network it could not join, so there is nothing to do with the
    // answer: the board is on the cable.
    (void)net::station_up();
    vTaskDelete(nullptr);
    for (;;) {
    }
}

// Does what the stored preference asks. Called with g_mutex held.
void reconcile(bool verify) {
    const NetworkPreference want = preference();
    const NetworkPreference before = g_previous.load();
    const auto undo = [&](const char* why) {
        std::printf("network: %s; keeping the %s preference\n", why,
                    network_preference_name(before));
        if (settings_set_network_preference(before)) {
            g_apply_pending = true;
            g_verify = false;
        }
    };
    switch (want) {
        case NetworkPreference::automatic:
            // The station's interface before the cable's (station.hpp), then the cable, then
            // the station's join: the cable's address is not held up by it.
            if (net::station_configured()) {
                net::station_prepare();
            }
            (void)net::ethernet_start();
            if (net::station_configured() && !net::station_ready()) {
                spawn("wifi_join", &join_task);
            }
            return;
        case NetworkPreference::ethernet:
            if (!net::ethernet_start() || !net::ethernet_wait(kLinkWaitMs, kAddressWaitMs)) {
                if (verify) {
                    undo("no cable answered, so the WiFi station is not switched off");
                }
                return;
            }
            net::station_stop();
            return;
        case NetworkPreference::wifi:
            if (!net::station_configured() || !net::station_up()) {
                if (verify) {
                    undo("the WiFi station did not join, so the cable is not switched off");
                }
                return;
            }
            net::ethernet_stop();
            return;
    }
}

[[noreturn]] void apply_task(void*) {
    vTaskDelay(kReplyGrace);
    for (;;) {
        while (g_apply_pending.exchange(false)) {
            const bool verify = g_verify.exchange(false);
            const std::lock_guard lock(g_mutex);
            reconcile(verify);
        }
        g_applier_running = false;
        // A request that arrived as the loop ended asks again, if no one else has started the task.
        if (!g_apply_pending || g_applier_running.exchange(true)) {
            break;
        }
    }
    vTaskDelete(nullptr);
    for (;;) {
    }
}

void request_apply(bool verify) {
    g_verify = verify;
    g_apply_pending = true;
    if (!g_applier_running.exchange(true)) {
        if (xTaskCreate(&apply_task, "net_apply", 4096, nullptr, 3, nullptr) != pdPASS) {
            g_applier_running = false;
            std::printf(
                "warning: no room for the network preference task; it applies at the next boot\n");
        }
    }
}

}  // namespace

bool network_up() {
    const std::lock_guard lock(g_mutex);
    if (network_ready()) {
        return true;
    }
    settings_load();
    const NetworkPreference p = preference();
    const bool station = station_allowed(p) && net::station_configured();
    // The station's interface is made before the cable's, so that the cable's is the one lwIP
    // routes the local network through (station.hpp). The driver is not started: nothing is joined.
    if (station) {
        net::station_prepare();
    }
    bool cable = false;
    if (cable_allowed(p)) {
        cable = net::ethernet_start();
    }
    if (p == NetworkPreference::wifi) {
        return net::station_up();
    }
    if (cable && net::ethernet_wait(kLinkWaitMs, kAddressWaitMs)) {
        // On the cable already. The station is joined behind it, as the board's way out if the
        // cable goes, and nothing waits for it.
        if (station) {
            spawn("wifi_join", &join_task);
        }
        return true;
    }
    if (p == NetworkPreference::ethernet) {
        std::printf(
            "network: the preference is the cable alone and no cable gave the board an "
            "address\n");
        return false;
    }
    // No cable, or none that gave an address: the station, as a board without one would.
    if (!station) {
        std::printf(
            "network: no cable, and no WiFi network stored or built in; plug the cable in "
            "or provision the board\n");
        return false;
    }
    (void)net::station_up();
    return network_ready();
}

bool network_ready() {
    return active() != Active::none;
}

std::string network_address() {
    switch (active()) {
        case Active::ethernet:
            return net::ethernet_address();
        case Active::wifi:
            return net::station_address();
        case Active::none:
            break;
    }
    return {};
}

NetworkLink network_link() {
    const NetworkPreference p = preference();
    const net::StationInfo station = net::station_info();
    const net::EthernetInfo cable = net::ethernet_info();

    NetworkLink link;
    link.preference = network_preference_name(p);

    link.wifi.present = true;
    link.wifi.enabled = station_allowed(p);
    link.wifi.link = link.wifi.enabled && station.associated;
    link.wifi.address = link.wifi.enabled ? station.address : std::string{};
    link.wifi.mac = station.mac;
    link.wifi.configured = station.configured;
    link.wifi.ssid = station.ssid;
    link.wifi.rssi_dbm = station.rssi_dbm;

    link.ethernet.present = true;
    link.ethernet.enabled = cable_allowed(p);
    link.ethernet.link = cable.link;
    link.ethernet.address = link.ethernet.enabled ? cable.address : std::string{};
    link.ethernet.mac = cable.mac;
    link.ethernet.speed_mbps = cable.speed_mbps;
    link.ethernet.full_duplex = cable.full_duplex;

    // The interface in use; while none holds an address, the one the board is waiting on.
    switch (active()) {
        case Active::ethernet:
            link.kind = "ethernet";
            break;
        case Active::wifi:
            link.kind = "wifi";
            link.ssid = station.ssid;
            link.rssi_dbm = station.rssi_dbm;
            break;
        case Active::none:
            if (p == NetworkPreference::wifi) {
                link.kind = "wifi";
            } else if (p == NetworkPreference::ethernet || cable.link || !station.configured) {
                link.kind = "ethernet";
            } else {
                link.kind = "wifi";
            }
            break;
    }
    return link;
}

const char* network_source() {
    const char* const source = net::station_source();
    // A cable needs nothing stored to join, so an image that has no WiFi network still comes up.
    return std::strcmp(source, "none") == 0 ? "wired" : source;
}

void network_adopt_built_in() {
    net::station_adopt_built_in();
}

bool network_has_choice() {
    return true;
}

bool network_set_preference(NetworkPreference next) {
    std::unique_lock lock(g_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        std::printf("network: busy joining a network; try the preference again in a moment\n");
        return false;
    }
    const NetworkPreference before = preference();
    if (next == before) {
        return true;
    }
    if (next == NetworkPreference::wifi && !net::station_configured()) {
        std::printf(
            "network: no WiFi network is stored, so the preference cannot be the station alone\n");
        return false;
    }
    if (!settings_set_network_preference(next)) {
        return false;
    }
    g_previous = before;
    std::printf("network: preference %s, was %s\n", network_preference_name(next),
                network_preference_name(before));
    // Done off this task, after a moment, with the change checked before it is kept (the policy,
    // above): this is the control surface's, and the reply goes out over an interface that this may
    // be about to switch off.
    request_apply(next != NetworkPreference::automatic);
    return true;
}

bool network_wifi_ready() {
    return station_allowed(preference()) && net::station_ready();
}

bool network_wifi_up() {
    if (!station_allowed(preference())) {
        std::printf(
            "network: the WiFi station is switched off by the network preference; the "
            "network is stored for when it is on\n");
        return false;
    }
    return net::station_up();
}

}  // namespace player
