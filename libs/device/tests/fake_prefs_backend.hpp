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
