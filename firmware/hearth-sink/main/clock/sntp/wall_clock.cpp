// The board's time of day, kept with a time server. See ../../wall_clock.hpp and planning/p4x-now-playing.md,
// stage 3.
//
// SNTP is lwIP's client, through esp_sntp.h. lwIP keeps a table of servers; this keeps it the way the owner
// asked (a server a person named, then the router's, then the pool when nobody named one) against a lease that
// rewrites it: when a lease is bound lwIP puts the router's server in the first slot and empties the rest, so
// the table is looked at once a second and put back as it should be. The router's server is remembered from
// the first slot whenever it is seen there as an address (net/stack.cpp turns DHCP option 42 on in lwIP at
// boot, so it is in the table from the first lease on, whether or not anything was asking yet).

#include "../../wall_clock.hpp"

#include <algorithm>
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
#include "esp_timer.h"
#include "lwip/ip_addr.h"
#include "sdkconfig.h"

#include "../../network.hpp"
#include "../../settings.hpp"

namespace player {
namespace {

namespace wc = iclforge::wallclock;

// The name lwIP is given: it keeps the pointer, so this lives for the whole run.
constexpr const char kPool[] = "pool.ntp.org";
constexpr std::int64_t kUpkeepUs = 1'000'000;

// Guards the TZ environment (setenv is not safe against a concurrent localtime) and everything below it that
// is not atomic.
std::mutex g_lock;
std::string g_zone;  // what TZ is set to; empty is none
bool g_events = false;
esp_timer_handle_t g_timer = nullptr;
// How many answers have been said on the console: each one is, so that the log has the hour's own.
std::uint32_t g_reported_syncs = 0;

// The time server a person named: two buffers, because lwIP keeps the pointer it was given and may be reading
// it on its own thread when a new name arrives; the one it had is left alone until the name after.
std::array<std::array<char, wc::kNtpServerMaxBytes + 1>, 2> g_manual{};
int g_manual_at = 0;
bool g_manual_set = false;
// The router's server, from lwIP's first slot.
ip_addr_t g_dhcp{};
bool g_have_dhcp = false;

std::atomic<bool> g_enabled{true};
std::atomic<bool> g_running{false};
// Set on lwIP's thread by the sync callback and read on others'.
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

// --- lwIP's table of servers ------------------------------------------------------------------------------

// One slot as it should be: nothing, a name lwIP resolves, or an address.
struct Slot {
    enum class Kind { kEmpty, kName, kAddress } kind = Kind::kEmpty;
    const char* name = nullptr;
    ip_addr_t address{};
};
using Table = std::array<Slot, CONFIG_LWIP_SNTP_MAX_SERVERS>;

// With g_lock held. What the table should say: the server a person named, then the router's, then the pool
// when nobody named one (naming a server is saying where the time comes from).
[[nodiscard]] Table desired_table() {
    Table table{};
    std::size_t at = 0;
    if (g_manual_set && at < table.size()) {
        table[at].kind = Slot::Kind::kName;
        table[at].name = g_manual[static_cast<std::size_t>(g_manual_at)].data();
        ++at;
    }
    if (g_have_dhcp && at < table.size()) {
        table[at].kind = Slot::Kind::kAddress;
        ip_addr_copy(table[at].address, g_dhcp);
        ++at;
    }
    if (!g_manual_set && at < table.size()) {
        table[at].kind = Slot::Kind::kName;
        table[at].name = kPool;
        ++at;
    }
    return table;
}

[[nodiscard]] bool slot_matches(std::size_t i, const Slot& want) {
    const char* const name = esp_sntp_getservername(static_cast<u8_t>(i));
    const ip_addr_t* const address = esp_sntp_getserver(static_cast<u8_t>(i));
    const bool has_address = address != nullptr && !ip_addr_isany(address);
    switch (want.kind) {
        case Slot::Kind::kName:
            return name != nullptr && std::strcmp(name, want.name) == 0;
        case Slot::Kind::kAddress:
            return name == nullptr && has_address && ip_addr_cmp(address, &want.address);
        case Slot::Kind::kEmpty:
            break;
    }
    return name == nullptr && !has_address;
}

// With g_lock held. The router's server is what lwIP puts in the first slot as an address; learn it, so that a
// lease renewed with another is the one used, and a lease that came before this asked is not lost.
void learn_dhcp_locked() {
    const char* const name = esp_sntp_getservername(0);
    const ip_addr_t* const address = esp_sntp_getserver(0);
    if (name == nullptr && address != nullptr && !ip_addr_isany(address)) {
        ip_addr_copy(g_dhcp, *address);
        g_have_dhcp = true;
    }
}

// With g_lock held. Puts the table as it should be, a slot at a time and only where it differs; true when it
// had to change anything.
bool reconcile_locked() {
    learn_dhcp_locked();
    const Table want = desired_table();
    bool changed = false;
    for (std::size_t i = 0; i < want.size(); ++i) {
        if (slot_matches(i, want[i])) {
            continue;
        }
        changed = true;
        switch (want[i].kind) {
            case Slot::Kind::kName:
                esp_sntp_setservername(static_cast<u8_t>(i), want[i].name);
                break;
            case Slot::Kind::kAddress:
                esp_sntp_setserver(static_cast<u8_t>(i), &want[i].address);
                break;
            case Slot::Kind::kEmpty:
                esp_sntp_setserver(static_cast<u8_t>(i), nullptr);
                break;
        }
    }
    return changed;
}

[[nodiscard]] std::string describe_servers() {
    std::string text;
    for (u8_t i = 0; i < CONFIG_LWIP_SNTP_MAX_SERVERS; ++i) {
        const char* const name = esp_sntp_getservername(i);
        const ip_addr_t* const address = esp_sntp_getserver(i);
        std::array<char, 48> buffer{};
        const bool have_address = address != nullptr && !ip_addr_isany(address) &&
                                  ipaddr_ntoa_r(address, buffer.data(), static_cast<int>(buffer.size())) != nullptr;
        std::string entry;
        if (name != nullptr) {
            // A name, and the address lwIP has resolved it to once it has (nothing is resolved before the
            // first request goes out).
            entry = name;
            if (have_address) {
                entry += std::string(" [") + buffer.data() + "]";
            }
        } else if (have_address) {
            entry = std::string(buffer.data()) + (g_have_dhcp ? " (from DHCP)" : "");
        }
        if (!entry.empty()) {
            text += text.empty() ? "" : ", ";
            text += entry;
        }
    }
    return text.empty() ? std::string("none") : text;
}

// --- the client ---------------------------------------------------------------------------------------------

// On lwIP's thread: no printing here, which has a small stack; the upkeep says it a second later.
void on_sync(struct timeval* /*tv*/) {
    ++g_syncs;
    g_synced = true;
}

// With g_lock held, and the board holding an address (the IP stack is up).
void start_locked() {
    if (g_running) {
        return;
    }
    g_synced = false;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    sntp_set_time_sync_notification_cb(&on_sync);
    (void)reconcile_locked();
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
    // days would be one the board made up, so what is shown starts from nothing at the next start.
    g_synced = false;
    std::printf("wall clock: no longer asking a time server\n");
}

// Once a second: start asking when the board holds an address and nothing has yet, keep lwIP's table as it
// should be while asking, and say on the console each time a server has answered.
void on_upkeep(void* /*arg*/) {
    const std::lock_guard lock(g_lock);
    if (!g_enabled) {
        return;
    }
    if (!g_running) {
        if (network_ready()) {
            start_locked();
        }
        return;
    }
    (void)reconcile_locked();
    if (const std::uint32_t syncs = g_syncs; syncs != g_reported_syncs) {
        g_reported_syncs = syncs;
        std::printf("wall clock: a time server answered (%s)\n", describe_servers().c_str());
    }
}

// An address on either interface: the router may have given another server, and, if no server has answered
// yet, ask again at once and not at the client's own back-off.
void on_got_ip(void* /*arg*/, esp_event_base_t /*base*/, int32_t /*id*/, void* /*data*/) {
    const std::lock_guard lock(g_lock);
    if (!g_enabled) {
        return;
    }
    if (!g_running) {
        start_locked();
        return;
    }
    (void)reconcile_locked();
    if (!g_synced) {
        (void)sntp_restart();
        std::printf("wall clock: a new address; asking again: %s\n", describe_servers().c_str());
    }
}

// With g_lock held.
void set_manual_locked(std::string_view server) {
    if (server.empty()) {
        g_manual_set = false;
        return;
    }
    // The other buffer, so that the name lwIP holds is not rewritten under it.
    const int next = 1 - g_manual_at;
    std::array<char, wc::kNtpServerMaxBytes + 1>& buffer = g_manual[static_cast<std::size_t>(next)];
    const std::size_t taken = std::min(server.size(), wc::kNtpServerMaxBytes);
    std::memcpy(buffer.data(), server.data(), taken);
    buffer[taken] = '\0';
    g_manual_at = next;
    g_manual_set = true;
}

}  // namespace

bool wall_clock_built() { return true; }

void wall_clock_start() {
    const std::lock_guard lock(g_lock);
    const Settings& stored = settings();
    if (!apply_zone_locked(stored.time_zone.data())) {
        std::printf("wall clock: the stored zone is not one the C library reads; UTC\n");
        (void)apply_zone_locked("");
    }
    set_manual_locked(stored.ntp_server.data());
    g_enabled = stored.time_sync;
    if (!g_events) {
        const esp_err_t eth = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &on_got_ip, nullptr);
        const esp_err_t sta = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_got_ip, nullptr);
        g_events = eth == ESP_OK && sta == ESP_OK;
        if (!g_events) {
            std::printf("wall clock: could not listen for an address; the upkeep starts asking when there is one\n");
        }
    }
    if (g_timer == nullptr) {
        const esp_timer_create_args_t args{.callback = &on_upkeep, .arg = nullptr,
                                           .dispatch_method = ESP_TIMER_TASK, .name = "wall_clock",
                                           .skip_unhandled_events = true};
        if (esp_timer_create(&args, &g_timer) == ESP_OK) {
            (void)esp_timer_start_periodic(g_timer, kUpkeepUs);
        } else {
            std::printf("wall clock: no timer; the time is kept only when an address arrives\n");
        }
    }
    if (g_enabled && network_ready()) {
        start_locked();
    }
}

bool wall_clock_set_zone(std::string_view zone) {
    const std::lock_guard lock(g_lock);
    return apply_zone_locked(zone);
}

void wall_clock_set_server(std::string_view server) {
    const std::lock_guard lock(g_lock);
    set_manual_locked(server);
    if (g_running) {
        // A new server is asked at once: the table is put as it should be and the client starts again, so that
        // it does not wait out an hour for the one it had.
        (void)reconcile_locked();
        g_synced = false;
        (void)sntp_restart();
        std::printf("wall clock: asking %s: %s\n", g_manual_set ? "the server named" : "the router's and the pool",
                    describe_servers().c_str());
    }
}

void wall_clock_set_sync(bool on) {
    const std::lock_guard lock(g_lock);
    g_enabled = on;
    if (!on) {
        stop_locked();
    } else if (network_ready()) {
        start_locked();
    }
}

iclforge::wallclock::Reading wall_clock_read() {
    wc::Reading out;
    if (!g_synced) {
        return out;
    }
    const std::lock_guard lock(g_lock);
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
    out.enabled = g_enabled;
    out.asking = g_running;
    out.synced = g_synced;
    out.syncs = g_syncs;
    out.starts = g_starts;
    return out;
}

std::string wall_clock_servers() {
    const std::lock_guard lock(g_lock);
    return describe_servers();
}

}  // namespace player
