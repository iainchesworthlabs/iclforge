// The firmware's preferences Backend. See preferences.hpp.

#include "preferences.hpp"

#include "board_caps.hpp"
#include "panel/panel.hpp"
#include "settings.hpp"
#include "wall_clock.hpp"

namespace player {
namespace {

class Board final : public iclforge::prefs::Backend {
   public:
    [[nodiscard]] iclforge::BoardCaps caps() const override { return board_caps(); }

    [[nodiscard]] iclforge::nowview::View now_view() const override { return settings().now_view; }

    // A view this build cannot draw is refused, and not stored for a later image to find; the screen follows at once.
    [[nodiscard]] bool set_now_view(iclforge::nowview::View view) override {
        if (!panel_has_view(view) || !settings_set_now_view(view)) {
            return false;
        }
        panel_set_view(view);
        return true;
    }

    [[nodiscard]] int dim_after_minutes() const override { return settings().dim_after_minutes; }

    [[nodiscard]] bool set_dim_after_minutes(int minutes) override {
        if (!settings_set_dim_after(minutes)) {
            return false;
        }
        panel_set_dim_after(minutes);
        return true;
    }

    [[nodiscard]] int brightness() const override { return settings().brightness_percent; }

    // A glass whose backlight is not under the board's control has no level to set, and is refused.
    [[nodiscard]] bool set_brightness(int percent) override {
        if (!panel_display_caps().backlight || !settings_set_brightness(percent)) {
            return false;
        }
        panel_set_brightness(percent);
        return true;
    }

    [[nodiscard]] iclforge::wallclock::ClockFormat clock_format() const override { return settings().clock_format; }

    [[nodiscard]] bool set_clock_format(iclforge::wallclock::ClockFormat format) override {
        if (!settings_set_clock_format(format)) {
            return false;
        }
        panel_set_clock_format(format);
        return true;
    }

    [[nodiscard]] iclforge::nowview::NightSchedule night() const override { return settings().night; }

    [[nodiscard]] bool set_night(const iclforge::nowview::NightSchedule& night) override {
        if (!settings_set_night(night)) {
            return false;
        }
        panel_set_night(night);
        return true;
    }

    [[nodiscard]] iclforge::wallclock::ZoneChoice time_zone() const override {
        return {.posix = settings().time_zone.data(), .name = settings().time_zone_name.data()};
    }

    // The zone is applied first, because the board's C library must read it as the route did, and is not left applied
    // if it cannot be stored.
    [[nodiscard]] bool set_time_zone(const iclforge::wallclock::ZoneChoice& zone) override {
        const std::string previous = settings().time_zone.data();
        if (!wall_clock_set_zone(zone.posix)) {
            return false;
        }
        if (!settings_set_time_zone(zone.posix, zone.name)) {
            (void)wall_clock_set_zone(previous);
            return false;
        }
        return true;
    }
};

}  // namespace

iclforge::prefs::Backend& preferences() {
    static Board board;
    return board;
}

}  // namespace player
