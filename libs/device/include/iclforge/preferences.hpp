#pragma once

// The preferences of a board: everything a person chooses once and leaves (planning/p4x-preferences.md), as a
// registry that the screen, the REST listing (GET /preferences) and the web page all draw from. Adjust is for
// the in-the-moment tweaks of the lyrics screen (delay, text size) and is not in here; the technical settings
// (slot width, the second I2S line, the time server) stay with the developer view and the routes.
//
// ONE ENTRY IS ONE PREFERENCE. An entry says what it is called, which group it belongs to (Display, Sound,
// Network, Time, Updates, System), what kind of control it wants (a choice among a few words, a stepper along an
// ordered list, a toggle, a text, an action), whether a change applies at once or at the next play or after a
// restart, which existing REST route reads and writes the same value, and - the point of the exercise - whether
// THIS board can do it: availability(caps) -> {available, reason}, once for the entry and once for each of its
// choices. An entry or choice the board cannot support is not hidden: the screen shows it greyed, with the
// reason ("A touch lights the dark view, and this screen has no touch."), and the listing says the same.
//
// HOW A NEW PREFERENCE IS ADDED (the contract the other sessions build against):
//   1. an entry in kRegistry below, with its choices and availability rules (plain functions of BoardCaps);
//   2. its value in `Backend`: one getter and one setter, typed, beside the others. Backend is implemented ONCE
//      for the firmware (firmware/hearth-sink/main/preferences.cpp), over the settings_* functions
//      (main/settings.hpp) and whatever acts on a change, and once for the tests (a fake that holds the values).
//      A pure virtual means a firmware that forgot the setter does not compile;
//   3. only when the control is not one of the five kinds (a layout list, a time-zone picker, read-only rows, a
//      keyboard, a grid): the kind joins `Kind` (added at the END, never renumbered, with its name in kind_name()) and
//      an editor draws it on the screen (panel/lvgl/prefs_screen.cpp, build_row()). Never a bespoke button in
//      lyric_screen.cpp. The entry's value is still a word the Backend reads and sets: a list's summary, a pair of times
//      as text, whatever the editor needs to hand over.
// A field every entry must say (whether `Reset preferences` puts it back, how a change is confirmed) is added to Pref
// with a default that a host test fails on, so that no entry can leave it out by accident.
// The host tests walk every entry against every known board (board_variants.hpp): a default that is not
// available where its entry is, an unavailable choice with no reason, a duplicate id, a stepper that cannot be
// stepped, all fail there and not on a bench.
//
// Free of ESP-IDF, like board_caps.hpp: libs/device/tests/test_preferences.cpp runs it on every host. Values are
// text throughout: the words a PUT takes and a GET answers ("speakers", "5", "never", "on").

#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "iclforge/about_view.hpp"
#include "iclforge/board_caps.hpp"
#include "iclforge/dev_view.hpp"
#include "iclforge/device_name.hpp"
#include "iclforge/now_view.hpp"
#include "iclforge/system_actions.hpp"

namespace iclforge::prefs {

// --- what an entry is ---------------------------------------------------------------------------------

enum class Group : std::uint8_t { kDisplay, kSound, kNetwork, kTime, kUpdates, kSystem };

inline constexpr std::array<Group, 6> kGroups{Group::kDisplay, Group::kSound, Group::kNetwork,
                                              Group::kTime,    Group::kUpdates, Group::kSystem};

[[nodiscard]] constexpr const char* group_name(Group group) {
    switch (group) {
        case Group::kDisplay:
            return "Display";
        case Group::kSound:
            return "Sound";
        case Group::kNetwork:
            return "Network";
        case Group::kTime:
            return "Time";
        case Group::kUpdates:
            return "Updates";
        case Group::kSystem:
            break;
    }
    return "System";
}

// kChoice: a few words, all shown as buttons. kStepper: an ordered list of stops, shown as - value +. kToggle: two
// choices, "on" and "off". kText: a value typed in (read-only on a screen until an editor is written for it).
// kAction: does something when pressed (a check, a restart); its setter is handed "run". kInfo: rows to be read, nothing to
// set (About). kList: a list of things, each with an action of its own (the paired servers, each with Forget); the
// entry's setter is handed "forget " and the item's id.
enum class Kind : std::uint8_t { kChoice, kStepper, kToggle, kText, kAction, kInfo, kList };

[[nodiscard]] constexpr const char* kind_name(Kind kind) {
    switch (kind) {
        case Kind::kStepper:
            return "stepper";
        case Kind::kToggle:
            return "toggle";
        case Kind::kText:
            return "text";
        case Kind::kAction:
            return "action";
        case Kind::kInfo:
            return "info";
        case Kind::kList:
            return "list";
        case Kind::kChoice:
            break;
    }
    return "choice";
}

// When a change takes effect.
enum class Apply : std::uint8_t { kLive, kNextPlay, kRestart };

[[nodiscard]] constexpr const char* apply_name(Apply apply) {
    switch (apply) {
        case Apply::kNextPlay:
            return "next-play";
        case Apply::kRestart:
            return "restart";
        case Apply::kLive:
            break;
    }
    return "live";
}

// Whether this board can do something, and if not, why - in words for the person looking at the greyed control.
// A reason is a sentence about THIS board ("this screen has no touch"), never about the code.
struct Availability {
    bool available = true;
    std::string reason;

    [[nodiscard]] static Availability yes() { return {}; }
    [[nodiscard]] static Availability no(std::string why) { return {false, std::move(why)}; }
};

using AvailabilityFn = Availability (*)(const BoardCaps&);

// --- the board's side ---------------------------------------------------------------------------------

// How the registry reaches a board's settings and what acts on them. The firmware implements it once
// (firmware/hearth-sink/main/preferences.cpp: the settings_* functions, plus the panel or the clock where a change
// has to be followed at once); the tests implement it with a fake. A setter returns false when the board refuses
// the value (it could not be stored, or this build cannot do it) and then changes nothing.
class Backend {
   public:
    virtual ~Backend() = default;

    // The board as it is now (board_caps.hpp): hardware filled at boot, configuration from the settings.
    [[nodiscard]] virtual BoardCaps caps() const = 0;

    // Display: the screen for a song with no lyrics, and the minutes of nothing touched after which it dims.
    [[nodiscard]] virtual nowview::View now_view() const = 0;
    [[nodiscard]] virtual bool set_now_view(nowview::View view) = 0;
    [[nodiscard]] virtual int dim_after_minutes() const = 0;
    [[nodiscard]] virtual bool set_dim_after_minutes(int minutes) = 0;

    // Network: which of its networks a board with a cable and Wi-Fi uses, as the word PUT /network-preference takes
    // ("auto", "ethernet", "wifi"). The setter returns at once and the change is made on a task of its own, as
    // network_set_preference() does; the board keeps the preference only once the interface it names holds an address.
    [[nodiscard]] virtual std::string network_preference() const = 0;
    [[nodiscard]] virtual bool set_network_preference(std::string_view word) = 0;

    // The name of this player (iclforge/device_name.hpp has the rule; the registry holds a name to it before it asks).
    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual bool set_name(std::string_view name) = 0;

    // System. The board as About and the network preference's confirming step read it: the network, the
    // configuration and the number of paired servers (panel::gather_about_facts in the firmware). May take tens of
    // milliseconds on a board whose radio is a companion chip, so it is asked when a sheet opens, not on every draw.
    [[nodiscard]] virtual devview::Facts facts() const = 0;
    // What the restart and the resets need to know (iclforge/system_actions.hpp): streaming, a firmware on trial or
    // being written, which networks the board has and whether the cable is in, how many servers are paired.
    [[nodiscard]] virtual sysactions::Situation situation() const = 0;
    // Restart, and the factory reset. Nothing once the work is done or under way (the restart itself follows a
    // moment later); otherwise why it was not done, in words for the person who asked.
    [[nodiscard]] virtual std::optional<std::string> restart() = 0;
    [[nodiscard]] virtual std::optional<std::string> factory_reset() = 0;
    // The Sendspin servers this board is paired with, and forgetting one of them by its server_id (a whole one: no
    // other id reaches the board). False when there is no such pairing.
    [[nodiscard]] virtual std::vector<sysactions::paired::Server> paired_servers() const = 0;
    [[nodiscard]] virtual bool forget_server(std::string_view server_id) = 0;
};

using GetFn = std::string (*)(const Backend&);
using SetFn = bool (*)(Backend&, std::string_view word);
// The confirming step to show before a change of this entry is made, handed the word it is about to be set to ("run"
// for an action): what is lost, what follows. Null for an entry that asks nothing.
using ConfirmFn = sysactions::Confirm (*)(const Backend&, std::string_view word);

// Whether Reset preferences sets an entry back to its default_word. EVERY entry says (kUnset fails the tests): the
// choices a person made once are reset; what the board IS - its name, its network, its wiring, its layout - and the
// entries that are not values at all (actions, lists, info) are kept. The reset is derived from this, so a preference
// added to the registry is reset or kept because its author said so, not because a second list was forgotten.
enum class OnReset : std::uint8_t { kUnset, kReset, kKeep };
// A reason the control does nothing RIGHT NOW because of another setting (not because of the hardware): non-empty
// text is shown instead of the control's effect, with the control greyed. Empty when the control applies.
using InertFn = std::string (*)(const Backend&);

struct ChoiceDef {
    const char* id = "";
    const char* label = "";
    // Null: available wherever the entry is.
    AvailabilityFn availability = nullptr;
};

struct Pref {
    // Lower-case words and hyphens, stable: it is the id the listing and a future PUT /preferences use.
    const char* id = "";
    Group group = Group::kSystem;
    const char* label = "";
    // One line under the label, in the muted colour; empty for none.
    const char* note = "";
    Kind kind = Kind::kChoice;
    Apply apply = Apply::kLive;
    // The REST route that reads and writes the same value ("/now-playing"); empty when there is none.
    const char* route = "";
    // The word a board starts with (the Kconfig default); it must be one of the choices, and available wherever the
    // entry is.
    const char* default_word = "";
    // A stepper's unit, said after a number that is not one of its stops ("7 min").
    const char* unit = "";
    // kChoice and kStepper: the choices, in the order they are shown (a stepper's, in the order it steps). kToggle:
    // "on" and "off". Empty for the kinds with none (a text, an action): GCC's -Wmissing-field-initializers, which the
    // CI flags turn into an error, wants a default for every member an entry may leave out.
    std::span<const ChoiceDef> choices = {};
    // The whole entry; null is always available.
    AvailabilityFn availability = nullptr;
    InertFn inert = nullptr;
    GetFn get = nullptr;
    SetFn set = nullptr;
    // Reset preferences (see OnReset).
    OnReset on_reset = OnReset::kUnset;
    // The step before a change, and, for a change that cannot be undone, the held button after it (the factory reset).
    ConfirmFn confirm = nullptr;
    ConfirmFn then_hold = nullptr;
    // What the button that runs or opens the entry says ("Restart", "View"): an action, an info entry and a list have
    // one. Empty is "Run".
    const char* button = "";
};

// --- the seed: what exists today ----------------------------------------------------------------------

namespace detail {

[[nodiscard]] inline Availability needs_screen(const BoardCaps& caps) {
    return caps.has_screen() ? Availability::yes() : Availability::no("This board has no screen.");
}

// The dim time wakes on a touch, so a screen with none would never wake.
[[nodiscard]] inline Availability needs_screen_with_touch(const BoardCaps& caps) {
    if (!caps.has_screen()) {
        return Availability::no("This board has no screen.");
    }
    return caps.display.touch ? Availability::yes()
                              : Availability::no("A touch wakes a dimmed screen, and this screen has no touch.");
}

[[nodiscard]] inline Availability needs_touch(const BoardCaps& caps) {
    return caps.display.touch ? Availability::yes()
                              : Availability::no("A touch lights the dark view, and this screen has no touch.");
}

[[nodiscard]] inline Availability needs_clock(const BoardCaps& caps) {
    return caps.wall_clock ? Availability::yes()
                           : Availability::no("This board keeps no time: it has no network to ask a time server on.");
}

inline constexpr std::array<ChoiceDef, 5> kNowViewChoices{{
    {.id = "speakers", .label = "Speakers"},
    {.id = "title", .label = "Title"},
    {.id = "clock", .label = "Clock", .availability = &needs_clock},
    {.id = "dark", .label = "Dark", .availability = &needs_touch},
    {.id = "off", .label = "Off"},
}};

// A stepper walks its stops in order: from a little to a lot, and "never" at the end, where the effect has gone.
inline constexpr std::array<ChoiceDef, 8> kDimAfterChoices{{
    {.id = "1", .label = "1 min"},
    {.id = "5", .label = "5 min"},
    {.id = "15", .label = "15 min"},
    {.id = "30", .label = "30 min"},
    {.id = "60", .label = "1 hour"},
    {.id = "120", .label = "2 hours"},
    {.id = "240", .label = "4 hours"},
    {.id = "never", .label = "Never"},
}};

[[nodiscard]] inline std::string get_now_view(const Backend& backend) {
    return nowview::view_name(backend.now_view());
}

[[nodiscard]] inline bool set_now_view(Backend& backend, std::string_view word) {
    const std::optional<nowview::View> view = nowview::parse_view(word);
    return view.has_value() && backend.set_now_view(*view);
}

[[nodiscard]] inline std::string get_dim_after(const Backend& backend) {
    return nowview::dim_after_text(backend.dim_after_minutes());
}

[[nodiscard]] inline bool set_dim_after(Backend& backend, std::string_view word) {
    const std::optional<int> minutes = nowview::parse_dim_after(word);
    return minutes.has_value() && backend.set_dim_after_minutes(*minutes);
}

// The Dark view is dark from the moment a song with no lyrics begins: it has no time to dim after.
[[nodiscard]] inline std::string dim_after_inert(const Backend& backend) {
    if (backend.now_view() != nowview::View::kDark) {
        return {};
    }
    return "Not used by the Dark view: a touch lights it for " + std::to_string(nowview::kDarkAwakeMs / 1000) +
           " seconds, then it is dark again.";
}

}  // namespace detail

// What Reset preferences does (below, once the registry is): which entries it set back, and what it calls them.
struct ResetReport {
    std::vector<std::string> reset;   // ids set back to their default
    std::vector<std::string> failed;  // ids the board refused
    [[nodiscard]] bool ok() const { return failed.empty(); }
};
inline ResetReport reset_preferences(Backend& backend);
[[nodiscard]] inline std::vector<std::string> reset_labels(const BoardCaps& caps);

namespace detail {

// --- Network and System (E4, planning/p4x-preferences.md) ------------------------------------------------------------

[[nodiscard]] inline Availability needs_network_choice(const BoardCaps& caps) {
    if (caps.network.choice()) {
        return Availability::yes();
    }
    if (caps.network.wifi) {
        return Availability::no("This board has Wi-Fi only, so there is nothing to choose between.");
    }
    if (caps.network.ethernet) {
        return Availability::no("This board has a cable only, so there is nothing to choose between.");
    }
    return Availability::no("This board has no network.");
}

[[nodiscard]] inline Availability needs_sendspin(const BoardCaps& caps) {
    return caps.sendspin ? Availability::yes()
                         : Availability::no("This build has no Sendspin player, so no server can pair with this board.");
}

inline constexpr std::array<ChoiceDef, 3> kNetworkChoices{{
    {.id = "auto", .label = "Automatic"},
    {.id = "ethernet", .label = "Cable"},
    {.id = "wifi", .label = "Wi-Fi"},
}};

[[nodiscard]] inline std::string get_network_preference(const Backend& backend) { return backend.network_preference(); }

[[nodiscard]] inline bool set_network_preference(Backend& backend, std::string_view word) {
    return devview::parse_preference(word).has_value() && backend.set_network_preference(word);
}

// The same words the developer view says before it changes the preference (devview::confirm_text), so the two
// controls never disagree about what a change does.
[[nodiscard]] inline sysactions::Confirm network_preference_confirm(const Backend& backend, std::string_view word) {
    sysactions::Confirm confirm;
    const std::optional<devview::Preference> target = devview::parse_preference(word);
    if (!target) {
        return confirm;
    }
    const devview::Confirm said = devview::confirm_text(backend.facts(), *target);
    confirm.title = said.headline;
    confirm.lines.push_back(said.detail);
    confirm.accept = "Switch";
    confirm.tone = said.tone;
    confirm.blocked = said.blocked;
    return confirm;
}

[[nodiscard]] inline std::string get_name(const Backend& backend) { return backend.name(); }

[[nodiscard]] inline bool set_name(Backend& backend, std::string_view word) {
    return devname::check_name(word).ok() && backend.set_name(word);
}

// About has rows to read and nothing to set.
[[nodiscard]] inline std::string get_nothing(const Backend&) { return {}; }
[[nodiscard]] inline bool set_nothing(Backend&, std::string_view) { return false; }

// "forget " and a server_id from the list: that server alone. Nothing else is a request; in particular a bare
// "forget", which would be every pairing and a new identity.
[[nodiscard]] inline bool set_paired_servers(Backend& backend, std::string_view word) {
    constexpr std::string_view kForget = "forget ";
    if (!word.starts_with(kForget)) {
        return false;
    }
    const std::string_view id = word.substr(kForget.size());
    return sysactions::paired::forget_body(id).has_value() && backend.forget_server(id);
}

// A reason the action cannot be done right now (a firmware still on trial, an update under way), the control
// greyed with it.
[[nodiscard]] inline std::string restart_inert(const Backend& backend) {
    const sysactions::Availability now = sysactions::availability(sysactions::Action::restart, backend.situation());
    return now.available ? std::string() : now.reason;
}
[[nodiscard]] inline std::string factory_reset_inert(const Backend& backend) {
    const sysactions::Availability now = sysactions::availability(sysactions::Action::factory_reset, backend.situation());
    return now.available ? std::string() : now.reason;
}
[[nodiscard]] inline std::string reset_preferences_inert(const Backend& backend) {
    const sysactions::Availability now =
        sysactions::availability(sysactions::Action::reset_preferences, backend.situation());
    return now.available ? std::string() : now.reason;
}

[[nodiscard]] inline bool set_restart(Backend& backend, std::string_view word) {
    return word == "run" && !backend.restart().has_value();
}
[[nodiscard]] inline bool set_factory_reset(Backend& backend, std::string_view word) {
    return word == "run" && !backend.factory_reset().has_value();
}
[[nodiscard]] inline bool set_reset_preferences(Backend& backend, std::string_view word) {
    return word == "run" && reset_preferences(backend).ok();
}

[[nodiscard]] inline sysactions::Confirm restart_confirm(const Backend& backend, std::string_view) {
    return sysactions::restart_confirm(backend.situation());
}
[[nodiscard]] inline sysactions::Confirm reset_preferences_confirm(const Backend& backend, std::string_view) {
    return sysactions::reset_preferences_confirm(reset_labels(backend.caps()));
}
[[nodiscard]] inline sysactions::Confirm factory_reset_confirm(const Backend& backend, std::string_view) {
    return sysactions::factory_reset_confirm(backend.situation());
}
[[nodiscard]] inline sysactions::Confirm factory_reset_hold(const Backend&, std::string_view) {
    return sysactions::hold_step(sysactions::Action::factory_reset);
}

}  // namespace detail

inline constexpr std::array<Pref, 9> kRegistry{{
    {.id = "now-view",
     .group = Group::kDisplay,
     .label = "Screen without lyrics",
     .note = "What the screen shows while a song has no lyrics.",
     .kind = Kind::kChoice,
     .apply = Apply::kLive,
     .route = "/now-playing",
     .default_word = "speakers",
     .choices = detail::kNowViewChoices,
     .availability = &detail::needs_screen,
     .get = &detail::get_now_view,
     .set = &detail::set_now_view,
     .on_reset = OnReset::kReset},
    {.id = "dim-after",
     .group = Group::kDisplay,
     .label = "Dim after",
     .note = "How long the screen is left alone before it dims. A touch wakes it.",
     .kind = Kind::kStepper,
     .apply = Apply::kLive,
     .route = "/dim-after",
     .default_word = "5",
     .unit = "min",
     .choices = detail::kDimAfterChoices,
     .availability = &detail::needs_screen_with_touch,
     .inert = &detail::dim_after_inert,
     .get = &detail::get_dim_after,
     .set = &detail::set_dim_after,
     .on_reset = OnReset::kReset},
    // --- Network
    // A board with a cable and Wi-Fi uses one of them, or whichever is linked. The control is the developer view's,
    // with its confirming step. Kept by Reset preferences: a reset must not change how the board is joined.
    {.id = "network-preference",
     .group = Group::kNetwork,
     .label = "Wired or Wi-Fi",
     .note = "Which network the player uses when it has both: the cable, Wi-Fi, or whichever is linked.",
     .kind = Kind::kChoice,
     .apply = Apply::kLive,
     .route = "/network-preference",
     .default_word = "auto",
     .choices = detail::kNetworkChoices,
     .availability = &detail::needs_network_choice,
     .get = &detail::get_network_preference,
     .set = &detail::set_network_preference,
     .on_reset = OnReset::kKeep,
     .confirm = &detail::network_preference_confirm},
    // What servers and the network call this player. Typed on the screen's keyboard. Kept by Reset preferences.
    {.id = "name",
     .group = Group::kNetwork,
     .label = "Name of this player",
     .note = "What servers and the network call this player.",
     .kind = Kind::kText,
     .apply = Apply::kLive,
     .route = "/name",
     .get = &detail::get_name,
     .set = &detail::set_name,
     .on_reset = OnReset::kKeep},
    // --- System
    {.id = "about",
     .group = Group::kSystem,
     .label = "About",
     .note = "The version, the name, the addresses and the chip.",
     .kind = Kind::kInfo,
     .apply = Apply::kLive,
     .route = "/hardware",
     .get = &detail::get_nothing,
     .set = &detail::set_nothing,
     .on_reset = OnReset::kKeep,
     .button = "View"},
    {.id = "paired-servers",
     .group = Group::kSystem,
     .label = "Paired servers",
     .note = "The servers that can play to this player. Forget one and it has to pair again.",
     .kind = Kind::kList,
     .apply = Apply::kLive,
     .route = "/pairing",
     .availability = &detail::needs_sendspin,
     .get = &detail::get_nothing,
     .set = &detail::set_paired_servers,
     .on_reset = OnReset::kKeep,
     .button = "Manage"},
    {.id = "restart",
     .group = Group::kSystem,
     .label = "Restart",
     .note = "The player restarts and is back in about half a minute.",
     .kind = Kind::kAction,
     .apply = Apply::kLive,
     .route = "/restart",
     .inert = &detail::restart_inert,
     .get = &detail::get_nothing,
     .set = &detail::set_restart,
     .on_reset = OnReset::kKeep,
     .confirm = &detail::restart_confirm,
     .button = "Restart"},
    {.id = "reset-preferences",
     .group = Group::kSystem,
     .label = "Reset preferences",
     .note = "The choices on this screen back to the defaults. The name, the network and the pairings stay.",
     .kind = Kind::kAction,
     .apply = Apply::kLive,
     .route = "/reset-preferences",
     .inert = &detail::reset_preferences_inert,
     .get = &detail::get_nothing,
     .set = &detail::set_reset_preferences,
     .on_reset = OnReset::kKeep,
     .confirm = &detail::reset_preferences_confirm,
     .button = "Reset"},
    {.id = "factory-reset",
     .group = Group::kSystem,
     .label = "Factory reset",
     .note = "Erases the name, the network, the pairings and every setting, and restarts.",
     .kind = Kind::kAction,
     .apply = Apply::kLive,
     .route = "/factory-reset",
     .inert = &detail::factory_reset_inert,
     .get = &detail::get_nothing,
     .set = &detail::set_factory_reset,
     .on_reset = OnReset::kKeep,
     .confirm = &detail::factory_reset_confirm,
     .then_hold = &detail::factory_reset_hold,
     .button = "Erase"},
}};

[[nodiscard]] inline std::span<const Pref> registry() { return kRegistry; }

[[nodiscard]] inline const Pref* find(std::string_view id) {
    for (const Pref& pref : kRegistry) {
        if (id == pref.id) {
            return &pref;
        }
    }
    return nullptr;
}

// --- what a board makes of an entry -------------------------------------------------------------------

struct ChoiceState {
    std::string id;
    std::string label;
    // The entry's availability folded in: a choice of an unavailable entry is unavailable, with the entry's reason.
    Availability availability;
};

struct Entry {
    const Pref* pref = nullptr;
    Availability availability;
    // The current value as its word, and as the label to show: the choice's, or "7 min" for a number a stepper has no
    // stop for. Empty when the board cannot read it.
    std::string value;
    std::string value_label;
    // See Pref::inert; empty when the control applies.
    std::string inert;
    std::vector<ChoiceState> choices;
};

[[nodiscard]] inline Availability availability_of(const Pref& pref, const BoardCaps& caps) {
    return pref.availability != nullptr ? pref.availability(caps) : Availability::yes();
}

[[nodiscard]] inline Availability availability_of(const Pref& pref, const ChoiceDef& choice, const BoardCaps& caps) {
    Availability whole = availability_of(pref, caps);
    if (!whole.available) {
        return whole;
    }
    return choice.availability != nullptr ? choice.availability(caps) : Availability::yes();
}

namespace detail {

[[nodiscard]] inline bool all_digits(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

// A stop's place on the number line: its number, and a word (like "never") past every number.
[[nodiscard]] inline long stop_rank(std::string_view id) {
    long value = 0;
    if (!all_digits(id) || id.size() > 9) {
        return 1L << 30;
    }
    for (const char c : id) {
        value = value * 10 + (c - '0');
    }
    return value;
}

}  // namespace detail

// One entry as this board sees it, its value read from the board.
[[nodiscard]] inline Entry resolve(const Pref& pref, const Backend& backend, const BoardCaps& caps) {
    Entry entry;
    entry.pref = &pref;
    entry.availability = availability_of(pref, caps);
    if (pref.get != nullptr) {
        entry.value = pref.get(backend);
    }
    if (pref.inert != nullptr) {
        entry.inert = pref.inert(backend);
    }
    for (const ChoiceDef& choice : pref.choices) {
        entry.choices.push_back({choice.id, choice.label, availability_of(pref, choice, caps)});
        if (entry.value == choice.id) {
            entry.value_label = choice.label;
        }
    }
    if (entry.value_label.empty() && !entry.value.empty()) {
        entry.value_label = entry.value;
        if (pref.unit[0] != '\0' && detail::all_digits(entry.value)) {
            entry.value_label += std::string(" ") + pref.unit;
        }
    }
    return entry;
}

// Every entry, grouped in the order of kGroups and in registry order within a group.
[[nodiscard]] inline std::vector<Entry> resolve(const Backend& backend) {
    const BoardCaps caps = backend.caps();
    std::vector<Entry> entries;
    for (const Group group : kGroups) {
        for (const Pref& pref : kRegistry) {
            if (pref.group == group) {
                entries.push_back(resolve(pref, backend, caps));
            }
        }
    }
    return entries;
}

// The stop a stepper moves to from `current` in a direction (+1 more, -1 less), skipping stops the board cannot
// take; nothing when it is already at the end. A value that is not a stop (a number a PUT stored) is placed on the
// number line first: a word such as "never" is past every number.
[[nodiscard]] inline std::optional<std::string> step_target(const Entry& entry, int direction) {
    if (entry.choices.empty() || direction == 0) {
        return std::nullopt;
    }
    const long here = detail::stop_rank(entry.value);
    // Along the stops in order, the first available one past `here` (or the last available one before it).
    std::optional<std::string> found;
    for (const ChoiceState& stop : entry.choices) {
        if (!stop.availability.available) {
            continue;
        }
        const long rank = detail::stop_rank(stop.id);
        if (direction > 0 && rank > here) {
            return stop.id;
        }
        if (direction < 0 && rank < here) {
            found = stop.id;
        }
    }
    return found;
}

struct SetResult {
    bool ok = false;
    // Why it did not happen, for the person who asked; empty on success.
    std::string message;
};

// Sets an entry to a word, refusing what this board cannot do before it asks the board: the whole entry, then the
// choice (when the entry lists choices; a stepper also takes a number between its stops). The board's own refusal is
// reported as such.
[[nodiscard]] inline SetResult set_value(Backend& backend, const Pref& pref, std::string_view word) {
    const BoardCaps caps = backend.caps();
    const Availability whole = availability_of(pref, caps);
    if (!whole.available) {
        return {false, whole.reason};
    }
    if (pref.kind != Kind::kText && pref.kind != Kind::kAction && !pref.choices.empty()) {
        const ChoiceDef* chosen = nullptr;
        for (const ChoiceDef& choice : pref.choices) {
            if (word == choice.id) {
                chosen = &choice;
            }
        }
        if (chosen == nullptr && !(pref.kind == Kind::kStepper && detail::all_digits(word))) {
            return {false, "That is not one of this setting's choices."};
        }
        if (chosen != nullptr) {
            const Availability one = availability_of(pref, *chosen, caps);
            if (!one.available) {
                return {false, one.reason};
            }
        }
    }
    if (pref.set == nullptr || !pref.set(backend, word)) {
        return {false, "The board could not take that."};
    }
    return {true, {}};
}

// --- Reset preferences ---------------------------------------------------------------------------------

// Every entry that says kReset, set back to its default_word through its own setter, so that whatever follows a
// change at once (the panel, the clock) follows this one. An entry the board cannot make (greyed) or already at its
// default is left alone; one the board refuses is reported and the rest are still done. The names, the networks and
// the pairings are not here: they are kKeep.
inline ResetReport reset_preferences(Backend& backend) {
    const BoardCaps caps = backend.caps();
    ResetReport report;
    for (const Pref& pref : kRegistry) {
        if (pref.on_reset != OnReset::kReset || pref.default_word[0] == '\0' || pref.get == nullptr ||
            pref.set == nullptr || !availability_of(pref, caps).available) {
            continue;
        }
        if (pref.get(backend) == pref.default_word) {
            continue;
        }
        (pref.set(backend, pref.default_word) ? report.reset : report.failed).emplace_back(pref.id);
    }
    return report;
}

// What Reset preferences would change on this board, as words in a sentence ("screen without lyrics", "dim after"):
// the labels of the entries it sets back, the greyed ones left out.
inline std::vector<std::string> reset_labels(const BoardCaps& caps) {
    std::vector<std::string> labels;
    for (const Pref& pref : kRegistry) {
        if (pref.on_reset != OnReset::kReset || !availability_of(pref, caps).available) {
            continue;
        }
        std::string label = pref.label;
        // The first letter in lower case, in the middle of a sentence, but not in "Wi-Fi".
        if (label.size() > 1 && std::islower(static_cast<unsigned char>(label[1])) != 0) {
            label[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(label[0])));
        }
        labels.push_back(std::move(label));
    }
    return labels;
}

// --- the listing --------------------------------------------------------------------------------------

namespace detail {

inline void json_text(std::string& out, std::string_view text) {
    out += '"';
    for (const char c : text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += ' ';
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

// An object being written into `out`: a comma before every member but the first. (text() and flag() are not overloads
// of one name: a `const char*` converts to bool before it converts to a string_view.)
struct JsonObject {
    std::string& out;
    bool first = true;

    explicit JsonObject(std::string& to) : out(to) { out += '{'; }
    void key(const char* name) {
        if (!first) {
            out += ',';
        }
        first = false;
        json_text(out, name);
        out += ':';
    }
    void text(const char* name, std::string_view value) {
        key(name);
        json_text(out, value);
    }
    void flag(const char* name, bool value) {
        key(name);
        out += value ? "true" : "false";
    }
    void close() { out += '}'; }
};

}  // namespace detail

// GET /preferences: every entry as this board sees it - id, group, label, kind, how a change applies, the route that
// sets it, the value, whether it is available and why not, a note when another setting makes it inert, and, for the
// kinds with choices, each choice with the same. Read-only: a change goes through the entry's own route.
[[nodiscard]] inline std::string to_json(const Backend& backend) {
    // Written straight from the registry and the board, with no Entry built: this is in every build, a board with no
    // screen as well, and the flash it takes is the C6's headroom.
    const BoardCaps caps = backend.caps();
    std::string out;
    detail::JsonObject top(out);
    top.text("profile", caps.profile);
    top.text("display", display_kind_name(caps.display.kind));
    top.key("preferences");
    out += '[';
    bool first = true;
    for (const Group group : kGroups) {
        for (const Pref& pref : kRegistry) {
            if (pref.group != group) {
                continue;
            }
            if (!first) {
                out += ',';
            }
            first = false;
            const Availability whole = availability_of(pref, caps);
            detail::JsonObject entry(out);
            entry.text("id", pref.id);
            entry.text("group", group_name(pref.group));
            entry.text("label", pref.label);
            entry.text("kind", kind_name(pref.kind));
            entry.text("apply", apply_name(pref.apply));
            entry.text("route", pref.route);
            entry.text("value", pref.get != nullptr ? pref.get(backend) : std::string());
            entry.flag("available", whole.available);
            entry.text("reason", whole.reason);
            if (pref.inert != nullptr) {
                if (const std::string note = pref.inert(backend); !note.empty()) {
                    entry.text("note", note);
                }
            }
            if (!pref.choices.empty()) {
                entry.key("choices");
                out += '[';
                for (std::size_t i = 0; i < pref.choices.size(); ++i) {
                    const ChoiceDef& choice = pref.choices[i];
                    if (i != 0) {
                        out += ',';
                    }
                    // An unavailable entry greys every choice with its own reason.
                    const Availability one = !whole.available || choice.availability == nullptr
                                                 ? whole
                                                 : choice.availability(caps);
                    detail::JsonObject item(out);
                    item.text("id", choice.id);
                    item.text("label", choice.label);
                    item.flag("available", one.available);
                    item.text("reason", one.reason);
                    item.close();
                }
                out += ']';
            }
            entry.close();
        }
    }
    out += ']';
    top.close();
    out += '\n';
    return out;
}

}  // namespace iclforge::prefs
