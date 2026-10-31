// The panel's task: LVGL, the lyric screen, the drawing statistics and the read-back route. See
// ../panel.hpp and planning/karaoke-p4x-panel.md.
//
// One task owns LVGL. Each cycle it brings the screen up to date from what the Sendspin player holds
// and lets LVGL draw what that dirtied; a cycle takes a few hundred microseconds when a word fills
// and nothing when the picture sits still, and the task sleeps the rest of its period. It is below
// the decoder and the I2S writer in priority: the screen is drawn late before the audio is.

#include "../panel.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "iclforge/lyrics/lyrics.hpp"
#include "iclforge/lyrics/stage.hpp"

#include "iclforge/now_view.hpp"
#include "iclforge/panel_follow.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/render/room_plan.hpp"

#include "../../network.hpp"
#include "../../preferences.hpp"
#include "../../sendspin.hpp"
#include "../../settings.hpp"
#include "../../wall_clock.hpp"
#include "board_info.hpp"
#include "dev_info.hpp"
#include "display.hpp"
#include "lyric_screen.hpp"
#include "meters_screen.hpp"
#include "now_screen.hpp"

namespace player {
namespace {

// What the screen follows of the board's settings (the screen without lyrics, when it dims, the speakers' style, the
// backlight's lit level, the clock's format, the night schedule, and the level the screen last asked the glass for, one
// of display.hpp's three: lit, the dim floor, off). A tap on the Preferences sheet reaches the setters ON THIS TASK, while
// g_lock is held, so none of them may take it: PanelFollow is atomics and nothing else (iclforge/panel_follow.hpp).
iclforge::panelfollow::PanelFollow g_follow{panel::kBacklightNormalPercent};

// The percent the glass is given for a level the screen asked for: the person's brightness for lit, the floor under
// it for dimmed, nothing for off (iclforge::nowview::backlight_percent).
int effective_backlight(int request) {
    using iclforge::nowview::Dim;
    const Dim dim = request <= panel::kBacklightOffPercent    ? Dim::kDark
                    : request <= panel::kBacklightDimPercent ? Dim::kDimmed
                                                              : Dim::kNone;
    return iclforge::nowview::backlight_percent(dim, g_follow.brightness());
}

}  // namespace
}  // namespace player

namespace player::panel {

// lv_memory.cpp: LVGL's heap, in PSRAM. Called below so that the object is linked at all.
bool psram_heap_ready();

InjectedPointer& injected_pointer() {
    static InjectedPointer pointer;
    return pointer;
}

TouchCounters& touch_counters() {
    static TouchCounters counters;
    return counters;
}

namespace {

// The song the demo plays: a word-timed file, looped, with no server and no network. For bringing a
// panel up before anything is paired to the board, and for loading the drawing while a stream
// plays (POST /panel/demo?mode=2). 0 is off, 1 is when the player holds no lyrics, 2 is always.
constexpr char kDemoLrc[] =
    "[ti:Demo: Auld Lang Syne]\n"
    "[ar:Traditional; words by Robert Burns, 1788]\n"
    "[length:00:20]\n"
    "[00:00.00]<00:00.00>Should <00:00.50>auld <00:01.25>ac<00:01.50>quain<00:02.00>tance "
    "<00:02.50>be <00:03.25>for<00:03.50>got,\n"
    "[00:04.00]<00:04.00>And <00:04.50>ne<00:05.25>ver <00:05.50>brought <00:06.00>to "
    "<00:06.50>mind?\n"
    "[00:08.00]<00:08.00>Should <00:08.50>auld <00:09.25>ac<00:09.50>quain<00:10.00>tance "
    "<00:10.50>be <00:11.25>for<00:11.50>got,\n"
    "[00:12.00]<00:12.00>And <00:12.50>auld <00:13.50>lang <00:14.50>syne!\n"
    "[00:16.50]\n";
constexpr std::int64_t kDemoLoopMs = 20'000;
constexpr std::int64_t kDemoItem = -2;

std::atomic<int> g_demo{0};

// Passes the player's lyrics to the screen's reader when there are any, and says whether it did.
class Forward final : public LyricsReader {
   public:
    explicit Forward(LyricsReader& to) : to_(to) {}
    void on_lyrics(std::int64_t item, const iclforge::lyrics::Lyrics* lyrics,
                   const iclforge::lyrics::Stage& stage) override {
        if (lyrics != nullptr) {
            to_.on_lyrics(item, lyrics, stage);
            forwarded = true;
        }
    }
    bool forwarded = false;

   private:
    LyricsReader& to_;
};

}  // namespace

bool read_lyrics(LyricsReader& reader, std::int64_t delay_ms,
                 const iclforge::lyrics::StageOptions& options) {
    const int mode = g_demo.load();
    if (mode == 0) {
        return sendspin_read_lyrics(reader, delay_ms, options);
    }
    if (mode == 1) {
        Forward forward(reader);
        const bool player = sendspin_read_lyrics(forward, delay_ms, options);
        if (forward.forwarded) {
            return player;
        }
    }
    static const std::optional<iclforge::lyrics::Lyrics> demo = [] {
        auto parsed = iclforge::lyrics::parse_lyrics(kDemoLrc);
        return parsed ? std::optional<iclforge::lyrics::Lyrics>(std::move(*parsed)) : std::nullopt;
    }();
    if (!demo) {
        return sendspin_read_lyrics(reader, delay_ms, options);
    }
    static const std::int64_t started_us = time_us();
    const std::int64_t position_ms = ((time_us() - started_us) / 1000) % kDemoLoopMs;
    reader.on_lyrics(kDemoItem, &*demo,
                     iclforge::lyrics::make_stage(&*demo, position_ms, delay_ms, options));
    return true;
}

// What the screen reads of the board (board_info.hpp), over the settings, the network and the player.
BoardInfo read_board_info() {
    BoardInfo info;
    info.name = settings().name.data();
    info.address = network_address();
    const NetworkLink link = network_link();
    info.network = link.ssid;
    info.via = link.kind != nullptr ? link.kind : "";
    if (const auto status = sendspin_status()) {
        info.player = true;
        info.server = status->server;
        info.stream = status->stream;
        info.pairing_code = status->pairing_code;
        info.paired = status->paired;
        info.connections = status->connections;
    }
    if (const esp_app_desc_t* const app = esp_app_get_description()) {
        info.firmware = app->version;
    }
    return info;
}

bool read_now_playing(NowPlaying& out) {
    const SendspinPlaying playing = sendspin_now_playing();
    if (!playing.active) {
        return false;
    }
    // The room is parsed again only when the layout text changes, and the heading made again only when
    // what it says does: this is called five times a second, and what it saves is the heap.
    static std::array<char, iclforge::render::OutputLayout::kTextBytes> room_text{};
    static iclforge::nowview::StreamFacts heading_facts{};
    static std::size_t heading_outputs = 0;
    static bool first = true;
    const bool room_changed = first || playing.layout != room_text;
    if (room_changed) {
        room_text = playing.layout;
        out.spots.clear();
        if (const auto layout = iclforge::render::OutputLayout::parse(playing.layout.data())) {
            const std::vector<iclforge::render::PlanSpot> spots = iclforge::render::plan_spots(*layout);
            out.spots.assign(spots.begin(), spots.end());
        }
    }
    if (room_changed || first || !(playing.facts == heading_facts) || playing.outputs != heading_outputs) {
        heading_facts = playing.facts;
        heading_outputs = playing.outputs;
        const std::string heading = iclforge::nowview::heading(playing.facts, playing.layout.data(), playing.outputs);
        out.heading.assign(heading.data(), heading.size());
    }
    first = false;
    // The bitrate of what was played, smoothed (a PCM stream has no coded bytes and no figure).
    static iclforge::nowview::BitrateMeter meter;
    static bool metered = false;
    if (playing.facts.codec == iclforge::nowview::Codec::kPcm) {
        meter.reset();
        metered = false;
    } else {
        if (!metered) {
            meter.reset();
            metered = true;
        }
        meter.observe(playing.coded_bytes, static_cast<std::uint32_t>(time_us() / 1000));
    }
    out.facts = playing.facts;
    out.kbps = meter.kbps();
    if (playing.have_levels) {
        out.peak_db.assign(playing.peak_db.begin(), playing.peak_db.begin() + static_cast<std::ptrdiff_t>(playing.outputs));
        out.rms_db.assign(playing.rms_db.begin(), playing.rms_db.begin() + static_cast<std::ptrdiff_t>(playing.outputs));
    } else {
        out.peak_db.clear();
        out.rms_db.clear();
    }
    return true;
}

bool read_now_levels(NowLevels& out) {
    const SendspinLevels levels = sendspin_now_levels();
    if (!levels.have) {
        return false;
    }
    out.serial = levels.serial;
    out.outputs = levels.outputs;
    out.peak_db = levels.peak_db;
    out.rms_db = levels.rms_db;
    return true;
}

bool read_now_title(NowTitle& out) {
    // The player calls the reader under its lock, so it copies and returns (now_copy_title copies the
    // strings only when the store's revision moved).
    class Copy final : public TitleReader {
       public:
        explicit Copy(NowTitle& to) : to_(to) {}
        void on_title(const iclforge::nowview::TitleStore* held, std::optional<std::int64_t> position_ms) override {
            now_copy_title(to_, held, position_ms);
        }

       private:
        NowTitle& to_;
    };
    Copy copy(out);
    return sendspin_read_title(copy);
}

NowSettings read_now_settings() {
    NowSettings settings;
    settings.view = g_follow.view();
    settings.dim_after_minutes = g_follow.dim_after_minutes();
    settings.speakers_style = g_follow.speakers_style();
    settings.night = g_follow.night();
    // The time of day is asked only by a schedule that is on: the clock is read under the wall clock's own lock.
    if (settings.night.enabled) {
        settings.minute_of_day = iclforge::wallclock::minute_of_day(player::wall_clock_read());
    }
    return settings;
}

iclforge::wallclock::Reading read_wall_clock() {
    iclforge::wallclock::Reading reading = player::wall_clock_read();
    // How the person who owns the board wants it written is the panel's to know, not the clock's.
    reading.format = g_follow.clock_format();
    return reading;
}

void set_backlight(int percent) {
    g_follow.ask_backlight(percent);
    display_set_backlight(effective_backlight(percent));
}

int backlight_lit_percent() { return effective_backlight(kBacklightNormalPercent); }

iclforge::prefs::Backend& preferences_backend() { return player::preferences(); }

std::int64_t time_us() { return esp_timer_get_time(); }

iclforge::meters::MeterTap& meters_tap() {
    // Made the first time the Meters view is built, in LVGL's heap (PSRAM: 32 KB of ring), and handed to the player once: it is asked for blocks
    // only while the view shows (MeterTap::set_wanted).
    static iclforge::meters::MeterTap* const tap = [] {
        void* const memory = lv_malloc(sizeof(iclforge::meters::MeterTap));
        if (memory == nullptr) {
            std::abort();
        }
        auto* const made = new (memory) iclforge::meters::MeterTap(+[]() noexcept -> std::uint64_t { return static_cast<std::uint64_t>(esp_timer_get_time()); });
        sendspin_set_block_tap(made);
        return made;
    }();
    return *tap;
}

int demo_mode() { return g_demo.load(); }

void set_demo_mode(int mode) { g_demo.store(std::clamp(mode, 0, 2)); }

}  // namespace player::panel

namespace player {

namespace {

constexpr int kFps = CONFIG_ICLFORGE_EXAMPLE_PANEL_FPS;
constexpr int kCapturePort = CONFIG_ICLFORGE_EXAMPLE_PANEL_CAPTURE_PORT;

// Guards LVGL and everything below it: the task holds it for a cycle, and the read-back route takes
// it to copy the picture whole.
std::mutex g_lock;

panel::Display g_display;
PanelStats g_stats;
std::atomic<bool> g_started{false};
// The display is open and the screen is built on it: g_display is read-only from here on.
std::atomic<bool> g_display_ready{false};
std::int64_t g_refresh_started_us = 0;
std::uint64_t g_render_total_us = 0;
TaskHandle_t g_task = nullptr;
httpd_handle_t g_server = nullptr;

std::uint32_t tick_ms() { return static_cast<std::uint32_t>(esp_timer_get_time() / 1000); }

// A refresh is LVGL's pass over the invalid areas: from the first to the last flush.
void on_refresh_start(lv_event_t* /*event*/) { g_refresh_started_us = esp_timer_get_time(); }

void on_refresh_ready(lv_event_t* /*event*/) {
    const auto took = static_cast<std::uint32_t>(esp_timer_get_time() - g_refresh_started_us);
    ++g_stats.refreshes;
    g_render_total_us += took;
    g_stats.render_us_last = took;
    g_stats.render_us_max = std::max(g_stats.render_us_max, took);
    g_stats.render_us_mean = static_cast<std::uint32_t>(g_render_total_us / g_stats.refreshes);
    g_stats.render_us_total = g_render_total_us;
    panel::now_meters_refresh(took);
}

void on_flush_start(lv_event_t* event) {
    const auto* const area = static_cast<const lv_area_t*>(lv_event_get_param(event));
    if (area != nullptr) {
        g_stats.pixels += static_cast<std::uint64_t>(lv_area_get_width(area)) * lv_area_get_height(area);
    }
}

// Waits out the rest of a period. LVGL reads the pointer inside lv_timer_handler(), so a tap is
// seen only when the task calls it, and a finger is down for 50 to 150 ms: called once a period
// (every 100 ms with nothing filling, and a tick is 10 ms) the task caught about half the taps,
// and the first run on the kit found a tap that did nothing until it was repeated all over the
// glass. So with a pointer to read the wait is cut into 20 ms slices, each followed by the call;
// LVGL draws nothing in it unless something changed, and the read of an idle controller is one
// I2C byte.
void rest(std::int64_t rest_us) {
    constexpr std::int64_t kSliceUs = 20'000;
    const std::int64_t until = esp_timer_get_time() + rest_us;
    for (;;) {
        const std::int64_t left = until - esp_timer_get_time();
        if (left <= 1000) {
            return;
        }
        const std::int64_t wait = g_display.touch ? std::min(left, kSliceUs) : left;
        vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS(wait / 1000)));
        if (g_display.touch && until - esp_timer_get_time() > 1000) {
            const std::lock_guard lock(g_lock);
            (void)lv_timer_handler();
        }
    }
}

// The audio comes first. A decoder that spends a long share of its 32 ms on a burst (a 7.1.4 object stream, AC-4) is slowed by the screen's memory
// traffic beside it: the kit measured the Meters view at +1.6 to 2.2 ms a burst under JOC, +0.5 to 0.7 with the screen's task kept on the decode
// core (planning/p4x-meters-view.md, "What the screen costs the audio path"). So while the mean burst is at CONFIG_ICLFORGE_EXAMPLE_PANEL_YIELD_BURST_US
// or more (in at that, out at 70 % of it, so it does not flap) the Meters view is held at ten updates a second. At YIELD 2 this task is also made
// on the decode core (start()), where it cannot run beside the decode task: FreeRTOS here has no call to move a task between cores, so that one is
// for always. Looked at once a second; a stream that is not bursts is never busy.
void follow_decoder_load() {
    if constexpr (CONFIG_ICLFORGE_EXAMPLE_PANEL_YIELD != 0) {
        static bool busy = false;
        std::uint64_t burst_us = 0;
        if (const auto status = sendspin_status()) {
            burst_us = status->stream == "bursts" ? static_cast<std::uint64_t>(status->burst_us) : 0;
        }
        constexpr std::uint64_t kEnterUs = CONFIG_ICLFORGE_EXAMPLE_PANEL_YIELD_BURST_US;
        const bool want = burst_us >= kEnterUs || (busy && burst_us >= kEnterUs * 7 / 10);
        if (want == busy) {
            return;
        }
        busy = want;
        {
            const std::lock_guard lock(g_lock);
            panel::now_meters_hold_slow(busy);
        }
        std::printf("panel: the decoder is %s (a burst of %llu us): the Meters view is %s\n", busy ? "busy" : "free again",
                    static_cast<unsigned long long>(burst_us), busy ? "held at ten a second" : "let go");
    }
}

void run(void* /*arg*/) {
    const std::int64_t fill_period_us = 1'000'000 / std::max(kFps, 1);
    constexpr std::int64_t kRestPeriodUs = 100'000;
    std::int64_t period_us = fill_period_us;
    std::uint32_t reported_ms = tick_ms();
    std::uint32_t followed_ms = reported_ms;
    for (;;) {
        const std::int64_t started = esp_timer_get_time();
        bool filling = false;
        {
            const std::lock_guard lock(g_lock);
            // A brightness set since the last cycle (by a PUT, or by a tap on this task): the glass is given the level
            // for what the screen last asked for, here, where the glass is the panel's alone.
            if (g_follow.take_backlight_stale()) {
                panel::set_backlight(g_follow.backlight_request());
            }
            filling = panel::screen_update();
            (void)lv_timer_handler();
            const panel::ScreenStats screen = panel::screen_stats();
            g_stats.screen = screen.name;
            g_stats.dimmed = screen.dimmed;
            g_stats.backlight = effective_backlight(screen.backlight);
            g_stats.rebuilds = screen.rebuilds;
            g_stats.rebuild_us_max = screen.rebuild_us_max;
            g_stats.dev_view = screen.dev_view;
            g_stats.prefs_view = screen.prefs_view;
            g_stats.prefs_builds = screen.prefs_builds;
            g_stats.prefs_build_us_max = screen.prefs_build_us_max;
            g_stats.dev_refreshes = screen.dev_refreshes;
            g_stats.dev_refresh_us_last = screen.dev_refresh_us_last;
            g_stats.dev_refresh_us_max = screen.dev_refresh_us_max;
            g_stats.stack_free = static_cast<std::uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
        }
        const auto cycle_us = static_cast<std::uint32_t>(esp_timer_get_time() - started);
        {
            const std::lock_guard lock(g_lock);
            g_stats.cycle_us_max = std::max(g_stats.cycle_us_max, cycle_us);
            if (static_cast<std::int64_t>(cycle_us) > period_us) {
                ++g_stats.late_cycles;
            }
        }
        period_us = filling ? fill_period_us : kRestPeriodUs;

        // A line on the console every ten seconds while something is being drawn, which is also what
        // GET /log carries to a board with no cable on it.
        const std::uint32_t now = tick_ms();
        if (now - followed_ms >= 1000) {
            followed_ms = now;
            follow_decoder_load();
        }
        if (now - reported_ms >= 10000) {
            reported_ms = now;
            const std::lock_guard lock(g_lock);
            if (g_stats.refreshes != 0) {
                std::printf("panel: %s, %lu refreshes, render %lu us mean %lu max, rebuild %lu us max, "
                            "%lu late, stack %lu free\n",
                            g_stats.screen, static_cast<unsigned long>(g_stats.refreshes),
                            static_cast<unsigned long>(g_stats.render_us_mean),
                            static_cast<unsigned long>(g_stats.render_us_max),
                            static_cast<unsigned long>(g_stats.rebuild_us_max),
                            static_cast<unsigned long>(g_stats.late_cycles),
                            static_cast<unsigned long>(g_stats.stack_free));
            }
        }

        const std::int64_t spent = esp_timer_get_time() - started;
        rest(std::max<std::int64_t>(period_us - spent, 1000));
    }
}

// --- the read-back route -----------------------------------------------------------------------

[[nodiscard]] std::string stats_json() {
    const PanelStats stats = panel_stats();
    const panel::TouchCounters& counters = panel::touch_counters();
    // The last presses' positions, newest first: [[x,y],...].
    char recent[128] = "[";
    {
        const std::uint32_t total = counters.recent_next.load();
        const std::uint32_t shown = std::min(total, panel::TouchCounters::kRecent);
        std::size_t used = 1;
        for (std::uint32_t i = 0; i < shown; ++i) {
            const std::uint32_t packed =
                counters.recent[(total - 1 - i) % panel::TouchCounters::kRecent].load();
            used += static_cast<std::size_t>(std::snprintf(
                recent + used, sizeof recent - used, "%s[%u,%u]", i == 0 ? "" : ",",
                static_cast<unsigned>(packed >> 16), static_cast<unsigned>(packed & 0xFFFFu)));
        }
        std::snprintf(recent + used, sizeof recent - used, "]");
    }
    const WallClockStats clock = wall_clock_stats();
    char buffer[1792];
    std::snprintf(buffer, sizeof buffer,
                  "{\"kind\":\"%s\",\"running\":%s,\"width\":%d,\"height\":%d,\"touch\":%s,"
                  "\"touch_reads\":%lu,\"touch_failures\":%lu,\"touch_points\":%lu,"
                  "\"touch_x\":%d,\"touch_y\":%d,\"touch_error\":%d,\"touch_recent\":%s,"
                  "\"screen\":\"%s\",\"view\":\"%s\",\"dimmed\":%s,\"backlight\":%d,\"dev_view\":%s,\"prefs_view\":%s,\"prefs_builds\":%lu,\"prefs_build_us_max\":%lu,\"dev_refreshes\":%lu,"
                  "\"dev_refresh_us_last\":%lu,\"dev_refresh_us_max\":%lu,"
                  "\"clock_enabled\":%s,\"clock_asking\":%s,\"clock_synced\":%s,\"clock_syncs\":%lu,\"demo\":%d,\"refreshes\":%lu,\"pixels\":%llu,\"render_us_last\":%lu,"
                  "\"render_us_mean\":%lu,\"render_us_max\":%lu,\"rebuilds\":%lu,"
                  "\"rebuild_us_max\":%lu,\"late_cycles\":%lu,\"cycle_us_max\":%lu,"
                  "\"stack_free\":%lu}\n",
                  stats.kind, stats.running ? "true" : "false", stats.width, stats.height,
                  stats.touch ? "true" : "false",
                  static_cast<unsigned long>(counters.reads.load()),
                  static_cast<unsigned long>(counters.failures.load()),
                  static_cast<unsigned long>(counters.touches.load()), counters.x.load(),
                  counters.y.load(), counters.last_error.load(), recent, stats.screen,
                  iclforge::nowview::view_name(g_follow.view()),
                  stats.dimmed ? "true" : "false", stats.backlight, stats.dev_view ? "true" : "false",
                  stats.prefs_view ? "true" : "false",
                  static_cast<unsigned long>(stats.prefs_builds),
                  static_cast<unsigned long>(stats.prefs_build_us_max),
                  static_cast<unsigned long>(stats.dev_refreshes),
                  static_cast<unsigned long>(stats.dev_refresh_us_last),
                  static_cast<unsigned long>(stats.dev_refresh_us_max), clock.enabled ? "true" : "false",
                  clock.asking ? "true" : "false", clock.synced ? "true" : "false",
                  static_cast<unsigned long>(clock.syncs), panel::demo_mode(),
                  static_cast<unsigned long>(stats.refreshes),
                  static_cast<unsigned long long>(stats.pixels),
                  static_cast<unsigned long>(stats.render_us_last),
                  static_cast<unsigned long>(stats.render_us_mean),
                  static_cast<unsigned long>(stats.render_us_max),
                  static_cast<unsigned long>(stats.rebuilds),
                  static_cast<unsigned long>(stats.rebuild_us_max),
                  static_cast<unsigned long>(stats.late_cycles),
                  static_cast<unsigned long>(stats.cycle_us_max),
                  static_cast<unsigned long>(stats.stack_free));
    return buffer;
}

esp_err_t on_stats(httpd_req_t* req) {
    const std::string json = stats_json();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json.data(), static_cast<ssize_t>(json.size()));
}

void put_u32(std::uint8_t* at, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        at[i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

// The picture as a 24-bit BMP, bottom row first. Copied whole under the lock, so that it is one
// picture and not the top of one and the bottom of the next, and sent after it.
esp_err_t on_bitmap(httpd_req_t* req) {
    const int width = g_display.width;
    const int height = g_display.height;
    if (g_display.frame == nullptr || width <= 0 || height <= 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "this panel has no frame to read back");
        return ESP_OK;
    }
    const std::size_t bytes = static_cast<std::size_t>(width) * height * sizeof(std::uint16_t);
    auto* const copy = static_cast<std::uint16_t*>(
        heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (copy == nullptr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory for a copy of the picture");
        return ESP_OK;
    }
    {
        const std::lock_guard lock(g_lock);
        std::memcpy(copy, g_display.frame, bytes);
    }

    const std::size_t row_bytes = (static_cast<std::size_t>(width) * 3 + 3) / 4 * 4;
    const std::size_t image_bytes = row_bytes * static_cast<std::size_t>(height);
    std::uint8_t header[54] = {'B', 'M'};
    put_u32(header + 2, static_cast<std::uint32_t>(54 + image_bytes));
    put_u32(header + 10, 54);
    put_u32(header + 14, 40);
    put_u32(header + 18, static_cast<std::uint32_t>(width));
    put_u32(header + 22, static_cast<std::uint32_t>(height));
    header[26] = 1;
    header[28] = 24;
    put_u32(header + 34, static_cast<std::uint32_t>(image_bytes));
    httpd_resp_set_type(req, "image/bmp");
    esp_err_t result = httpd_resp_send_chunk(req, reinterpret_cast<const char*>(header), sizeof header);

    constexpr int kRowsAtOnce = 16;
    auto* const block = static_cast<std::uint8_t*>(
        heap_caps_malloc(row_bytes * kRowsAtOnce, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (block == nullptr) {
        result = ESP_FAIL;
    }
    for (int top = height - 1; result == ESP_OK && top >= 0; top -= kRowsAtOnce) {
        const int rows = std::min(kRowsAtOnce, top + 1);
        for (int r = 0; r < rows; ++r) {
            const std::uint16_t* const source = copy + static_cast<std::size_t>(top - r) * width;
            std::uint8_t* out = block + static_cast<std::size_t>(r) * row_bytes;
            for (int x = 0; x < width; ++x) {
                const std::uint16_t p = source[x];
                const std::uint8_t r5 = (p >> 11) & 0x1F;
                const std::uint8_t g6 = (p >> 5) & 0x3F;
                const std::uint8_t b5 = p & 0x1F;
                *out++ = static_cast<std::uint8_t>((b5 << 3) | (b5 >> 2));
                *out++ = static_cast<std::uint8_t>((g6 << 2) | (g6 >> 4));
                *out++ = static_cast<std::uint8_t>((r5 << 3) | (r5 >> 2));
            }
            for (std::size_t pad = static_cast<std::size_t>(width) * 3; pad < row_bytes; ++pad) {
                *out++ = 0;
            }
        }
        result = httpd_resp_send_chunk(req, reinterpret_cast<const char*>(block),
                                       static_cast<ssize_t>(row_bytes * rows));
    }
    heap_caps_free(block);
    heap_caps_free(copy);
    if (result == ESP_OK) {
        httpd_resp_send_chunk(req, nullptr, 0);
    }
    return result;
}

esp_err_t on_demo(httpd_req_t* req) {
    char query[32] = {};
    char value[8] = {};
    int mode = -1;
    if (httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK &&
        httpd_query_key_value(query, "mode", value, sizeof value) == ESP_OK) {
        mode = std::atoi(value);
    }
    if (mode < 0 || mode > 2) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "mode is 0 (off), 1 (when the player holds no lyrics) or 2 (always)");
        return ESP_OK;
    }
    panel::set_demo_mode(mode);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "ok\n", HTTPD_RESP_USE_STRLEN);
}

esp_err_t on_tap(httpd_req_t* req) {
    char query[64] = {};
    char value[16] = {};
    int x = -1;
    int y = -1;
    if (httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK) {
        if (httpd_query_key_value(query, "x", value, sizeof value) == ESP_OK) {
            x = std::atoi(value);
        }
        if (httpd_query_key_value(query, "y", value, sizeof value) == ESP_OK) {
            y = std::atoi(value);
        }
    }
    if (x < 0 || y < 0 || x >= g_display.width || y >= g_display.height) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "x and y are pixels on the panel");
        return ESP_OK;
    }
    panel::InjectedPointer& pointer = panel::injected_pointer();
    pointer.x.store(x);
    pointer.y.store(y);
    pointer.down.store(true);
    // LVGL reads the pointer every refresh period: hold it down for two of them.
    vTaskDelay(pdMS_TO_TICKS(120));
    pointer.down.store(false);
    vTaskDelay(pdMS_TO_TICKS(60));
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "tapped\n", HTTPD_RESP_USE_STRLEN);
}

void start_capture_server() {
    if (kCapturePort == 0) {
        return;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = kCapturePort;
    // Each server has a UDP control socket of its own: the control surface's keeps the default,
    // 32768, and the Sendspin host's took the next (found on a board: 32769 was in use, errno 112).
    config.ctrl_port = 32781;
    config.max_uri_handlers = 5;
    config.max_open_sockets = 2;
    config.lru_purge_enable = true;
    config.stack_size = 6144;
    config.task_priority = 3;
    if (httpd_start(&g_server, &config) != ESP_OK) {
        std::printf("panel: could not start the read-back server on port %d\n", kCapturePort);
        g_server = nullptr;
        return;
    }
    httpd_uri_t bitmap{};
    bitmap.uri = "/panel.bmp";
    bitmap.method = HTTP_GET;
    bitmap.handler = &on_bitmap;
    httpd_register_uri_handler(g_server, &bitmap);
    httpd_uri_t stats{};
    stats.uri = "/panel";
    stats.method = HTTP_GET;
    stats.handler = &on_stats;
    httpd_register_uri_handler(g_server, &stats);
    httpd_uri_t tap{};
    tap.uri = "/panel/tap";
    tap.method = HTTP_POST;
    tap.handler = &on_tap;
    httpd_register_uri_handler(g_server, &tap);
    httpd_uri_t demo{};
    demo.uri = "/panel/demo";
    demo.method = HTTP_POST;
    demo.handler = &on_demo;
    httpd_register_uri_handler(g_server, &demo);
    std::printf("panel: the picture is at http://<address>:%d/panel.bmp\n", kCapturePort);
}

}  // namespace

bool panel_built() { return true; }

bool panel_has_view(iclforge::nowview::View view) { return panel::now_view_drawn(view); }

iclforge::DisplayCaps panel_display_caps() {
    iclforge::DisplayCaps caps;
    if (!g_display_ready.load()) {
        return caps;
    }
    const std::string_view kind = g_display.kind;
    caps.kind = kind == "dsi" ? iclforge::DisplayKind::kDsi : iclforge::DisplayKind::kFrameBuffer;
    caps.width = g_display.width;
    caps.height = g_display.height;
    caps.touch = g_display.touch;
    caps.backlight = g_display.backlight;
    return caps;
}

iclforge::devview::Facts panel_board_facts() { return panel::gather_about_facts(); }

// The setters below are called by the preferences' backend: on the control surface's task (a PUT), and on THIS panel's
// task for a tap on the Preferences sheet, which holds g_lock for the frame that reads the tap. None of them takes g_lock
// (std::mutex is not recursive: the brightness setter did, and a tap on Brightness - or + hung the screen for good), and
// none touches the glass: PanelFollow is atomics, and the brightness only marks the glass stale for the loop below.
void panel_set_view(iclforge::nowview::View view) { g_follow.set_view(view); }

void panel_set_dim_after(int minutes) { g_follow.set_dim_after(minutes); }

void panel_set_speakers_style(iclforge::nowview::SpeakersStyle style) { g_follow.set_speakers_style(style); }

void panel_set_brightness(int percent) { g_follow.set_brightness(percent); }

void panel_set_clock_format(iclforge::wallclock::ClockFormat format) { g_follow.set_clock_format(format); }

void panel_set_night(const iclforge::nowview::NightSchedule& night) { g_follow.set_night(night); }

bool panel_start() {
    if (g_started.exchange(true)) {
        return g_stats.running;
    }
    if (!panel::psram_heap_ready()) {
        std::printf("panel: no PSRAM, which LVGL's heap is; no screen\n");
        return false;
    }
    g_follow.set_view(settings().now_view);
    g_follow.set_dim_after(settings().dim_after_minutes);
    g_follow.set_speakers_style(settings().speakers_style);
    g_follow.set_brightness(settings().brightness_percent);
    g_follow.set_clock_format(settings().clock_format);
    g_follow.set_night(settings().night);
    lv_init();
    lv_tick_set_cb(&tick_ms);
    if (!panel::display_open(g_display)) {
        std::printf("panel: no screen; the board plays as it would without one\n");
        return false;
    }
    lv_display_add_event_cb(g_display.lv, &on_refresh_start, LV_EVENT_REFR_START, nullptr);
    lv_display_add_event_cb(g_display.lv, &on_refresh_ready, LV_EVENT_REFR_READY, nullptr);
    lv_display_add_event_cb(g_display.lv, &on_flush_start, LV_EVENT_FLUSH_START, nullptr);
    if (!panel::screen_init(g_display.lv, g_display.width, g_display.height)) {
        return false;
    }

    g_display_ready.store(true);
    g_stats.kind = g_display.kind;
    g_stats.width = g_display.width;
    g_stats.height = g_display.height;
    g_stats.touch = g_display.touch;
    g_stats.screen = "idle";

    // The stack in PSRAM: the screen's own cost in internal RAM is nothing, which is the point. On any core, unless the audio is to come first
    // by the strictest rule (PANEL_YIELD 2): then on the decode task's, where the two cannot run side by side.
    constexpr BaseType_t kPanelCore = CONFIG_ICLFORGE_EXAMPLE_PANEL_YIELD == 2 && CONFIG_ICLFORGE_EXAMPLE_DECODE_CORE >= 0
                                          ? static_cast<BaseType_t>(CONFIG_ICLFORGE_EXAMPLE_DECODE_CORE)
                                          : tskNO_AFFINITY;
    const BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        &run, "panel", CONFIG_ICLFORGE_EXAMPLE_PANEL_STACK_BYTES, nullptr,
        CONFIG_ICLFORGE_EXAMPLE_PANEL_TASK_PRIORITY, &g_task, kPanelCore,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        std::printf("panel: its task could not be created\n");
        return false;
    }
    g_stats.running = true;
    start_capture_server();
    std::printf("panel: %s %d by %d, drawing at up to %d fps\n", g_stats.kind, g_stats.width,
                g_stats.height, kFps);
    return true;
}

PanelStats panel_stats() {
    // Not from the panel's own task, which holds g_lock for the frame: it reads the counters as they stand, as the
    // developer view does (read_dev_facts()), and a caller that forgot would otherwise hang the screen.
    if (xTaskGetCurrentTaskHandle() == g_task) {
        return g_stats;
    }
    const std::lock_guard lock(g_lock);
    return g_stats;
}

std::optional<std::string> panel_stats_text() {
    PanelStats snapshot;
    iclforge::panelstats::MetersStats meters;
    {
        // The panel's task holds g_lock for the frame and reads the counters as they stand; any other task takes it, which also keeps the Meters
        // view's figures from changing under the copy.
        std::optional<std::lock_guard<std::mutex>> guard;
        if (xTaskGetCurrentTaskHandle() != g_task) {
            guard.emplace(g_lock);
        }
        snapshot = g_stats;
        meters = panel::now_meters_stats();
    }
    if (!snapshot.running) {
        return std::nullopt;
    }
    return panel::panel_stats_report(snapshot, meters);
}

}  // namespace player

namespace player::panel {

// The time between the loop's frames while it draws: the panel's frame rate (kFps, above).
int panel_frame_us() { return 1'000'000 / std::max(kFps, 1); }

// What the developer view shows (dev_info.hpp). It is called from the panel's task, which holds
// g_lock for the frame, so the counters are read as they stand and not through panel_stats().
iclforge::devview::Facts read_dev_facts() { return gather_dev_facts(g_stats, touch_counters()); }

}  // namespace player::panel
