// The wall clock of the clock view. See wall_clock.hpp and planning/p4x-now-playing.md, stage 3.
//
// SNTP is lwIP's client, through esp_sntp.h. It is started only while the clock view is the chosen one and
// stopped, with the time it had forgotten, when another view is (the board contacts no time server
// otherwise). The servers are the ones the router gave over DHCP (option 42: stored by lwIP when the lease is
// bound, which net/stack.cpp turns on at boot) and, behind them, pool.ntp.org.

#include "wall_clock.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "lwip/ip_addr.h"
#include "sdkconfig.h"

#include "../../network.hpp"

namespace player::panel {
namespace {

namespace wc = iclforge::wallclock;

// The name lwIP is given: it keeps the pointer, so this lives for the whole run.
constexpr const char kPool[] = "pool.ntp.org";

// Guards the TZ environment (setenv is not safe against a concurrent localtime) and the fields below it.
std::mutex g_lock;
std::string g_zone;  // what TZ is set to; empty is none
bool g_events = false;
// How many answers have been said on the console: each one is, so that the log has the hour's own.
std::uint32_t g_reported_syncs = 0;

std::atomic<bool> g_wanted{false};
std::atomic<bool> g_running{false};
// Set on lwIP's thread by the sync callback and read on the panel's.
std::atomic<bool> g_synced{false};
std::atomic<std::uint32_t> g_syncs{0};
std::atomic<std::uint32_t> g_starts{0};

// --- the zone --------------------------------------------------------------------------------------------

[[nodiscard]] wc::LocalTime local_of(const struct tm& tm) {
    return wc::LocalTime{tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, tm.tm_wday};
}

// What the C library says the offset from UTC is at `at`, seconds east, with TZ as it stands.
[[nodiscard]] bool library_offset(std::int64_t at, int& east_s) {
    const time_t when = static_cast<time_t>(at);
    struct tm tm{};
    if (localtime_r(&when, &tm) == nullptr) {
        return false;
    }
    east_s = wc::offset_of(local_of(tm), at);
    return true;
}

void set_tz(const std::string& zone) {
    (void)setenv("TZ", zone.empty() ? "UTC0" : zone.c_str(), 1);
    tzset();
}

// With g_lock held.
[[nodiscard]] bool apply_zone_locked(std::string_view zone) {
    if (zone.empty()) {
        g_zone.clear();
        set_tz(g_zone);
        return true;
    }
    const auto offsets = wc::parse_time_zone(zone);
    if (!offsets) {
        return false;
    }
    const std::string previous = g_zone;
    g_zone.assign(zone.data(), zone.size());
    set_tz(g_zone);
    int january = 0;
    int july = 0;
    if (library_offset(wc::kProbeJanuary, january) && library_offset(wc::kProbeJuly, july) &&
        wc::zone_agrees(*offsets, january, july)) {
        return true;
    }
    std::printf("wall clock: the C library reads '%.*s' as %+d s in January and %+d s in July, not as it is "
                "written; refused\n",
                static_cast<int>(zone.size()), zone.data(), january, july);
    g_zone = previous;
    set_tz(g_zone);
    return false;
}

// --- the time server -------------------------------------------------------------------------------------

// On lwIP's thread: no printing here, which has a small stack; the panel's task says it when it reads.
void on_sync(struct timeval* /*tv*/) {
    ++g_syncs;
    g_synced = true;
}

// lwIP's server table, as text, for the console: the router's (an address) and the pool (a name).
[[nodiscard]] std::string describe_servers() {
    std::string text;
    for (u8_t i = 0; i < CONFIG_LWIP_SNTP_MAX_SERVERS; ++i) {
        const char* const name = esp_sntp_getservername(i);
        const ip_addr_t* const address = esp_sntp_getserver(i);
        std::string entry;
        std::array<char, 48> buffer{};
        const bool have_address = address != nullptr && !ip_addr_isany(address) &&
                                  ipaddr_ntoa_r(address, buffer.data(), static_cast<int>(buffer.size())) != nullptr;
        if (name != nullptr) {
            // A name, and the address lwIP has resolved it to once it has (nothing is resolved before the
            // first request goes out).
            entry = name;
            if (have_address) {
                entry += std::string(" [") + buffer.data() + "]";
            }
        } else if (have_address) {
            entry = std::string(buffer.data()) + " (from DHCP)";
        }
        if (!entry.empty()) {
            text += text.empty() ? "" : ", ";
            text += entry;
        }
    }
    return text.empty() ? std::string("none") : text;
}

// The pool goes in the first free slot, behind whatever the router gave. A lease bound after this
// (renewed, or a new address) empties the slots above the router's, so it is put back whenever the board
// gets an address as well as when it starts asking.
void put_pool_behind_dhcp() {
    bool present = false;
    int free_slot = -1;
    for (u8_t i = 0; i < CONFIG_LWIP_SNTP_MAX_SERVERS; ++i) {
        const char* const name = esp_sntp_getservername(i);
        if (name != nullptr) {
            present = present || std::strcmp(name, kPool) == 0;
        } else if (free_slot < 0 && ip_addr_isany(esp_sntp_getserver(i))) {
            free_slot = i;
        }
    }
    if (!present && free_slot >= 0) {
        esp_sntp_setservername(static_cast<u8_t>(free_slot), kPool);
    }
}

// With g_lock held, and the board holding an address (the IP stack is up).
void start_locked() {
    if (g_running) {
        return;
    }
    g_synced = false;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    sntp_set_time_sync_notification_cb(&on_sync);
    put_pool_behind_dhcp();
    esp_sntp_init();
    g_running = true;
    ++g_starts;
    std::printf("wall clock: asking for the time: %s\n", describe_servers().c_str());
}

void stop_locked() {
    if (!g_running) {
        return;
    }
    esp_sntp_stop();
    g_running = false;
    // The clock keeps counting after the client is stopped, but nothing is correcting it: a time kept for
    // days would be one the board made up, so the next time the view is chosen starts from nothing.
    g_synced = false;
    std::printf("wall clock: no longer asking a time server\n");
}

// An address on either interface: the router may have given other servers, and the pool slot may have been
// emptied by the lease; and, if no server has answered yet, ask again at once and not at the client's own
// back-off.
void on_got_ip(void* /*arg*/, esp_event_base_t /*base*/, int32_t /*id*/, void* /*data*/) {
    const std::lock_guard lock(g_lock);
    if (!g_wanted) {
        return;
    }
    if (!g_running) {
        start_locked();
        return;
    }
    put_pool_behind_dhcp();
    if (!g_synced) {
        (void)sntp_restart();
        std::printf("wall clock: a new address; asking again: %s\n", describe_servers().c_str());
    }
}

}  // namespace

void wall_clock_start(std::string_view zone) {
    const std::lock_guard lock(g_lock);
    if (!apply_zone_locked(zone)) {
        std::printf("wall clock: the stored zone is not one the C library reads; UTC\n");
        (void)apply_zone_locked("");
    }
    if (!g_events) {
        const esp_err_t eth = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &on_got_ip, nullptr);
        const esp_err_t sta = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_got_ip, nullptr);
        g_events = eth == ESP_OK && sta == ESP_OK;
        if (!g_events) {
            std::printf("wall clock: could not listen for an address; the clock starts when it is chosen\n");
        }
    }
}

void wall_clock_follow_view(iclforge::nowview::View view) {
    const bool wanted = view == iclforge::nowview::View::kClock;
    const std::lock_guard lock(g_lock);
    g_wanted = wanted;
    if (!wanted) {
        stop_locked();
        return;
    }
    // Without an address there is nothing to ask over (and, on a build with no network, no IP stack to
    // start the client on): the address event starts it.
    if (network_ready()) {
        start_locked();
    }
}

bool wall_clock_set_zone(std::string_view zone) {
    const std::lock_guard lock(g_lock);
    return apply_zone_locked(zone);
}

iclforge::wallclock::Reading wall_clock_read() {
    wc::Reading out;
    if (!g_synced) {
        return out;
    }
    const std::lock_guard lock(g_lock);
    if (const std::uint32_t syncs = g_syncs; syncs != g_reported_syncs) {
        g_reported_syncs = syncs;
        std::printf("wall clock: a time server answered (%s)\n", describe_servers().c_str());
    }
    const time_t now = time(nullptr);
    if (static_cast<std::int64_t>(now) < wc::kNotBefore) {
        return out;
    }
    struct tm tm{};
    if (localtime_r(&now, &tm) == nullptr) {
        return out;
    }
    out.known = true;
    out.local = local_of(tm);
    out.zone_set = !g_zone.empty();
    return out;
}

WallClockStats wall_clock_stats() {
    WallClockStats out;
    out.wanted = g_wanted;
    out.running = g_running;
    out.synced = g_synced;
    out.syncs = g_syncs;
    out.starts = g_starts;
    return out;
}

}  // namespace player::panel
