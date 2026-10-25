// The firmware's preferences Backend. See preferences.hpp.

#include "preferences.hpp"

#include "board_caps.hpp"
#include "panel/panel.hpp"
#include "settings.hpp"
#include "sound.hpp"

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

    // Sound: sound.cpp, the same code the routes and the Speakers sheet go through.
    [[nodiscard]] bool identify_running() const override { return sound_identify_running(); }
    [[nodiscard]] std::string identify_blocked() const override { return sound_identify_blocked(); }
    [[nodiscard]] bool identify_each(bool start) override { return sound_identify_request(start ? "all" : "off").empty(); }
    [[nodiscard]] std::string speaker_trim_summary() const override { return sound_trim_summary(); }
    [[nodiscard]] bool set_speaker_trim_text(std::string_view statements) override {
        return sound_set_trim_text(statements).empty();
    }
};

}  // namespace

iclforge::prefs::Backend& preferences() {
    static Board board;
    return board;
}

}  // namespace player
