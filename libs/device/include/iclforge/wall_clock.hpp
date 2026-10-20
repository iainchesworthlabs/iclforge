#pragma once

// The clock view of the P4X panel (planning/p4x-now-playing.md, stage 3): the time zone a board is given and
// the words the view says. Standard library only, so that the host tests it (libs/device/tests/test_wall_clock.cpp):
// the zone is what PUT /timezone takes and NVS holds, and a board that read one wrong would show the wrong
// time to someone who trusts a clock.
//
// A time zone is a POSIX string (IEEE 1003.1 section 8.3), because the firmware has no zone database and a
// list of names would be a table to keep: `AEST-10AEDT,M10.1.0,M4.1.0/3`. The offset is the hours WEST of
// UTC, so Australia's `-10` is ten hours east. This header says whether a string is one this board will take;
// the board's C library then reads it and is held to the offsets read here (zone_agrees), so a string both
// accept is the zone they both mean.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace iclforge::wallclock {

// --- the time zone ---------------------------------------------------------------------------------------

// The longest a zone string may be (NVS holds it in a field of this many bytes and a NUL).
inline constexpr std::size_t kTimeZoneMaxBytes = 63;

// The word that takes a zone away: the clock then says UTC, and says that no zone is set.
inline constexpr const char* kNoZone = "none";

// What a zone string says about the offsets from UTC, in seconds EAST of it (the opposite way round to the
// string's own sign).
struct ZoneOffsets {
    int std_east_s = 0;
    bool has_dst = false;
    int dst_east_s = 0;

    friend constexpr bool operator==(const ZoneOffsets&, const ZoneOffsets&) = default;
};

namespace detail {

// A cursor over the zone string: the grammar reads left to right and never looks back.
class Zone {
   public:
    explicit constexpr Zone(std::string_view text) : text_(text) {}

    [[nodiscard]] constexpr bool done() const { return at_ >= text_.size(); }
    [[nodiscard]] constexpr char peek() const { return done() ? '\0' : text_[at_]; }
    constexpr void bump() { ++at_; }
    constexpr bool take(char c) {
        if (peek() == c && !done()) {
            ++at_;
            return true;
        }
        return false;
    }

    // A zone's name: three or more letters, or three or more letters, digits, '+' and '-' between < and >.
    // `max` keeps a string a C library has to hold to a length it can.
    constexpr bool name() {
        const std::size_t start = at_;
        if (take('<')) {
            while (!done() && peek() != '>') {
                const char c = peek();
                if (!(alpha(c) || digit(c) || c == '+' || c == '-')) {
                    return false;
                }
                bump();
            }
            if (!take('>')) {
                return false;
            }
            const std::size_t length = at_ - start - 2;
            return length >= 3 && length <= 16;
        }
        while (alpha(peek())) {
            bump();
        }
        const std::size_t length = at_ - start;
        return length >= 3 && length <= 16;
    }

    // [+|-]hh[:mm[:ss]] as seconds, with the hours up to `max_hours` (24 for an offset, 167 for the time of a
    // rule). The sign is the string's own.
    constexpr std::optional<int> hms(int max_hours) {
        int sign = 1;
        if (take('-')) {
            sign = -1;
        } else {
            (void)take('+');
        }
        const std::optional<int> hours = number(1, max_hours >= 100 ? 3 : 2);
        if (!hours || *hours > max_hours) {
            return std::nullopt;
        }
        int seconds = *hours * 3600;
        if (take(':')) {
            const std::optional<int> minutes = number(2, 2);
            if (!minutes || *minutes > 59) {
                return std::nullopt;
            }
            seconds += *minutes * 60;
            if (take(':')) {
                const std::optional<int> secs = number(2, 2);
                if (!secs || *secs > 59) {
                    return std::nullopt;
                }
                seconds += *secs;
            }
        }
        return sign * seconds;
    }

    // Jn (1 to 365, no leap day), n (0 to 365) or Mm.w.d (month 1 to 12, week 1 to 5, day 0 to 6, Sunday 0),
    // then an optional /time.
    constexpr bool rule() {
        if (take('J')) {
            const std::optional<int> day = number(1, 3);
            if (!day || *day < 1 || *day > 365) {
                return false;
            }
        } else if (take('M')) {
            const std::optional<int> month = number(1, 2);
            if (!month || *month < 1 || *month > 12 || !take('.')) {
                return false;
            }
            const std::optional<int> week = number(1, 1);
            if (!week || *week < 1 || *week > 5 || !take('.')) {
                return false;
            }
            const std::optional<int> weekday = number(1, 1);
            if (!weekday || *weekday > 6) {
                return false;
            }
        } else {
            const std::optional<int> day = number(1, 3);
            if (!day || *day > 365) {
                return false;
            }
        }
        if (take('/')) {
            return hms(167).has_value();
        }
        return true;
    }

   private:
    [[nodiscard]] static constexpr bool alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
    [[nodiscard]] static constexpr bool digit(char c) { return c >= '0' && c <= '9'; }

    // Between `least` and `most` digits, as a number.
    constexpr std::optional<int> number(int least, int most) {
        int value = 0;
        int count = 0;
        while (digit(peek()) && count < most) {
            value = value * 10 + (peek() - '0');
            bump();
            ++count;
        }
        if (count < least) {
            return std::nullopt;
        }
        return value;
    }

    std::string_view text_;
    std::size_t at_ = 0;
};

[[nodiscard]] constexpr std::string_view trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) {
        text.remove_suffix(1);
    }
    return text;
}

}  // namespace detail

// A POSIX zone string, as `std offset [dst [offset] ,start[/time],end[/time]]`, and the offsets it names.
// A zone with daylight saving MUST give its rules: the string without them has a meaning that differs from one C
// library to the next, and a clock that is an hour out for half the year is not one the board should show.
// Nothing: the string is not one this board takes.
[[nodiscard]] constexpr std::optional<ZoneOffsets> parse_time_zone(std::string_view text) {
    if (text.empty() || text.size() > kTimeZoneMaxBytes) {
        return std::nullopt;
    }
    for (const char c : text) {
        if (c < '!' || c > '~') {  // printable and not a space: a zone is typed on one line
            return std::nullopt;
        }
    }
    detail::Zone zone(text);
    ZoneOffsets out;
    if (!zone.name()) {
        return std::nullopt;
    }
    const std::optional<int> standard = zone.hms(24);
    if (!standard) {
        return std::nullopt;
    }
    out.std_east_s = -*standard;
    if (zone.done()) {
        return out;
    }
    if (!zone.name()) {
        return std::nullopt;
    }
    out.has_dst = true;
    out.dst_east_s = out.std_east_s + 3600;
    if (zone.peek() != ',') {
        const std::optional<int> daylight = zone.hms(24);
        if (!daylight) {
            return std::nullopt;
        }
        out.dst_east_s = -*daylight;
    }
    if (!zone.take(',') || !zone.rule() || !zone.take(',') || !zone.rule() || !zone.done()) {
        return std::nullopt;
    }
    return out;
}

[[nodiscard]] constexpr bool valid_time_zone(std::string_view text) { return parse_time_zone(text).has_value(); }

// What PUT /timezone takes: a zone string, or `none`, with or without a line ending. nullopt is a body that is
// neither; an empty string is `none` (the zone taken away).
[[nodiscard]] inline std::optional<std::string> parse_zone_setting(std::string_view body) {
    body = detail::trimmed(body);
    if (body == kNoZone) {
        return std::string();
    }
    if (!valid_time_zone(body)) {
        return std::nullopt;
    }
    return std::string(body);
}

// What GET /timezone says: the zone, or `none` while there is not one.
[[nodiscard]] inline std::string zone_setting_text(std::string_view stored) {
    return stored.empty() ? std::string(kNoZone) : std::string(stored);
}

// Whether the board's C library reads a zone the way this header does. `january_east_s` and `july_east_s` are
// the offsets from UTC the library gave (seconds east) at two instants it was asked about, one in each half of
// the year: in standard time the library must give the standard offset both times, and with daylight saving
// each time one of the two. A library that did not understand the string falls back to UTC and fails this for
// every zone that is not UTC.
[[nodiscard]] constexpr bool zone_agrees(const ZoneOffsets& zone, int january_east_s, int july_east_s) {
    const auto fits = [&zone](int east_s) {
        return east_s == zone.std_east_s || (zone.has_dst && east_s == zone.dst_east_s);
    };
    return fits(january_east_s) && fits(july_east_s);
}

// --- the time -------------------------------------------------------------------------------------------

// A date and a time on a wall: what the clock shows, which a zone and a count of seconds make.
struct LocalTime {
    int year = 1970;
    int month = 1;   // 1 to 12
    int day = 1;     // 1 to 31
    int hour = 0;    // 0 to 23
    int minute = 0;  // 0 to 59
    int second = 0;  // 0 to 59
    int weekday = 4; // 0 is Sunday

    friend constexpr bool operator==(const LocalTime&, const LocalTime&) = default;
};

// Days since 1970-01-01 of a civil date, and back (the proleptic Gregorian calendar; H. Hinnant's algorithms).
[[nodiscard]] constexpr std::int64_t days_from_civil(int year, int month, int day) {
    const std::int64_t y = static_cast<std::int64_t>(year) - (month <= 2 ? 1 : 0);
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const std::int64_t yoe = y - era * 400;
    const std::int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// The wall time of `unix_s` seconds after the epoch in a zone `offset_s` seconds east of UTC.
[[nodiscard]] constexpr LocalTime local_from_unix(std::int64_t unix_s, int offset_s) {
    const std::int64_t shifted = unix_s + offset_s;
    std::int64_t days = shifted / 86400;
    std::int64_t rest = shifted % 86400;
    if (rest < 0) {
        rest += 86400;
        --days;
    }
    const std::int64_t z = days + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::int64_t doe = z - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const int day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    const int month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    LocalTime out;
    out.year = static_cast<int>(yoe + era * 400 + (month <= 2 ? 1 : 0));
    out.month = month;
    out.day = day;
    out.hour = static_cast<int>(rest / 3600);
    out.minute = static_cast<int>(rest % 3600 / 60);
    out.second = static_cast<int>(rest % 60);
    const std::int64_t week = (days + 4) % 7;
    out.weekday = static_cast<int>(week >= 0 ? week : week + 7);
    return out;
}

// How far east of UTC a wall time is at `unix_s`: what a C library's broken-down time says about its zone,
// for zone_agrees().
[[nodiscard]] constexpr int offset_of(const LocalTime& local, std::int64_t unix_s) {
    const std::int64_t as_if_utc = days_from_civil(local.year, local.month, local.day) * 86400 +
                                   local.hour * 3600 + local.minute * 60 + local.second;
    return static_cast<int>(as_if_utc - unix_s);
}

// Two instants a zone with daylight saving is in different halves of at, in any year of the board's life.
inline constexpr std::int64_t kProbeJanuary = 1768478400;  // 2026-01-15 12:00:00 UTC
inline constexpr std::int64_t kProbeJuly = 1784116800;     // 2026-07-15 12:00:00 UTC

// The earliest count of seconds a time server's answer can be taken for (2026-01-01 00:00:00 UTC, before this
// code was written): an answer older than that is a server with a wrong clock, and the board says no time
// before it says one that cannot be.
inline constexpr std::int64_t kNotBefore = 1767225600;

// --- the words ---------------------------------------------------------------------------------------------

// What the seam gives the screen: no time until a time server has answered, and never one the board made up.
struct Reading {
    bool known = false;
    LocalTime local;
    // A zone was set. Without one the time is UTC, and the screen says so.
    bool zone_set = false;

    friend constexpr bool operator==(const Reading&, const Reading&) = default;
};

// What the screen draws: the time (24 hours, no seconds: the screen redraws when the minute does), the date
// and the sentence for a board with no zone. Empty strings while the time is not known.
struct Face {
    std::string time;
    std::string date;
    std::string note;

    friend bool operator==(const Face&, const Face&) = default;
};

inline constexpr const char* kUtcNote = "UTC. No time zone is set.";

[[nodiscard]] inline Face face(const Reading& reading) {
    static constexpr const char* kWeekdays[7] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                                 "Thursday", "Friday", "Saturday"};
    static constexpr const char* kMonths[12] = {"January", "February", "March", "April", "May", "June", "July",
                                                "August", "September", "October", "November", "December"};
    Face out;
    if (!reading.known) {
        return out;
    }
    const LocalTime& at = reading.local;
    const auto two = [](int value) {
        std::string text;
        text += static_cast<char>('0' + value / 10 % 10);
        text += static_cast<char>('0' + value % 10);
        return text;
    };
    out.time = two(at.hour) + ":" + two(at.minute);
    const int weekday = at.weekday >= 0 && at.weekday < 7 ? at.weekday : 0;
    const int month = at.month >= 1 && at.month <= 12 ? at.month : 1;
    out.date = std::string(kWeekdays[weekday]) + " " + std::to_string(at.day) + " " + kMonths[month - 1];
    if (!reading.zone_set) {
        out.note = kUtcNote;
    }
    return out;
}

}  // namespace iclforge::wallclock
