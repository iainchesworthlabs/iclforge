// The clock view's time zone and its words (iclforge/wall_clock.hpp), on the host.
//
// What is held here is what PUT /timezone takes, what NVS may hold, and what the screen says: a zone read the
// wrong way round puts a clock ten hours out, and a clock that says a time it does not know is worse than one
// that says nothing.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>

#include "iclforge/wall_clock.hpp"

namespace wc = iclforge::wallclock;

TEST_CASE("wall_clock: a POSIX zone is read with its offset the way round the string means it", "[wall_clock]") {
    // The string's offset is hours WEST of UTC, so a zone east of Greenwich has a minus sign.
    const auto sydney = wc::parse_time_zone("AEST-10AEDT,M10.1.0,M4.1.0/3");
    REQUIRE(sydney.has_value());
    CHECK(sydney->std_east_s == 10 * 3600);
    CHECK(sydney->has_dst);
    CHECK(sydney->dst_east_s == 11 * 3600);

    const auto new_york = wc::parse_time_zone("EST5EDT,M3.2.0,M11.1.0");
    REQUIRE(new_york.has_value());
    CHECK(new_york->std_east_s == -5 * 3600);
    CHECK(new_york->dst_east_s == -4 * 3600);

    const auto tokyo = wc::parse_time_zone("JST-9");
    REQUIRE(tokyo.has_value());
    CHECK(tokyo->std_east_s == 9 * 3600);
    CHECK_FALSE(tokyo->has_dst);

    const auto utc = wc::parse_time_zone("UTC0");
    REQUIRE(utc.has_value());
    CHECK(utc->std_east_s == 0);
    CHECK_FALSE(utc->has_dst);
}

TEST_CASE("wall_clock: half-hour zones, quoted names and a daylight offset that is not an hour", "[wall_clock]") {
    const auto india = wc::parse_time_zone("<+0530>-5:30");
    REQUIRE(india.has_value());
    CHECK(india->std_east_s == 5 * 3600 + 30 * 60);

    const auto adelaide = wc::parse_time_zone("ACST-9:30ACDT,M10.1.0,M4.1.0/3");
    REQUIRE(adelaide.has_value());
    CHECK(adelaide->std_east_s == 9 * 3600 + 30 * 60);
    CHECK(adelaide->dst_east_s == 10 * 3600 + 30 * 60);

    // Lord Howe Island: half an hour of daylight saving, written out.
    const auto lord_howe = wc::parse_time_zone("LHST-10:30LHDT-11,M10.1.0,M4.1.0");
    REQUIRE(lord_howe.has_value());
    CHECK(lord_howe->std_east_s == 10 * 3600 + 30 * 60);
    CHECK(lord_howe->dst_east_s == 11 * 3600);

    // Ireland writes its summer time as standard and its winter time as the daylight one: the "daylight"
    // offset is the smaller.
    const auto dublin = wc::parse_time_zone("IST-1GMT0,M10.5.0,M3.5.0/1");
    REQUIRE(dublin.has_value());
    CHECK(dublin->std_east_s == 3600);
    CHECK(dublin->dst_east_s == 0);

    const auto brazil = wc::parse_time_zone("<-03>3");
    REQUIRE(brazil.has_value());
    CHECK(brazil->std_east_s == -3 * 3600);

    CHECK(wc::valid_time_zone("NZST-12NZDT,M9.5.0,M4.1.0/3"));
    CHECK(wc::valid_time_zone("CET-1CEST,M3.5.0,M10.5.0/3"));
    CHECK(wc::valid_time_zone("EST5EDT,J60,J300/2:30:00"));
    CHECK(wc::valid_time_zone("XXX1YYY,0,300"));
    // A rule's time may run past a day, either way (POSIX 2018 allows up to 167 hours).
    CHECK(wc::valid_time_zone("EST5EDT,M3.2.0/-1,M11.1.0/25"));
}

TEST_CASE("wall_clock: what is not a zone is refused", "[wall_clock]") {
    // Nothing to read, or nothing that is an offset.
    CHECK_FALSE(wc::valid_time_zone(""));
    CHECK_FALSE(wc::valid_time_zone("AEST"));
    CHECK_FALSE(wc::valid_time_zone("-10"));
    CHECK_FALSE(wc::valid_time_zone("none"));
    CHECK_FALSE(wc::valid_time_zone("Australia/Sydney"));
    // A name of fewer than three letters, or with something in it that is not one.
    CHECK_FALSE(wc::valid_time_zone("AE-10"));
    CHECK_FALSE(wc::valid_time_zone("AE5T-10"));
    CHECK_FALSE(wc::valid_time_zone("<>-10"));
    CHECK_FALSE(wc::valid_time_zone("<+10-10"));
    CHECK_FALSE(wc::valid_time_zone("<+1 0>-10"));
    // An offset out of range.
    CHECK_FALSE(wc::valid_time_zone("AEST-25"));
    CHECK_FALSE(wc::valid_time_zone("AEST-10:60"));
    CHECK_FALSE(wc::valid_time_zone("AEST-10:30:61"));
    CHECK_FALSE(wc::valid_time_zone("AEST-100"));
    // Daylight saving without its rules: a C library's own default would stand in, and they differ.
    CHECK_FALSE(wc::valid_time_zone("AEST-10AEDT"));
    CHECK_FALSE(wc::valid_time_zone("AEST-10AEDT-11"));
    CHECK_FALSE(wc::valid_time_zone("AEST-10AEDT,M10.1.0"));
    CHECK_FALSE(wc::valid_time_zone("AEST-10AEDT,M10.1.0,"));
    CHECK_FALSE(wc::valid_time_zone("AEST-10AEDT,,M4.1.0"));
    // A rule with a month, week or day that is not one.
    CHECK_FALSE(wc::valid_time_zone("AEST-10AEDT,M13.1.0,M4.1.0"));
    CHECK_FALSE(wc::valid_time_zone("AEST-10AEDT,M0.1.0,M4.1.0"));
    CHECK_FALSE(wc::valid_time_zone("AEST-10AEDT,M10.0.0,M4.1.0"));
    CHECK_FALSE(wc::valid_time_zone("AEST-10AEDT,M10.6.0,M4.1.0"));
    CHECK_FALSE(wc::valid_time_zone("AEST-10AEDT,M10.1.7,M4.1.0"));
    CHECK_FALSE(wc::valid_time_zone("AEST-10AEDT,J0,J300"));
    CHECK_FALSE(wc::valid_time_zone("AEST-10AEDT,J366,J300"));
    CHECK_FALSE(wc::valid_time_zone("AEST-10AEDT,366,300"));
    CHECK_FALSE(wc::valid_time_zone("AEST-10AEDT,M10.1.0/,M4.1.0"));
    // Anything after the end.
    CHECK_FALSE(wc::valid_time_zone("UTC0 "));
    CHECK_FALSE(wc::valid_time_zone("UTC0x"));
    CHECK_FALSE(wc::valid_time_zone("AEST-10AEDT,M10.1.0,M4.1.0,M1.1.1"));
    // Whitespace, control characters and bytes that are not ASCII.
    CHECK_FALSE(wc::valid_time_zone("AEST -10"));
    CHECK_FALSE(wc::valid_time_zone("UTC0\n"));
    CHECK_FALSE(wc::valid_time_zone(std::string("UTC0\0", 5)));
    CHECK_FALSE(wc::valid_time_zone("UTC\xC3\xA9" "0"));
}

TEST_CASE("wall_clock: a zone string is held to the 63 bytes NVS has for it", "[wall_clock]") {
    // A long but valid zone: the longest names the grammar and the board allow, padded with rule times.
    std::string longest = "<+0000>-0:00<+0000>-0:00,M10.5.0/167:59:59,M4.5.0/167:59:59";
    REQUIRE(longest.size() <= wc::kTimeZoneMaxBytes);
    CHECK(wc::valid_time_zone(longest));
    std::string too_long = "AEST-10AEDT,M10.1.0/" + std::string(wc::kTimeZoneMaxBytes, '1') + ",M4.1.0";
    CHECK(too_long.size() > wc::kTimeZoneMaxBytes);
    CHECK_FALSE(wc::valid_time_zone(too_long));
    // A name of seventeen letters is more than a C library keeps.
    CHECK_FALSE(wc::valid_time_zone("ABCDEFGHIJKLMNOPQ-10"));
    CHECK(wc::valid_time_zone("ABCDEFGHIJKLMNOP-10"));
}

TEST_CASE("wall_clock: PUT /timezone takes a zone or `none`, with a line ending or without", "[wall_clock]") {
    CHECK(wc::parse_zone_setting("AEST-10AEDT,M10.1.0,M4.1.0/3") == std::string("AEST-10AEDT,M10.1.0,M4.1.0/3"));
    CHECK(wc::parse_zone_setting("  AEST-10AEDT,M10.1.0,M4.1.0/3\r\n") == std::string("AEST-10AEDT,M10.1.0,M4.1.0/3"));
    // `none` takes the zone away: an empty string, which is a body that was understood.
    const auto none = wc::parse_zone_setting("none\n");
    REQUIRE(none.has_value());
    CHECK(none->empty());
    CHECK_FALSE(wc::parse_zone_setting("").has_value());
    CHECK_FALSE(wc::parse_zone_setting("None").has_value());
    CHECK_FALSE(wc::parse_zone_setting("Sydney").has_value());
    CHECK_FALSE(wc::parse_zone_setting("AEST-10AEDT").has_value());
}

TEST_CASE("wall_clock: GET /timezone reads back as the words PUT took", "[wall_clock]") {
    CHECK(wc::zone_setting_text("") == "none");
    CHECK(wc::zone_setting_text("JST-9") == "JST-9");
    for (const char* zone : {"", "JST-9", "AEST-10AEDT,M10.1.0,M4.1.0/3", "UTC0"}) {
        const auto parsed = wc::parse_zone_setting(wc::zone_setting_text(zone));
        REQUIRE(parsed.has_value());
        CHECK(*parsed == std::string(zone));
    }
}

TEST_CASE("wall_clock: a C library that ignored the string is caught, and one that read it is not", "[wall_clock]") {
    const auto sydney = *wc::parse_time_zone("AEST-10AEDT,M10.1.0,M4.1.0/3");
    // January is summer there, July is winter.
    CHECK(wc::zone_agrees(sydney, 11 * 3600, 10 * 3600));
    // UTC, which is what a library does with a string it cannot parse.
    CHECK_FALSE(wc::zone_agrees(sydney, 0, 0));
    // The offset the wrong way round.
    CHECK_FALSE(wc::zone_agrees(sydney, -11 * 3600, -10 * 3600));

    const auto tokyo = *wc::parse_time_zone("JST-9");
    CHECK(wc::zone_agrees(tokyo, 9 * 3600, 9 * 3600));
    CHECK_FALSE(wc::zone_agrees(tokyo, 9 * 3600, 10 * 3600));
    CHECK_FALSE(wc::zone_agrees(tokyo, 0, 0));

    // UTC0 agrees with a library that did fall back to UTC, which is the zone it asked for.
    CHECK(wc::zone_agrees(*wc::parse_time_zone("UTC0"), 0, 0));
}

TEST_CASE("wall_clock: the two probe instants are mid-January and mid-July of 2026", "[wall_clock]") {
    const wc::LocalTime january = wc::local_from_unix(wc::kProbeJanuary, 0);
    CHECK(january.year == 2026);
    CHECK(january.month == 1);
    CHECK(january.day == 15);
    CHECK(january.hour == 12);
    CHECK(january.minute == 0);
    const wc::LocalTime july = wc::local_from_unix(wc::kProbeJuly, 0);
    CHECK(july.year == 2026);
    CHECK(july.month == 7);
    CHECK(july.day == 15);
    CHECK(july.hour == 12);
}

TEST_CASE("wall_clock: no answer is taken for the time from before the first of January 2026", "[wall_clock]") {
    CHECK(wc::kNotBefore == wc::days_from_civil(2026, 1, 1) * 86400);
    CHECK(wc::kNotBefore < wc::kProbeJanuary);
    const wc::LocalTime start = wc::local_from_unix(wc::kNotBefore, 0);
    CHECK(start == wc::LocalTime{2026, 1, 1, 0, 0, 0, 4});
}

TEST_CASE("wall_clock: a count of seconds is a date and a time in the zone it is read in", "[wall_clock]") {
    const wc::LocalTime epoch = wc::local_from_unix(0, 0);
    CHECK(epoch == wc::LocalTime{1970, 1, 1, 0, 0, 0, 4});  // a Thursday

    // The prototype's picture: Tuesday 20 October 2026, 21:47 in Sydney (AEDT, +11) is 10:47 UTC.
    const std::int64_t at = wc::days_from_civil(2026, 10, 20) * 86400 + 10 * 3600 + 47 * 60;
    const wc::LocalTime utc = wc::local_from_unix(at, 0);
    CHECK(utc == wc::LocalTime{2026, 10, 20, 10, 47, 0, 2});
    const wc::LocalTime sydney = wc::local_from_unix(at, 11 * 3600);
    CHECK(sydney == wc::LocalTime{2026, 10, 20, 21, 47, 0, 2});
    // An offset that carries over midnight carries the date and the day of the week with it.
    const wc::LocalTime next_day = wc::local_from_unix(at + 3 * 3600, 11 * 3600);
    CHECK(next_day == wc::LocalTime{2026, 10, 21, 0, 47, 0, 3});
    const wc::LocalTime previous_day = wc::local_from_unix(at, -11 * 3600);
    CHECK(previous_day == wc::LocalTime{2026, 10, 19, 23, 47, 0, 1});

    // A leap day, the end of a year and a date before the epoch.
    CHECK(wc::local_from_unix(wc::days_from_civil(2028, 2, 29) * 86400, 0) == wc::LocalTime{2028, 2, 29, 0, 0, 0, 2});
    CHECK(wc::local_from_unix(wc::days_from_civil(2027, 12, 31) * 86400 + 86399, 0) ==
          wc::LocalTime{2027, 12, 31, 23, 59, 59, 5});
    CHECK(wc::local_from_unix(-1, 0) == wc::LocalTime{1969, 12, 31, 23, 59, 59, 3});
    CHECK(wc::local_from_unix(wc::days_from_civil(1900, 3, 1) * 86400, 0) == wc::LocalTime{1900, 3, 1, 0, 0, 0, 4});
}

TEST_CASE("wall_clock: the offset a broken-down time implies is the one it was made with", "[wall_clock]") {
    for (const int offset : {-12 * 3600, -3 * 3600 - 1800, 0, 9 * 3600 + 1800, 10 * 3600, 11 * 3600, 14 * 3600}) {
        for (std::int64_t t = -90000; t < 4'000'000'000LL; t += 1'234'567'891LL / 3 + 7) {
            const wc::LocalTime local = wc::local_from_unix(t, offset);
            REQUIRE(wc::offset_of(local, t) == offset);
        }
        // And densely, through a leap year, so that every month's end and the leap day are crossed.
        for (std::int64_t t = wc::days_from_civil(2027, 12, 1) * 86400; t < wc::days_from_civil(2029, 1, 10) * 86400; t += 3607) {
            const wc::LocalTime local = wc::local_from_unix(t, offset);
            REQUIRE(wc::offset_of(local, t) == offset);
            REQUIRE(local.month >= 1);
            REQUIRE(local.month <= 12);
            REQUIRE(local.day >= 1);
            REQUIRE(local.day <= 31);
            REQUIRE(local.weekday >= 0);
            REQUIRE(local.weekday <= 6);
        }
    }
}

TEST_CASE("wall_clock: the face is the time to the minute, the date, and a note when no zone is set", "[wall_clock]") {
    wc::Reading reading;
    // The board does not know the time yet: nothing is said, not a time made up.
    CHECK(wc::face(reading) == wc::Face{});

    reading.known = true;
    reading.local = wc::LocalTime{2026, 10, 20, 21, 47, 12, 2};
    reading.zone_set = true;
    wc::Face face = wc::face(reading);
    CHECK(face.time == "21:47");
    CHECK(face.date == "Tuesday 20 October");
    CHECK(face.note.empty());

    // Without a zone it is UTC and says so.
    reading.zone_set = false;
    face = wc::face(reading);
    CHECK(face.time == "21:47");
    CHECK(face.note == wc::kUtcNote);

    // 24 hours, with the leading zero a clock has.
    reading.local = wc::LocalTime{2026, 1, 1, 0, 5, 59, 4};
    face = wc::face(reading);
    CHECK(face.time == "00:05");
    CHECK(face.date == "Thursday 1 January");
    reading.local = wc::LocalTime{2026, 12, 31, 23, 59, 0, 4};
    CHECK(wc::face(reading).time == "23:59");
    CHECK(wc::face(reading).date == "Thursday 31 December");
    reading.local = wc::LocalTime{2026, 7, 9, 12, 0, 0, 4};
    CHECK(wc::face(reading).time == "12:00");

    // The seconds are not shown, so the face is the same for a whole minute: that is what lets the screen
    // redraw once a minute and not once a second.
    reading.local = wc::LocalTime{2026, 10, 20, 21, 47, 0, 2};
    const wc::Face start = wc::face(reading);
    reading.local.second = 59;
    CHECK(wc::face(reading) == start);
    reading.local.minute = 48;
    CHECK_FALSE(wc::face(reading) == start);
}

TEST_CASE("wall_clock: the rules of a zone are read as the days they fall on", "[wall_clock]") {
    const auto melbourne = wc::parse_zone_rules("AEST-10AEDT,M10.1.0,M4.1.0/3");
    REQUIRE(melbourne.has_value());
    CHECK(melbourne->offsets == *wc::parse_time_zone("AEST-10AEDT,M10.1.0,M4.1.0/3"));
    CHECK(melbourne->start.kind == wc::RuleKind::kMonthWeekDay);
    CHECK(melbourne->start.month == 10);
    CHECK(melbourne->start.week == 1);
    CHECK(melbourne->start.day == 0);
    CHECK(melbourne->start.time_s == 7200);  // the default: 02:00
    CHECK(melbourne->end.month == 4);
    CHECK(melbourne->end.time_s == 3 * 3600);

    // A time past a day and a negative one, as Cairo's and Nuuk's rules have.
    const auto cairo = wc::parse_zone_rules("EET-2EEST,M4.5.5/0,M10.5.4/24");
    REQUIRE(cairo.has_value());
    CHECK(cairo->start.time_s == 0);
    CHECK(cairo->end.time_s == 24 * 3600);
    const auto nuuk = wc::parse_zone_rules("<-02>2<-01>,M3.5.0/-1,M10.5.0/0");
    REQUIRE(nuuk.has_value());
    CHECK(nuuk->start.time_s == -3600);

    // The three kinds of day.
    const auto julian = wc::parse_zone_rules("XXX1YYY,J60/1:30,300");
    REQUIRE(julian.has_value());
    CHECK(julian->start.kind == wc::RuleKind::kJulian1);
    CHECK(julian->start.day == 60);
    CHECK(julian->start.time_s == 5400);
    CHECK(julian->end.kind == wc::RuleKind::kJulian0);
    CHECK(julian->end.day == 300);

    // What parse_time_zone refuses, parse_zone_rules refuses, and it answers the same for what it takes.
    for (const char* text : {"", "AEST", "AEST-10AEDT", "AEST-10AEDT,M13.1.0,M4.1.0", "AEST-10AEDT,M10.1.0,M4.1.0,M1.1.1"}) {
        CHECK(wc::parse_zone_rules(text).has_value() == wc::parse_time_zone(text).has_value());
    }
}

TEST_CASE("wall_clock: the day a rule falls on, in a leap year and not", "[wall_clock]") {
    const auto day_of = [](const char* rule_text, int year) {
        const auto rules = wc::parse_zone_rules(rule_text);
        REQUIRE(rules.has_value());
        return wc::rule_day(rules->start, year);
    };
    // The first Sunday of October 2026 is the 4th; the last Sunday of March 2026 is the 29th, and in 2027 the 28th.
    CHECK(day_of("AEST-10AEDT,M10.1.0,M4.1.0", 2026) == wc::days_from_civil(2026, 10, 4));
    CHECK(day_of("AEST-10AEDT,M10.1.0,M4.1.0", 2027) == wc::days_from_civil(2027, 10, 3));
    CHECK(day_of("CET-1CEST,M3.5.0,M10.5.0", 2026) == wc::days_from_civil(2026, 3, 29));
    CHECK(day_of("CET-1CEST,M3.5.0,M10.5.0", 2027) == wc::days_from_civil(2027, 3, 28));
    // Week 5 is the last such day even when the month has only four: the last Sunday of February 2026 is the 22nd.
    CHECK(day_of("CET-1CEST,M2.5.0,M10.5.0", 2026) == wc::days_from_civil(2026, 2, 22));
    // ... and in a leap year the fifth Sunday of February 2032 exists (the 29th).
    CHECK(day_of("CET-1CEST,M2.5.0,M10.5.0", 2032) == wc::days_from_civil(2032, 2, 29));
    // Jn skips February 29 and n counts it: day 60 is March 1 in a common year, and in a leap year J60 is March 1
    // while 59 is February 29.
    CHECK(day_of("XXX1YYY,J60,300", 2026) == wc::days_from_civil(2026, 3, 1));
    CHECK(day_of("XXX1YYY,J60,300", 2028) == wc::days_from_civil(2028, 3, 1));
    CHECK(day_of("XXX1YYY,59,300", 2028) == wc::days_from_civil(2028, 2, 29));
    CHECK(day_of("XXX1YYY,59,300", 2026) == wc::days_from_civil(2026, 3, 1));
    CHECK(wc::is_leap_year(2028));
    CHECK_FALSE(wc::is_leap_year(2100));
    CHECK(wc::is_leap_year(2000));
    CHECK(wc::days_in_month(2028, 2) == 29);
    CHECK(wc::days_in_month(2026, 2) == 28);
    // 1970-01-01 was a Thursday and 2026-10-25 a Sunday.
    CHECK(wc::weekday_of_days(0) == 4);
    CHECK(wc::weekday_of_days(wc::days_from_civil(2026, 10, 25)) == 0);
    CHECK(wc::weekday_of_days(-1) == 3);
}

TEST_CASE("wall_clock: a zone's offset follows its rules across the year, in either hemisphere", "[wall_clock]") {
    const auto at = [](int year, int month, int day, int hour, int minute = 0, int second = 0) {
        return wc::days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
    };
    const auto sydney = *wc::parse_zone_rules("AEST-10AEDT,M10.1.0,M4.1.0/3");
    // Southern: daylight time runs across New Year.
    CHECK(wc::zone_offset_at(sydney, at(2026, 1, 15, 12)) == 11 * 3600);
    CHECK(wc::zone_offset_at(sydney, at(2026, 7, 15, 12)) == 10 * 3600);
    CHECK(wc::zone_offset_at(sydney, at(2026, 12, 31, 23, 59, 59)) == 11 * 3600);
    CHECK(wc::zone_offset_at(sydney, at(2026, 1, 1, 0)) == 11 * 3600);
    const auto new_york = *wc::parse_zone_rules("EST5EDT,M3.2.0,M11.1.0");
    // Northern: the other way about.
    CHECK(wc::zone_offset_at(new_york, at(2026, 1, 15, 12)) == -5 * 3600);
    CHECK(wc::zone_offset_at(new_york, at(2026, 7, 15, 12)) == -4 * 3600);
    // A zone with no daylight saving has one offset.
    const auto tokyo = *wc::parse_zone_rules("JST-9");
    CHECK(wc::zone_offset_at(tokyo, at(2026, 1, 15, 12)) == 9 * 3600);
    CHECK(wc::zone_offset_at(tokyo, at(2026, 7, 15, 12)) == 9 * 3600);
    // The probe instants the board checks its C library with are in different halves for any zone with daylight time.
    CHECK(wc::zone_offset_at(sydney, wc::kProbeJanuary) != wc::zone_offset_at(sydney, wc::kProbeJuly));
    CHECK(wc::zone_offset_at(new_york, wc::kProbeJanuary) != wc::zone_offset_at(new_york, wc::kProbeJuly));
    // And the wall time is the offset applied: 12:00 UTC in Sydney in January is 23:00.
    const wc::LocalTime local = wc::zone_local_time(sydney, at(2026, 1, 15, 12));
    CHECK(local.hour == 23);
    CHECK(local.day == 15);
}

TEST_CASE("wall_clock: the clock format is 24 or 12 hours, with or without seconds, and stored as one number", "[wall_clock]") {
    CHECK(wc::parse_clock_format("24") == wc::ClockFormat{});
    CHECK(wc::parse_clock_format("12") == wc::ClockFormat{.twelve_hour = true});
    CHECK(wc::parse_clock_format("24 seconds") == wc::ClockFormat{.seconds = true});
    CHECK(wc::parse_clock_format("12 seconds\n") == wc::ClockFormat{.twelve_hour = true, .seconds = true});
    CHECK(wc::parse_clock_format(" 12\r\n") == wc::ClockFormat{.twelve_hour = true});
    for (const char* bad : {"", "13", "2", "24h", "12 second", "12  seconds", "seconds", "24 seconds now", "twelve", "12seconds",
                            "12 Seconds", "240"}) {
        CHECK_FALSE(wc::parse_clock_format(bad).has_value());
    }
    // It reads back as the words it was set with.
    for (const bool twelve : {false, true}) {
        for (const bool seconds : {false, true}) {
            const wc::ClockFormat format{.twelve_hour = twelve, .seconds = seconds};
            CHECK(wc::parse_clock_format(wc::clock_format_text(format)) == format);
            CHECK(wc::clock_format_from_stored(wc::clock_format_stored(format)) == format);
        }
    }
    CHECK(wc::clock_format_text({}) == "24");
    CHECK(wc::clock_format_text({.twelve_hour = true, .seconds = true}) == "12 seconds");
    // The stored numbers are these, never renumbered; the default is 0, and a number nobody knows is refused.
    CHECK(wc::clock_format_stored({}) == 0);
    CHECK(wc::clock_format_stored({.twelve_hour = true}) == 1);
    CHECK(wc::clock_format_stored({.seconds = true}) == 2);
    CHECK(wc::clock_format_stored({.twelve_hour = true, .seconds = true}) == 3);
    CHECK_FALSE(wc::clock_format_from_stored(-1).has_value());
    CHECK_FALSE(wc::clock_format_from_stored(4).has_value());
}

TEST_CASE("wall_clock: the face writes the time as the format says, in 24 hours or 12", "[wall_clock]") {
    wc::Reading reading;
    reading.known = true;
    reading.zone_set = true;
    const auto face_at = [&reading](int hour, int minute, int second, wc::ClockFormat format) {
        reading.local = wc::LocalTime{2026, 10, 20, hour, minute, second, 2};
        reading.format = format;
        return wc::face(reading);
    };
    const wc::ClockFormat twelve{.twelve_hour = true};
    const wc::ClockFormat with_seconds{.seconds = true};
    const wc::ClockFormat twelve_seconds{.twelve_hour = true, .seconds = true};

    // The default is what it always was: 24 hours, no seconds, nothing beside it.
    wc::Face face = face_at(21, 47, 12, {});
    CHECK(face.time == "21:47");
    CHECK(face.suffix.empty());
    // 12 hours has no leading zero and no hour 0.
    face = face_at(21, 47, 12, twelve);
    CHECK(face.time == "9:47");
    CHECK(face.suffix == "PM");
    CHECK(face_at(9, 5, 0, twelve).time == "9:05");
    CHECK(face_at(9, 5, 0, twelve).suffix == "AM");
    CHECK(face_at(0, 5, 0, twelve).time == "12:05");  // midnight is 12 AM
    CHECK(face_at(0, 5, 0, twelve).suffix == "AM");
    CHECK(face_at(12, 0, 0, twelve).time == "12:00");  // noon is 12 PM
    CHECK(face_at(12, 0, 0, twelve).suffix == "PM");
    CHECK(face_at(11, 59, 59, twelve).time == "11:59");
    CHECK(face_at(11, 59, 59, twelve).suffix == "AM");
    CHECK(face_at(13, 0, 0, twelve).time == "1:00");
    CHECK(face_at(23, 59, 0, twelve).time == "11:59");
    CHECK(face_at(23, 59, 0, twelve).suffix == "PM");
    // Seconds go small beside the time: the minute's rectangle does not change with them.
    face = face_at(21, 47, 5, with_seconds);
    CHECK(face.time == "21:47");
    CHECK(face.suffix == ":05");
    face = face_at(21, 47, 59, twelve_seconds);
    CHECK(face.time == "9:47");
    CHECK(face.suffix == ":59 PM");
    CHECK(face_at(0, 0, 0, twelve_seconds).time == "12:00");
    CHECK(face_at(0, 0, 0, twelve_seconds).suffix == ":00 AM");
    // The date and the note do not depend on the format; and with no time known nothing is said, in either.
    CHECK(face_at(21, 47, 12, twelve).date == face_at(21, 47, 12, {}).date);
    reading.known = false;
    reading.format = twelve_seconds;
    CHECK(wc::face(reading) == wc::Face{});
    // Every hour of the day reads back to the hour it was: 12-hour text and the AM or PM say the same time as 24.
    reading.known = true;
    for (int hour = 0; hour < 24; ++hour) {
        const wc::Face twelve_face = face_at(hour, 30, 0, twelve);
        const int shown = std::stoi(twelve_face.time);
        CHECK(shown >= 1);
        CHECK(shown <= 12);
        CHECK((shown % 12) + (twelve_face.suffix == "PM" ? 12 : 0) == hour);
    }
}

TEST_CASE("wall_clock: every weekday and month has its name", "[wall_clock]") {
    wc::Reading reading;
    reading.known = true;
    reading.zone_set = true;
    const char* days[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    for (int weekday = 0; weekday < 7; ++weekday) {
        reading.local = wc::LocalTime{2026, 10, 18 + weekday, 9, 0, 0, weekday};
        CHECK(wc::face(reading).date.rfind(days[weekday], 0) == 0);
    }
    const char* months[12] = {"January", "February", "March", "April", "May", "June", "July",
                              "August", "September", "October", "November", "December"};
    for (int month = 1; month <= 12; ++month) {
        reading.local = wc::LocalTime{2026, month, 15, 9, 0, 0, 4};
        const std::string date = wc::face(reading).date;
        CHECK(date.size() >= std::string(months[month - 1]).size());
        CHECK(date.substr(date.size() - std::string(months[month - 1]).size()) == months[month - 1]);
    }
    // A broken-down time that is out of range is a name and not a crash.
    reading.local = wc::LocalTime{2026, 13, 15, 9, 0, 0, 9};
    CHECK_FALSE(wc::face(reading).date.empty());
}

TEST_CASE("wall_clock: a time server is a host name or an IPv4 address", "[wall_clock]") {
    CHECK(wc::valid_ntp_server("pool.ntp.org"));
    CHECK(wc::valid_ntp_server("time.cloudflare.com"));
    CHECK(wc::valid_ntp_server("0.au.pool.ntp.org"));
    CHECK(wc::valid_ntp_server("ntp"));
    CHECK(wc::valid_ntp_server("my-router.lan"));
    CHECK(wc::valid_ntp_server("NTP1.Example.COM"));
    CHECK(wc::valid_ntp_server("192.168.1.5"));
    CHECK(wc::valid_ntp_server("10.0.0.0"));
    CHECK(wc::valid_ntp_server("255.255.255.255"));
    CHECK(wc::valid_ntp_server("a1.b2"));
    // Nothing, or too much of it.
    CHECK_FALSE(wc::valid_ntp_server(""));
    CHECK_FALSE(wc::valid_ntp_server(std::string(wc::kNtpServerMaxBytes + 1, 'a')));
    CHECK(wc::valid_ntp_server(std::string(wc::kNtpServerMaxBytes, 'a')));
    CHECK_FALSE(wc::valid_ntp_server("a." + std::string(64, 'b')));
    // An address that is not one: a part out of range, the wrong number of parts, a leading zero (which a
    // resolver reads as octal), a part with a sign or a space in it.
    CHECK_FALSE(wc::valid_ntp_server("256.1.1.1"));
    CHECK_FALSE(wc::valid_ntp_server("1.2.3"));
    CHECK_FALSE(wc::valid_ntp_server("1.2.3.4.5"));
    CHECK_FALSE(wc::valid_ntp_server("192.168.01.5"));
    CHECK_FALSE(wc::valid_ntp_server("192.168.1."));
    CHECK_FALSE(wc::valid_ntp_server(".192.168.1.5"));
    CHECK_FALSE(wc::valid_ntp_server("1..2.3"));
    CHECK_FALSE(wc::valid_ntp_server("1000.1.1.1"));
    CHECK_FALSE(wc::valid_ntp_server("192.168.1.-5"));
    // A name that is not one.
    CHECK_FALSE(wc::valid_ntp_server("-pool.ntp.org"));
    CHECK_FALSE(wc::valid_ntp_server("pool-.ntp.org"));
    CHECK_FALSE(wc::valid_ntp_server("pool..ntp.org"));
    CHECK_FALSE(wc::valid_ntp_server("pool.ntp.org."));
    CHECK_FALSE(wc::valid_ntp_server("pool.ntp.org/x"));
    CHECK_FALSE(wc::valid_ntp_server("pool ntp org"));
    CHECK_FALSE(wc::valid_ntp_server("pool_ntp.org"));
    CHECK_FALSE(wc::valid_ntp_server("http://pool.ntp.org"));
    CHECK_FALSE(wc::valid_ntp_server("pool.ntp.org:123"));
    CHECK_FALSE(wc::valid_ntp_server("[::1]"));
    CHECK_FALSE(wc::valid_ntp_server("ntp.123"));
    CHECK_FALSE(wc::valid_ntp_server("p\xC3\xB6ol.ntp.org"));
    CHECK_FALSE(wc::valid_ntp_server("pool.ntp.org\n"));
}

TEST_CASE("wall_clock: PUT /ntp-server takes a server or `none`, and GET reads it back", "[wall_clock]") {
    CHECK(wc::parse_ntp_server_setting("192.168.1.5") == std::string("192.168.1.5"));
    CHECK(wc::parse_ntp_server_setting("  pool.ntp.org\r\n") == std::string("pool.ntp.org"));
    const auto none = wc::parse_ntp_server_setting("none\n");
    REQUIRE(none.has_value());
    CHECK(none->empty());
    CHECK_FALSE(wc::parse_ntp_server_setting("").has_value());
    CHECK_FALSE(wc::parse_ntp_server_setting("None").has_value());
    CHECK_FALSE(wc::parse_ntp_server_setting("NONE").has_value());
    CHECK(wc::valid_ntp_server("nonesuch"));
    CHECK_FALSE(wc::parse_ntp_server_setting("pool.ntp.org:123").has_value());
    CHECK(wc::ntp_server_setting_text("") == "none");
    CHECK(wc::ntp_server_setting_text("192.168.1.5") == "192.168.1.5");
    for (const char* server : {"", "pool.ntp.org", "192.168.1.5"}) {
        const auto parsed = wc::parse_ntp_server_setting(wc::ntp_server_setting_text(server));
        REQUIRE(parsed.has_value());
        CHECK(*parsed == std::string(server));
    }
}

TEST_CASE("wall_clock: PUT /time-sync takes on or off", "[wall_clock]") {
    CHECK(wc::parse_time_sync("on") == true);
    CHECK(wc::parse_time_sync("off\n") == false);
    CHECK(wc::parse_time_sync(" on \r\n") == true);
    CHECK_FALSE(wc::parse_time_sync("").has_value());
    CHECK_FALSE(wc::parse_time_sync("On").has_value());
    CHECK_FALSE(wc::parse_time_sync("1").has_value());
    CHECK_FALSE(wc::parse_time_sync("true").has_value());
    CHECK(std::string(wc::time_sync_text(true)) == "on");
    CHECK(std::string(wc::time_sync_text(false)) == "off");
    for (const bool on : {true, false}) {
        CHECK(wc::parse_time_sync(wc::time_sync_text(on)) == on);
    }
}

TEST_CASE("wall_clock: the time as GET /status writes it is to the second, with the zeros a date has", "[wall_clock]") {
    CHECK(wc::local_text(wc::LocalTime{2026, 10, 21, 8, 9, 44, 3}) == "2026-10-21 08:09:44");
    CHECK(wc::local_text(wc::LocalTime{2026, 1, 5, 0, 0, 0, 1}) == "2026-01-05 00:00:00");
    CHECK(wc::local_text(wc::LocalTime{2027, 12, 31, 23, 59, 59, 5}) == "2027-12-31 23:59:59");
    CHECK(wc::local_text(wc::local_from_unix(wc::kProbeJanuary, 11 * 3600)) == "2026-01-15 23:00:00");
}

TEST_CASE("wall_clock: the minute of the day is nothing until the time is known", "[wall_clock]") {
    wc::Reading reading;
    CHECK_FALSE(wc::minute_of_day(reading).has_value());
    reading.known = true;
    reading.local = wc::LocalTime{2026, 10, 25, 0, 0, 0, 0};
    CHECK(wc::minute_of_day(reading) == 0);
    reading.local = wc::LocalTime{2026, 10, 25, 7, 5, 59, 0};
    CHECK(wc::minute_of_day(reading) == 7 * 60 + 5);
    reading.local = wc::LocalTime{2026, 10, 25, 23, 59, 59, 0};
    CHECK(wc::minute_of_day(reading) == 23 * 60 + 59);
}
