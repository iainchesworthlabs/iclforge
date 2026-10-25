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
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "iclforge/board_caps.hpp"
#include "iclforge/layout_options.hpp"
#include "iclforge/now_view.hpp"
#include "iclforge/time_zones.hpp"
#include "iclforge/wall_clock.hpp"

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

// How a screen draws an entry's value. kButtons: a kChoice's choices side by side, wrapping, with the reasons of the
// greyed ones in lines under the row. kList: one choice a line, its note or reason beside it, for a list too long to
// read as buttons (the speaker layouts, whose limits each need a sentence). kTimes and kZone are for a kText entry
// whose value is chosen in an editor of its own over the sheet and not typed (a pair of times, a place in a list):
// panel/lvgl/prefs_editors.cpp draws them. A page or the REST routes need none.
enum class Editor : std::uint8_t { kButtons, kList, kTimes, kZone };

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
    // How the speakers view shows a level: a glow, or a glow that also makes each speaker's circle grow and shrink a little.
    [[nodiscard]] virtual nowview::SpeakersStyle speakers_style() const = 0;
    [[nodiscard]] virtual bool set_speakers_style(nowview::SpeakersStyle style) = 0;

    // Display: the backlight's lit level in percent (nowview::kBrightnessMin to kBrightnessMax), how the clock view
    // writes the time, and the night schedule (when the screen goes dark by the board's own time).
    [[nodiscard]] virtual int brightness() const = 0;
    [[nodiscard]] virtual bool set_brightness(int percent) = 0;
    [[nodiscard]] virtual wallclock::ClockFormat clock_format() const = 0;
    [[nodiscard]] virtual bool set_clock_format(wallclock::ClockFormat format) = 0;
    [[nodiscard]] virtual nowview::NightSchedule night() const = 0;
    [[nodiscard]] virtual bool set_night(const nowview::NightSchedule& night) = 0;

    // Time: the zone as the clock reads it (a POSIX string, empty for none) and the name a person chose it by
    // (wallclock::ZoneChoice; the name is empty for a string they typed).
    [[nodiscard]] virtual wallclock::ZoneChoice time_zone() const = 0;
    [[nodiscard]] virtual bool set_time_zone(const wallclock::ZoneChoice& zone) = 0;

    // Sound: the speakers this player drives, as the name or the list PUT /layout takes. The setter is that route's
    // own decision (layout_options.hpp, the same verdict set_value() has already asked), and false when the layout
    // could not be queued or stored.
    [[nodiscard]] virtual std::string layout() const = 0;
    [[nodiscard]] virtual bool set_layout(std::string_view layout) = 0;

    // Whether a stream is playing now: what an entry with a `confirm` asks about before it breaks the playback.
    [[nodiscard]] virtual bool playing() const = 0;
};

using GetFn = std::string (*)(const Backend&);
using SetFn = bool (*)(Backend&, std::string_view word);
// A reason the control does nothing RIGHT NOW because of another setting (not because of the hardware): non-empty
// text is shown instead of the control's effect, with the control greyed. Empty when the control applies.
using InertFn = std::string (*)(const Backend&);
// How a kText entry's value is shown, where the word a route takes is not what a person reads ("Australia/Melbourne"
// as "Melbourne (Australia)"). Null: the value as it is.
using LabelFn = std::string (*)(std::string_view value);

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
    // How the screen draws the choices of a kChoice (kButtons, kList), or the editor of its own that a kText's value
    // is chosen in over the sheet (kTimes, kZone: prefs_editors.cpp). See Editor.
    Editor editor = Editor::kButtons;
    // How a kText entry with an editor shows its value, where the word a route takes is not what a person reads.
    LabelFn label_of = nullptr;
    // What a change costs while a stream plays ("Changing the speakers makes the music drop out for a moment."); the
    // screen asks "<this> Change to <choice>?" before it applies a change then, and never when nothing plays. Empty:
    // a change costs nothing and is applied at a tap.
    const char* confirm = "";
};

// Whether the entry's value is chosen in an editor of its own over the sheet (a pair of times, a place in a list).
[[nodiscard]] constexpr bool has_screen_editor(const Pref& pref) {
    return pref.editor == Editor::kTimes || pref.editor == Editor::kZone;
}

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

// The backlight is what brightness sets: a glass whose backlight is not under the board's control cannot be dimmed.
[[nodiscard]] inline Availability needs_backlight(const BoardCaps& caps) {
    if (!caps.has_screen()) {
        return Availability::no("This board has no screen.");
    }
    return caps.display.backlight ? Availability::yes()
                                  : Availability::no("This screen's backlight is not under the board's control.");
}

// The clock view's format means something where there is a screen and a time to write on it.
[[nodiscard]] inline Availability needs_screen_with_clock(const BoardCaps& caps) {
    if (!caps.has_screen()) {
        return Availability::no("This board has no screen.");
    }
    return needs_clock(caps);
}

// The night schedule: a screen that goes dark by the board's own time, and that a touch can light again.
[[nodiscard]] inline Availability needs_screen_with_touch_and_clock(const BoardCaps& caps) {
    const Availability screen = needs_screen_with_touch(caps);
    return screen.available ? needs_clock(caps) : screen;
}

inline constexpr std::array<ChoiceDef, 5> kNowViewChoices{{
    {.id = "speakers", .label = "Speakers"},
    {.id = "title", .label = "Title"},
    {.id = "clock", .label = "Clock", .availability = &needs_clock},
    {.id = "dark", .label = "Dark", .availability = &needs_touch},
    {.id = "off", .label = "Off"},
}};

inline constexpr std::array<ChoiceDef, 2> kSpeakersStyleChoices{{
    {.id = "glow", .label = "Glow"},
    {.id = "pulse", .label = "Pulse"},
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

// Brightness: a stepper of ten stops, every tenth from 10 to 100 (nowview::brightness_stepped); a route takes any
// whole number between.
inline constexpr std::array<ChoiceDef, 10> kBrightnessChoices{{
    {.id = "10", .label = "10%"},  {.id = "20", .label = "20%"},  {.id = "30", .label = "30%"},
    {.id = "40", .label = "40%"},  {.id = "50", .label = "50%"},  {.id = "60", .label = "60%"},
    {.id = "70", .label = "70%"},  {.id = "80", .label = "80%"},  {.id = "90", .label = "90%"},
    {.id = "100", .label = "100%"},
}};

inline constexpr std::array<ChoiceDef, 2> kClockHoursChoices{{
    {.id = "24", .label = "24 hour"},
    {.id = "12", .label = "12 hour"},
}};

inline constexpr std::array<ChoiceDef, 2> kOnOffChoices{{
    {.id = "on", .label = "On"},
    {.id = "off", .label = "Off"},
}};

[[nodiscard]] inline std::string get_brightness(const Backend& backend) { return std::to_string(backend.brightness()); }

[[nodiscard]] inline bool set_brightness(Backend& backend, std::string_view word) {
    const std::optional<int> percent = nowview::parse_brightness(word);
    return percent.has_value() && backend.set_brightness(*percent);
}

[[nodiscard]] inline std::string get_clock_hours(const Backend& backend) {
    return backend.clock_format().twelve_hour ? "12" : "24";
}

[[nodiscard]] inline bool set_clock_hours(Backend& backend, std::string_view word) {
    if (word != "12" && word != "24") {
        return false;
    }
    wallclock::ClockFormat format = backend.clock_format();
    format.twelve_hour = word == "12";
    return backend.set_clock_format(format);
}

[[nodiscard]] inline std::string get_clock_seconds(const Backend& backend) {
    return nowview::on_off_text(backend.clock_format().seconds);
}

[[nodiscard]] inline bool set_clock_seconds(Backend& backend, std::string_view word) {
    const std::optional<bool> on = nowview::parse_on_off(word);
    if (!on) {
        return false;
    }
    wallclock::ClockFormat format = backend.clock_format();
    format.seconds = *on;
    return backend.set_clock_format(format);
}

[[nodiscard]] inline std::string get_night_schedule(const Backend& backend) {
    return nowview::on_off_text(backend.night().enabled);
}

[[nodiscard]] inline bool set_night_schedule(Backend& backend, std::string_view word) {
    const std::optional<bool> on = nowview::parse_on_off(word);
    if (!on) {
        return false;
    }
    nowview::NightSchedule night = backend.night();
    night.enabled = *on;
    return backend.set_night(night);
}

// The two times, as the routes write them (`22:00-07:00`), without the state: setting them leaves the schedule as on or
// off as it was.
[[nodiscard]] inline std::string get_night_times(const Backend& backend) {
    const nowview::NightSchedule night = backend.night();
    return nowview::time_of_day_text(night.from_min) + "-" + nowview::time_of_day_text(night.to_min);
}

[[nodiscard]] inline bool set_night_times(Backend& backend, std::string_view word) {
    // Two times and nothing else: `on`, `off` and a state after the times are the toggle's and the route's words.
    if (word.size() != 11) {
        return false;
    }
    const nowview::NightSchedule now = backend.night();
    const std::optional<nowview::NightSchedule> night =
        nowview::parse_night_schedule(std::string(word) + (now.enabled ? " on" : " off"), now);
    return night.has_value() && backend.set_night(*night);
}

// The schedule is moot where the screen is dark already, and where it has nothing to darken: the Dark view, and the
// Off view (its sentence is not what the schedule covers).
[[nodiscard]] inline std::string night_inert(const Backend& backend) {
    switch (backend.now_view()) {
        case nowview::View::kDark:
            return "The Dark view is dark already: a touch lights it for " + std::to_string(nowview::kDarkAwakeMs / 1000) +
                   " seconds, as at night.";
        case nowview::View::kOff:
            return "Not used while the screen without lyrics is Off.";
        case nowview::View::kSpeakers:
        case nowview::View::kTitle:
        case nowview::View::kClock:
            break;
    }
    return {};
}

[[nodiscard]] inline std::string get_night_idle(const Backend& backend) {
    return nowview::on_off_text(backend.night().idle);
}

[[nodiscard]] inline bool set_night_idle(Backend& backend, std::string_view word) {
    const std::optional<bool> on = nowview::parse_on_off(word);
    if (!on) {
        return false;
    }
    nowview::NightSchedule night = backend.night();
    night.idle = *on;
    return backend.set_night(night);
}

// The idle screen is darkened by the schedule only while the schedule is on.
[[nodiscard]] inline std::string night_idle_inert(const Backend& backend) {
    return backend.night().enabled ? std::string() : "Used while the night schedule is on.";
}

// The zone, as a route says it: its name where a person chose it by name, else its string, else `none`.
[[nodiscard]] inline std::string get_time_zone(const Backend& backend) {
    const wallclock::ZoneChoice zone = backend.time_zone();
    if (!zone.name.empty()) {
        return zone.name;
    }
    return wallclock::zone_setting_text(zone.posix);
}

[[nodiscard]] inline bool set_time_zone(Backend& backend, std::string_view word) {
    const std::optional<wallclock::ZoneChoice> zone = wallclock::parse_zone_choice(word);
    return zone.has_value() && backend.set_time_zone(*zone);
}

// "22:00-07:00" as a person reads it on the button that opens the editor: "22:00 to 07:00".
[[nodiscard]] inline std::string night_times_label(std::string_view value) {
    const std::size_t dash = value.find('-');
    if (dash == std::string_view::npos) {
        return std::string(value);
    }
    return std::string(value.substr(0, dash)) + " to " + std::string(value.substr(dash + 1));
}

// What a person reads for a zone: "Melbourne (Australia)" for a name, "UTC" for none, a string as it is.
[[nodiscard]] inline std::string time_zone_label(std::string_view value) {
    if (value == wallclock::kNoZone) {
        return "UTC";
    }
    const std::optional<wallclock::NamedZone> named = wallclock::find_time_zone(value);
    if (!named) {
        return std::string(value);
    }
    // A city with a place between it and its region already says where it is ("Buenos Aires (Argentina)").
    const std::string city = wallclock::zone_label(named->name);
    const std::string_view region = wallclock::zone_region(named->name);
    if (region == wallclock::kOtherRegion || city.find(" (") != std::string::npos) {
        return city;
    }
    return city + " (" + std::string(region) + ")";
}

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

[[nodiscard]] inline std::string get_speakers_style(const Backend& backend) {
    return nowview::speakers_style_name(backend.speakers_style());
}

[[nodiscard]] inline bool set_speakers_style(Backend& backend, std::string_view word) {
    const std::optional<nowview::SpeakersStyle> style = nowview::parse_speakers_style(word);
    return style.has_value() && backend.set_speakers_style(*style);
}

// The speakers are what the Speakers, Title and Dark views show (the Title view stands on them until a server says a
// title); the Clock view and the old sentence draw none.
[[nodiscard]] inline std::string speakers_style_inert(const Backend& backend) {
    const nowview::View view = backend.now_view();
    if (view == nowview::View::kClock || view == nowview::View::kOff) {
        return std::string("Not used by the ") + (view == nowview::View::kClock ? "Clock" : "Off") +
               " view: it draws no speakers.";
    }
    return {};
}

// The Dark view is dark from the moment a song with no lyrics begins: it has no time to dim after.
[[nodiscard]] inline std::string dim_after_inert(const Backend& backend) {
    if (backend.now_view() != nowview::View::kDark) {
        return {};
    }
    return "Not used by the Dark view: a touch lights it for " + std::to_string(nowview::kDarkAwakeMs / 1000) +
           " seconds, then it is dark again.";
}

// The speaker layout: one choice for each layout in the catalog (layout_options.hpp), each asking the same verdict
// the route and the web page ask. A layout with a limit is available and carries its sentence as the note.
[[nodiscard]] inline Availability layout_needs_sink(const BoardCaps& caps) {
    return caps.sink.slots_most() > 0 ? Availability::yes()
                                      : Availability::no("This board has no audio output to set a layout for.");
}

template <std::size_t I>
[[nodiscard]] inline Availability layout_choice(const BoardCaps& caps) {
    const LayoutOption option = evaluate_layout(caps, kLayoutCatalog[I].name, kLayoutCatalog[I].slots);
    return option.accepted() ? Availability{true, option.reason} : Availability::no(option.reason);
}

template <std::size_t... I>
[[nodiscard]] constexpr std::array<ChoiceDef, sizeof...(I)> layout_choices(std::index_sequence<I...>) {
    return {{{.id = kLayoutCatalog[I].name.data(),
              .label = kLayoutCatalog[I].name.data(),
              .availability = &layout_choice<I>}...}};
}

inline constexpr std::array<ChoiceDef, kLayoutCatalog.size()> kLayoutChoices =
    layout_choices(std::make_index_sequence<kLayoutCatalog.size()>{});

[[nodiscard]] inline std::string get_layout(const Backend& backend) { return backend.layout(); }

[[nodiscard]] inline bool set_layout(Backend& backend, std::string_view word) { return backend.set_layout(word); }

}  // namespace detail

inline constexpr std::array<Pref, 11> kRegistry{{
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
    {.id = "speakers-style",
     .group = Group::kDisplay,
     .label = "Speakers style",
     .note = "How the speakers show the sound: a glow, or a glow with the circle growing and shrinking too.",
     .kind = Kind::kChoice,
     .apply = Apply::kLive,
     .route = "/speakers-style",
     .default_word = "glow",
     .choices = detail::kSpeakersStyleChoices,
     .availability = &detail::needs_screen,
     .inert = &detail::speakers_style_inert,
     .get = &detail::get_speakers_style,
     .set = &detail::set_speakers_style},
    {.id = "brightness",
     .group = Group::kDisplay,
     .label = "Brightness",
     .note = "How bright the screen is while it is lit. Dimmed, it is never brighter than this.",
     .kind = Kind::kStepper,
     .apply = Apply::kLive,
     .route = "/brightness",
     .default_word = "80",
     .unit = "%",
     .choices = detail::kBrightnessChoices,
     .availability = &detail::needs_backlight,
     .get = &detail::get_brightness,
     .set = &detail::set_brightness},
    {.id = "clock-hours",
     .group = Group::kDisplay,
     .label = "Clock hours",
     .note = "How the clock view writes the time.",
     .kind = Kind::kChoice,
     .apply = Apply::kLive,
     .route = "/clock-format",
     .default_word = "24",
     .choices = detail::kClockHoursChoices,
     .availability = &detail::needs_screen_with_clock,
     .get = &detail::get_clock_hours,
     .set = &detail::set_clock_hours},
    {.id = "clock-seconds",
     .group = Group::kDisplay,
     .label = "Clock seconds",
     .note = "Show the seconds beside the time.",
     .kind = Kind::kToggle,
     .apply = Apply::kLive,
     .route = "/clock-format",
     .default_word = "off",
     .choices = detail::kOnOffChoices,
     .availability = &detail::needs_screen_with_clock,
     .get = &detail::get_clock_seconds,
     .set = &detail::set_clock_seconds},
    {.id = "night-schedule",
     .group = Group::kDisplay,
     .label = "Night schedule",
     .note = "Go dark between two times. A touch lights the screen for a while.",
     .kind = Kind::kToggle,
     .apply = Apply::kLive,
     .route = "/night-schedule",
     .default_word = "off",
     .choices = detail::kOnOffChoices,
     .availability = &detail::needs_screen_with_touch_and_clock,
     .inert = &detail::night_inert,
     .get = &detail::get_night_schedule,
     .set = &detail::set_night_schedule},
    {.id = "night-times",
     .group = Group::kDisplay,
     .label = "Night from and to",
     .note = "The board's own time.",
     .kind = Kind::kText,
     .apply = Apply::kLive,
     .route = "/night-schedule",
     .default_word = "22:00-07:00",
     .availability = &detail::needs_screen_with_touch_and_clock,
     .inert = &detail::night_inert,
     .get = &detail::get_night_times,
     .set = &detail::set_night_times,
     .editor = Editor::kTimes,
     .label_of = &detail::night_times_label},
    {.id = "night-idle",
     .group = Group::kDisplay,
     .label = "Night covers idle screen",
     .note = "Also go dark between songs, not only while a song with no lyrics plays.",
     .kind = Kind::kToggle,
     .apply = Apply::kLive,
     .route = "/night-idle",
     .default_word = "off",
     .choices = detail::kOnOffChoices,
     .availability = &detail::needs_screen_with_touch_and_clock,
     .inert = &detail::night_idle_inert,
     .get = &detail::get_night_idle,
     .set = &detail::set_night_idle},
    {.id = "time-zone",
     .group = Group::kTime,
     .label = "Time zone",
     .note = "Where the board is, for its clock.",
     .kind = Kind::kText,
     .apply = Apply::kLive,
     .route = "/timezone",
     .default_word = "none",
     .availability = &detail::needs_clock,
     .get = &detail::get_time_zone,
     .set = &detail::set_time_zone,
     .editor = Editor::kZone,
     .label_of = &detail::time_zone_label},
    {.id = "speaker-layout",
     .group = Group::kSound,
     .label = "Speaker layout",
     .note = "The speakers this player drives, one for each output. Greyed ones are not available on this board.",
     .kind = Kind::kChoice,
     .apply = Apply::kLive,
     .route = "/layout",
     .default_word = "2.0",
     .choices = detail::kLayoutChoices,
     .availability = &detail::layout_needs_sink,
     .get = &detail::get_layout,
     .set = &detail::set_layout,
     .editor = Editor::kList,
     .confirm = "Changing the speakers makes the music drop out for a moment."},
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
    if (entry.value_label.empty() && pref.label_of != nullptr) {
        entry.value_label = pref.label_of(entry.value);
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
