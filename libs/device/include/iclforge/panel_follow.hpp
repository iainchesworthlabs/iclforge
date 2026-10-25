#pragma once

// What the panel follows of the board's settings (planning/p4x-preferences.md): the screen without lyrics, when it dims,
// how the speakers are drawn, the backlight's lit level, how the clock is written and the night schedule. Each is set
// by a setter of the preferences' backend, which a tap on the Preferences sheet reaches **on the panel's own task**
// (while it holds the lock that guards LVGL) and a PUT reaches on the control surface's, and each is read by the
// panel's task, a frame at a time.
//
// So nothing here takes a lock. A setter that did would be a deadlock for the tap: the panel's task holds that lock for
// the frame in which the tap is read, and a std::mutex is not recursive (the first run on the kit found it: a tap on
// Brightness - or + hung the screen for good, and port 80 went on answering). Everything is an atomic, written whole;
// the backlight is not changed by a setter at all: it marks the level stale, and the panel's task, which is the only
// one that talks to the glass, gives the glass the new level in its next cycle. Standard library only, so that the host
// tests it (libs/device/tests/test_panel_follow.cpp), which holds a lock of its own across every setter.

#include <atomic>
#include <cstdint>

#include "iclforge/now_view.hpp"
#include "iclforge/wall_clock.hpp"

namespace iclforge::panelfollow {

class PanelFollow {
   public:
    // `backlight_request` is the level the screen has asked the glass for until it asks for another.
    explicit PanelFollow(int backlight_request = 0) noexcept : backlight_request_(backlight_request) {}
    PanelFollow(const PanelFollow&) = delete;
    PanelFollow& operator=(const PanelFollow&) = delete;

    // --- the setters: any thread, any lock held, never a wait ------------------------------------------------------

    void set_view(nowview::View view) noexcept { view_.store(static_cast<int>(view)); }
    void set_dim_after(int minutes) noexcept { dim_after_minutes_.store(minutes); }
    void set_speakers_style(nowview::SpeakersStyle style) noexcept { speakers_style_.store(static_cast<int>(style)); }
    void set_clock_format(wallclock::ClockFormat format) noexcept {
        clock_format_.store(wallclock::clock_format_stored(format));
    }
    void set_night(const nowview::NightSchedule& night) noexcept { night_.store(nowview::night_stored(night)); }

    // The level the glass is at while the screen is lit. The glass itself is not touched here: it is marked stale, and
    // the panel's task gives it the level (take_backlight_stale(), then what it asked for) in its next cycle, so a tap
    // and a PUT cost the same and neither waits on the panel's lock.
    void set_brightness(int percent) noexcept {
        brightness_.store(percent);
        backlight_stale_.store(true);
    }

    // --- the panel's task ------------------------------------------------------------------------------------------

    [[nodiscard]] nowview::View view() const noexcept { return static_cast<nowview::View>(view_.load()); }
    [[nodiscard]] int dim_after_minutes() const noexcept { return dim_after_minutes_.load(); }
    [[nodiscard]] nowview::SpeakersStyle speakers_style() const noexcept {
        return static_cast<nowview::SpeakersStyle>(speakers_style_.load());
    }
    [[nodiscard]] int brightness() const noexcept { return brightness_.load(); }
    [[nodiscard]] wallclock::ClockFormat clock_format() const noexcept {
        return wallclock::clock_format_from_stored(clock_format_.load()).value_or(wallclock::ClockFormat{});
    }
    [[nodiscard]] nowview::NightSchedule night() const noexcept {
        return nowview::night_from_stored(night_.load()).value_or(nowview::NightSchedule{});
    }

    // The level the screen last asked the glass for (display.hpp's three: lit, the dim floor, off), which the person's
    // brightness turns into a percent (nowview::backlight_percent).
    void ask_backlight(int level) noexcept { backlight_request_.store(level); }
    [[nodiscard]] int backlight_request() const noexcept { return backlight_request_.load(); }

    // True once for any number of set_brightness() calls since it last was: the glass is behind, and the caller (the
    // panel's task) gives it the level for backlight_request() again.
    [[nodiscard]] bool take_backlight_stale() noexcept { return backlight_stale_.exchange(false); }

   private:
    std::atomic<int> view_{static_cast<int>(nowview::kDefaultView)};
    std::atomic<int> dim_after_minutes_{nowview::kDefaultDimMinutes};
    std::atomic<int> speakers_style_{static_cast<int>(nowview::kDefaultSpeakersStyle)};
    std::atomic<int> brightness_{nowview::kDefaultBrightness};
    std::atomic<int> clock_format_{0};
    std::atomic<std::int32_t> night_{nowview::night_stored(nowview::NightSchedule{})};
    std::atomic<int> backlight_request_;
    std::atomic<bool> backlight_stale_{false};

    static_assert(std::atomic<int>::is_always_lock_free && std::atomic<std::int32_t>::is_always_lock_free &&
                      std::atomic<bool>::is_always_lock_free,
                  "a setter that may be called with the panel's lock held must not take a lock of its own");
};

}  // namespace iclforge::panelfollow
