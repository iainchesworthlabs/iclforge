// The panel's task: LVGL, the lyric screen, the drawing statistics and the read-back route. See
// ../panel.hpp and planning/karaoke-p4x-panel.md.
//
// One task owns LVGL. Each cycle it brings the screen up to date from what the Sendspin player holds
// and lets LVGL draw what that dirtied; a cycle takes a few hundred microseconds when a word fills
// and nothing when the picture sits still, and the task sleeps the rest of its period. It is below
// the decoder and the I2S writer in priority: the screen is drawn late before the audio is.

#include "../panel.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>

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

#include "../../network.hpp"
#include "../../sendspin.hpp"
#include "../../settings.hpp"
#include "board_info.hpp"
#include "display.hpp"
#include "lyric_screen.hpp"

namespace player::panel {

// lv_memory.cpp: LVGL's heap, in PSRAM. Called below so that the object is linked at all.
bool psram_heap_ready();

InjectedPointer& injected_pointer() {
    static InjectedPointer pointer;
    return pointer;
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

std::int64_t time_us() { return esp_timer_get_time(); }

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
}

void on_flush_start(lv_event_t* event) {
    const auto* const area = static_cast<const lv_area_t*>(lv_event_get_param(event));
    if (area != nullptr) {
        g_stats.pixels += static_cast<std::uint64_t>(lv_area_get_width(area)) * lv_area_get_height(area);
    }
}

void run(void* /*arg*/) {
    const std::int64_t fill_period_us = 1'000'000 / std::max(kFps, 1);
    constexpr std::int64_t kRestPeriodUs = 100'000;
    std::int64_t period_us = fill_period_us;
    std::uint32_t reported_ms = tick_ms();
    for (;;) {
        const std::int64_t started = esp_timer_get_time();
        bool filling = false;
        {
            const std::lock_guard lock(g_lock);
            filling = panel::screen_update();
            (void)lv_timer_handler();
            const panel::ScreenStats screen = panel::screen_stats();
            g_stats.screen = screen.name;
            g_stats.rebuilds = screen.rebuilds;
            g_stats.rebuild_us_max = screen.rebuild_us_max;
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
        const std::int64_t rest_us = std::max<std::int64_t>(period_us - spent, 1000);
        vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS(rest_us / 1000)));
    }
}

// --- the read-back route -----------------------------------------------------------------------

[[nodiscard]] std::string stats_json() {
    const PanelStats stats = panel_stats();
    char buffer[640];
    std::snprintf(buffer, sizeof buffer,
                  "{\"kind\":\"%s\",\"running\":%s,\"width\":%d,\"height\":%d,\"touch\":%s,"
                  "\"screen\":\"%s\",\"demo\":%d,\"refreshes\":%lu,\"pixels\":%llu,\"render_us_last\":%lu,"
                  "\"render_us_mean\":%lu,\"render_us_max\":%lu,\"rebuilds\":%lu,"
                  "\"rebuild_us_max\":%lu,\"late_cycles\":%lu,\"cycle_us_max\":%lu,"
                  "\"stack_free\":%lu}\n",
                  stats.kind, stats.running ? "true" : "false", stats.width, stats.height,
                  stats.touch ? "true" : "false", stats.screen, panel::demo_mode(),
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

bool panel_start() {
    if (g_started.exchange(true)) {
        return g_stats.running;
    }
    if (!panel::psram_heap_ready()) {
        std::printf("panel: no PSRAM, which LVGL's heap is; no screen\n");
        return false;
    }
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

    g_stats.kind = g_display.kind;
    g_stats.width = g_display.width;
    g_stats.height = g_display.height;
    g_stats.touch = g_display.touch;
    g_stats.screen = "idle";

    // The stack in PSRAM: the screen's own cost in internal RAM is nothing, which is the point.
    const BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        &run, "panel", CONFIG_ICLFORGE_EXAMPLE_PANEL_STACK_BYTES, nullptr,
        CONFIG_ICLFORGE_EXAMPLE_PANEL_TASK_PRIORITY, &g_task, tskNO_AFFINITY,
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
    const std::lock_guard lock(g_lock);
    return g_stats;
}

}  // namespace player
