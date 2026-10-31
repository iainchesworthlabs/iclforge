// What the panel follows of the board's settings (iclforge/panel_follow.hpp): every setter works with the panel's lock
// held, the glass is told of a brightness by the panel's own task, and no `panel_set_*` of the firmware takes a lock.
//
// The last is the regression test of the hang the first run on the kit found (2026-10-26): a tap on Brightness - or +
// on the Preferences sheet called panel_set_brightness() ON the panel's task, which took the std::mutex that task already
// held for the frame, and the screen never answered again. The panel's code is ESP-IDF's and cannot be built on the host,
// so the test reads it, as test_settings_keys.cpp reads settings.cpp: every `panel_set_*` body is held to the rule.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <future>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "iclforge/panel_follow.hpp"

namespace nv = iclforge::nowview;
namespace wc = iclforge::wallclock;
using iclforge::panelfollow::PanelFollow;

namespace {

constexpr int kLit = 80;

std::string read_file(const std::string& relative) {
    std::ifstream in(std::string(ICLFORGE_REPO_ROOT) + "/" + relative, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// The text between the braces of the function whose signature starts at `at`.
std::string body_from(const std::string& source, std::size_t at) {
    const std::size_t open = source.find('{', at);
    if (open == std::string::npos) {
        return {};
    }
    int depth = 0;
    for (std::size_t i = open; i < source.size(); ++i) {
        if (source[i] == '{') {
            ++depth;
        } else if (source[i] == '}' && --depth == 0) {
            return source.substr(open + 1, i - open - 1);
        }
    }
    return {};
}

}  // namespace

TEST_CASE("the panel follows the defaults the settings start from", "[panel_follow]") {
    const PanelFollow follow(kLit);
    CHECK(follow.view() == nv::kDefaultView);
    CHECK(follow.dim_after_minutes() == nv::kDefaultDimMinutes);
    CHECK(follow.speakers_style() == nv::kDefaultSpeakersStyle);
    CHECK(follow.brightness() == nv::kDefaultBrightness);
    CHECK(follow.clock_format() == wc::ClockFormat{});
    CHECK(follow.night() == nv::NightSchedule{});
    CHECK(follow.backlight_request() == kLit);
    CHECK_FALSE(PanelFollow(kLit).take_backlight_stale());
}

TEST_CASE("every setter is read back whole", "[panel_follow]") {
    PanelFollow follow(kLit);
    follow.set_view(nv::View::kClock);
    follow.set_dim_after(30);
    follow.set_speakers_style(nv::SpeakersStyle::kPulse);
    follow.set_brightness(40);
    follow.set_clock_format({.twelve_hour = true, .seconds = true});
    const nv::NightSchedule night{.enabled = true, .from_min = 23 * 60 + 15, .to_min = 6 * 60, .idle = true};
    follow.set_night(night);
    CHECK(follow.view() == nv::View::kClock);
    CHECK(follow.dim_after_minutes() == 30);
    CHECK(follow.speakers_style() == nv::SpeakersStyle::kPulse);
    CHECK(follow.brightness() == 40);
    CHECK(follow.clock_format() == wc::ClockFormat{.twelve_hour = true, .seconds = true});
    CHECK(follow.night() == night);
}

TEST_CASE("a brightness is the glass's to be told of, once, by the panel's task", "[panel_follow]") {
    PanelFollow follow(kLit);
    // Nothing is marked until a brightness is set, and setting the others marks nothing.
    follow.set_view(nv::View::kDark);
    follow.set_dim_after(5);
    follow.set_clock_format({.twelve_hour = true});
    CHECK_FALSE(follow.take_backlight_stale());
    // Any number of changes since the loop last looked is one thing to do: the level the screen last asked for, at the
    // brightness the person has now.
    follow.set_brightness(70);
    follow.set_brightness(60);
    follow.set_brightness(50);
    CHECK(follow.take_backlight_stale());
    CHECK_FALSE(follow.take_backlight_stale());
    CHECK(follow.brightness() == 50);
    follow.ask_backlight(4);
    CHECK(follow.backlight_request() == 4);
    CHECK_FALSE(follow.take_backlight_stale());
}

TEST_CASE("a setter called while the panel's lock is held does not wait for it", "[panel_follow]") {
    // The panel's task holds its (non-recursive) lock for the frame in which it reads a tap, and the tap's setter runs on
    // that task. Held here as it is there; each setter is called under it from the same thread, and from another thread
    // too (a PUT). A setter that took the lock would hang the first and block the second: the test would not finish.
    std::mutex panel_lock;
    PanelFollow follow(kLit);
    const auto call_every_setter = [&follow](int round) {
        follow.set_view(round % 2 == 0 ? nv::View::kSpeakers : nv::View::kClock);
        follow.set_dim_after(round % 240 + 1);
        follow.set_speakers_style(round % 2 == 0 ? nv::SpeakersStyle::kGlow : nv::SpeakersStyle::kPulse);
        follow.set_brightness(10 + round % 91);
        follow.set_clock_format({.twelve_hour = round % 2 == 0, .seconds = round % 3 == 0});
        follow.set_night({.enabled = round % 2 == 0, .from_min = round % 1440, .to_min = (round * 7) % 1440, .idle = false});
    };
    {
        const std::lock_guard lock(panel_lock);
        for (int round = 0; round < 50; ++round) {
            call_every_setter(round);
        }
        CHECK(follow.take_backlight_stale());
        // A PUT while the panel's task is mid-frame.
        auto from_another_task = std::async(std::launch::async, [&] {
            for (int round = 50; round < 100; ++round) {
                call_every_setter(round);
            }
        });
        REQUIRE(from_another_task.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    }
    CHECK(follow.take_backlight_stale());
    CHECK(follow.brightness() == 10 + 99 % 91);
}

TEST_CASE("the panel's task sees the last brightness set from another task", "[panel_follow]") {
    PanelFollow follow(kLit);
    std::atomic<bool> stop{false};
    std::atomic<int> applied{0};
    std::mutex panel_lock;
    // The panel's loop: under its lock, gives the glass the level when it is stale.
    std::thread loop([&] {
        while (!stop.load()) {
            const std::lock_guard lock(panel_lock);
            if (follow.take_backlight_stale()) {
                applied.store(follow.brightness());
            }
            std::this_thread::yield();
        }
    });
    for (int percent = 10; percent <= 100; ++percent) {
        follow.set_brightness(percent);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (applied.load() != 100 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    stop.store(true);
    loop.join();
    CHECK(applied.load() == 100);
}

TEST_CASE("no panel_set_* of the firmware takes the panel's lock or touches the glass", "[panel_follow]") {
    const std::string source = read_file("firmware/hearth-sink/main/panel/lvgl/panel.cpp");
    REQUIRE_FALSE(source.empty());
    // The loop is the one place that gives the glass a level, and panel_stats() the one reader that takes the lock off
    // the panel's own task.
    const std::vector<std::string> forbidden = {"g_lock",       "lock_guard",    "unique_lock", "std::mutex", "xSemaphore",
                                                "vTaskDelay",   "set_backlight", "display_set", "std::this_thread"};
    std::size_t at = 0;
    int found = 0;
    while ((at = source.find("void panel_set_", at)) != std::string::npos) {
        const std::size_t name_end = source.find('(', at);
        const std::string name = source.substr(at + 5, name_end - at - 5);
        const std::string body = body_from(source, at);
        INFO("function " << name);
        CHECK_FALSE(body.empty());
        for (const std::string& word : forbidden) {
            INFO("mentions " << word);
            CHECK(body.find(word) == std::string::npos);
        }
        ++found;
        at = name_end;
    }
    // View, dim, speakers' style, brightness, clock format and night schedule: a setter added without the rule is read too.
    CHECK(found >= 6);
    // The one reader of g_stats that may be asked on the panel's own task does not wait for itself.
    const std::size_t stats = source.find("PanelStats panel_stats()");
    REQUIRE(stats != std::string::npos);
    const std::string stats_body = body_from(source, stats);
    CHECK(stats_body.find("xTaskGetCurrentTaskHandle() == g_task") != std::string::npos);
    CHECK(stats_body.find("xTaskGetCurrentTaskHandle() == g_task") < stats_body.find("lock_guard"));
}

TEST_CASE("the panel's loop gives the glass a brightness set since its last cycle", "[panel_follow]") {
    const std::string source = read_file("firmware/hearth-sink/main/panel/lvgl/panel.cpp");
    REQUIRE_FALSE(source.empty());
    const std::size_t loop = source.find("void run(void*");
    REQUIRE(loop != std::string::npos);
    const std::string body = body_from(source, loop);
    const std::size_t take = body.find("g_follow.take_backlight_stale()");
    REQUIRE(take != std::string::npos);
    // Under the lock the frame takes, before the screen is updated.
    CHECK(body.find("lock_guard") < take);
    CHECK(take < body.find("panel::screen_update()"));
}
