#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/dev_view.hpp"
#include "iclforge/settings_keys.hpp"

// The System group of a board's Preferences (planning/p4x-preferences.md, E4): Restart, Reset
// preferences, Factory reset, and the list of paired servers with a Forget on each. What the screen
// says before each one, whether the board can do it now, how long a finger holds a button that cannot be
// tapped by mistake, and what a row of the list says. Free of ESP-IDF and LVGL like dev_view.hpp: the
// firmware reads the facts (hearth_sink.cpp, system_actions.cpp), the preview fills them from a fixture,
// and test_system_actions.cpp holds the words to the letter.
//
// THE WORDS ARE FOR A PERSON IN A ROOM. A confirming step says what happens in plain sentences, what is
// lost, and for the one action that can strand a board, what brings it back.

namespace iclforge::sysactions {

using devview::Tone;

enum class Action : std::uint8_t { restart, reset_preferences, factory_reset, forget_server };

// What the board is doing and has, as far as these actions care.
struct Situation {
    // A stream is playing.
    bool streaming = false;
    // The running image is still on trial: a restart now goes back to the image before it.
    bool firmware_on_trial = false;
    // An update is being written, or the board is in flash mode.
    bool update_busy = false;
    // The build has a cable (Ethernet), and one with a link partner is plugged in.
    bool has_cable = false;
    bool cable_link = false;
    // The build has Wi-Fi, and the image itself carries a network to fall back to (network_source()
    // "built-in": a development build, never a published one).
    bool has_wifi = false;
    bool network_built_in = false;
    // The Sendspin servers this board is paired with.
    unsigned paired = 0;
};

struct Availability {
    bool available = true;
    // Why not, when it is not: shown greyed beside the entry.
    std::string reason;
};

[[nodiscard]] inline Availability availability(Action action, const Situation& situation) {
    if (situation.update_busy) {
        return {false, "A firmware update is under way."};
    }
    // A restart on trial goes back to the image before it, and so does the one a factory reset ends in.
    const bool restarts = action == Action::restart || action == Action::factory_reset;
    if (restarts && situation.firmware_on_trial) {
        return {false, "The new firmware is still being checked, and a restart now would go back to the old one. "
                       "Try again in a minute."};
    }
    return {};
}

// One confirming step: what it says and how it is answered.
struct Confirm {
    std::string title;
    // Sentences, one to a paragraph, in order.
    std::vector<std::string> lines;
    // The button that goes ahead, and the one that does not.
    std::string accept;
    std::string cancel = "Cancel";
    Tone tone = Tone::text;
    // The accept button is held (HoldConfirm::kHoldMs), not tapped.
    bool hold = false;
    // The board is certain to refuse what this step asks (the network preference's switch to an interface that holds
    // no address): the step says so and offers no way to go on.
    bool blocked = false;
};

namespace detail {

[[nodiscard]] inline std::string plural(unsigned count, std::string_view one, std::string_view many) {
    return std::to_string(count) + " " + std::string(count == 1 ? one : many);
}

}  // namespace detail

// --- restart, reset preferences, factory reset -----------------------------------------------------

[[nodiscard]] inline Confirm restart_confirm(const Situation& situation) {
    Confirm confirm;
    confirm.title = "Restart this player?";
    confirm.lines.push_back("It is back in about half a minute, with everything as it was.");
    if (situation.streaming) {
        confirm.lines.push_back("What is playing stops.");
    }
    confirm.accept = "Restart";
    return confirm;
}

// `changed` is what Reset preferences sets back on THIS board, as words for a sentence (the registry's
// reset_labels(caps)): "screen without lyrics", "dim after".
[[nodiscard]] inline Confirm reset_preferences_confirm(const std::vector<std::string>& changed) {
    Confirm confirm;
    confirm.title = "Reset preferences?";
    confirm.lines.push_back("These go back to how a new player behaves: " +
                            (changed.empty() ? std::string("nothing on this board") : settingskeys::joined(changed)) + ".");
    confirm.lines.push_back("These stay as they are: " + settingskeys::kept_words() + ", and the paired servers.");
    confirm.lines.push_back("The screen follows at once; the player does not restart.");
    confirm.accept = "Reset";
    return confirm;
}

// ... and as the key table words them, for a caller that has no registry to ask.
[[nodiscard]] inline Confirm reset_preferences_confirm(const Situation&) {
    return reset_preferences_confirm(settingskeys::detail::words(true));
}

// The first step of the factory reset: what is lost, whether the board can still be reached, and what
// brings it back. The second step is a held button (hold_step()).
[[nodiscard]] inline Confirm factory_reset_confirm(const Situation& situation) {
    Confirm confirm;
    confirm.title = "Erase everything?";
    confirm.tone = Tone::attention;
    confirm.lines.push_back(
        "This erases everything this player has been told: its name, its Wi-Fi network and password, its "
        "paired servers, the speaker layout, the DAC wiring and every preference. The firmware stays.");
    if (situation.paired > 0) {
        confirm.lines.push_back(detail::plural(situation.paired, "server has", "servers have") +
                                " to pair with this player again, and has to remove it from its list first.");
    }
    const bool reachable_on_cable = situation.has_cable && situation.cable_link;
    if (reachable_on_cable) {
        confirm.lines.push_back(
            "The wired network stays in use: the player asks your router for an address again, so look for it there. "
            "Give it a Wi-Fi network again from its web page, or leave it on wired.");
    } else if (situation.network_built_in) {
        confirm.lines.push_back("This image carries a Wi-Fi network of its own, which the player falls back to.");
    } else {
        confirm.lines.push_back(
            std::string(situation.has_cable ? "The wired network has no link. " : "This player has Wi-Fi only. ") +
            "After the reset it has NO network, and nothing on your network can reach it.");
        confirm.lines.push_back(
            "To put it back on Wi-Fi, connect it to a computer with a USB cable, open improv-wifi.com in "
            "Chrome or Edge, and send it the network's name and password.");
    }
    confirm.accept = "Continue";
    return confirm;
}

// How long the factory reset's button has to be held, and the step that asks for it.
[[nodiscard]] inline Confirm hold_step(Action action) {
    Confirm confirm;
    confirm.tone = Tone::attention;
    confirm.hold = true;
    confirm.cancel = "Cancel";
    if (action == Action::factory_reset) {
        confirm.title = "Hold to erase";
        confirm.lines.push_back("Keep your finger on the button for 3 seconds. Letting go cancels.");
        confirm.accept = "Hold to erase everything";
    }
    return confirm;
}

[[nodiscard]] inline Confirm confirm_for(Action action, const Situation& situation) {
    switch (action) {
        case Action::restart:
            return restart_confirm(situation);
        case Action::reset_preferences:
            return reset_preferences_confirm(situation);
        case Action::factory_reset:
            return factory_reset_confirm(situation);
        case Action::forget_server:
            break;
    }
    return {};
}

// What a screen says once a restart or reset has been asked for, and what it is waiting for.
[[nodiscard]] inline std::string working_text(Action action) {
    switch (action) {
        case Action::restart:
            return "Restarting...";
        case Action::reset_preferences:
            return "Preferences are back to their defaults.";
        case Action::factory_reset:
            return "Erasing everything and restarting...";
        case Action::forget_server:
            return "Forgotten.";
    }
    return {};
}

// --- the held button -------------------------------------------------------------------------------

// A button that goes ahead only when it has been held down for kHoldMs without a break. press() starts
// the hold, release() ends it, and done() says when it has gone on long enough; a finger that slips off
// has released. Once it has fired it stays fired until reset(), so the action runs once. Times are
// milliseconds on any steady clock that may wrap.
class HoldConfirm {
   public:
    static constexpr std::uint32_t kHoldMs = 3000;

    void press(std::uint32_t now_ms) {
        if (!fired_) {
            holding_ = true;
            since_ms_ = now_ms;
        }
    }
    void release() { holding_ = false; }
    void reset() {
        holding_ = false;
        fired_ = false;
    }

    // 0 to 1: how far the hold has gone.
    [[nodiscard]] float progress(std::uint32_t now_ms) const {
        if (fired_) {
            return 1.0F;
        }
        if (!holding_) {
            return 0.0F;
        }
        const std::uint32_t held = now_ms - since_ms_;
        return held >= kHoldMs ? 1.0F : static_cast<float>(held) / static_cast<float>(kHoldMs);
    }

    // True once, for the call that finds the hold complete.
    [[nodiscard]] bool done(std::uint32_t now_ms) {
        if (fired_ || !holding_ || now_ms - since_ms_ < kHoldMs) {
            return false;
        }
        fired_ = true;
        holding_ = false;
        return true;
    }

    [[nodiscard]] bool holding() const { return holding_; }

   private:
    bool holding_ = false;
    bool fired_ = false;
    std::uint32_t since_ms_ = 0;
};

// --- the list of paired servers ------------------------------------------------------------------------

namespace paired {

// One Sendspin server this board is paired with, as GET /pairing lists it.
struct Server {
    // base64url of the server's key, 43 characters: what POST /pairing forgets it by.
    std::string server_id;
    // What its hello called it; empty for a record from a firmware that kept no names, until that server
    // connects again.
    std::string name;
    // A connection this record authenticated is open.
    bool connected = false;
    // The server that last played here.
    bool last_playback = false;
    // The board has used the record since it started.
    bool seen = false;
};

struct Row {
    std::string server_id;
    std::string title;
    std::string detail;
    Tone tone = Tone::text;
    // It is playing to this board now.
    bool playing_now = false;
};

inline constexpr std::size_t kIdChars = 43;

// A server_id as the board writes it: base64url without padding, 43 characters for a 32-byte key.
[[nodiscard]] inline bool valid_server_id(std::string_view id) {
    if (id.size() != kIdChars) {
        return false;
    }
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

// The body of the POST that forgets this server alone. Nothing for anything but a whole server_id: a bare
// `forget` is every pairing and a new identity, and no screen sends it from a list row.
[[nodiscard]] inline std::optional<std::string> forget_body(std::string_view server_id) {
    if (!valid_server_id(server_id)) {
        return std::nullopt;
    }
    return "forget " + std::string(server_id);
}

// While a stream plays, the server that is playing it is the one this board last played for, and holds
// an open connection.
[[nodiscard]] inline bool playing_now(const Server& server, bool streaming) {
    return streaming && server.connected && server.last_playback;
}

[[nodiscard]] inline std::vector<Row> describe(const std::vector<Server>& servers, bool streaming) {
    std::vector<Row> rows;
    rows.reserve(servers.size());
    for (const Server& server : servers) {
        Row row;
        row.server_id = server.server_id;
        row.title = server.name.empty() ? std::string("(no name yet)") : server.name;
        row.playing_now = playing_now(server, streaming);
        std::vector<std::string_view> parts;
        if (row.playing_now) {
            parts.push_back("playing now");
        } else if (server.connected) {
            parts.push_back("connected");
        }
        if (server.last_playback && !row.playing_now) {
            parts.push_back("played here last");
        }
        if (!server.seen) {
            parts.push_back("not seen since the player started");
        }
        for (const std::string_view part : parts) {
            row.detail += row.detail.empty() ? "" : " \xC2\xB7 ";
            row.detail += part;
        }
        if (row.detail.empty()) {
            row.detail = "paired";
        }
        row.tone = server.connected ? Tone::text : Tone::muted;
        rows.push_back(std::move(row));
    }
    return rows;
}

[[nodiscard]] inline std::string empty_text() {
    return "No server is paired with this player. Pair it from your server: Music Assistant finds it as a "
           "Sendspin player.";
}

// The step before a server is forgotten. The server that is playing now is allowed: it is the one a
// person gives a board away from. The step says so, and that the music stops.
[[nodiscard]] inline Confirm forget_confirm(const Row& row, std::size_t total) {
    Confirm confirm;
    confirm.title = "Forget " + row.title + "?";
    confirm.accept = "Forget";
    if (row.playing_now) {
        confirm.tone = Tone::attention;
        confirm.lines.push_back("It is playing to this player now: the music stops.");
    }
    confirm.lines.push_back("This player stops listening to it and erases its pairing.");
    confirm.lines.push_back("To play to this player again, " + (row.title == "(no name yet)" ? std::string("that server") : row.title) +
                            " has to pair with it again, and has to remove this player from its list first.");
    if (total == 1) {
        confirm.tone = Tone::attention;
        confirm.lines.push_back("It is the only server paired: nothing can play here until one pairs again.");
    }
    return confirm;
}

}  // namespace paired

}  // namespace iclforge::sysactions
