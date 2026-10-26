// The System group of a board's Preferences, tested on the host - see iclforge/system_actions.hpp's own
// header comment: whether each action is available, the words of each confirming step, the held button
// and the list of paired servers.

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "iclforge/system_actions.hpp"

using namespace iclforge::sysactions;

namespace {

bool says(const Confirm& confirm, std::string_view text) {
    for (const std::string& line : confirm.lines) {
        if (line.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

const std::string kMusicAssistant = "gS3cmMlDUaQGhxYd0PF0x0jWR2OGdhxwUwBBwyD3O1c";
const std::string kHearth = "Q1vGr0WkzZ5c2hXU8eYy0fKp3tNnJmAs7LbD4oHqIwE";

paired::Server music_assistant() {
    paired::Server s;
    s.server_id = kMusicAssistant;
    s.name = "Music Assistant";
    s.connected = true;
    s.last_playback = true;
    s.seen = true;
    return s;
}

paired::Server hearth() {
    paired::Server s;
    s.server_id = kHearth;
    s.name = "Hearth on STUDIO-PC";
    s.seen = true;
    return s;
}

}  // namespace

TEST_CASE("restart is offered unless the new firmware is on trial or an update is under way", "[sysactions]") {
    Situation situation;
    CHECK(availability(Action::restart, situation).available);

    situation.firmware_on_trial = true;
    const Availability on_trial = availability(Action::restart, situation);
    CHECK_FALSE(on_trial.available);
    // A restart on trial goes back to the old image: the reason says so, and that waiting is the answer.
    CHECK(on_trial.reason.find("old one") != std::string::npos);
    CHECK(on_trial.reason.find("minute") != std::string::npos);

    situation = {};
    situation.update_busy = true;
    for (const Action action : {Action::restart, Action::reset_preferences, Action::factory_reset}) {
        const Availability busy = availability(action, situation);
        CHECK_FALSE(busy.available);
        CHECK(busy.reason == "A firmware update is under way.");
    }
}

TEST_CASE("a firmware on trial holds back what restarts the board, not what does not", "[sysactions]") {
    Situation situation;
    situation.firmware_on_trial = true;
    // Reset preferences restarts nothing, and forgetting a server none either.
    CHECK(availability(Action::reset_preferences, situation).available);
    CHECK(availability(Action::forget_server, situation).available);
    // The factory reset ends in a restart, which on trial goes back to the old image.
    const Availability erase = availability(Action::factory_reset, situation);
    CHECK_FALSE(erase.available);
    CHECK(erase.reason.find("old one") != std::string::npos);
}

TEST_CASE("the restart step says what stops", "[sysactions]") {
    Situation idle;
    const Confirm quiet = confirm_for(Action::restart, idle);
    CHECK(quiet.title == "Restart this player?");
    CHECK(quiet.accept == "Restart");
    CHECK_FALSE(quiet.hold);
    CHECK(quiet.tone == Tone::text);
    CHECK_FALSE(says(quiet, "stops"));

    Situation playing;
    playing.streaming = true;
    CHECK(says(confirm_for(Action::restart, playing), "What is playing stops."));
}

TEST_CASE("the reset-preferences step lists what changes and what stays, from the key table", "[sysactions]") {
    const Confirm confirm = confirm_for(Action::reset_preferences, Situation{});
    CHECK(confirm.title == "Reset preferences?");
    CHECK(confirm.accept == "Reset");
    CHECK_FALSE(confirm.hold);
    REQUIRE(confirm.lines.size() == 3);
    CHECK(confirm.lines[0].find("what the screen shows without lyrics") != std::string::npos);
    CHECK(confirm.lines[0].find("time zone") == std::string::npos);
    CHECK(confirm.lines[1].find("name") != std::string::npos);
    CHECK(confirm.lines[1].find("time zone") != std::string::npos);
    CHECK(confirm.lines[1].find("Wi-Fi network") != std::string::npos);
    CHECK(confirm.lines[1].find("paired servers") != std::string::npos);
    CHECK(confirm.lines[2].find("does not restart") != std::string::npos);
}

TEST_CASE("the factory reset lists what is lost", "[sysactions]") {
    Situation situation;
    situation.has_cable = true;
    situation.cable_link = true;
    situation.has_wifi = true;
    const Confirm confirm = confirm_for(Action::factory_reset, situation);
    CHECK(confirm.title == "Erase everything?");
    CHECK(confirm.tone == Tone::attention);
    for (const char* lost : {"name", "Wi-Fi network and password", "paired servers", "speaker layout", "DAC wiring",
                             "every preference", "The firmware stays"}) {
        INFO(lost);
        CHECK(says(confirm, lost));
    }
}

TEST_CASE("the factory reset on a board with a wired network says it stays", "[sysactions]") {
    Situation situation;
    situation.has_cable = true;
    situation.cable_link = true;
    situation.has_wifi = true;
    situation.paired = 2;
    const Confirm confirm = confirm_for(Action::factory_reset, situation);
    CHECK(says(confirm, "The wired network stays in use"));
    CHECK(says(confirm, "asks your router for an address"));
    CHECK_FALSE(says(confirm, "NO network"));
    CHECK_FALSE(says(confirm, "improv-wifi.com"));
    CHECK(says(confirm, "2 servers have to pair with this player again"));
}

TEST_CASE("the factory reset on a Wi-Fi-only board says it is stranded, and what recovers it", "[sysactions]") {
    Situation situation;
    situation.has_wifi = true;
    const Confirm confirm = confirm_for(Action::factory_reset, situation);
    CHECK(says(confirm, "This player has Wi-Fi only."));
    CHECK(says(confirm, "NO network"));
    // Improv over the USB serial port is what gives it one again.
    CHECK(says(confirm, "USB cable"));
    CHECK(says(confirm, "improv-wifi.com"));
    CHECK(says(confirm, "name and password"));
    CHECK_FALSE(says(confirm, "have to pair"));  // none paired: nothing to say of them
    CHECK(confirm.accept == "Continue");
}

TEST_CASE("the factory reset on a board whose wired network has no link is a stranded board too", "[sysactions]") {
    Situation situation;
    situation.has_cable = true;
    situation.cable_link = false;
    situation.has_wifi = true;
    const Confirm confirm = confirm_for(Action::factory_reset, situation);
    CHECK(says(confirm, "The wired network has no link."));
    CHECK(says(confirm, "NO network"));
    CHECK(says(confirm, "improv-wifi.com"));
}

TEST_CASE("an image that carries its own network falls back to it", "[sysactions]") {
    Situation situation;
    situation.has_wifi = true;
    situation.network_built_in = true;
    const Confirm confirm = confirm_for(Action::factory_reset, situation);
    CHECK(says(confirm, "which the player falls back to"));
    CHECK_FALSE(says(confirm, "NO network"));
}

TEST_CASE("the factory reset's second step is a held button, and the first is not", "[sysactions]") {
    Situation situation;
    CHECK_FALSE(confirm_for(Action::factory_reset, situation).hold);
    const Confirm hold = hold_step(Action::factory_reset);
    CHECK(hold.hold);
    CHECK(hold.tone == Tone::attention);
    CHECK(hold.title == "Hold to erase");
    CHECK(says(hold, "3 seconds"));
    CHECK(says(hold, "Letting go cancels"));
    // The restart and the plain reset are never held.
    CHECK_FALSE(confirm_for(Action::restart, situation).hold);
    CHECK_FALSE(confirm_for(Action::reset_preferences, situation).hold);
    // And the seconds the words say are the seconds the button takes.
    CHECK(HoldConfirm::kHoldMs == 3000);
}

TEST_CASE("a held button fires after the hold, once", "[sysactions]") {
    HoldConfirm hold;
    CHECK(hold.progress(0) == 0.0F);
    CHECK_FALSE(hold.done(0));

    hold.press(1000);
    CHECK(hold.holding());
    CHECK(hold.progress(1000) == 0.0F);
    CHECK(hold.progress(2500) == 0.5F);
    CHECK_FALSE(hold.done(3999));
    CHECK(hold.done(4000));
    CHECK(hold.progress(4000) == 1.0F);
    CHECK_FALSE(hold.holding());
    // Once: asking again, or holding on, does not run it a second time.
    CHECK_FALSE(hold.done(4001));
    hold.press(5000);
    CHECK_FALSE(hold.done(9000));
    hold.reset();
    CHECK(hold.progress(9000) == 0.0F);
}

TEST_CASE("letting go before the end cancels the hold", "[sysactions]") {
    HoldConfirm hold;
    hold.press(0);
    CHECK_FALSE(hold.done(2999));
    hold.release();
    CHECK(hold.progress(3000) == 0.0F);
    CHECK_FALSE(hold.done(3000));
    CHECK_FALSE(hold.done(100000));
    // Pressing again starts from nothing.
    hold.press(10000);
    CHECK_FALSE(hold.done(12999));
    CHECK(hold.done(13000));
}

TEST_CASE("a hold across the millisecond clock's wrap still counts", "[sysactions]") {
    HoldConfirm hold;
    const std::uint32_t start = 0xFFFFFF00U;
    hold.press(start);
    CHECK_FALSE(hold.done(start + 2999));
    CHECK(hold.progress(start + 1500) == 0.5F);
    CHECK(hold.done(start + 3000));  // 0x00000BB8 + wrap
}

TEST_CASE("an action's word once it has been asked for", "[sysactions]") {
    CHECK(working_text(Action::restart) == "Restarting...");
    CHECK(working_text(Action::factory_reset).find("Erasing everything") == 0);
    CHECK(working_text(Action::reset_preferences).find("back to their defaults") != std::string::npos);
    CHECK(working_text(Action::forget_server) == "Forgotten.");
}

// --- the list of paired servers ----------------------------------------------------------------------

TEST_CASE("a server_id is 43 characters of base64url", "[sysactions][paired]") {
    CHECK(paired::valid_server_id(kMusicAssistant));
    CHECK(paired::valid_server_id(kHearth));
    CHECK_FALSE(paired::valid_server_id(""));
    CHECK_FALSE(paired::valid_server_id("forget"));
    CHECK_FALSE(paired::valid_server_id(kMusicAssistant.substr(1)));
    CHECK_FALSE(paired::valid_server_id(kMusicAssistant + "A"));
    CHECK_FALSE(paired::valid_server_id(kMusicAssistant.substr(0, 42) + "="));
    CHECK_FALSE(paired::valid_server_id(kMusicAssistant.substr(0, 42) + " "));
    CHECK_FALSE(paired::valid_server_id(kMusicAssistant.substr(0, 8)));  // the console's prefix is not enough
}

TEST_CASE("the body that forgets one server names it, and a bare forget is never made", "[sysactions][paired]") {
    // POST /pairing "forget" alone forgets every pairing and makes a new identity: no list row makes it.
    CHECK(paired::forget_body(kMusicAssistant) == "forget " + kMusicAssistant);
    CHECK_FALSE(paired::forget_body("").has_value());
    CHECK_FALSE(paired::forget_body(" ").has_value());
    CHECK_FALSE(paired::forget_body("forget").has_value());
    CHECK_FALSE(paired::forget_body("reset").has_value());
    CHECK_FALSE(paired::forget_body(kMusicAssistant.substr(0, 20)).has_value());
    // A body that smuggles a second command through the id is not an id.
    CHECK_FALSE(paired::forget_body(kMusicAssistant.substr(0, 30) + "\nforget").has_value());
}

TEST_CASE("the list says what each server is doing", "[sysactions][paired]") {
    std::vector<paired::Server> servers = {music_assistant(), hearth()};
    const std::vector<paired::Row> idle = paired::describe(servers, false);
    REQUIRE(idle.size() == 2);
    CHECK(idle[0].title == "Music Assistant");
    CHECK(idle[0].detail == "connected \xC2\xB7 played here last");
    CHECK_FALSE(idle[0].playing_now);
    CHECK(idle[0].tone == Tone::text);
    CHECK(idle[0].server_id == kMusicAssistant);
    CHECK(idle[1].title == "Hearth on STUDIO-PC");
    CHECK(idle[1].detail == "paired");
    CHECK(idle[1].tone == Tone::muted);

    const std::vector<paired::Row> playing = paired::describe(servers, true);
    CHECK(playing[0].playing_now);
    CHECK(playing[0].detail == "playing now");
    CHECK_FALSE(playing[1].playing_now);
}

TEST_CASE("a stream that plays is the last server's, and only when it holds a connection", "[sysactions][paired]") {
    paired::Server s = music_assistant();
    CHECK(paired::playing_now(s, true));
    CHECK_FALSE(paired::playing_now(s, false));
    s.connected = false;
    CHECK_FALSE(paired::playing_now(s, true));
    s = music_assistant();
    s.last_playback = false;
    CHECK_FALSE(paired::playing_now(s, true));
}

TEST_CASE("a record with no name yet and one not seen since the start are said so", "[sysactions][paired]") {
    paired::Server s = hearth();
    s.name.clear();
    s.seen = false;
    const std::vector<paired::Row> rows = paired::describe({s}, false);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].title == "(no name yet)");
    CHECK(rows[0].detail == "not seen since the player started");
}

TEST_CASE("forgetting a server asks first, and says what follows", "[sysactions][paired]") {
    const std::vector<paired::Row> rows = paired::describe({music_assistant(), hearth()}, false);
    const Confirm confirm = paired::forget_confirm(rows[1], 2);
    CHECK(confirm.title == "Forget Hearth on STUDIO-PC?");
    CHECK(confirm.accept == "Forget");
    CHECK_FALSE(confirm.hold);
    CHECK(confirm.tone == Tone::text);
    CHECK(says(confirm, "erases its pairing"));
    CHECK(says(confirm, "has to pair with it again"));
    CHECK(says(confirm, "remove this player from its list"));
    CHECK_FALSE(says(confirm, "music stops"));
}

TEST_CASE("forgetting the server that is playing is allowed, with a warning that the music stops", "[sysactions][paired]") {
    const std::vector<paired::Row> rows = paired::describe({music_assistant(), hearth()}, true);
    REQUIRE(rows[0].playing_now);
    const Confirm confirm = paired::forget_confirm(rows[0], 2);
    CHECK(confirm.tone == Tone::attention);
    REQUIRE_FALSE(confirm.lines.empty());
    CHECK(confirm.lines.front() == "It is playing to this player now: the music stops.");
    CHECK(confirm.accept == "Forget");
}

TEST_CASE("forgetting the only paired server says nothing can play until one pairs again", "[sysactions][paired]") {
    const std::vector<paired::Row> rows = paired::describe({hearth()}, false);
    const Confirm confirm = paired::forget_confirm(rows[0], 1);
    CHECK(confirm.tone == Tone::attention);
    CHECK(says(confirm, "only server paired"));
}

TEST_CASE("a record with no name is forgotten as 'that server'", "[sysactions][paired]") {
    paired::Server s = hearth();
    s.name.clear();
    const std::vector<paired::Row> rows = paired::describe({s, music_assistant()}, false);
    const Confirm confirm = paired::forget_confirm(rows[0], 2);
    CHECK(says(confirm, "that server has to pair with it again"));
}

TEST_CASE("the empty list points at the server", "[sysactions][paired]") {
    CHECK(paired::describe({}, false).empty());
    CHECK(paired::empty_text().find("Music Assistant") != std::string::npos);
}
