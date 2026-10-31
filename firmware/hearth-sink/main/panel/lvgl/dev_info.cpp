// The developer view's side of the board: what it reads and the one thing it asks. See dev_info.hpp
// and planning/karaoke-p4x-panel.md, "The developer view".
//
// Nothing here is new knowledge. Each fact is read from what the board already keeps for GET /status,
// GET /hardware and GET /firmware (the network seam, the Sendspin player, the settings, the heap, the
// partition table, the console's log ring); this gathers them into iclforge::devview::Facts, whose
// words are libs/device's and tested there. What it must never read is the Wi-Fi passphrase and the
// Sendspin pairing token, and Facts has no field to put either in.
//
// Called from the panel's task with LVGL's lock held, once a second while the view is open
// and never otherwise. Nothing here waits for long, and the console is read a few hundred bytes at a
// time from a cursor, so that a refresh does not ask the internal heap, which a decoder has little
// of, for more than a kilobyte.

#include "dev_info.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#if CONFIG_SPIRAM
#include "esp_psram.h"
#endif

#include "iclforge/log.hpp"

#include "../../audio_sink.hpp"
#include "../../board_report.hpp"
#include "meters_screen.hpp"
#include "../../discovery.hpp"
#include "../../network.hpp"
#include "../../sendspin.hpp"
#include "../../settings.hpp"
#include "../panel.hpp"
#include "display.hpp"
#include "glyphs.hpp"
#include "lv_alloc.hpp"

namespace dv = iclforge::devview;

namespace player::panel {

namespace {

// How long a change of the network preference may take before the view calls it refused. The board
// keeps a preference only once the interface it names holds an address, and waits up to 30 s for one
// (net/dual/network.cpp), on a task of its own, after a short grace for the reply.
constexpr std::int64_t kApplyWaitUs = 45'000'000;
// How often the core dump is looked for: it is a flash read, and it changes only when the board
// crashes (and so restarts) or someone erases it.
constexpr std::int64_t kCoreDumpEveryUs = 30'000'000;
// How often the network is asked what it holds. On a board whose radio is a companion chip, the
// station's signal is a call across SDIO to the C6 (about 40 ms), and the idle screen asks the same
// of the network twice a second already: the view asks no more often than every two seconds, and at
// once while a change of
// the preference is under way.
constexpr std::int64_t kNetworkEveryUs = 2'000'000;
// And while a stream plays, when the board has its audio to keep and the Sendspin traffic shares that
// bus: the view asks every ten seconds. The first run of plays on the kit with the view open (four,
// refreshing twice a second and asking every half second) had one with three late chunks and an
// underrun against none of four with it shut, which is within the one-in-four that start badly with
// nothing open (planning/karaoke-p4x-panel.md) and not enough to say it is not the view.
constexpr std::int64_t kNetworkWhilePlayingEveryUs = 10'000'000;
// The console is read only while the internal heap has a block this big: a refresh that would make
// a decoder's allocation fail is skipped, and the lines stay as they were.
constexpr std::size_t kLogReadNeedsInternal = 4 * 1024;
constexpr std::size_t kLogChunk = 512;
// What the tail keeps of the console, for the view's few lines.
constexpr std::size_t kLogKeep = 6 * 1024;

dv::NetChange g_change = dv::NetChange::idle;
dv::Preference g_wanted = dv::Preference::automatic;
std::int64_t g_requested_us = 0;

// A name the fonts may not have the letters of: shown as a short phrase rather than a row of boxes.
std::string drawable_text(std::string_view text) {
    return is_drawable(text) ? std::string(text) : std::string("(letters this screen cannot show)");
}

// The words GET /firmware gives for why the board booted (firmware.cpp, reset_reason_name).
const char* reset_reason_word(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:
            return "poweron";
        case ESP_RST_EXT:
            return "ext";
        case ESP_RST_SW:
            return "sw";
        case ESP_RST_PANIC:
            return "panic";
        case ESP_RST_INT_WDT:
            return "int_wdt";
        case ESP_RST_TASK_WDT:
            return "task_wdt";
        case ESP_RST_WDT:
            return "wdt";
        case ESP_RST_DEEPSLEEP:
            return "deepsleep";
        case ESP_RST_BROWNOUT:
            return "brownout";
        case ESP_RST_SDIO:
            return "sdio";
        case ESP_RST_USB:
            return "usb";
        case ESP_RST_JTAG:
            return "jtag";
        case ESP_RST_PWR_GLITCH:
            return "glitch";
        case ESP_RST_CPU_LOCKUP:
            return "lockup";
        default:
            return "unknown";
    }
}

dv::NetInterface convert(const InterfaceStatus& from) {
    dv::NetInterface to;
    to.present = from.present;
    to.enabled = from.enabled;
    to.link = from.link;
    to.address = from.address;
    to.mac = from.mac;
    to.configured = from.configured;
    to.ssid = drawable_text(from.ssid);
    to.rssi_dbm = from.rssi_dbm;
    to.speed_mbps = from.speed_mbps;
    to.full_duplex = from.full_duplex;
    return to;
}

void read_network_now(dv::Facts& facts) {
    const NetworkLink link = network_link();
    const std::string address = network_address();
    if (link.preference != nullptr) {
        // A cable and a station: how each stands, and the choice between them.
        facts.preference = link.preference;
        facts.ethernet = convert(link.ethernet);
        facts.wifi = convert(link.wifi);
    } else if (link.kind != nullptr && std::string_view(link.kind) == "wifi") {
        // The station alone: one interface, whose facts are the link's own.
        facts.wifi.present = true;
        facts.wifi.enabled = true;
        facts.wifi.configured = true;
        facts.wifi.link = !link.ssid.empty();
        facts.wifi.address = address;
        facts.wifi.ssid = drawable_text(link.ssid);
        facts.wifi.rssi_dbm = link.rssi_dbm;
    } else if (link.kind != nullptr && std::string_view(link.kind) == "ethernet") {
        facts.ethernet.present = true;
        facts.ethernet.enabled = true;
        facts.ethernet.link = !address.empty();
        facts.ethernet.address = address;
    }
    // In use means holding an address: while none does, the link names the interface it waits on.
    facts.active = (link.kind != nullptr && !address.empty()) ? link.kind : "";
    facts.name = drawable_text(settings().name.data());
    facts.mdns = discovery_host_name() + ".local";
    facts.web_address = address;
    facts.web_port = CONFIG_ICLFORGE_EXAMPLE_CONTROL_PORT;
}

void read_network(dv::Facts& facts, bool playing) {
    static dv::Facts cache;
    static std::int64_t at_us = -kNetworkEveryUs;
    const std::int64_t now = esp_timer_get_time();
    const std::int64_t every = playing ? kNetworkWhilePlayingEveryUs : kNetworkEveryUs;
    if (now - at_us >= every || g_change == dv::NetChange::working) {
        at_us = now;
        cache = dv::Facts{};
        read_network_now(cache);
    }
    facts.active = cache.active;
    facts.preference = cache.preference;
    facts.ethernet = cache.ethernet;
    facts.wifi = cache.wifi;
    facts.name = cache.name;
    facts.mdns = cache.mdns;
    facts.web_address = cache.web_address;
    facts.web_port = cache.web_port;
}

void read_configuration(dv::Facts& facts, const BoardReport& report) {
    facts.profile = report.profile;
    facts.profile_hash = report.profile_hash;
    if (const esp_app_desc_t* const app = esp_app_get_description()) {
        facts.firmware = app->version;
        facts.idf = app->idf_ver;
    }
    if (const esp_partition_t* const running = esp_ota_get_running_partition()) {
        facts.slot = running->label;
    }
    esp_chip_info_t chip{};
    esp_chip_info(&chip);
    facts.chip = chip.model == CHIP_ESP32P4 ? "ESP32-P4" : CONFIG_IDF_TARGET;
    facts.revision_major = chip.revision / 100;
    facts.revision_minor = chip.revision % 100;
    facts.cores = chip.cores;
    facts.cpu_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
#if CONFIG_SPIRAM
    facts.psram_bytes = esp_psram_get_size();
#ifdef CONFIG_SPIRAM_SPEED
    facts.psram_mhz = CONFIG_SPIRAM_SPEED;
#endif
#endif
    facts.layout = report.layout;
    facts.sink = sink_name();
    facts.slots = sink_slots();
    facts.slot_bits = sink_slot_bits();
    facts.uptime_ms = static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
    facts.reset_reason = reset_reason_word(esp_reset_reason());

    // A core dump of the last crash, looked for every so often and not every refresh.
    static std::int64_t checked_us = -kCoreDumpEveryUs;
    static bool dump = false;
    static std::size_t dump_bytes = 0;
    const std::int64_t now = esp_timer_get_time();
    if (now - checked_us >= kCoreDumpEveryUs) {
        checked_us = now;
        std::size_t address = 0;
        std::size_t size = 0;
        dump = esp_core_dump_image_get(&address, &size) == ESP_OK;
        dump_bytes = dump ? size : 0;
    }
    facts.core_dump = dump;
    facts.core_dump_bytes = dump_bytes;
}

void read_stream(dv::Facts& facts, const BoardReport& report) {
    if (const auto status = sendspin_status()) {
        facts.player = true;
        facts.server = drawable_text(status->server);
        facts.connections = status->connections;
        facts.paired = status->paired;
        facts.clock_converged = status->clock_converged;
        facts.clock_error_us = status->clock_error_us;
        facts.stream = status->stream;
        facts.bursts = status->bursts;
        facts.underruns = status->underruns;
        facts.late = status->late;
        facts.dropped = status->dropped;
        facts.invalid = status->invalid;
        facts.resyncs = status->resyncs;
        facts.error_us = status->error_us;
        facts.worst_error_us = status->worst_error_us;
        facts.burst_us = status->burst_us;
        facts.worst_burst_us = status->worst_burst_us;
        facts.peak_db.assign(status->peak_db.begin(), status->peak_db.end());
    }
    facts.played = report.played;
    facts.us_per_frame = report.us_per_frame;
    facts.worst_frame_us = report.worst_frame_us;
    facts.sink_us_per_frame = report.sink_us_per_frame;
    facts.render_us_per_frame = report.render_us_per_frame;
}

void read_performance(dv::Facts& facts, const PanelStats& panel, const TouchCounters& touch) {
    constexpr std::uint32_t kInternal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    constexpr std::uint32_t kExternal = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    facts.internal_free = heap_caps_get_free_size(kInternal);
    facts.internal_low = heap_caps_get_minimum_free_size(kInternal);
    facts.internal_largest = heap_caps_get_largest_free_block(kInternal);
    facts.psram_free = heap_caps_get_free_size(kExternal);
    facts.psram_low = heap_caps_get_minimum_free_size(kExternal);
    facts.refreshes = panel.refreshes;
    facts.render_us_mean = panel.render_us_mean;
    facts.render_us_max = panel.render_us_max;
    facts.late_cycles = panel.late_cycles;
    facts.cycle_us_max = panel.cycle_us_max;
    facts.touch_reads = touch.reads.load();
    facts.touch_failures = touch.failures.load();
    facts.touch_points = touch.touches.load();
    // The Meters view, once it has been opened.
    const iclforge::panelstats::MetersStats meters = now_meters_stats();
    facts.meters_opens = meters.opens;
    facts.meters_showing = meters.showing;
    facts.meters_rate_hz = meters.rate_hz;
    facts.meters_shape = std::string(iclforge::panelstats::representation_name(meters.representation)) + " " + std::to_string(meters.outputs);
    facts.meters_analysis_us = static_cast<unsigned long>(meters.analysis.mean_us());
    facts.meters_widgets_us = static_cast<unsigned long>(meters.widgets.mean_us());
    facts.meters_render_us = static_cast<unsigned long>(meters.render.mean_us());
    facts.meters_late = static_cast<unsigned long>(meters.governor.late);
    facts.meters_dropped = static_cast<unsigned long>(meters.governor.dropped);
    facts.meters_tap_us = static_cast<unsigned long>(meters.tap.us_per_burst() + 0.5);
}

// The console's tail, kept as it is read: from a cursor, so a refresh takes only what is new. In
// PSRAM (LvString), like the screen's other copies.
LvString g_tail;
std::uint64_t g_cursor = 0;

}  // namespace

std::string panel_stats_report(const PanelStats& panel, const iclforge::panelstats::MetersStats& meters) {
    constexpr std::uint32_t kInternal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    constexpr std::uint32_t kExternal = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    iclforge::panelstats::PanelStats out;
    out.width = panel.width;
    out.height = panel.height;
    out.refreshes = panel.refreshes;
    out.refresh.last_us = panel.render_us_last;
    out.refresh.max_us = panel.render_us_max;
    out.refresh.count = panel.refreshes;
    out.refresh.total_us = panel.render_us_total;
    out.task_stack_free_min = panel.stack_free;
    out.psram_free = static_cast<std::uint32_t>(heap_caps_get_free_size(kExternal));
    out.psram_free_min = static_cast<std::uint32_t>(heap_caps_get_minimum_free_size(kExternal));
    out.psram_largest = static_cast<std::uint32_t>(heap_caps_get_largest_free_block(kExternal));
    out.internal_free = static_cast<std::uint32_t>(heap_caps_get_free_size(kInternal));
    out.internal_free_min = static_cast<std::uint32_t>(heap_caps_get_minimum_free_size(kInternal));
    out.internal_largest = static_cast<std::uint32_t>(heap_caps_get_largest_free_block(kInternal));
    out.has_meters = true;
    out.meters = meters;
    return iclforge::panelstats::panel_stats_json(out);
}

dv::Facts gather_dev_facts(const PanelStats& panel, const TouchCounters& touch) {
    dv::Facts facts;
    const BoardReport report = board_report();
    read_stream(facts, report);
    read_network(facts, facts.player && !facts.stream.empty() && facts.stream != "idle");
    read_configuration(facts, report);
    read_performance(facts, panel, touch);

    // A change of the preference that was asked for is under way until the board says it has the
    // preference, or it is long past the time the board gives itself.
    if (g_change == dv::NetChange::working) {
        if (facts.preference == dv::preference_word(g_wanted)) {
            g_change = dv::NetChange::done;
        } else if (esp_timer_get_time() - g_requested_us > kApplyWaitUs) {
            g_change = dv::NetChange::refused;
        }
    }
    facts.net_change = g_change;
    facts.net_change_word = dv::preference_word(g_wanted);
    return facts;
}

dv::Facts gather_about_facts() {
    dv::Facts facts;
    const BoardReport report = board_report();
    read_stream(facts, report);
    read_network(facts, facts.player && !facts.stream.empty() && facts.stream != "idle");
    read_configuration(facts, report);
    return facts;
}

std::string read_log_tail(std::size_t limit) {
    if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= kLogReadNeedsInternal) {
        // Whatever is new, a chunk at a time; the first read after the view opens takes the whole ring.
        for (;;) {
            const std::optional<iclforge::LogRing::Read> read = iclforge::log_read(g_cursor, kLogChunk);
            if (!read || read->text.empty()) {
                break;
            }
            g_tail.append(read->text.data(), read->text.size());
            g_cursor = read->next;
        }
        if (g_tail.size() > kLogKeep) {
            g_tail.erase(0, g_tail.size() - kLogKeep);
        }
    }
    const std::size_t take = std::min(limit, g_tail.size());
    return std::string(g_tail.data() + (g_tail.size() - take), take);
}

bool request_network_preference(dv::Preference preference) {
    g_wanted = preference;
    g_requested_us = esp_timer_get_time();
    NetworkPreference want = NetworkPreference::automatic;
    switch (preference) {
        case dv::Preference::ethernet:
            want = NetworkPreference::ethernet;
            break;
        case dv::Preference::wifi:
            want = NetworkPreference::wifi;
            break;
        case dv::Preference::automatic:
            break;
    }
    // The call PUT /network-preference makes. It returns at once, having started the change on a task
    // of its own, or false when the board will not (no network stored for the station, or a join under
    // way): either way there is nothing here to wait for.
    if (!network_has_choice() || !network_set_preference(want)) {
        g_change = dv::NetChange::refused;
        return false;
    }
    g_change = dv::NetChange::working;
    return true;
}

}  // namespace player::panel
