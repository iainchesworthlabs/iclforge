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

   private:
    bool take_refusal() {
        const bool refused = refuse_next;
        refuse_next = false;
        return refused;
    }
};

}  // namespace iclforge::prefs::testing
