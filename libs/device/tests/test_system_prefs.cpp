// The Network and System entries of the preference registry (iclforge/preferences.hpp), E4: the choice between a
// cable and Wi-Fi, the name, About, the paired servers, Restart, Reset preferences and Factory reset. Against every
// known board (iclforge/board_variants.hpp), with the in-memory backend of the other registry tests.

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "fake_prefs_backend.hpp"
#include "iclforge/board_variants.hpp"
#include "iclforge/preferences.hpp"

namespace prefs = iclforge::prefs;
namespace nv = iclforge::nowview;
namespace sa = iclforge::sysactions;
using iclforge::BoardCaps;
using prefs::testing::FakeBackend;

namespace {

bool contains(const std::string& text, std::string_view needle) { return text.find(needle) != std::string::npos; }

const std::string kMusicAssistant = "gS3cmMlDUaQGhxYd0PF0x0jWR2OGdhxwUwBBwyD3O1c";
const std::string kHearth = "Q1vGr0WkzZ5c2hXU8eYy0fKp3tNnJmAs7LbD4oHqIwE";

sa::paired::Server server(const std::string& id, const std::string& name, bool connected = false) {
    sa::paired::Server s;
    s.server_id = id;
    s.name = name;
    s.connected = connected;
    s.seen = true;
    return s;
}

prefs::Entry resolved(FakeBackend& backend, std::string_view id) {
    return prefs::resolve(*prefs::find(id), backend, backend.caps());
}

}  // namespace

TEST_CASE("the System group has its entries, and they are in the groups the owner asked for", "[prefs-system]") {
    const auto group_of = [](std::string_view id) { return prefs::find(id)->group; };
    REQUIRE(group_of("network-preference") == prefs::Group::kNetwork);
    REQUIRE(group_of("name") == prefs::Group::kNetwork);
    for (const char* id : {"about", "paired-servers", "restart", "reset-preferences", "factory-reset"}) {
        INFO(id);
        REQUIRE(group_of(id) == prefs::Group::kSystem);
    }
    REQUIRE(prefs::find("about")->kind == prefs::Kind::kInfo);
    REQUIRE(prefs::find("paired-servers")->kind == prefs::Kind::kList);
    REQUIRE(prefs::find("name")->kind == prefs::Kind::kText);
    for (const char* id : {"restart", "reset-preferences", "factory-reset"}) {
        REQUIRE(prefs::find(id)->kind == prefs::Kind::kAction);
    }
    REQUIRE(std::string(prefs::kind_name(prefs::Kind::kInfo)) == "info");
    REQUIRE(std::string(prefs::kind_name(prefs::Kind::kList)) == "list");
    // The routes they stand for.
    REQUIRE(std::string(prefs::find("name")->route) == "/name");
    REQUIRE(std::string(prefs::find("restart")->route) == "/restart");
    REQUIRE(std::string(prefs::find("reset-preferences")->route) == "/reset-preferences");
    REQUIRE(std::string(prefs::find("factory-reset")->route) == "/factory-reset");
}

TEST_CASE("every entry says whether Reset preferences touches it: values reset, what the board is is kept", "[prefs-system]") {
    std::set<std::string> reset;
    std::set<std::string> kept;
    for (const prefs::Pref& pref : prefs::registry()) {
        REQUIRE(pref.on_reset != prefs::OnReset::kUnset);
        (pref.on_reset == prefs::OnReset::kReset ? reset : kept).insert(pref.id);
    }
    // The choices a person made once.
    REQUIRE(reset.contains("now-view"));
    REQUIRE(reset.contains("dim-after"));
    for (const char* id : {"brightness", "clock-hours", "clock-seconds", "night-schedule", "night-times", "night-idle"}) {
        INFO(id);
        REQUIRE(reset.contains(id));
    }
    // What the board IS (where it is, how it is joined, what speakers it drives), and every entry that is not a value.
    for (const char* id : {"time-zone", "speaker-layout", "network-preference", "name", "about", "paired-servers", "restart",
                           "reset-preferences", "factory-reset"}) {
        INFO(id);
        REQUIRE(kept.contains(id));
    }
}

TEST_CASE("the wired-or-Wi-Fi choice is there only on a board that has both", "[prefs-system]") {
    for (const auto& variant : iclforge::known_variants()) {
        INFO(variant.name);
        FakeBackend backend(variant.caps);
        const prefs::Entry entry = resolved(backend, "network-preference");
        REQUIRE(entry.availability.available == variant.caps.network.choice());
        if (!entry.availability.available) {
            // The one interface a board has is named: the radio, wired, or neither.
            const char* which = variant.caps.network.wifi       ? "Wi-Fi only"
                                : variant.caps.network.ethernet ? "wired only"
                                                                : "no network";
            REQUIRE(contains(entry.availability.reason, which));
        }
    }
    BoardCaps cable_only = iclforge::variants::p4_rev3_kit();
    cable_only.network = {.ethernet = true};
    FakeBackend backend(cable_only);
    REQUIRE(contains(resolved(backend, "network-preference").availability.reason, "wired only"));
    BoardCaps none = iclforge::variants::p4_rev3_kit();
    none.network = {};
    FakeBackend isolated(none);
    REQUIRE(resolved(isolated, "network-preference").availability.reason == "This board has no network.");
}

TEST_CASE("setting the network preference goes to the board on a board with a choice, and not on one without", "[prefs-system]") {
    FakeBackend kit(iclforge::variants::p4_rev3_kit());
    REQUIRE(resolved(kit, "network-preference").value == "auto");
    for (const char* word : {"ethernet", "wifi", "auto"}) {
        INFO(word);
        const prefs::SetResult result = prefs::set_value(kit, *prefs::find("network-preference"), word);
        REQUIRE(result.ok);
        REQUIRE(kit.network_pref_ == word);
    }
    // "wired" is the label's word, not the route's.
    REQUIRE_FALSE(prefs::set_value(kit, *prefs::find("network-preference"), "wired").ok);
    REQUIRE(kit.network_pref_ == "auto");

    FakeBackend s3(iclforge::variants::s3_n16r8());
    const int before = s3.sets;
    const prefs::SetResult refused = prefs::set_value(s3, *prefs::find("network-preference"), "ethernet");
    REQUIRE_FALSE(refused.ok);
    REQUIRE(contains(refused.message, "Wi-Fi only"));
    REQUIRE(s3.sets == before);
}

TEST_CASE("the network preference asks first, in the words of the developer view", "[prefs-system]") {
    FakeBackend kit(iclforge::variants::p4_rev3_kit());
    iclforge::devview::Facts& facts = kit.facts_;
    facts.active = "ethernet";
    facts.preference = "auto";
    facts.ethernet.present = facts.ethernet.enabled = facts.ethernet.link = true;
    facts.ethernet.address = "192.168.1.163";
    facts.wifi.present = facts.wifi.enabled = facts.wifi.link = facts.wifi.configured = true;
    facts.wifi.address = "192.168.1.251";
    const prefs::Pref& pref = *prefs::find("network-preference");
    REQUIRE(pref.confirm_step != nullptr);

    const sa::Confirm wifi = pref.confirm_step(kit, "wifi");
    const iclforge::devview::Confirm said = iclforge::devview::confirm_text(facts, iclforge::devview::Preference::wifi);
    REQUIRE(wifi.title == said.headline);
    REQUIRE(wifi.lines.size() == 1);
    REQUIRE(wifi.lines[0] == said.detail);
    REQUIRE(wifi.title == "Use Wi-Fi only?");
    REQUIRE(contains(wifi.lines[0], "192.168.1.251"));
    REQUIRE(wifi.accept == "Switch");
    REQUIRE_FALSE(wifi.blocked);

    // A switch the board is certain to refuse says so and offers no way on.
    facts.wifi.configured = false;
    const sa::Confirm blocked = pref.confirm_step(kit, "wifi");
    REQUIRE(blocked.blocked);
    REQUIRE(blocked.tone == iclforge::devview::Tone::attention);
    REQUIRE(contains(blocked.lines[0], "No Wi-Fi network is stored"));
    // A word that is not a preference has no step.
    REQUIRE(pref.confirm_step(kit, "wired").title.empty());
}

TEST_CASE("a name is held to the rule before the board is asked", "[prefs-system]") {
    FakeBackend kit(iclforge::variants::p4_rev3_kit());
    const prefs::Pref& pref = *prefs::find("name");
    REQUIRE(resolved(kit, "name").value == "hearth-a1b2c3");
    REQUIRE(prefs::set_value(kit, pref, "Sitting room").ok);
    REQUIRE(kit.name_ == "Sitting room");
    REQUIRE(resolved(kit, "name").value == "Sitting room");
    REQUIRE(prefs::set_value(kit, pref, "K\xC3\xBC" "che").ok);

    const int sets = kit.sets;
    for (const std::string& bad : {std::string(), std::string(33, 'x'), std::string(" Attic"), std::string("At\ttic"),
                                   std::string("At\xFF" "tic"), std::string("\xC3\xA9") + std::string(31, 'x')}) {
        INFO(bad.size());
        REQUIRE_FALSE(prefs::set_value(kit, pref, bad).ok);
    }
    // None of them reached the board, and the name it has is the last good one.
    REQUIRE(kit.sets == sets);
    REQUIRE(kit.name_ == "K\xC3\xBC" "che");

    kit.refuse_next = true;
    REQUIRE_FALSE(prefs::set_value(kit, pref, "Attic").ok);
    REQUIRE(kit.name_ == "K\xC3\xBC" "che");
}

TEST_CASE("the name is available on every board, and so is About", "[prefs-system]") {
    for (const auto& variant : iclforge::known_variants()) {
        INFO(variant.name);
        FakeBackend backend(variant.caps);
        REQUIRE(resolved(backend, "name").availability.available);
        REQUIRE(resolved(backend, "about").availability.available);
        REQUIRE(resolved(backend, "restart").availability.available);
        REQUIRE(resolved(backend, "reset-preferences").availability.available);
        REQUIRE(resolved(backend, "factory-reset").availability.available);
    }
}

TEST_CASE("About has rows to read and nothing to set", "[prefs-system]") {
    FakeBackend kit(iclforge::variants::p4_rev3_kit());
    const prefs::Pref& pref = *prefs::find("about");
    REQUIRE_FALSE(prefs::set_value(kit, pref, "anything").ok);
    REQUIRE(kit.sets == 0);
    REQUIRE(resolved(kit, "about").value.empty());
    // The rows come from the board's facts through iclforge/about_view.hpp.
    kit.facts_.firmware = "v0.10.0-beta.1-5228-g0e45f23a7";
    kit.facts_.name = "Study";
    const std::vector<iclforge::aboutview::Row> rows = iclforge::aboutview::about_rows(kit.facts());
    REQUIRE(rows.front().label == "Version");
    REQUIRE(rows.front().value == "v0.10.0-beta.1-5228");
}

TEST_CASE("paired servers are listed only where there is a player to pair with", "[prefs-system]") {
    BoardCaps no_player = iclforge::variants::s3_n16r8();
    no_player.sendspin = false;
    FakeBackend backend(no_player);
    const prefs::Entry entry = resolved(backend, "paired-servers");
    REQUIRE_FALSE(entry.availability.available);
    REQUIRE(contains(entry.availability.reason, "no Sendspin player"));
    REQUIRE_FALSE(prefs::set_value(backend, *prefs::find("paired-servers"), "forget " + kMusicAssistant).ok);
}

TEST_CASE("forgetting a paired server names it, and nothing else forgets", "[prefs-system]") {
    FakeBackend kit(iclforge::variants::p4_rev3_kit());
    kit.paired_ = {server(kMusicAssistant, "Music Assistant", true), server(kHearth, "Hearth on STUDIO-PC")};
    const prefs::Pref& pref = *prefs::find("paired-servers");

    // A bare forget is every pairing and a new identity: the list never asks for it.
    for (const std::string& word : {std::string("forget"), std::string("forget "), std::string("reset"), std::string(""),
                                    "forget " + kMusicAssistant.substr(0, 8), "forget " + kMusicAssistant + "x",
                                    "forgetting " + kMusicAssistant, kMusicAssistant}) {
        INFO(word);
        REQUIRE_FALSE(prefs::set_value(kit, pref, word).ok);
    }
    REQUIRE(kit.paired_.size() == 2);
    REQUIRE(kit.forgotten_.empty());

    REQUIRE(prefs::set_value(kit, pref, "forget " + kHearth).ok);
    REQUIRE(kit.paired_.size() == 1);
    REQUIRE(kit.paired_[0].server_id == kMusicAssistant);
    REQUIRE(kit.forgotten_ == std::vector<std::string>{kHearth});

    // A whole id of a server the board has no pairing with is the board's refusal.
    const prefs::SetResult unknown = prefs::set_value(kit, pref, "forget " + kHearth);
    REQUIRE_FALSE(unknown.ok);
    REQUIRE(kit.paired_.size() == 1);
}

TEST_CASE("Restart asks first, runs through the board and says why it cannot", "[prefs-system]") {
    FakeBackend kit(iclforge::variants::p4_rev3_kit());
    const prefs::Pref& pref = *prefs::find("restart");
    REQUIRE(pref.confirm_step != nullptr);
    REQUIRE(pref.then_hold == nullptr);
    kit.situation_.streaming = true;
    REQUIRE(contains(pref.confirm_step(kit, "run").lines.back(), "What is playing stops."));

    REQUIRE(resolved(kit, "restart").inert.empty());
    REQUIRE(prefs::set_value(kit, pref, "run").ok);
    REQUIRE(kit.restarts == 1);
    // Only the word run starts it.
    REQUIRE_FALSE(prefs::set_value(kit, pref, "now").ok);
    REQUIRE(kit.restarts == 1);

    // The new firmware is still on trial: the control is greyed with the reason, and the board refuses.
    kit.situation_.firmware_on_trial = true;
    REQUIRE(contains(resolved(kit, "restart").inert, "old one"));
    kit.action_refusal = "The new firmware is still being checked.";
    REQUIRE_FALSE(prefs::set_value(kit, pref, "run").ok);
    REQUIRE(kit.restarts == 1);
}

TEST_CASE("Factory reset is two steps, the second a held button, and its first step reads the board", "[prefs-system]") {
    FakeBackend kit(iclforge::variants::p4_rev3_kit());
    const prefs::Pref& pref = *prefs::find("factory-reset");
    REQUIRE(pref.confirm_step != nullptr);
    REQUIRE(pref.then_hold != nullptr);

    kit.situation_.has_cable = true;
    kit.situation_.cable_link = true;
    kit.situation_.has_wifi = true;
    kit.situation_.paired = 2;
    const sa::Confirm first = pref.confirm_step(kit, "run");
    REQUIRE(first.title == "Erase everything?");
    REQUIRE_FALSE(first.hold);
    REQUIRE(contains(first.lines[0], "its name, its Wi-Fi network and password"));
    REQUIRE(contains(first.lines[1], "2 servers have to pair with this player again"));
    REQUIRE(contains(first.lines[2], "The wired network stays in use"));

    // No cable plugged in: the same step says the board is stranded, and what brings it back.
    kit.situation_.cable_link = false;
    const sa::Confirm stranded = pref.confirm_step(kit, "run");
    REQUIRE(contains(stranded.lines.back(), "improv-wifi.com"));
    REQUIRE(stranded.tone == sa::Tone::attention);

    const sa::Confirm second = pref.then_hold(kit, "run");
    REQUIRE(second.hold);
    REQUIRE(second.title == "Hold to erase");

    REQUIRE(prefs::set_value(kit, pref, "run").ok);
    REQUIRE(kit.factory_resets == 1);
    REQUIRE(kit.restarts == 0);
    kit.action_refusal = "A firmware update is under way.";
    REQUIRE_FALSE(prefs::set_value(kit, pref, "run").ok);
    REQUIRE(kit.factory_resets == 1);
    REQUIRE_FALSE(prefs::set_value(kit, pref, "erase everything").ok);
}

TEST_CASE("Reset preferences sets the values back and keeps what the board is", "[prefs-system]") {
    FakeBackend kit(iclforge::variants::p4_rev3_kit());
    kit.view_ = nv::View::kClock;
    kit.dim_minutes_ = 30;
    kit.name_ = "Attic";
    kit.network_pref_ = "wifi";
    kit.paired_ = {server(kMusicAssistant, "Music Assistant", true)};

    const prefs::ResetReport report = prefs::reset_preferences(kit);
    REQUIRE(report.ok());
    REQUIRE(report.reset == std::vector<std::string>{"now-view", "dim-after"});
    REQUIRE(kit.view_ == nv::kDefaultView);
    REQUIRE(kit.dim_minutes_ == nv::kDefaultDimMinutes);
    // The name, the network and the pairings stay.
    REQUIRE(kit.name_ == "Attic");
    REQUIRE(kit.network_pref_ == "wifi");
    REQUIRE(kit.paired_.size() == 1);
    REQUIRE(kit.restarts == 0);
    REQUIRE(kit.factory_resets == 0);

    // At the defaults, the board is not asked at all.
    kit.sets = 0;
    REQUIRE(prefs::reset_preferences(kit).reset.empty());
    REQUIRE(kit.sets == 0);
}

TEST_CASE("Reset preferences leaves alone what the board cannot make, and says what it could not do", "[prefs-system]") {
    // No screen: nothing of Display to reset, and the board is never asked.
    FakeBackend s3(iclforge::variants::s3_n16r8());
    s3.view_ = nv::View::kClock;
    s3.dim_minutes_ = 30;
    const prefs::ResetReport none = prefs::reset_preferences(s3);
    REQUIRE(none.reset.empty());
    REQUIRE(none.failed.empty());
    REQUIRE(s3.sets == 0);

    // A board that refuses one value: that one is reported, the other is still done.
    FakeBackend kit(iclforge::variants::p4_rev3_kit());
    kit.view_ = nv::View::kClock;
    kit.dim_minutes_ = 30;
    kit.refuse_next = true;
    const prefs::ResetReport partial = prefs::reset_preferences(kit);
    REQUIRE_FALSE(partial.ok());
    REQUIRE(partial.failed == std::vector<std::string>{"now-view"});
    REQUIRE(partial.reset == std::vector<std::string>{"dim-after"});
    REQUIRE(kit.view_ == nv::View::kClock);
    REQUIRE(kit.dim_minutes_ == nv::kDefaultDimMinutes);
}

TEST_CASE("the entry for Reset preferences runs the reset and is refused while the board cannot", "[prefs-system]") {
    FakeBackend kit(iclforge::variants::p4_rev3_kit());
    kit.dim_minutes_ = 30;
    const prefs::Pref& pref = *prefs::find("reset-preferences");
    REQUIRE(prefs::set_value(kit, pref, "run").ok);
    REQUIRE(kit.dim_minutes_ == nv::kDefaultDimMinutes);
    REQUIRE_FALSE(prefs::set_value(kit, pref, "dim-after").ok);

    kit.situation_.update_busy = true;
    REQUIRE(resolved(kit, "reset-preferences").inert == "A firmware update is under way.");
    // And the step before it names what changes on THIS board, from the registry.
    const sa::Confirm confirm = pref.confirm_step(kit, "run");
    REQUIRE(contains(confirm.lines[0], "screen without lyrics"));
    REQUIRE(contains(confirm.lines[0], "dim after"));
    REQUIRE(contains(confirm.lines[0], "brightness"));
    // The time zone is where the room is: kept, so never in what changes.
    REQUIRE_FALSE(contains(confirm.lines[0], "time zone"));
    REQUIRE(contains(confirm.lines[1], "name"));
    REQUIRE(contains(confirm.lines[1], "paired servers"));
}

TEST_CASE("Reset preferences puts back the display and time choices and keeps the time zone", "[prefs-system]") {
    FakeBackend kit(iclforge::variants::p4_rev3_kit());
    kit.brightness_ = 30;
    kit.clock_ = {.twelve_hour = true, .seconds = true};
    kit.night_ = {.enabled = true, .from_min = 23 * 60, .to_min = 6 * 60 + 30, .idle = true};
    kit.zone_ = {.posix = "AEST-10AEDT,M10.1.0,M4.1.0/3", .name = "Australia/Melbourne"};
    const iclforge::wallclock::ZoneChoice zone = kit.zone_;

    const prefs::ResetReport report = prefs::reset_preferences(kit);
    REQUIRE(report.ok());
    REQUIRE(kit.brightness_ == iclforge::nowview::kDefaultBrightness);
    REQUIRE(kit.clock_ == iclforge::wallclock::ClockFormat{});
    REQUIRE(kit.night_ == iclforge::nowview::NightSchedule{});
    // Where the room is is not a matter of taste.
    REQUIRE(kit.zone_ == zone);
    REQUIRE(prefs::find("time-zone")->on_reset == prefs::OnReset::kKeep);
}

TEST_CASE("what Reset preferences changes is worded from the registry, for the board it is on", "[prefs-system]") {
    const auto has = [](const std::vector<std::string>& labels, const char* label) {
        return std::find(labels.begin(), labels.end(), label) != labels.end();
    };
    const std::vector<std::string> kit = prefs::reset_labels(iclforge::variants::p4_rev3_kit());
    for (const char* label : {"screen without lyrics", "dim after", "brightness", "clock hours", "clock seconds",
                              "night schedule"}) {
        INFO(label);
        REQUIRE(has(kit, label));
    }
    // What is kept is never in the sentence: the time zone, the layout, the name, the network.
    for (const char* label : {"time zone", "speaker layout", "name of this player", "wired or Wi-Fi"}) {
        INFO(label);
        REQUIRE_FALSE(has(kit, label));
    }
    // A screen with no touch cannot dim and cannot be touched awake, so the sentence does not promise either.
    const std::vector<std::string> no_touch = prefs::reset_labels(iclforge::variants::c3_spi_320x240_no_touch());
    REQUIRE(has(no_touch, "screen without lyrics"));
    REQUIRE_FALSE(has(no_touch, "dim after"));
    REQUIRE_FALSE(has(no_touch, "night schedule"));
    // A board with a player and no screen has late night to put back, and nothing else.
    REQUIRE(prefs::reset_labels(iclforge::variants::s3_n16r8()) == std::vector<std::string>{"late night"});
    // One with neither a screen nor a player has nothing at all.
    iclforge::BoardCaps bare = iclforge::variants::s3_n16r8();
    bare.sendspin = false;
    REQUIRE(prefs::reset_labels(bare).empty());
    const sa::Confirm none = sa::reset_preferences_confirm(prefs::reset_labels(bare));
    REQUIRE(contains(none.lines[0], "nothing on this board"));
}

TEST_CASE("the listing carries the new kinds and the routes of the System entries", "[prefs-system]") {
    FakeBackend kit(iclforge::variants::p4_rev3_kit());
    const std::string json = prefs::to_json(kit);
    REQUIRE(contains(json, "\"id\":\"network-preference\",\"group\":\"Network\",\"label\":\"Wired or Wi-Fi\",\"kind\":\"choice\""));
    REQUIRE(contains(json, "\"id\":\"name\",\"group\":\"Network\",\"label\":\"Name of this player\",\"kind\":\"text\""));
    REQUIRE(contains(json, "\"value\":\"hearth-a1b2c3\""));
    REQUIRE(contains(json, "\"id\":\"about\",\"group\":\"System\",\"label\":\"About\",\"kind\":\"info\""));
    REQUIRE(contains(json, "\"kind\":\"list\",\"apply\":\"live\",\"route\":\"/pairing\""));
    REQUIRE(contains(json, "\"id\":\"factory-reset\",\"group\":\"System\",\"label\":\"Factory reset\",\"kind\":\"action\""));
    // Network before System, as the groups run.
    REQUIRE(json.find("\"id\":\"name\"") < json.find("\"id\":\"about\""));

    FakeBackend s3(iclforge::variants::s3_n16r8());
    const std::string plain = prefs::to_json(s3);
    REQUIRE(contains(plain, "\"id\":\"network-preference\""));
    REQUIRE(contains(plain, "\"reason\":\"This board has Wi-Fi only, so there is nothing to choose between.\""));
}
