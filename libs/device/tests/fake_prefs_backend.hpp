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
    bool late_night_ = false;
    int brightness_ = nowview::kDefaultBrightness;
    wallclock::ClockFormat clock_;
    nowview::NightSchedule night_;
    wallclock::ZoneChoice zone_;
    std::string layout_ = "2.0";
    bool playing_ = false;
    nowview::SpeakersStyle style_ = nowview::kDefaultSpeakersStyle;

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
    [[nodiscard]] nowview::SpeakersStyle speakers_style() const override { return style_; }
    [[nodiscard]] bool set_speakers_style(nowview::SpeakersStyle style) override {
        ++sets;
        if (take_refusal()) {
            return false;
        }
        style_ = style;
        return true;
    }

    // Sound: the tone is running or not, something else may have the sink, and the trim is held as the words of PUT /trim
    // would leave it (the host test of the real table is libs/render's; this holds what the registry handed over).
    bool identifying_ = false;
    std::string blocked_;
    std::string trim_summary_ = "none set";
    std::string trim_text_;
    int tones_started = 0;

    [[nodiscard]] bool identify_running() const override { return identifying_; }
    [[nodiscard]] std::string identify_blocked() const override { return blocked_; }
    [[nodiscard]] bool identify_each(bool start) override {
        ++sets;
        if (take_refusal() || (start && !blocked_.empty())) {
            return false;
        }
        identifying_ = start;
        tones_started += start ? 1 : 0;
        return true;
    }
    [[nodiscard]] std::string speaker_trim_summary() const override { return trim_summary_; }
    [[nodiscard]] bool set_speaker_trim_text(std::string_view statements) override {
        ++sets;
        if (take_refusal()) {
            return false;
        }
        trim_text_ = std::string(statements);
        trim_summary_ = statements == "reset" ? "none set" : "1 speaker set";
        return true;
    }
    [[nodiscard]] bool late_night() const override { return late_night_; }
    [[nodiscard]] bool set_late_night(bool on) override {
        ++sets;
        if (take_refusal()) {
            return false;
        }
        late_night_ = on;
        return true;
    }

    // --- Network and System (E4)
    std::string network_pref_ = "auto";
    std::string name_ = "hearth-a1b2c3";
    // What facts() and situation() answer: a test sets the part it is about.
    devview::Facts facts_;
    sysactions::Situation situation_;
    // What restart() and factory_reset() answer (nothing is done, and the reason is given) and how often each ran.
    std::optional<std::string> action_refusal;
    int restarts = 0;
    int factory_resets = 0;
    std::vector<sysactions::paired::Server> paired_;
    std::vector<std::string> forgotten_;

    [[nodiscard]] std::string network_preference() const override { return network_pref_; }
    [[nodiscard]] bool set_network_preference(std::string_view word) override {
        ++sets;
        if (take_refusal()) {
            return false;
        }
        network_pref_ = std::string(word);
        return true;
    }
    [[nodiscard]] std::string name() const override { return name_; }
    [[nodiscard]] bool set_name(std::string_view name) override {
        ++sets;
        if (take_refusal()) {
            return false;
        }
        name_ = std::string(name);
        return true;
    }
    [[nodiscard]] devview::Facts facts() const override { return facts_; }
    [[nodiscard]] sysactions::Situation situation() const override { return situation_; }
    [[nodiscard]] std::optional<std::string> restart() override {
        if (action_refusal) {
            return action_refusal;
        }
        ++restarts;
        return std::nullopt;
    }
    [[nodiscard]] std::optional<std::string> factory_reset() override {
        if (action_refusal) {
            return action_refusal;
        }
        ++factory_resets;
        return std::nullopt;
    }
    [[nodiscard]] std::vector<sysactions::paired::Server> paired_servers() const override { return paired_; }
    [[nodiscard]] bool forget_server(std::string_view server_id) override {
        for (auto it = paired_.begin(); it != paired_.end(); ++it) {
            if (it->server_id == server_id) {
                forgotten_.push_back(it->server_id);
                paired_.erase(it);
                return true;
            }
        }
        return false;
    }

   private:
    bool take_refusal() {
        const bool refused = refuse_next;
        refuse_next = false;
        return refused;
    }
};

}  // namespace iclforge::prefs::testing
