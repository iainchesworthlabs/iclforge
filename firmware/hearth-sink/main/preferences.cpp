// The firmware's preferences Backend. See preferences.hpp.

#include "preferences.hpp"

#include "board_caps.hpp"
#include "discovery.hpp"
#include "layout_control.hpp"
#include "network.hpp"
#include "panel/panel.hpp"
#include "sendspin.hpp"
#include "settings.hpp"
#include "sound.hpp"
#include "system_control.hpp"
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

    [[nodiscard]] iclforge::nowview::SpeakersStyle speakers_style() const override { return settings().speakers_style; }

    [[nodiscard]] bool set_speakers_style(iclforge::nowview::SpeakersStyle style) override {
        if (!settings_set_speakers_style(style)) {
            return false;
        }
        panel_set_speakers_style(style);
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

    [[nodiscard]] std::string layout() const override { return layout_text(); }

    // The same decision PUT /layout makes (judged over board_caps(), queued, stored); the registry has asked the
    // verdict already, and a layout the board cannot carry never gets here.
    [[nodiscard]] bool set_layout(std::string_view layout) override { return layout_set(layout).accepted; }

    [[nodiscard]] bool playing() const override { return player::playing(); }

    // Sound: sound.cpp, the same code the routes and the Speakers sheet go through.
    [[nodiscard]] bool identify_running() const override { return sound_identify_running(); }
    [[nodiscard]] std::string identify_blocked() const override { return sound_identify_blocked(); }
    [[nodiscard]] bool identify_each(bool start) override { return sound_identify_request(start ? "all" : "off").empty(); }
    [[nodiscard]] std::string speaker_trim_summary() const override { return sound_trim_summary(); }
    [[nodiscard]] bool set_speaker_trim_text(std::string_view statements) override {
        return sound_set_trim_text(statements).empty();
    }

    // Late night (planning/hearth-late-night.md): stored, and the Sendspin player follows from the next stream.
    [[nodiscard]] bool late_night() const override {
        return settings().dynamic_range == iclforge::ac3::DynamicRange::kLateNight;
    }
    [[nodiscard]] bool set_late_night(bool on) override {
        const iclforge::ac3::DynamicRange range =
            on ? iclforge::ac3::DynamicRange::kLateNight : iclforge::ac3::DynamicRange::kStandard;
        if (!settings_set_dynamic_range(range)) {
            return false;
        }
        sendspin_set_dynamic_range(range);
        return true;
    }
};

}  // namespace

iclforge::prefs::Backend& preferences() {
    static Board board;
    return board;
}

}  // namespace player
