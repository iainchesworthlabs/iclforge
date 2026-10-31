// The table of time zones a board offers by name (iclforge/time_zones.hpp, time_zone_data.hpp), on the host.
//
// What is held here is that every string in the table is one the board takes (parse_time_zone), that Australia is
// complete, that Melbourne, the owner's zone, changes over on the real days, and that the night schedule, which
// is kept by the wall clock, is dark for the right stretch of real time across both of its changeovers.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "iclforge/now_view.hpp"
#include "iclforge/time_zones.hpp"
#include "iclforge/wall_clock.hpp"

namespace wc = iclforge::wallclock;
namespace nv = iclforge::nowview;

namespace {

// 2026-10-03 16:00:00 UTC as utc(2026, 10, 3, 16): readable where a count of seconds would not be.
constexpr std::int64_t utc(int year, int month, int day, int hour = 0, int minute = 0, int second = 0) {
    return wc::days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
}

wc::ZoneRules rules_of(std::string_view name) {
    const auto zone = wc::find_time_zone(name);
    REQUIRE(zone.has_value());
    const auto rules = wc::parse_zone_rules(zone->posix);
    REQUIRE(rules.has_value());
    return *rules;
}

int minute_of_day(const wc::ZoneRules& zone, std::int64_t unix_s) {
    const wc::LocalTime local = wc::zone_local_time(zone, unix_s);
    return local.hour * 60 + local.minute;
}

}  // namespace

TEST_CASE("time_zones: the table is sorted, unique, and every string in it is a zone the board takes", "[time_zones]") {
    REQUIRE(wc::time_zone_count() > 400);
    CHECK(wc::time_zone_count() == 420);
    std::set<std::string> names;
    std::string_view previous;
    for (std::size_t i = 0; i < wc::time_zone_count(); ++i) {
        const wc::NamedZone zone = wc::time_zone_at(i);
        INFO(zone.name << " " << zone.posix);
        CHECK(zone.name > previous);  // sorted, so the binary search finds it and no name is twice
        previous = zone.name;
        names.insert(std::string(zone.name));
        // The board's own reader takes it, as PUT /timezone would, and NVS has room for it.
        CHECK(wc::parse_time_zone(zone.posix).has_value());
        CHECK(wc::parse_zone_rules(zone.posix).has_value());
        CHECK(wc::valid_time_zone(zone.posix));
        CHECK(zone.posix.size() <= wc::kTimeZoneMaxBytes);
        CHECK(zone.name.size() <= wc::kTimeZoneNameMaxBytes);
        // A name is letters, digits and the three marks a tzdata name has, and a body PUT /timezone reads as a name
        // and never as a POSIX string: no digit-led offset follows its first word.
        for (const char c : zone.name) {
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' ||
                            c == '_' || c == '-' || c == '+';
            CHECK(ok);
        }
        CHECK_FALSE(wc::valid_time_zone(zone.name));
        // A zone with daylight saving gives its rules (parse_time_zone refuses one that does not).
        if (const auto offsets = wc::parse_time_zone(zone.posix); offsets && offsets->has_dst) {
            CHECK(zone.posix.find(',') != std::string_view::npos);
        }
    }
    CHECK(names.size() == wc::time_zone_count());
    CHECK(std::string(wc::data::kTzdataVersion).size() >= 5);
}

TEST_CASE("time_zones: a zone is found by its exact name and by no other spelling", "[time_zones]") {
    const auto melbourne = wc::find_time_zone("Australia/Melbourne");
    REQUIRE(melbourne.has_value());
    CHECK(melbourne->name == "Australia/Melbourne");
    CHECK(melbourne->posix == "AEST-10AEDT,M10.1.0,M4.1.0/3");
    CHECK(wc::find_time_zone("America/New_York")->posix == "EST5EDT,M3.2.0,M11.1.0");
    CHECK(wc::find_time_zone("Europe/London")->posix == "GMT0BST,M3.5.0/1,M10.5.0");
    CHECK(wc::find_time_zone("UTC")->posix == "UTC0");
    CHECK_FALSE(wc::find_time_zone("australia/melbourne").has_value());
    CHECK_FALSE(wc::find_time_zone("Australia/Melbourn").has_value());
    CHECK_FALSE(wc::find_time_zone("Australia/Melbourne ").has_value());
    CHECK_FALSE(wc::find_time_zone("Melbourne").has_value());
    CHECK_FALSE(wc::find_time_zone("").has_value());
    CHECK_FALSE(wc::find_time_zone("none").has_value());
    // Every name in the table is found, and by binary search at that.
    for (std::size_t i = 0; i < wc::time_zone_count(); ++i) {
        const wc::NamedZone zone = wc::time_zone_at(i);
        const auto found = wc::find_time_zone(zone.name);
        REQUIRE(found.has_value());
        CHECK(*found == zone);
    }
}

TEST_CASE("time_zones: Australia is complete", "[time_zones]") {
    // Every zone tzdata has for Australia, and the capital that is a link to Sydney.
    const char* const expected[] = {"Australia/Adelaide", "Australia/Brisbane", "Australia/Broken_Hill",
                                    "Australia/Canberra", "Australia/Darwin",   "Australia/Eucla",
                                    "Australia/Hobart",   "Australia/Lindeman", "Australia/Lord_Howe",
                                    "Australia/Melbourne", "Australia/Perth",   "Australia/Sydney"};
    for (const char* name : expected) {
        INFO(name);
        CHECK(wc::find_time_zone(name).has_value());
    }
    // ... and only those are in the region.
    std::size_t in_region = 0;
    for (std::size_t i = 0; i < wc::time_zone_count(); ++i) {
        in_region += wc::zone_region(wc::time_zone_at(i).name) == "Australia" ? 1U : 0U;
    }
    CHECK(in_region == std::size(expected));
    // The ones that share a rule share the string, and the ones that do not have their own.
    const std::string_view sydney = wc::find_time_zone("Australia/Sydney")->posix;
    CHECK(wc::find_time_zone("Australia/Canberra")->posix == sydney);
    CHECK(wc::find_time_zone("Australia/Melbourne")->posix == sydney);
    CHECK(wc::find_time_zone("Australia/Hobart")->posix == sydney);
    CHECK(wc::find_time_zone("Australia/Brisbane")->posix == "AEST-10");
    CHECK(wc::find_time_zone("Australia/Perth")->posix == "AWST-8");
    CHECK(wc::find_time_zone("Australia/Darwin")->posix == "ACST-9:30");
    CHECK(wc::find_time_zone("Australia/Adelaide")->posix == "ACST-9:30ACDT,M10.1.0,M4.1.0/3");
    CHECK(wc::find_time_zone("Australia/Eucla")->posix == "<+0845>-8:45");
    CHECK(wc::find_time_zone("Australia/Lord_Howe")->posix == "<+1030>-10:30<+11>-11,M10.1.0,M4.1.0");
    // Lord Howe's daylight saving is half an hour, and the parser says so.
    const auto lord_howe = wc::parse_time_zone(wc::find_time_zone("Australia/Lord_Howe")->posix);
    REQUIRE(lord_howe.has_value());
    CHECK(lord_howe->std_east_s == 10 * 3600 + 1800);
    CHECK(lord_howe->dst_east_s == 11 * 3600);
}

TEST_CASE("time_zones: Melbourne changes over on the first Sunday of October and the first of April", "[time_zones]") {
    const wc::ZoneRules melbourne = rules_of("Australia/Melbourne");
    constexpr int kAest = 10 * 3600;
    constexpr int kAedt = 11 * 3600;

    // 2026: clocks go forward at 02:00 AEST on Sunday 4 October (16:00 UTC on Saturday the 3rd) ...
    CHECK(wc::zone_offset_at(melbourne, utc(2026, 10, 3, 15, 59, 59)) == kAest);
    CHECK(wc::zone_offset_at(melbourne, utc(2026, 10, 3, 16, 0, 0)) == kAedt);
    wc::LocalTime local = wc::zone_local_time(melbourne, utc(2026, 10, 3, 15, 59, 59));
    CHECK(local == wc::LocalTime{2026, 10, 4, 1, 59, 59, 0});
    local = wc::zone_local_time(melbourne, utc(2026, 10, 3, 16, 0, 0));
    CHECK(local == wc::LocalTime{2026, 10, 4, 3, 0, 0, 0});  // 02:00 never happens
    // ... and back at 03:00 AEDT on Sunday 5 April (16:00 UTC on Saturday the 4th), so 02:00 to 02:59 happens twice.
    CHECK(wc::zone_offset_at(melbourne, utc(2026, 4, 4, 15, 59, 59)) == kAedt);
    CHECK(wc::zone_offset_at(melbourne, utc(2026, 4, 4, 16, 0, 0)) == kAest);
    local = wc::zone_local_time(melbourne, utc(2026, 4, 4, 15, 59, 59));
    CHECK(local == wc::LocalTime{2026, 4, 5, 2, 59, 59, 0});
    local = wc::zone_local_time(melbourne, utc(2026, 4, 4, 16, 0, 0));
    CHECK(local == wc::LocalTime{2026, 4, 5, 2, 0, 0, 0});  // the clock has gone back an hour

    // 2027: the 3rd of October and the 4th of April, a Sunday each.
    CHECK(wc::zone_offset_at(melbourne, utc(2027, 10, 2, 15, 59, 59)) == kAest);
    CHECK(wc::zone_offset_at(melbourne, utc(2027, 10, 2, 16, 0, 0)) == kAedt);
    CHECK(wc::zone_offset_at(melbourne, utc(2027, 4, 3, 15, 59, 59)) == kAedt);
    CHECK(wc::zone_offset_at(melbourne, utc(2027, 4, 3, 16, 0, 0)) == kAest);
    // 2028, a leap year: the 1st of October and the 2nd of April.
    CHECK(wc::zone_offset_at(melbourne, utc(2028, 9, 30, 15, 59, 59)) == kAest);
    CHECK(wc::zone_offset_at(melbourne, utc(2028, 9, 30, 16, 0, 0)) == kAedt);
    CHECK(wc::zone_offset_at(melbourne, utc(2028, 4, 1, 15, 59, 59)) == kAedt);
    CHECK(wc::zone_offset_at(melbourne, utc(2028, 4, 1, 16, 0, 0)) == kAest);

    // Summer and winter, and the New Year that daylight time runs across.
    CHECK(wc::zone_offset_at(melbourne, wc::kProbeJanuary) == kAedt);
    CHECK(wc::zone_offset_at(melbourne, wc::kProbeJuly) == kAest);
    CHECK(wc::zone_offset_at(melbourne, utc(2026, 12, 31, 13, 0, 0)) == kAedt);
    CHECK(wc::zone_local_time(melbourne, utc(2026, 12, 31, 13, 0, 0)) == wc::LocalTime{2027, 1, 1, 0, 0, 0, 5});
    // A zone that changes over on the same Sundays, by name, agrees to the second.
    const wc::ZoneRules sydney = rules_of("Australia/Sydney");
    for (const std::int64_t t : {utc(2026, 10, 3, 15, 59, 59), utc(2026, 10, 3, 16), utc(2026, 4, 4, 15, 59, 59), utc(2026, 4, 4, 16)}) {
        CHECK(wc::zone_offset_at(sydney, t) == wc::zone_offset_at(melbourne, t));
    }
}

TEST_CASE("time_zones: the real changeovers of 16 zones, from tzdata, in 2027 and 2028", "[time_zones]") {
    // {zone, the first second of the new offset, the offset before it, the offset from it}, seconds east of UTC; made
    // by zoneinfo from the same tzdata release the table is (tools/hearth/gen_time_zones.py reads the same files).
    struct Change {
        const char* zone;
        std::int64_t at;
        int before;
        int after;
    };
    const Change changes[] = {
        {"Australia/Melbourne", 1806768000, 39600, 36000}, {"Australia/Melbourne", 1822492800, 36000, 39600},
        {"Australia/Melbourne", 1838217600, 39600, 36000}, {"Australia/Melbourne", 1853942400, 36000, 39600},
        {"Australia/Sydney", 1806768000, 39600, 36000},    {"Australia/Sydney", 1822492800, 36000, 39600},
        {"Australia/Sydney", 1838217600, 39600, 36000},    {"Australia/Sydney", 1853942400, 36000, 39600},
        {"Australia/Adelaide", 1806769800, 37800, 34200},  {"Australia/Adelaide", 1822494600, 34200, 37800},
        {"Australia/Adelaide", 1838219400, 37800, 34200},  {"Australia/Adelaide", 1853944200, 34200, 37800},
        {"Australia/Lord_Howe", 1806764400, 39600, 37800}, {"Australia/Lord_Howe", 1822491000, 37800, 39600},
        {"Australia/Lord_Howe", 1838214000, 39600, 37800}, {"Australia/Lord_Howe", 1853940600, 37800, 39600},
        {"Australia/Hobart", 1806768000, 39600, 36000},    {"Australia/Hobart", 1822492800, 36000, 39600},
        {"Australia/Hobart", 1838217600, 39600, 36000},    {"Australia/Hobart", 1853942400, 36000, 39600},
        {"America/New_York", 1805007600, -18000, -14400},  {"America/New_York", 1825567200, -14400, -18000},
        {"America/New_York", 1836457200, -18000, -14400},  {"America/New_York", 1857016800, -14400, -18000},
        {"America/Los_Angeles", 1805018400, -28800, -25200}, {"America/Los_Angeles", 1825578000, -25200, -28800},
        {"America/Los_Angeles", 1836468000, -28800, -25200}, {"America/Los_Angeles", 1857027600, -25200, -28800},
        {"Europe/London", 1806195600, 0, 3600},            {"Europe/London", 1824944400, 3600, 0},
        {"Europe/London", 1837645200, 0, 3600},            {"Europe/London", 1856394000, 3600, 0},
        {"Europe/Dublin", 1806195600, 0, 3600},            {"Europe/Dublin", 1824944400, 3600, 0},
        {"Europe/Dublin", 1837645200, 0, 3600},            {"Europe/Dublin", 1856394000, 3600, 0},
        {"Europe/Berlin", 1806195600, 3600, 7200},         {"Europe/Berlin", 1824944400, 7200, 3600},
        {"Europe/Berlin", 1837645200, 3600, 7200},         {"Europe/Berlin", 1856394000, 7200, 3600},
        {"America/Santiago", 1806807600, -10800, -14400},  {"America/Santiago", 1820116800, -14400, -10800},
        {"America/Santiago", 1838257200, -10800, -14400},  {"America/Santiago", 1851566400, -14400, -10800},
        {"America/Nuuk", 1806195600, -7200, -3600},        {"America/Nuuk", 1824944400, -3600, -7200},
        {"America/Nuuk", 1837645200, -7200, -3600},        {"America/Nuuk", 1856394000, -3600, -7200},
        {"Africa/Cairo", 1809036000, 7200, 10800},         {"Africa/Cairo", 1824757200, 10800, 7200},
        {"Africa/Cairo", 1840485600, 7200, 10800},         {"Africa/Cairo", 1856206800, 10800, 7200},
        {"Pacific/Auckland", 1806760800, 46800, 43200},    {"Pacific/Auckland", 1821880800, 43200, 46800},
        {"Pacific/Auckland", 1838210400, 46800, 43200},    {"Pacific/Auckland", 1853330400, 43200, 46800},
        {"Antarctica/Troll", 1806195600, 0, 7200},         {"Antarctica/Troll", 1824944400, 7200, 0},
        {"Antarctica/Troll", 1837645200, 0, 7200},         {"Antarctica/Troll", 1856394000, 7200, 0},
        {"Asia/Gaza", 1806105600, 7200, 10800},            {"Asia/Gaza", 1824850800, 10800, 7200},
        {"Asia/Gaza", 1837555200, 7200, 10800},            {"Asia/Gaza", 1856300400, 10800, 7200},
    };
    for (const Change& change : changes) {
        INFO(change.zone << " at " << change.at);
        const wc::ZoneRules rules = rules_of(change.zone);
        CHECK(wc::zone_offset_at(rules, change.at - 1) == change.before);
        CHECK(wc::zone_offset_at(rules, change.at) == change.after);
        // Half a day either side is the same offset: nothing else changes in between.
        CHECK(wc::zone_offset_at(rules, change.at - 12 * 3600) == change.before);
        CHECK(wc::zone_offset_at(rules, change.at + 12 * 3600) == change.after);
    }
    // Zones with no daylight saving never change.
    for (const char* name : {"Australia/Perth", "Australia/Brisbane", "Australia/Darwin", "Asia/Tokyo", "Asia/Kolkata",
                             "Asia/Kathmandu", "Asia/Tehran", "Asia/Singapore", "UTC"}) {
        const wc::ZoneRules rules = rules_of(name);
        CHECK_FALSE(rules.offsets.has_dst);
        CHECK(wc::zone_offset_at(rules, utc(2027, 1, 15, 12)) == wc::zone_offset_at(rules, utc(2027, 7, 15, 12)));
    }
    CHECK(rules_of("Asia/Kolkata").offsets.std_east_s == 5 * 3600 + 1800);
    CHECK(rules_of("Asia/Kathmandu").offsets.std_east_s == 5 * 3600 + 2700);
    CHECK(rules_of("Australia/Perth").offsets.std_east_s == 8 * 3600);
}

TEST_CASE("time_zones: the night schedule keeps wall-clock time through Melbourne's changeovers", "[time_zones]") {
    const wc::ZoneRules melbourne = rules_of("Australia/Melbourne");
    nv::NightSchedule night;
    night.enabled = true;  // 22:00 to 07:00
    const auto dark_at = [&](std::int64_t t) { return nv::night_active(night, minute_of_day(melbourne, t)); };

    // Clocks forward, the night of Saturday 3 October 2026: 22:00 AEST (12:00 UTC) to 07:00 AEDT (20:00 UTC) is
    // eight hours of dark and not nine, because the clocks skip the hour from 02:00 to 03:00.
    CHECK_FALSE(dark_at(utc(2026, 10, 3, 11, 59)));
    CHECK(dark_at(utc(2026, 10, 3, 12, 0)));
    CHECK(dark_at(utc(2026, 10, 3, 15, 59)));  // 01:59 AEST
    CHECK(dark_at(utc(2026, 10, 3, 16, 0)));   // 03:00 AEDT: no gap in the dark at the jump
    CHECK(dark_at(utc(2026, 10, 3, 19, 59)));  // 06:59 AEDT
    CHECK_FALSE(dark_at(utc(2026, 10, 3, 20, 0)));  // 07:00 AEDT
    CHECK_FALSE(dark_at(utc(2026, 10, 4, 10, 59)));  // 21:59 AEDT
    CHECK(dark_at(utc(2026, 10, 4, 11, 0)));         // 22:00 AEDT: the next night, from the same wall time

    // Clocks back, the night of Saturday 4 April 2026: 22:00 AEDT (11:00 UTC) to 07:00 AEST (21:00 UTC) is ten hours,
    // the hour from 02:00 to 03:00 being lived twice.
    CHECK_FALSE(dark_at(utc(2026, 4, 4, 10, 59)));
    CHECK(dark_at(utc(2026, 4, 4, 11, 0)));
    CHECK(dark_at(utc(2026, 4, 4, 15, 59)));  // 02:59 AEDT
    CHECK(dark_at(utc(2026, 4, 4, 16, 0)));   // 02:00 AEST, again
    CHECK(dark_at(utc(2026, 4, 4, 20, 59)));  // 06:59 AEST
    CHECK_FALSE(dark_at(utc(2026, 4, 4, 21, 0)));  // 07:00 AEST

    // Every minute of the two days either side of each changeover is dark exactly when its wall time is in 22:00-07:00.
    for (const std::int64_t first : {utc(2026, 10, 2), utc(2026, 4, 3), utc(2027, 10, 1), utc(2027, 4, 2)}) {
        std::int64_t dark = 0;
        for (std::int64_t t = first; t < first + 4 * 86400; t += 60) {
            const int minute = minute_of_day(melbourne, t);
            const bool wall_night = minute >= 22 * 60 || minute < 7 * 60;
            CHECK(dark_at(t) == wall_night);
            dark += dark_at(t) ? 1 : 0;
        }
        // Three nights of nine hours and the changeover night, which is eight real hours or ten: some, not all.
        CHECK(dark > 0);
        CHECK(dark < 4 * 24 * 60);
    }

    // A window that starts in the hour that does not exist begins at the jump, and one that starts in the hour
    // lived twice is dark in both passes: a schedule is kept by the wall clock, and says so.
    nv::NightSchedule small_hours;
    small_hours.enabled = true;
    small_hours.from_min = 2 * 60 + 30;
    small_hours.to_min = 4 * 60;
    const auto small_at = [&](std::int64_t t) { return nv::night_active(small_hours, minute_of_day(melbourne, t)); };
    CHECK_FALSE(small_at(utc(2026, 10, 3, 15, 59)));  // 01:59 AEST
    CHECK(small_at(utc(2026, 10, 3, 16, 0)));         // 03:00 AEDT, the first minute that exists after 02:30
    CHECK(small_at(utc(2026, 10, 3, 16, 59)));
    CHECK_FALSE(small_at(utc(2026, 10, 3, 17, 0)));   // 04:00 AEDT
    CHECK(small_at(utc(2026, 4, 4, 15, 30)));         // 02:30 AEDT
    CHECK_FALSE(small_at(utc(2026, 4, 4, 16, 0)));    // 02:00 AEST, before 02:30
    CHECK(small_at(utc(2026, 4, 4, 16, 30)));         // 02:30 AEST
    CHECK(small_at(utc(2026, 4, 4, 17, 59)));         // 03:59 AEST
    CHECK_FALSE(small_at(utc(2026, 4, 4, 18, 0)));    // 04:00 AEST

    // The screen follows: lit at 21:59, dark at 22:00, dark through the jump, lit again at 07:00, whatever the glass
    // had been touched for, and a touch lights it for 30 s at any of those minutes.
    const auto screen_at = [&](std::int64_t t, std::uint32_t inactive_ms) {
        return nv::dim_state(nv::View::kSpeakers, true, nv::kDimNever, inactive_ms, 3'600'000, night, minute_of_day(melbourne, t));
    };
    CHECK(screen_at(utc(2026, 10, 3, 11, 59), 3'600'000) == nv::Dim::kNone);
    CHECK(screen_at(utc(2026, 10, 3, 12, 0), 3'600'000) == nv::Dim::kDark);
    CHECK(screen_at(utc(2026, 10, 3, 15, 59), 3'600'000) == nv::Dim::kDark);
    CHECK(screen_at(utc(2026, 10, 3, 16, 0), 3'600'000) == nv::Dim::kDark);
    CHECK(screen_at(utc(2026, 10, 3, 16, 0), 5'000) == nv::Dim::kNone);  // a tap at 03:00 AEDT
    CHECK(screen_at(utc(2026, 10, 3, 19, 59), 3'600'000) == nv::Dim::kDark);
    CHECK(screen_at(utc(2026, 10, 3, 20, 0), 3'600'000) == nv::Dim::kNone);
}

TEST_CASE("time_zones: a clock that has not converged is never dark, in any zone", "[time_zones]") {
    nv::NightSchedule night;
    night.enabled = true;
    // The reading the seam gives before a time server has answered has no time in it: the panel passes no minute.
    const wc::Reading unknown;
    CHECK_FALSE(unknown.known);
    const std::optional<int> minute = unknown.known ? std::optional<int>(unknown.local.hour * 60 + unknown.local.minute) : std::nullopt;
    CHECK_FALSE(nv::night_active(night, minute));
    CHECK(nv::dim_state(nv::View::kSpeakers, true, nv::kDimNever, 3'600'000, 3'600'000, night, minute) == nv::Dim::kNone);
    // A reading with nothing in it says 00:00, which is inside a window that wraps midnight: that is why the panel passes
    // no minute at all for it, rather than the zeroed one.
    CHECK(nv::in_night_window(night.from_min, night.to_min, unknown.local.hour * 60 + unknown.local.minute));
}

TEST_CASE("time_zones: regions are runs of the table, and a city has a label a person reads", "[time_zones]") {
    const std::size_t regions = wc::region_count();
    CHECK(regions == 11);
    std::size_t covered = 0;
    std::set<std::string> seen;
    for (std::size_t i = 0; i < regions; ++i) {
        const wc::ZoneRegion region = wc::region_at(i);
        INFO(region.name);
        CHECK_FALSE(region.name.empty());
        CHECK(region.count > 0);
        CHECK(region.first == covered);  // the next run begins where the last ended
        covered += region.count;
        CHECK(seen.insert(std::string(region.name)).second);  // each region once
        for (std::size_t j = region.first; j < region.first + region.count; ++j) {
            CHECK(wc::zone_region(wc::time_zone_at(j).name) == region.name);
        }
    }
    CHECK(covered == wc::time_zone_count());
    CHECK(wc::region_at(regions).count == 0);
    CHECK(wc::region_at(regions).name.empty());
    CHECK(wc::region_at(0).name == "Africa");
    CHECK(wc::region_at(regions - 1).name == "Other");
    for (const char* name : {"Africa", "America", "Antarctica", "Arctic", "Asia", "Atlantic", "Australia", "Europe", "Indian",
                             "Pacific", "Other"}) {
        CHECK(seen.count(name) == 1);
    }
    CHECK(wc::zone_region("UTC") == "Other");
    CHECK(wc::zone_region("Australia/Melbourne") == "Australia");

    CHECK(wc::zone_label("Australia/Melbourne") == "Melbourne");
    CHECK(wc::zone_label("America/New_York") == "New York");
    CHECK(wc::zone_label("America/Argentina/Buenos_Aires") == "Buenos Aires (Argentina)");
    CHECK(wc::zone_label("America/North_Dakota/New_Salem") == "New Salem (North Dakota)");
    CHECK(wc::zone_label("America/Indiana/Indianapolis") == "Indianapolis (Indiana)");
    CHECK(wc::zone_label("UTC") == "UTC");
    // No two cities of one region have the same label.
    for (std::size_t i = 0; i < regions; ++i) {
        const wc::ZoneRegion region = wc::region_at(i);
        std::set<std::string> labels;
        for (std::size_t j = region.first; j < region.first + region.count; ++j) {
            CHECK(labels.insert(wc::zone_label(wc::time_zone_at(j).name)).second);
        }
    }
}

TEST_CASE("time_zones: PUT /timezone takes a name, a string or none, and a name is stored with its string", "[time_zones]") {
    const auto named = wc::parse_zone_choice("Australia/Melbourne\n");
    REQUIRE(named.has_value());
    CHECK(named->name == "Australia/Melbourne");
    CHECK(named->posix == "AEST-10AEDT,M10.1.0,M4.1.0/3");
    // A string as ever: no name, because the string cannot say which city was meant.
    const auto typed = wc::parse_zone_choice("AEST-10AEDT,M10.1.0,M4.1.0/3");
    REQUIRE(typed.has_value());
    CHECK(typed->posix == "AEST-10AEDT,M10.1.0,M4.1.0/3");
    CHECK(typed->name.empty());
    // `none` takes the zone away, name and all.
    const auto none = wc::parse_zone_choice(" none ");
    REQUIRE(none.has_value());
    CHECK(none->posix.empty());
    CHECK(none->name.empty());
    for (const char* bad : {"", "Australia/Melburne", "australia/melbourne", "Melbourne", "AEST", "AEST-10AEDT", "None"}) {
        INFO(bad);
        CHECK_FALSE(wc::parse_zone_choice(bad).has_value());
    }
    // UTC is a zone one can choose by name, which is not the same as having none.
    const auto utc_zone = wc::parse_zone_choice("UTC");
    REQUIRE(utc_zone.has_value());
    CHECK(utc_zone->posix == "UTC0");
    CHECK(utc_zone->name == "UTC");
    // Every name the table has is taken, and gives back its own string.
    for (std::size_t i = 0; i < wc::time_zone_count(); ++i) {
        const wc::NamedZone zone = wc::time_zone_at(i);
        const auto choice = wc::parse_zone_choice(zone.name);
        REQUIRE(choice.has_value());
        CHECK(choice->name == zone.name);
        CHECK(choice->posix == zone.posix);
    }
}

TEST_CASE("time_zones: what the board holds after reading its store follows a name it knows and drops one it does not", "[time_zones]") {
    // A name the table knows is followed to the table's own rule: a release that changes a rule moves the boards that chose
    // that city, with the old string still in the store.
    const wc::ZoneChoice moved = wc::reconcile_zone("AEST-10", "Australia/Melbourne");
    CHECK(moved.name == "Australia/Melbourne");
    CHECK(moved.posix == "AEST-10AEDT,M10.1.0,M4.1.0/3");
    CHECK(wc::reconcile_zone("AEST-10AEDT,M10.1.0,M4.1.0/3", "Australia/Melbourne") == moved);
    // A name this build does not know (a partition written by another image) or none is a string somebody typed.
    const wc::ZoneChoice typed = wc::reconcile_zone("JST-9", "Asia/Nowhere");
    CHECK(typed.posix == "JST-9");
    CHECK(typed.name.empty());
    CHECK(wc::reconcile_zone("JST-9", "").name.empty());
    CHECK(wc::reconcile_zone("JST-9", "").posix == "JST-9");
    CHECK(wc::reconcile_zone("", "").posix.empty());
}

// --- what a person reads: the place, never the string -------------------------------------------------------------

TEST_CASE("a POSIX string is read as the place the table holds for it", "[time_zones]") {
    // Exact, and the one place a string stands for when it has one.
    const auto tokyo = wc::find_time_zone_by_string("JST-9");
    REQUIRE(tokyo.has_value());
    CHECK(tokyo->name == "Asia/Tokyo");
    // A string several places share is the one the generator prefers (the most populous): Melbourne, Sydney, Hobart,
    // Canberra and Macquarie are one string, and the owner's zone is Melbourne.
    const auto melbourne = wc::find_time_zone_by_string("AEST-10AEDT,M10.1.0,M4.1.0/3");
    REQUIRE(melbourne.has_value());
    CHECK(melbourne->name == "Australia/Melbourne");
    CHECK(wc::zone_readable("AEST-10AEDT,M10.1.0,M4.1.0/3") == "Melbourne (Australia)");
    // The name a person chose wins over the string: Sydney is Sydney though Melbourne is what the string alone says.
    CHECK(wc::zone_readable("AEST-10AEDT,M10.1.0,M4.1.0/3", "Australia/Sydney") == "Sydney (Australia)");
    CHECK(wc::zone_readable("AEST-10AEDT,M10.1.0,M4.1.0/3", "Australia/Hobart") == "Hobart (Australia)");
    // A name the table does not know does not say the zone; the string does.
    CHECK(wc::zone_readable("AEST-10AEDT,M10.1.0,M4.1.0/3", "Australia/Atlantis") == "Melbourne (Australia)");
    // Not exact is not found: a string one rule away is another zone.
    CHECK_FALSE(wc::find_time_zone_by_string("AEST-10AEDT,M10.1.0,M4.1.0/2").has_value());
    CHECK_FALSE(wc::find_time_zone_by_string("aest-10aedt,m10.1.0,m4.1.0/3").has_value());
    CHECK_FALSE(wc::find_time_zone_by_string("").has_value());
    CHECK_FALSE(wc::find_time_zone_by_string("none").has_value());
}

TEST_CASE("every string in the table reads as a place the table holds for it", "[time_zones]") {
    std::set<std::string> shared;
    std::set<std::string> seen;
    for (std::size_t i = 0; i < wc::time_zone_count(); ++i) {
        const wc::NamedZone zone = wc::time_zone_at(i);
        INFO(zone.name << " " << zone.posix);
        const auto place = wc::find_time_zone_by_string(zone.posix);
        REQUIRE(place.has_value());
        // The place has the string, so its clock is the zone's; and the text a person reads is a place, not the string.
        CHECK(place->posix == zone.posix);
        const std::string text = wc::zone_readable(zone.posix);
        CHECK(text == wc::zone_display(place->name));
        CHECK(text.find(zone.posix) == std::string::npos);
        // A name that is chosen is the one that is read, whatever place its string would give.
        CHECK(wc::zone_readable(zone.posix, zone.name) == wc::zone_display(zone.name));
        if (!seen.insert(std::string(zone.posix)).second) {
            shared.insert(std::string(zone.posix));
        }
    }
    // One place is preferred for each string that is shared, and none for a string that is not.
    CHECK(wc::data::kPreferredCount == shared.size());
    std::set<std::string> preferred;
    for (std::size_t i = 0; i < wc::data::kPreferredCount; ++i) {
        const std::string posix = wc::data::kZones[wc::data::kPreferred[i]].posix;
        INFO(wc::data::kZones[wc::data::kPreferred[i]].name);
        CHECK(shared.count(posix) == 1);
        CHECK(preferred.insert(posix).second);
    }
}

TEST_CASE("a string in no table is read as its offset, and that it is the person's own", "[time_zones]") {
    CHECK(wc::zone_readable("ZZZ-10") == "UTC+10:00 (custom)");
    CHECK(wc::zone_readable("ZZZ-9:45") == "UTC+09:45 (custom)");
    CHECK(wc::zone_readable("<-0330>3:30") == "UTC-03:30 (custom)");
    CHECK(wc::zone_readable("ZZZ5") == "UTC-05:00 (custom)");
    CHECK(wc::zone_readable("ZZZ0") == "UTC+00:00 (custom)");
    // With daylight saving the rule says so, in words.
    CHECK(wc::zone_readable("CUT-9:30CUD,M10.1.0,M4.1.0/3") == "UTC+09:30 (custom, with daylight saving)");
    // None, and the empty string, are UTC; the table's own UTC is too.
    CHECK(wc::zone_readable("none") == "UTC");
    CHECK(wc::zone_readable("") == "UTC");
    CHECK(wc::zone_readable("UTC0") == "UTC");
    CHECK(wc::zone_readable("none", "UTC") == "UTC");
    // The string is never in what is shown.
    for (const char* custom : {"ZZZ-10", "ZZZ-9:45", "<-0330>3:30", "CUT-9:30CUD,M10.1.0,M4.1.0/3", "ZZZ0"}) {
        INFO(custom);
        CHECK(wc::zone_readable(custom).find(custom) == std::string::npos);
    }
    // A string the board would not read at all is not shown either.
    CHECK(wc::zone_readable("not a zone") == "a zone this board does not read");
}

TEST_CASE("an offset is written the way people say it", "[time_zones]") {
    CHECK(wc::offset_text(0) == "UTC+00:00");
    CHECK(wc::offset_text(10 * 3600) == "UTC+10:00");
    CHECK(wc::offset_text(-(3 * 3600 + 30 * 60)) == "UTC-03:30");
    CHECK(wc::offset_text(12 * 3600 + 45 * 60) == "UTC+12:45");
    CHECK(wc::offset_text(-12 * 3600) == "UTC-12:00");
    CHECK(wc::offset_text(14 * 3600) == "UTC+14:00");
}
