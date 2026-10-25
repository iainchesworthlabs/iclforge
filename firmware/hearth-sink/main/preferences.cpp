// The firmware's preferences Backend. See preferences.hpp.

#include "preferences.hpp"

#include "board_caps.hpp"
#include "discovery.hpp"
#include "layout_control.hpp"
#include "network.hpp"
#include "panel/panel.hpp"
#include "sendspin.hpp"
#include "settings.hpp"
#include "system_control.hpp"

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

    // Network. Only a board with both a cable and Wi-Fi has a choice; the call returns at once and the work is done on
    // a task of its own (network_set_preference), which the board keeps only once the interface holds an address.
    [[nodiscard]] std::string network_preference() const override {
        return network_has_choice() ? network_preference_name(settings().network_preference) : std::string();
    }

    [[nodiscard]] bool set_network_preference(std::string_view word) override {
        if (!network_has_choice()) {
            return false;
        }
        if (word == "auto") {
            return network_set_preference(NetworkPreference::automatic);
        }
        if (word == "ethernet") {
            return network_set_preference(NetworkPreference::ethernet);
        }
        return word == "wifi" && network_set_preference(NetworkPreference::wifi);
    }

    [[nodiscard]] std::string name() const override { return settings().name.data(); }

    // What a rename changes is iclforge/device_name.hpp's to say: the mDNS names now, with no restart, and the
    // Sendspin hello now or when the stream that plays has ended.
    [[nodiscard]] bool set_name(std::string_view name) override {
        if (!settings_set_name(name)) {
            return false;
        }
        discovery_rename();
        sendspin_name_changed();
        return true;
    }

    // System.
    [[nodiscard]] iclforge::devview::Facts facts() const override { return panel_board_facts(); }
    [[nodiscard]] iclforge::sysactions::Situation situation() const override { return system_situation(); }
    [[nodiscard]] std::optional<std::string> restart() override { return system_restart(); }
    [[nodiscard]] std::optional<std::string> factory_reset() override { return system_factory_reset(); }

    // The pairings are read from the Sendspin player's store; a build without the player has none.
    [[nodiscard]] std::vector<iclforge::sysactions::paired::Server> paired_servers() const override {
        std::vector<iclforge::sysactions::paired::Server> servers;
        if (const std::optional<iclforge::ControlPairings> listed = sendspin_pairings()) {
            servers.reserve(listed->servers.size());
            for (const iclforge::ControlPairing& p : listed->servers) {
                iclforge::sysactions::paired::Server server;
                server.server_id = p.server_id;
                server.name = p.name;
                server.connected = p.connected;
                server.last_playback = p.last_playback;
                server.seen = p.seen;
                servers.push_back(std::move(server));
            }
        }
        return servers;
    }

    [[nodiscard]] bool forget_server(std::string_view server_id) override {
        return sendspin_forget_server(server_id).value_or(false);
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
