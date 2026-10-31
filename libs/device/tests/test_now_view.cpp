// The no-lyrics screen's setting, its dimming and its wording (iclforge/now_view.hpp), on the host.
//
// What is held here is what GET and PUT /now-playing and /dim-after take, what NVS may hold, and the
// words the screen says about a stream: a stored number that is renumbered turns every board's screen
// into another one, and a heading that names a 5.1 stream a 2.0 one is the screen lying.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <optional>
#include <string>

#include "iclforge/now_view.hpp"

namespace nv = iclforge::nowview;

TEST_CASE("now_view: the five views and their words", "[now_view]") {
    CHECK(std::string(nv::view_name(nv::View::kSpeakers)) == "speakers");
    CHECK(std::string(nv::view_name(nv::View::kTitle)) == "title");
    CHECK(std::string(nv::view_name(nv::View::kClock)) == "clock");
    CHECK(std::string(nv::view_name(nv::View::kOff)) == "off");
    CHECK(std::string(nv::view_name(nv::View::kDark)) == "dark");
    for (const nv::View view :
         {nv::View::kSpeakers, nv::View::kTitle, nv::View::kClock, nv::View::kOff, nv::View::kDark}) {
        const auto parsed = nv::parse_view(nv::view_name(view));
        REQUIRE(parsed.has_value());
        CHECK(*parsed == view);
    }
    CHECK(nv::kDefaultView == nv::View::kSpeakers);
}

TEST_CASE("now_view: a view is read with or without a line ending, and nothing else is a view", "[now_view]") {
    CHECK(nv::parse_view("speakers\n") == nv::View::kSpeakers);
    CHECK(nv::parse_view("  off\r\n") == nv::View::kOff);
    CHECK(nv::parse_view("dark\n") == nv::View::kDark);
    CHECK(nv::parse_view("meters\n") == nv::View::kMeters);
    CHECK_FALSE(nv::parse_view("Meters").has_value());
    CHECK_FALSE(nv::parse_view("Dark").has_value());
    CHECK_FALSE(nv::parse_view("dark screen").has_value());
    CHECK_FALSE(nv::parse_view("").has_value());
    CHECK_FALSE(nv::parse_view("Speakers").has_value());
    CHECK_FALSE(nv::parse_view("speaker").has_value());
    CHECK_FALSE(nv::parse_view("off now").has_value());
    CHECK_FALSE(nv::parse_view("1").has_value());
}

TEST_CASE("now_view: the stored numbers are the ones written down, and a number nobody knows is refused", "[now_view]") {
    // Never renumbered: a partition written by an earlier build is read by a later one.
    CHECK(static_cast<int>(nv::View::kSpeakers) == 0);
    CHECK(static_cast<int>(nv::View::kTitle) == 1);
    CHECK(static_cast<int>(nv::View::kClock) == 2);
    CHECK(static_cast<int>(nv::View::kOff) == 3);
    CHECK(static_cast<int>(nv::View::kDark) == 4);
    CHECK(static_cast<int>(nv::View::kMeters) == 5);
    CHECK(nv::view_from_stored(0) == nv::View::kSpeakers);
    CHECK(nv::view_from_stored(3) == nv::View::kOff);
    CHECK(nv::view_from_stored(4) == nv::View::kDark);
    CHECK(nv::view_from_stored(5) == nv::View::kMeters);
    CHECK(std::string(nv::view_name(nv::View::kMeters)) == "meters");
    CHECK_FALSE(nv::view_from_stored(-1).has_value());
    CHECK_FALSE(nv::view_from_stored(6).has_value());
}

TEST_CASE("now_view: the two speakers styles, their words and their stored numbers", "[now_view]") {
    CHECK(std::string(nv::speakers_style_name(nv::SpeakersStyle::kGlow)) == "glow");
    CHECK(std::string(nv::speakers_style_name(nv::SpeakersStyle::kPulse)) == "pulse");
    CHECK(nv::kDefaultSpeakersStyle == nv::SpeakersStyle::kGlow);
    // What NVS holds is these numbers: a renumbering turns every board's screen into another one.
    CHECK(static_cast<int>(nv::SpeakersStyle::kGlow) == 0);
    CHECK(static_cast<int>(nv::SpeakersStyle::kPulse) == 1);
    CHECK(nv::speakers_style_from_stored(0) == nv::SpeakersStyle::kGlow);
    CHECK(nv::speakers_style_from_stored(1) == nv::SpeakersStyle::kPulse);
    CHECK_FALSE(nv::speakers_style_from_stored(-1).has_value());
    CHECK_FALSE(nv::speakers_style_from_stored(2).has_value());
    for (const nv::SpeakersStyle style : {nv::SpeakersStyle::kGlow, nv::SpeakersStyle::kPulse}) {
        const auto parsed = nv::parse_speakers_style(nv::speakers_style_name(style));
        REQUIRE(parsed.has_value());
        CHECK(*parsed == style);
    }
    CHECK(nv::parse_speakers_style("pulse\n") == nv::SpeakersStyle::kPulse);
    CHECK(nv::parse_speakers_style("  glow\r\n") == nv::SpeakersStyle::kGlow);
    CHECK_FALSE(nv::parse_speakers_style("Glow").has_value());
    CHECK_FALSE(nv::parse_speakers_style("pulse now").has_value());
    CHECK_FALSE(nv::parse_speakers_style("").has_value());
    CHECK_FALSE(nv::parse_speakers_style("speakers").has_value());
}

TEST_CASE("now_view: dim-after is never or whole minutes up to four hours", "[now_view]") {
    CHECK(nv::parse_dim_after("never") == 0);
    CHECK(nv::parse_dim_after("never\n") == 0);
    CHECK(nv::parse_dim_after("5") == 5);
    CHECK(nv::parse_dim_after(" 30 \r\n") == 30);
    CHECK(nv::parse_dim_after("240") == 240);
    CHECK(nv::parse_dim_after("05") == 5);
    // 0 is spelled never, and nothing past the ceiling or short of a minute is a time.
    CHECK_FALSE(nv::parse_dim_after("0").has_value());
    CHECK_FALSE(nv::parse_dim_after("241").has_value());
    CHECK_FALSE(nv::parse_dim_after("-5").has_value());
    CHECK_FALSE(nv::parse_dim_after("").has_value());
    CHECK_FALSE(nv::parse_dim_after("5 min").has_value());
    CHECK_FALSE(nv::parse_dim_after("1000").has_value());
    CHECK_FALSE(nv::parse_dim_after("Never").has_value());
}

TEST_CASE("now_view: dim-after reads back as the words it was set with", "[now_view]") {
    CHECK(nv::dim_after_text(0) == "never");
    CHECK(nv::dim_after_text(5) == "5");
    CHECK(nv::dim_after_text(240) == "240");
    for (const int minutes : {0, 1, 5, 30, 240}) {
        CHECK(nv::parse_dim_after(nv::dim_after_text(minutes)) == minutes);
    }
}

TEST_CASE("now_view: the screen dims after the time set with nothing touched, and only on its own view", "[now_view]") {
    constexpr std::uint32_t kMinute = 60'000;
    // On the view, five minutes set: not a millisecond early, and from the minute on.
    CHECK_FALSE(nv::dimmed(true, 5, 5 * kMinute - 1));
    CHECK(nv::dimmed(true, 5, 5 * kMinute));
    CHECK(nv::dimmed(true, 5, 60 * kMinute));
    // Never is never.
    CHECK_FALSE(nv::dimmed(true, nv::kDimNever, 0xFFFFFFFFU));
    // A lyric screen, the idle screen, a song with the view off: nothing to dim.
    CHECK_FALSE(nv::dimmed(false, 5, 600 * kMinute));
    // The ceiling does not overflow the arithmetic.
    CHECK_FALSE(nv::dimmed(true, nv::kDimMaxMinutes, 240 * kMinute - 1));
    CHECK(nv::dimmed(true, nv::kDimMaxMinutes, 240 * kMinute));
}

TEST_CASE("now_view: the dark view is dark from the moment it shows, and a touch lights it for a while", "[now_view]") {
    using nv::Dim;
    constexpr std::uint32_t kSecond = 1000;
    constexpr nv::View kDark = nv::View::kDark;
    // The song has shown for 10 s and nothing has touched the glass since before it began (it has been
    // idle for an hour): dark, however long that was.
    CHECK(nv::dim_state(kDark, true, 5, 3'600 * kSecond, 10 * kSecond) == Dim::kDark);
    // At the first instant it shows, too.
    CHECK(nv::dim_state(kDark, true, 5, 0, 0) == Dim::kDark);
    // A touch 2 s ago, after the song began (it has shown for 10 s): lit.
    CHECK(nv::dim_state(kDark, true, 5, 2 * kSecond, 10 * kSecond) == Dim::kNone);
    // Lit for exactly kDarkAwakeMs from the last touch, and not a millisecond longer.
    CHECK(nv::dim_state(kDark, true, 5, nv::kDarkAwakeMs - 1, 600 * kSecond) == Dim::kNone);
    CHECK(nv::dim_state(kDark, true, 5, nv::kDarkAwakeMs, 600 * kSecond) == Dim::kDark);
    CHECK(nv::kDarkAwakeMs == 30 * kSecond);
    // Another touch lights it for another 30 s: the time counts from the last.
    CHECK(nv::dim_state(kDark, true, 5, 1 * kSecond, 600 * kSecond) == Dim::kNone);
    // A touch just before the song began is not a touch of this song: the screen goes dark with it.
    CHECK(nv::dim_state(kDark, true, 5, 4 * kSecond, 2 * kSecond) == Dim::kDark);
    // The dim time of the speakers view means nothing here, even "never".
    CHECK(nv::dim_state(kDark, true, nv::kDimNever, 3'600 * kSecond, 10 * kSecond) == Dim::kDark);
    // Not on the no-lyrics view (a lyric screen, the idle screen): lit.
    CHECK(nv::dim_state(kDark, false, 5, 3'600 * kSecond, 3'600 * kSecond) == Dim::kNone);
}

TEST_CASE("now_view: the speakers view dims as it always did, and the dark view does not change it", "[now_view]") {
    using nv::Dim;
    constexpr std::uint32_t kMinute = 60'000;
    // Five minutes set, the song has shown for ten: the glass untouched for five minutes dims it, a
    // millisecond short does not, and a touch lights it.
    CHECK(nv::dim_state(nv::View::kSpeakers, true, 5, 5 * kMinute - 1, 10 * kMinute) == Dim::kNone);
    CHECK(nv::dim_state(nv::View::kSpeakers, true, 5, 5 * kMinute, 10 * kMinute) == Dim::kDimmed);
    CHECK(nv::dim_state(nv::View::kSpeakers, true, 5, 100, 10 * kMinute) == Dim::kNone);
    // The time counts from the song as well: it began a minute ago on a panel nobody touched for hours.
    CHECK(nv::dim_state(nv::View::kSpeakers, true, 5, 600 * kMinute, 1 * kMinute) == Dim::kNone);
    CHECK(nv::dim_state(nv::View::kSpeakers, true, 5, 600 * kMinute, 5 * kMinute) == Dim::kDimmed);
    // Never is never; and a view that is not showing is not dimmed.
    CHECK(nv::dim_state(nv::View::kSpeakers, true, nv::kDimNever, 600 * kMinute, 600 * kMinute) == Dim::kNone);
    CHECK(nv::dim_state(nv::View::kSpeakers, false, 5, 600 * kMinute, 600 * kMinute) == Dim::kNone);
    // It agrees with dimmed() on every input it takes, for the speakers view.
    for (const std::uint32_t inactive : {0U, 1U, 4 * kMinute, 5 * kMinute, 600 * kMinute}) {
        for (const std::uint32_t showing_ms : {0U, 3 * kMinute, 5 * kMinute, 600 * kMinute}) {
            const std::uint32_t quiet = inactive < showing_ms ? inactive : showing_ms;
            CHECK((nv::dim_state(nv::View::kSpeakers, true, 5, inactive, showing_ms) == Dim::kDimmed) ==
                  nv::dimmed(true, 5, quiet));
        }
    }
}

TEST_CASE("now_view: brightness is a whole number from 10 to 100, and nothing else is", "[now_view]") {
    CHECK(nv::parse_brightness("80") == 80);
    CHECK(nv::parse_brightness(" 100\r\n") == 100);
    CHECK(nv::parse_brightness("10") == 10);
    CHECK(nv::parse_brightness("55") == 55);  // a route takes any; the stepper's steps are for fingers
    CHECK_FALSE(nv::parse_brightness("9").has_value());
    CHECK_FALSE(nv::parse_brightness("101").has_value());
    CHECK_FALSE(nv::parse_brightness("0").has_value());
    CHECK_FALSE(nv::parse_brightness("").has_value());
    CHECK_FALSE(nv::parse_brightness("80%").has_value());
    CHECK_FALSE(nv::parse_brightness("-80").has_value());
    CHECK_FALSE(nv::parse_brightness("1000").has_value());
    CHECK_FALSE(nv::parse_brightness("eighty").has_value());
    // What was stored before is the default, and a number outside the range is not believed.
    CHECK(nv::kDefaultBrightness == 80);
    CHECK(nv::brightness_from_stored(10) == 10);
    CHECK(nv::brightness_from_stored(100) == 100);
    CHECK_FALSE(nv::brightness_from_stored(9).has_value());
    CHECK_FALSE(nv::brightness_from_stored(101).has_value());
    CHECK_FALSE(nv::brightness_from_stored(-1).has_value());
}

TEST_CASE("now_view: the brightness stepper moves to the next step and stops at the ends", "[now_view]") {
    CHECK(nv::brightness_stepped(80, +1) == 90);
    CHECK(nv::brightness_stepped(80, -1) == 70);
    CHECK(nv::brightness_stepped(100, +1) == 100);
    CHECK(nv::brightness_stepped(10, -1) == 10);
    CHECK(nv::brightness_stepped(90, +1) == 100);
    CHECK(nv::brightness_stepped(20, -1) == 10);
    // A value a route set between two steps lands on the next one in the direction pressed.
    CHECK(nv::brightness_stepped(85, +1) == 90);
    CHECK(nv::brightness_stepped(85, -1) == 80);
    CHECK(nv::brightness_stepped(11, -1) == 10);
    CHECK(nv::brightness_stepped(99, +1) == 100);
    // Several steps at once, and none.
    CHECK(nv::brightness_stepped(40, +3) == 70);
    CHECK(nv::brightness_stepped(40, -5) == 10);
    CHECK(nv::brightness_stepped(40, 0) == 40);
    // Every value in the range reaches both ends by stepping, never leaving the range.
    for (int percent = nv::kBrightnessMin; percent <= nv::kBrightnessMax; ++percent) {
        int up = percent;
        int down = percent;
        for (int i = 0; i < 12; ++i) {
            up = nv::brightness_stepped(up, +1);
            down = nv::brightness_stepped(down, -1);
            CHECK(up >= nv::kBrightnessMin);
            CHECK(up <= nv::kBrightnessMax);
            CHECK(down >= nv::kBrightnessMin);
            CHECK(down <= nv::kBrightnessMax);
        }
        CHECK(up == nv::kBrightnessMax);
        CHECK(down == nv::kBrightnessMin);
    }
}

TEST_CASE("now_view: the backlight is the lit level, the dim floor under it, or off", "[now_view]") {
    using nv::Dim;
    CHECK(nv::backlight_percent(Dim::kNone, 80) == 80);
    CHECK(nv::backlight_percent(Dim::kNone, 10) == 10);
    CHECK(nv::backlight_percent(Dim::kDimmed, 80) == nv::kDimFloorPercent);
    CHECK(nv::kDimFloorPercent == 4);
    CHECK(nv::backlight_percent(Dim::kDark, 80) == 0);
    CHECK(nv::backlight_percent(Dim::kDark, 100) == 0);
    // The floor is never above the lit level: a screen set as low as it goes does not light up when it dims.
    CHECK(nv::kBrightnessMin > nv::kDimFloorPercent);
    CHECK(nv::backlight_percent(Dim::kDimmed, 3) == 3);
    CHECK(nv::backlight_percent(Dim::kDimmed, 4) == 4);
    CHECK(nv::backlight_percent(Dim::kDimmed, 5) == 4);
    for (int lit = nv::kBrightnessMin; lit <= nv::kBrightnessMax; ++lit) {
        CHECK(nv::backlight_percent(Dim::kDimmed, lit) <= nv::backlight_percent(Dim::kNone, lit));
        CHECK(nv::backlight_percent(Dim::kDark, lit) == 0);
    }
}

TEST_CASE("now_view: a night window is from the first time up to the second, across midnight when it ends before it begins",
          "[now_view]") {
    constexpr int kEvening = 22 * 60;
    constexpr int kMorning = 7 * 60;
    // 22:00 to 07:00: in from the first minute of 22:00 to the last of 06:59, out at 07:00.
    CHECK(nv::in_night_window(kEvening, kMorning, kEvening));
    CHECK(nv::in_night_window(kEvening, kMorning, 23 * 60 + 59));
    CHECK(nv::in_night_window(kEvening, kMorning, 0));
    CHECK(nv::in_night_window(kEvening, kMorning, 3 * 60));
    CHECK(nv::in_night_window(kEvening, kMorning, kMorning - 1));
    CHECK_FALSE(nv::in_night_window(kEvening, kMorning, kMorning));
    CHECK_FALSE(nv::in_night_window(kEvening, kMorning, 12 * 60));
    CHECK_FALSE(nv::in_night_window(kEvening, kMorning, kEvening - 1));
    // A window inside one day.
    CHECK(nv::in_night_window(13 * 60, 15 * 60, 13 * 60));
    CHECK(nv::in_night_window(13 * 60, 15 * 60, 14 * 60 + 59));
    CHECK_FALSE(nv::in_night_window(13 * 60, 15 * 60, 15 * 60));
    CHECK_FALSE(nv::in_night_window(13 * 60, 15 * 60, 12 * 60 + 59));
    // From midnight, to midnight.
    CHECK(nv::in_night_window(0, 6 * 60, 0));
    CHECK(nv::in_night_window(22 * 60, 0, 23 * 60 + 59));
    CHECK_FALSE(nv::in_night_window(22 * 60, 0, 0));
    // The same time twice is an empty window, not a whole day: never dark.
    for (const int minute : {0, 6 * 60, 12 * 60, 22 * 60, 23 * 60 + 59}) {
        CHECK_FALSE(nv::in_night_window(22 * 60, 22 * 60, minute));
        CHECK_FALSE(nv::in_night_window(0, 0, minute));
    }
}

TEST_CASE("now_view: the night schedule is in force only when it is on, the time is known and the time is inside it",
          "[now_view]") {
    nv::NightSchedule night;
    night.enabled = true;
    night.from_min = 22 * 60;
    night.to_min = 7 * 60;
    CHECK(nv::night_active(night, 23 * 60));
    CHECK(nv::night_active(night, 6 * 60 + 59));
    CHECK_FALSE(nv::night_active(night, 7 * 60));
    CHECK_FALSE(nv::night_active(night, 12 * 60));
    // A clock that has not converged is not a time: the schedule is not in force, and the screen stays lit.
    CHECK_FALSE(nv::night_active(night, std::nullopt));
    // And a minute that is not one of a day is not a time either.
    CHECK_FALSE(nv::night_active(night, -1));
    CHECK_FALSE(nv::night_active(night, nv::kMinutesPerDay));
    // Off, the times are kept and nothing is in force.
    night.enabled = false;
    CHECK_FALSE(nv::night_active(night, 23 * 60));
    // The default is off, 22:00 to 07:00, and the idle screen not covered.
    const nv::NightSchedule defaults;
    CHECK_FALSE(defaults.enabled);
    CHECK(defaults.from_min == 22 * 60);
    CHECK(defaults.to_min == 7 * 60);
    CHECK_FALSE(defaults.idle);
}

TEST_CASE("now_view: PUT /night-schedule takes two times, on, off, or both, and reads back as it was set", "[now_view]") {
    const nv::NightSchedule off;
    // Two times set them and turn the schedule on.
    const auto set = nv::parse_night_schedule("23:30-06:15", off);
    REQUIRE(set.has_value());
    CHECK(set->enabled);
    CHECK(set->from_min == 23 * 60 + 30);
    CHECK(set->to_min == 6 * 60 + 15);
    CHECK_FALSE(set->idle);
    // `off` keeps the times, `on` brings the schedule back with them.
    const auto stopped = nv::parse_night_schedule("off\n", *set);
    REQUIRE(stopped.has_value());
    CHECK_FALSE(stopped->enabled);
    CHECK(stopped->from_min == 23 * 60 + 30);
    const auto resumed = nv::parse_night_schedule("on", *stopped);
    REQUIRE(resumed.has_value());
    CHECK(resumed->enabled);
    CHECK(resumed->to_min == 6 * 60 + 15);
    // Both: the times and the state.
    const auto kept_off = nv::parse_night_schedule("21:00-05:00 off", off);
    REQUIRE(kept_off.has_value());
    CHECK_FALSE(kept_off->enabled);
    CHECK(kept_off->from_min == 21 * 60);
    CHECK(kept_off->to_min == 5 * 60);
    CHECK(nv::parse_night_schedule("21:00-05:00 on", off)->enabled);
    // Whatever the idle setting was stays.
    nv::NightSchedule idle_too = off;
    idle_too.idle = true;
    CHECK(nv::parse_night_schedule("21:00-05:00", idle_too)->idle);
    // Equal times are a schedule that is never in force, and are taken: the editor passes through them.
    CHECK(nv::parse_night_schedule("22:00-22:00", off).has_value());
    // The end of the day is 23:59, and 24:00 is not a time.
    CHECK(nv::parse_night_schedule("00:00-23:59", off).has_value());
    for (const char* bad : {"", "22:00", "22:00-", "-07:00", "24:00-07:00", "22:60-07:00", "22:00-07:60", "2:00-07:00",
                            "22:00 07:00", "22:00-07:00 maybe", "22:00-07:00 on off", "ON", "night", "22.00-07.00",
                            "22:00-07:000", "22:00-07:00  on"}) {
        CHECK_FALSE(nv::parse_night_schedule(bad, off).has_value());
    }
    // GET reads it back in the form PUT takes.
    CHECK(nv::night_schedule_text(*set) == "23:30-06:15 on");
    CHECK(nv::night_schedule_text(*stopped) == "23:30-06:15 off");
    CHECK(nv::night_schedule_text(off) == "22:00-07:00 off");
    for (const int from : {0, 1, 59, 60, 22 * 60, 1439}) {
        for (const int to : {0, 7 * 60, 1439}) {
            nv::NightSchedule night;
            night.enabled = from % 2 == 0;
            night.from_min = from;
            night.to_min = to;
            const auto again = nv::parse_night_schedule(nv::night_schedule_text(night), nv::NightSchedule{});
            REQUIRE(again.has_value());
            CHECK(*again == night);
        }
    }
    CHECK(nv::time_of_day_text(5) == "00:05");
    CHECK(nv::time_of_day_text(1439) == "23:59");
    CHECK(nv::parse_time_of_day("07:05") == 7 * 60 + 5);
    CHECK_FALSE(nv::parse_time_of_day("7:05").has_value());
    CHECK_FALSE(nv::parse_time_of_day("07:5").has_value());
    CHECK_FALSE(nv::parse_time_of_day("07-05").has_value());
}

TEST_CASE("now_view: PUT /night-idle takes on or off", "[now_view]") {
    CHECK(nv::parse_on_off("on") == true);
    CHECK(nv::parse_on_off("off\r\n") == false);
    CHECK_FALSE(nv::parse_on_off("On").has_value());
    CHECK_FALSE(nv::parse_on_off("1").has_value());
    CHECK_FALSE(nv::parse_on_off("").has_value());
    CHECK(std::string(nv::on_off_text(true)) == "on");
    CHECK(std::string(nv::on_off_text(false)) == "off");
}

TEST_CASE("now_view: the night schedule makes any view the dark view while it is in force", "[now_view]") {
    using nv::Dim;
    constexpr std::uint32_t kSecond = 1000;
    constexpr std::uint32_t kMinute = 60'000;
    nv::NightSchedule night;
    night.enabled = true;  // 22:00 to 07:00
    const std::optional<int> at_23 = 23 * 60;
    const std::optional<int> at_noon = 12 * 60;
    for (const nv::View view : {nv::View::kSpeakers, nv::View::kTitle, nv::View::kClock, nv::View::kOff}) {
        // A song that begins at 23:00 on a panel nobody has touched for hours is dark at once.
        CHECK(nv::dim_state(view, true, 5, 3'600 * kSecond, 0, night, at_23) == Dim::kDark);
        CHECK(nv::dim_state(view, true, 5, 3'600 * kSecond, 10 * kSecond, night, at_23) == Dim::kDark);
        // A tap lights it for kDarkAwakeMs from the last touch, and not a millisecond longer.
        CHECK(nv::dim_state(view, true, 5, 2 * kSecond, 10 * kMinute, night, at_23) == Dim::kNone);
        CHECK(nv::dim_state(view, true, 5, nv::kDarkAwakeMs - 1, 10 * kMinute, night, at_23) == Dim::kNone);
        CHECK(nv::dim_state(view, true, 5, nv::kDarkAwakeMs, 10 * kMinute, night, at_23) == Dim::kDark);
        // The dim time means nothing at night, as in the dark view, even never.
        CHECK(nv::dim_state(view, true, nv::kDimNever, 3'600 * kSecond, 10 * kSecond, night, at_23) == Dim::kDark);
        // A touch just before the song began is not a touch of this song.
        CHECK(nv::dim_state(view, true, 5, 4 * kSecond, 2 * kSecond, night, at_23) == Dim::kDark);
        // Outside the window the screen is what it is without a schedule.
        CHECK(nv::dim_state(view, true, 5, 3'600 * kSecond, 10 * kSecond, night, at_noon) ==
              nv::dim_state(view, true, 5, 3'600 * kSecond, 10 * kSecond));
        // Not on the no-lyrics screen at all: lit, schedule or no.
        CHECK(nv::dim_state(view, false, 5, 3'600 * kSecond, 3'600 * kSecond, night, at_23) == Dim::kNone);
    }
    // Night begins while a song is playing and the screen has been lit by a touch a minute ago: lit until nothing
    // has touched it for 30 s, then dark; the next touch lights it again.
    CHECK(nv::dim_state(nv::View::kSpeakers, true, 5, 20 * kSecond, 30 * kMinute, night, at_23) == Dim::kNone);
    CHECK(nv::dim_state(nv::View::kSpeakers, true, 5, 31 * kSecond, 30 * kMinute, night, at_23) == Dim::kDark);
    CHECK(nv::dim_state(nv::View::kSpeakers, true, 5, 1 * kSecond, 30 * kMinute, night, at_23) == Dim::kNone);
    // A clock that has not converged: the schedule is not in force, and a screen that was lit stays lit.
    CHECK(nv::dim_state(nv::View::kSpeakers, true, 5, 3'600 * kSecond, 10 * kSecond, night, std::nullopt) == Dim::kNone);
    CHECK(nv::dim_state(nv::View::kClock, true, 5, 3'600 * kSecond, 10 * kSecond, night, std::nullopt) == Dim::kNone);
    // Off, or an empty window: never dark.
    nv::NightSchedule off = night;
    off.enabled = false;
    CHECK(nv::dim_state(nv::View::kSpeakers, true, 5, 3'600 * kSecond, 10 * kSecond, off, at_23) == Dim::kNone);
    nv::NightSchedule empty = night;
    empty.to_min = empty.from_min;
    CHECK(nv::dim_state(nv::View::kSpeakers, true, 5, 3'600 * kSecond, 10 * kSecond, empty, at_23) == Dim::kNone);
    // The dark view stays dark with the schedule off, in any minute, as it was.
    CHECK(nv::dim_state(nv::View::kDark, true, 5, 3'600 * kSecond, 10 * kSecond, off, at_noon) == Dim::kDark);
    CHECK(nv::dim_state(nv::View::kDark, true, 5, 3'600 * kSecond, 10 * kSecond, off, std::nullopt) == Dim::kDark);
    // dark_state is the rule on its own, which the idle screen can use when its time comes.
    CHECK(nv::dark_state(3'600 * kSecond, 0) == Dim::kDark);
    CHECK(nv::dark_state(2 * kSecond, 10 * kSecond) == Dim::kNone);
}

TEST_CASE("now_view: a stream is named for its codec and the layout it was coded in", "[now_view]") {
    using nv::Codec;
    CHECK(nv::stream_name({.codec = Codec::kPcm}) == "PCM");
    // PCM has no coded layout whatever the fields say.
    CHECK(nv::stream_name({.codec = Codec::kPcm, .have_decoder = true, .acmod = 7, .lfe = true}) == "PCM");
    // Before the decoder has looked at a unit, only the codec is known.
    CHECK(nv::stream_name({.codec = Codec::kEac3}) == "Dolby Digital Plus");
    CHECK(nv::stream_name({.codec = Codec::kUnknown}) == "Coded audio");
    CHECK(nv::stream_name({.codec = Codec::kAc3, .have_decoder = true, .acmod = 7, .lfe = true}) == "Dolby Digital 5.1");
    CHECK(nv::stream_name({.codec = Codec::kAc3, .have_decoder = true, .acmod = 2, .lfe = false}) ==
          "Dolby Digital 2.0");
    CHECK(nv::stream_name({.codec = Codec::kEac3, .have_decoder = true, .acmod = 7, .lfe = true}) ==
          "Dolby Digital Plus 5.1");
    CHECK(nv::stream_name({.codec = Codec::kAc4, .have_decoder = true, .acmod = 7, .lfe = true}) ==
          "Dolby AC-4 5.1");
}

TEST_CASE("now_view: every audio coding mode has its channel count", "[now_view]") {
    // A/52 Table 5.8: 1+1, 1/0, 2/0, 3/0, 2/1, 3/1, 2/2, 3/2.
    const int expected[8] = {2, 1, 2, 3, 3, 4, 4, 5};
    for (int acmod = 0; acmod < 8; ++acmod) {
        CHECK(nv::acmod_channels(acmod) == expected[acmod]);
    }
    CHECK(nv::acmod_channels(-1) == 0);
    CHECK(nv::acmod_channels(8) == 0);
    CHECK(nv::stream_name({.codec = nv::Codec::kAc3, .have_decoder = true, .acmod = 0}) == "Dolby Digital dual mono");
    CHECK(nv::stream_name({.codec = nv::Codec::kAc3, .have_decoder = true, .acmod = 6, .lfe = true}) == "Dolby Digital 4.1");
}

TEST_CASE("now_view: objects are said once the stream carries any", "[now_view]") {
    using nv::Codec;
    CHECK(nv::stream_name({.codec = Codec::kEac3, .have_decoder = true, .acmod = 7, .lfe = true, .objects = 15}) ==
          "Dolby Digital Plus 5.1 + 15 objects");
    CHECK(nv::stream_name({.codec = Codec::kEac3, .have_decoder = true, .acmod = 7, .lfe = true, .objects = 1}) ==
          "Dolby Digital Plus 5.1 + 1 object");
}

TEST_CASE("now_view: a room is named by its name, or by how many outputs it has", "[now_view]") {
    CHECK(nv::room_name("5.1", 6) == "5.1");
    CHECK(nv::room_name("7.1.4", 12) == "7.1.4");
    CHECK(nv::room_name("7.1.4:top", 12) == "7.1.4:top");
    CHECK(nv::room_name("2.0\n", 2) == "2.0");
    // A list of speakers is too long to say, and starts with a letter or an angle.
    CHECK(nv::room_name("L:small,R:small,C,LFE,Ls,Rs", 6) == "6 outputs");
    CHECK(nv::room_name("30/0,-30/0,lfe", 3) == "3 outputs");
    CHECK(nv::room_name("", 2) == "2 outputs");
    CHECK(nv::room_name("", 1) == "1 output");
    CHECK(nv::room_name("", 0).empty());
}

TEST_CASE("now_view: the heading says what arrives and what it is played on", "[now_view]") {
    using nv::Codec;
    CHECK(nv::heading({.codec = Codec::kEac3, .have_decoder = true, .acmod = 7, .lfe = true}, "7.1.4", 12) ==
          "Dolby Digital Plus 5.1 to 7.1.4");
    CHECK(nv::heading({.codec = Codec::kPcm}, "2.0", 2) == "PCM to 2.0");
    // No room known: the stream alone.
    CHECK(nv::heading({.codec = Codec::kPcm}, "", 0) == "PCM");
}

TEST_CASE("now_view: a server's name is shortened to what a person reads", "[now_view]") {
    CHECK(nv::server_label("Music Assistant (d5369777-music-assistant)") == "Music Assistant");
    CHECK(nv::server_label("  Hearth on STUDIO-PC ") == "Hearth on STUDIO-PC");
    CHECK(nv::server_label("") == "");
    // Nothing before the brackets: the whole string stays.
    CHECK(nv::server_label("(id)") == "(id)");
    CHECK(nv::server_label("Plain (not closed") == "Plain (not closed");
}

TEST_CASE("now_view: the bitrate settles on the rate of the bytes played", "[now_view]") {
    nv::BitrateMeter meter;
    CHECK(meter.kbps() == 0);
    // 768 kb/s is 96,000 bytes a second.
    meter.observe(0, 10'000);
    CHECK(meter.kbps() == 0);
    meter.observe(48'000, 10'500);  // under a window: not measured yet
    CHECK(meter.kbps() == 0);
    meter.observe(96'000, 11'000);
    CHECK(meter.kbps() == 768);
    // A burst of network does not show: the next window is twice as fast and the figure moves a third of the way.
    meter.observe(96'000 + 192'000, 12'000);
    CHECK(meter.kbps() == 1024);
    meter.observe(96'000 + 192'000 + 96'000, 13'000);
    CHECK(meter.kbps() == 939);
}

TEST_CASE("now_view: the bitrate starts again with a stream and is unknown while nothing plays", "[now_view]") {
    nv::BitrateMeter meter;
    meter.observe(0, 0);
    meter.observe(56'000, 1'000);
    CHECK(meter.kbps() == 448);
    // Nothing played in a window: no figure rather than the last one.
    meter.observe(56'000, 2'000);
    CHECK(meter.kbps() == 0);
    // A smaller total is a new stream.
    meter.observe(12'000, 3'000);
    CHECK(meter.kbps() == 0);
    meter.observe(12'000 + 96'000, 4'000);
    CHECK(meter.kbps() == 768);
    meter.reset();
    CHECK(meter.kbps() == 0);
}

TEST_CASE("now_view: a coded stream says it is decoded on this board, most to least", "[now_view]") {
    using Codec = nv::Codec;
    const nv::StreamFacts atmos{.codec = Codec::kEac3, .have_decoder = true, .acmod = 7, .lfe = true, .objects = 15};
    const auto full = nv::stream_info_options(atmos, 768, "Music Assistant (d5369777-music-assistant)");
    REQUIRE(full.size() == 5);
    CHECK(full[0] == "Decoded on this board - 768 kb/s - from Music Assistant");
    CHECK(full[1] == "Decoded here - 768 kb/s - from Music Assistant");
    CHECK(full[2] == "Decoded on this board - 768 kb/s");
    CHECK(full[3] == "Decoded here - 768 kb/s");
    CHECK(full[4] == "768 kb/s");
    // No figure yet, no server: nothing is made up.
    const auto bare = nv::stream_info_options(atmos, 0, "");
    REQUIRE(bare.size() == 2);
    CHECK(bare[0] == "Decoded on this board");
    CHECK(bare[1] == "Decoded here");
    // Before the first unit has decoded it is still the board that will decode it.
    CHECK(nv::stream_info_options({.codec = Codec::kUnknown}, 0, "Hearth")[0] == "Decoded on this board - from Hearth");
}

TEST_CASE("now_view: a PCM stream says the server decoded it", "[now_view]") {
    const auto pcm = nv::stream_info_options({.codec = nv::Codec::kPcm}, 0, "Music Assistant (d5369777-music-assistant)");
    REQUIRE(pcm.size() == 2);
    CHECK(pcm[0] == "PCM from Music Assistant");
    CHECK(pcm[1] == "PCM");
    CHECK(nv::stream_info_options({.codec = nv::Codec::kPcm}, 0, "")[0] == "PCM");
}

TEST_CASE("now_view: the night schedule's time editor steps a quarter of an hour round the clock", "[now_view]") {
    CHECK(nv::kNightStepMinutes == 15);
    CHECK(nv::night_time_stepped(22 * 60, +1) == 22 * 60 + 15);
    CHECK(nv::night_time_stepped(22 * 60, -1) == 21 * 60 + 45);
    CHECK(nv::night_time_stepped(22 * 60, +4) == 23 * 60);
    CHECK(nv::night_time_stepped(23 * 60 + 45, +1) == 0);
    CHECK(nv::night_time_stepped(0, -1) == 23 * 60 + 45);
    CHECK(nv::night_time_stepped(0, 0) == 0);
    // A time a route set between two steps lands on the next in the direction given.
    CHECK(nv::night_time_stepped(7 * 60 + 5, +1) == 7 * 60 + 15);
    CHECK(nv::night_time_stepped(7 * 60 + 5, -1) == 7 * 60);
    CHECK(nv::night_time_stepped(7 * 60 + 14, -1) == 7 * 60);
    // A whole round of the day is the day.
    CHECK(nv::night_time_stepped(5 * 60, +96) == 5 * 60);
    CHECK(nv::night_time_stepped(5 * 60, -96) == 5 * 60);
    // Every minute of the day stays a minute of the day, however it is stepped.
    for (int minute = 0; minute < nv::kMinutesPerDay; ++minute) {
        CHECK(nv::valid_minute_of_day(nv::night_time_stepped(minute, +1)));
        CHECK(nv::valid_minute_of_day(nv::night_time_stepped(minute, -1)));
        CHECK(nv::night_time_stepped(minute, +1) % nv::kNightStepMinutes == 0);
        CHECK(nv::night_time_stepped(minute, -1) % nv::kNightStepMinutes == 0);
    }
}

TEST_CASE("now_view: the night schedule is stored as one number, and a number no build wrote is refused", "[now_view]") {
    const nv::NightSchedule night{.enabled = true, .from_min = 22 * 60, .to_min = 7 * 60 + 15, .idle = false};
    const std::int32_t stored = nv::night_stored(night);
    CHECK(nv::night_from_stored(stored) == night);
    // The layout is the one the key's comment names: from, then to, then the state, then the idle screen.
    CHECK(stored == (22 * 60) + ((7 * 60 + 15) << 11) + (1 << 22));
    // Every state of the two switches survives, at both ends of the day.
    for (const bool enabled : {false, true}) {
        for (const bool idle : {false, true}) {
            for (const int from : {0, 1, 719, 1439}) {
                for (const int to : {0, 1, 719, 1439}) {
                    const nv::NightSchedule each{.enabled = enabled, .from_min = from, .to_min = to, .idle = idle};
                    CHECK(nv::night_from_stored(nv::night_stored(each)) == each);
                }
            }
        }
    }
    // The default a board starts with is off, from 22:00 to 07:00.
    CHECK(nv::night_from_stored(nv::night_stored(nv::NightSchedule{})) == nv::NightSchedule{});
    // A time of day that is not one, a bit above the idle bit, and a number below nothing.
    CHECK_FALSE(nv::night_from_stored(1440).has_value());
    CHECK_FALSE(nv::night_from_stored(1440 << 11).has_value());
    CHECK_FALSE(nv::night_from_stored(std::int64_t{1} << 24).has_value());
    CHECK_FALSE(nv::night_from_stored(-1).has_value());
    CHECK_FALSE(nv::night_from_stored(std::int64_t{1} << 40).has_value());
}

TEST_CASE("now_view: the night schedule covers the idle screen only when it is told to, and only in force", "[now_view]") {
    nv::NightSchedule night{.enabled = true, .from_min = 22 * 60, .to_min = 7 * 60, .idle = false};
    // Off by default, in the window or not.
    CHECK_FALSE(nv::night_covers_idle(night, 23 * 60));
    night.idle = true;
    CHECK(nv::night_covers_idle(night, 23 * 60));
    CHECK(nv::night_covers_idle(night, 6 * 60 + 59));
    CHECK_FALSE(nv::night_covers_idle(night, 7 * 60));
    CHECK_FALSE(nv::night_covers_idle(night, 12 * 60));
    // A clock that has not converged is never dark by accident; a schedule that is off covers nothing.
    CHECK_FALSE(nv::night_covers_idle(night, std::nullopt));
    night.enabled = false;
    CHECK_FALSE(nv::night_covers_idle(night, 23 * 60));
}
