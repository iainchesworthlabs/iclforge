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
