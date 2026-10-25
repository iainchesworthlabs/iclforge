#pragma once

// A preferences Backend (iclforge/preferences.hpp) that holds its values in memory, for the host tests: the board
// is a BoardCaps (iclforge/board_variants.hpp has them all) and the settings are fields. A preference added to the
// registry adds its pair of virtuals to Backend, and this is where the tests' side of that goes.

#include "iclforge/preferences.hpp"

namespace iclforge::prefs::testing {

class FakeBackend final : public Backend {
   public:
    explicit FakeBackend(BoardCaps board) : board_(std::move(board)) {}

    // The next setter call is refused, as a board that could not store the value would.
    bool refuse_next = false;
    // How many setters were called, so that a test can say "the board was never asked".
    int sets = 0;

    BoardCaps board_;
    nowview::View view_ = nowview::kDefaultView;
    int dim_minutes_ = nowview::kDefaultDimMinutes;
    int brightness_ = nowview::kDefaultBrightness;
    wallclock::ClockFormat clock_;
    nowview::NightSchedule night_;
    wallclock::ZoneChoice zone_;
    std::string layout_ = "2.0";
    bool playing_ = false;

    [[nodiscard]] BoardCaps caps() const override { return board_; }
    [[nodiscard]] nowview::View now_view() const override { return view_; }
    [[nodiscard]] bool set_now_view(nowview::View view) override {
        ++sets;
        if (take_refusal()) {
            return false;
        }
        view_ = view;
        return true;
    }
    [[nodiscard]] int dim_after_minutes() const override { return dim_minutes_; }
    [[nodiscard]] bool set_dim_after_minutes(int minutes) override {
        ++sets;
        if (take_refusal()) {
            return false;
        }
        dim_minutes_ = minutes;
        return true;
    }
    [[nodiscard]] int brightness() const override { return brightness_; }
    [[nodiscard]] bool set_brightness(int percent) override {
        ++sets;
        if (take_refusal()) {
            return false;
        }
        brightness_ = percent;
        return true;
    }
    [[nodiscard]] wallclock::ClockFormat clock_format() const override { return clock_; }
    [[nodiscard]] bool set_clock_format(wallclock::ClockFormat format) override {
        ++sets;
        if (take_refusal()) {
            return false;
        }
        clock_ = format;
        return true;
    }
    [[nodiscard]] nowview::NightSchedule night() const override { return night_; }
    [[nodiscard]] bool set_night(const nowview::NightSchedule& night) override {
        ++sets;
        if (take_refusal()) {
            return false;
        }
        night_ = night;
        return true;
    }
    [[nodiscard]] wallclock::ZoneChoice time_zone() const override { return zone_; }
    [[nodiscard]] bool set_time_zone(const wallclock::ZoneChoice& zone) override {
        ++sets;
        if (take_refusal()) {
            return false;
        }
        zone_ = zone;
        return true;
    }
    [[nodiscard]] std::string layout() const override { return layout_; }
    [[nodiscard]] bool set_layout(std::string_view layout) override {
        ++sets;
        if (take_refusal()) {
            return false;
        }
        layout_ = std::string(layout);
        return true;
    }
    [[nodiscard]] bool playing() const override { return playing_; }

   private:
    bool take_refusal() {
        const bool refused = refuse_next;
        refuse_next = false;
        return refused;
    }
};

}  // namespace iclforge::prefs::testing
