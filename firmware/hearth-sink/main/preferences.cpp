// The firmware's preferences Backend. See preferences.hpp.

#include "preferences.hpp"

#include "board_caps.hpp"
#include "layout_control.hpp"
#include "panel/panel.hpp"
#include "settings.hpp"

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

    [[nodiscard]] std::string layout() const override { return layout_text(); }

    // The same decision PUT /layout makes (judged over board_caps(), queued, stored); the registry has asked the
    // verdict already, and a layout the board cannot carry never gets here.
    [[nodiscard]] bool set_layout(std::string_view layout) override { return layout_set(layout).accepted; }

    [[nodiscard]] bool playing() const override { return player::playing(); }
};

}  // namespace

iclforge::prefs::Backend& preferences() {
    static Board board;
    return board;
}

}  // namespace player
