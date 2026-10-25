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
//   3. only when the control is not one of the five kinds (a layout list, a time-zone picker): an editor on the
//      screen (panel/lvgl/prefs_screen.cpp). Never a bespoke button in lyric_screen.cpp.
// The host tests walk every entry against every known board (board_variants.hpp): a default that is not
// available where its entry is, an unavailable choice with no reason, a duplicate id, a stepper that cannot be
// stepped, all fail there and not on a bench.
//
// Free of ESP-IDF, like board_caps.hpp: libs/device/tests/test_preferences.cpp runs it on every host. Values are
// text throughout: the words a PUT takes and a GET answers ("speakers", "5", "never", "on").

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "iclforge/board_caps.hpp"
#include "iclforge/now_view.hpp"

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
// kAction: does something when pressed (a check, a restart); its setter is handed "run".
enum class Kind : std::uint8_t { kChoice, kStepper, kToggle, kText, kAction };

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
};

using GetFn = std::string (*)(const Backend&);
using SetFn = bool (*)(Backend&, std::string_view word);
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
    // "on" and "off".
    std::span<const ChoiceDef> choices;
    // The whole entry; null is always available.
    AvailabilityFn availability = nullptr;
    InertFn inert = nullptr;
    GetFn get = nullptr;
    SetFn set = nullptr;
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

inline constexpr std::array<Pref, 2> kRegistry{{
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
     .set = &detail::set_now_view},
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
     .set = &detail::set_dim_after},
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

inline void json_member(std::string& out, const char* key, std::string_view text) {
    json_text(out, key);
    out += ':';
    json_text(out, text);
}

// Not an overload of json_member: a `const char*` converts to bool before it converts to a string_view.
inline void json_flag(std::string& out, const char* key, bool value) {
    json_text(out, key);
    out += value ? ":true" : ":false";
}

}  // namespace detail

// GET /preferences: every entry as this board sees it - id, group, label, kind, how a change applies, the route that
// sets it, the value, whether it is available and why not, a note when another setting makes it inert, and, for the
// kinds with choices, each choice with the same. Read-only: a change goes through the entry's own route.
[[nodiscard]] inline std::string to_json(const Backend& backend) {
    const BoardCaps caps = backend.caps();
    std::string out = "{";
    detail::json_member(out, "profile", caps.profile);
    out += ',';
    detail::json_member(out, "display", display_kind_name(caps.display.kind));
    out += ",\"preferences\":[";
    bool first = true;
    for (const Group group : kGroups) {
        for (const Pref& pref : kRegistry) {
            if (pref.group != group) {
                continue;
            }
            const Entry entry = resolve(pref, backend, caps);
            out += first ? "{" : ",{";
            first = false;
            detail::json_member(out, "id", pref.id);
            out += ',';
            detail::json_member(out, "group", group_name(pref.group));
            out += ',';
            detail::json_member(out, "label", pref.label);
            out += ',';
            detail::json_member(out, "kind", kind_name(pref.kind));
            out += ',';
            detail::json_member(out, "apply", apply_name(pref.apply));
            out += ',';
            detail::json_member(out, "route", pref.route);
            out += ',';
            detail::json_member(out, "value", entry.value);
            out += ',';
            detail::json_flag(out, "available", entry.availability.available);
            out += ',';
            detail::json_member(out, "reason", entry.availability.reason);
            if (!entry.inert.empty()) {
                out += ',';
                detail::json_member(out, "note", entry.inert);
            }
            if (!entry.choices.empty()) {
                out += ",\"choices\":[";
                for (std::size_t i = 0; i < entry.choices.size(); ++i) {
                    const ChoiceState& choice = entry.choices[i];
                    out += i == 0 ? "{" : ",{";
                    detail::json_member(out, "id", choice.id);
                    out += ',';
                    detail::json_member(out, "label", choice.label);
                    out += ',';
                    detail::json_flag(out, "available", choice.availability.available);
                    out += ',';
                    detail::json_member(out, "reason", choice.availability.reason);
                    out += '}';
                }
                out += ']';
            }
            out += '}';
        }
    }
    out += "]}\n";
    return out;
}

}  // namespace iclforge::prefs
