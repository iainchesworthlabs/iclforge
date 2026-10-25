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

// The day a daylight saving rule falls on: Jn (the 1st to the 365th, February 29 not counted), n (the 0th to
// the 365th, February 29 counted) or Mm.w.d (the w-th d-day of month m, 5 meaning the last).
enum class RuleKind : std::uint8_t { kJulian1, kJulian0, kMonthWeekDay };

struct TransitionRule {
    RuleKind kind = RuleKind::kMonthWeekDay;
    int month = 0;  // Mm.w.d only
    int week = 0;   // Mm.w.d only, 1 to 5
    int day = 0;    // the day number (Jn, n) or the weekday, 0 to 6 with Sunday 0 (Mm.w.d)
    // The wall time of the change, seconds after midnight (it may be negative or past a day, as `/-1` and
    // `/25` are); the string's own default is 02:00.
    int time_s = 7200;

    friend constexpr bool operator==(const TransitionRule&, const TransitionRule&) = default;
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
    // then an optional /time. `out`, when given, is what the rule said (parse_zone_rules).
    constexpr bool rule(TransitionRule* out = nullptr) {
        TransitionRule read;
        if (take('J')) {
            const std::optional<int> day = number(1, 3);
            if (!day || *day < 1 || *day > 365) {
                return false;
            }
            read.kind = RuleKind::kJulian1;
            read.day = *day;
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
            read.kind = RuleKind::kMonthWeekDay;
            read.month = *month;
            read.week = *week;
            read.day = *weekday;
        } else {
            const std::optional<int> day = number(1, 3);
            if (!day || *day > 365) {
                return false;
            }
            read.kind = RuleKind::kJulian0;
            read.day = *day;
        }
        if (take('/')) {
            const std::optional<int> at = hms(167);
            if (!at) {
                return false;
            }
            read.time_s = *at;
        }
        if (out != nullptr) {
            *out = read;
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

// What a zone string says: its offsets and, with daylight saving, the two rules that switch between them (a
// zone with none has rules that are never looked at).
struct ZoneRules {
    ZoneOffsets offsets;
    TransitionRule start;  // standard to daylight time, in the wall time in force before it (standard time)
    TransitionRule end;    // daylight to standard time, in the wall time in force before it (daylight time)

    friend constexpr bool operator==(const ZoneRules&, const ZoneRules&) = default;
};

// A POSIX zone string, as `std offset [dst [offset] ,start[/time],end[/time]]`, and what it names.
// A zone with daylight saving MUST give its rules: the string without them has a meaning that differs from one C
// library to the next, and a clock that is an hour out for half the year is not one the board should show.
// Nothing: the string is not one this board takes.
[[nodiscard]] constexpr std::optional<ZoneRules> parse_zone_rules(std::string_view text) {
    if (text.empty() || text.size() > kTimeZoneMaxBytes) {
        return std::nullopt;
    }
    for (const char c : text) {
        if (c < '!' || c > '~') {  // printable and not a space: a zone is typed on one line
            return std::nullopt;
        }
    }
    detail::Zone zone(text);
    ZoneRules out;
    ZoneOffsets& offsets = out.offsets;
    if (!zone.name()) {
        return std::nullopt;
    }
    const std::optional<int> standard = zone.hms(24);
    if (!standard) {
        return std::nullopt;
    }
    offsets.std_east_s = -*standard;
    if (zone.done()) {
        return out;
    }
    if (!zone.name()) {
        return std::nullopt;
    }
    offsets.has_dst = true;
    offsets.dst_east_s = offsets.std_east_s + 3600;
    if (zone.peek() != ',') {
        const std::optional<int> daylight = zone.hms(24);
        if (!daylight) {
            return std::nullopt;
        }
        offsets.dst_east_s = -*daylight;
    }
    if (!zone.take(',') || !zone.rule(&out.start) || !zone.take(',') || !zone.rule(&out.end) || !zone.done()) {
        return std::nullopt;
    }
    return out;
}

[[nodiscard]] constexpr std::optional<ZoneOffsets> parse_time_zone(std::string_view text) {
    if (const std::optional<ZoneRules> rules = parse_zone_rules(text)) {
        return rules->offsets;
    }
    return std::nullopt;
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

// --- the time server --------------------------------------------------------------------------------------

// The longest a time server's name may be (NVS holds it in a field of this many bytes and a NUL).
inline constexpr std::size_t kNtpServerMaxBytes = 63;

namespace detail {

[[nodiscard]] constexpr bool is_digit(char c) { return c >= '0' && c <= '9'; }

// a.b.c.d, each part 0 to 255 and written without a leading zero (lwIP, like most resolvers, reads "010" as
// octal, and a three-part "10.0.1" as 10.0.0.1: neither is what anyone typed).
[[nodiscard]] constexpr bool is_ipv4(std::string_view text) {
    int parts = 0;
    std::size_t at = 0;
    while (at <= text.size()) {
        const std::size_t start = at;
        int value = 0;
        while (at < text.size() && is_digit(text[at])) {
            value = value * 10 + (text[at] - '0');
            if (value > 255) {
                return false;
            }
            ++at;
        }
        const std::size_t digits = at - start;
        if (digits == 0 || digits > 3 || (digits > 1 && text[start] == '0')) {
            return false;
        }
        ++parts;
        if (at == text.size()) {
            break;
        }
        if (text[at] != '.') {
            return false;
        }
        ++at;
    }
    return parts == 4;
}

}  // namespace detail

// A host name (RFC 1123 labels of letters, digits and hyphens, none starting or ending with a hyphen, the last
// not all digits) or an IPv4 address, of at most kNtpServerMaxBytes. What PUT /ntp-server takes for a server of
// the owner's own, in place of the pool: a time server on the network the board is on.
[[nodiscard]] constexpr bool valid_ntp_server(std::string_view text) {
    if (text.empty() || text.size() > kNtpServerMaxBytes) {
        return false;
    }
    bool numeric = true;
    for (const char c : text) {
        numeric = numeric && (detail::is_digit(c) || c == '.');
    }
    if (numeric) {
        return detail::is_ipv4(text);
    }
    std::size_t label = 0;
    bool last_all_digits = true;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        const bool end = i == text.size() || text[i] == '.';
        if (end) {
            if (label == 0 || label > 63 || text[i - 1] == '-') {
                return false;
            }
            if (i == text.size()) {
                break;
            }
            label = 0;
            last_all_digits = true;
            continue;
        }
        const char c = text[i];
        const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (!(letter || detail::is_digit(c) || c == '-') || (label == 0 && c == '-')) {
            return false;
        }
        last_all_digits = last_all_digits && detail::is_digit(c);
        ++label;
    }
    return !last_all_digits;
}

// What PUT /ntp-server takes: a server, or `none`, with or without a line ending. nullopt is a body that is
// neither; an empty string is `none` (the board's own choice of servers again: the router's, then the pool).
[[nodiscard]] inline std::optional<std::string> parse_ntp_server_setting(std::string_view body) {
    body = detail::trimmed(body);
    if (body == kNoZone) {
        return std::string();
    }
    // "None" is a host name as far as the grammar goes, and a typo for the word as far as a person goes.
    if (body.size() == 4 && (body[0] | 0x20) == 'n' && (body[1] | 0x20) == 'o' && (body[2] | 0x20) == 'n' &&
        (body[3] | 0x20) == 'e') {
        return std::nullopt;
    }
    if (!valid_ntp_server(body)) {
        return std::nullopt;
    }
    return std::string(body);
}

// What GET /ntp-server says: the server, or `none` while there is not one.
[[nodiscard]] inline std::string ntp_server_setting_text(std::string_view stored) {
    return stored.empty() ? std::string(kNoZone) : std::string(stored);
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

// --- what a zone's rules say at a moment ----------------------------------------------------------------

// The board's C library applies the zone on the board (a zone is checked against it before it is taken, by
// zone_agrees); this reads the same rules, so that what a schedule or a table of zones promises can be held to the
// real changeovers on the host, in a test that needs no C library's time zone at all.

[[nodiscard]] constexpr bool is_leap_year(int year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

[[nodiscard]] constexpr int days_in_month(int year, int month) {
    constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && is_leap_year(year) ? 29 : kDays[month >= 1 && month <= 12 ? month - 1 : 0];
}

// The weekday of a day count since 1970-01-01, 0 for Sunday (1970-01-01 was a Thursday).
[[nodiscard]] constexpr int weekday_of_days(std::int64_t days) {
    const std::int64_t week = (days + 4) % 7;
    return static_cast<int>(week >= 0 ? week : week + 7);
}

// The day (since 1970-01-01) a rule falls on in `year`.
[[nodiscard]] constexpr std::int64_t rule_day(const TransitionRule& rule, int year) {
    const std::int64_t new_year = days_from_civil(year, 1, 1);
    switch (rule.kind) {
        case RuleKind::kJulian1:
            return new_year + (rule.day - 1) + (is_leap_year(year) && rule.day >= 60 ? 1 : 0);
        case RuleKind::kJulian0:
            return new_year + rule.day;
        case RuleKind::kMonthWeekDay:
            break;
    }
    const std::int64_t first = days_from_civil(year, rule.month, 1);
    int day = 1 + (rule.day - weekday_of_days(first) + 7) % 7 + (rule.week - 1) * 7;
    while (day > days_in_month(year, rule.month)) {
        day -= 7;  // week 5 is the last such day, which may be the fourth
    }
    return first + (day - 1);
}

// How far east of UTC a zone is at `unix_s`. The start rule's wall time is standard time and the end rule's is
// daylight time, as in IEEE 1003.1; a zone whose daylight time runs across New Year (the southern hemisphere's)
// is in it before the end and from the start.
[[nodiscard]] constexpr int zone_offset_at(const ZoneRules& zone, std::int64_t unix_s) {
    const ZoneOffsets& offsets = zone.offsets;
    if (!offsets.has_dst) {
        return offsets.std_east_s;
    }
    const int year = local_from_unix(unix_s, offsets.std_east_s).year;
    const std::int64_t start = rule_day(zone.start, year) * 86400 + zone.start.time_s - offsets.std_east_s;
    const std::int64_t end = rule_day(zone.end, year) * 86400 + zone.end.time_s - offsets.dst_east_s;
    const bool daylight = start < end ? (unix_s >= start && unix_s < end) : (unix_s >= start || unix_s < end);
    return daylight ? offsets.dst_east_s : offsets.std_east_s;
}

// The wall time in a zone at `unix_s`.
[[nodiscard]] constexpr LocalTime zone_local_time(const ZoneRules& zone, std::int64_t unix_s) {
    return local_from_unix(unix_s, zone_offset_at(zone, unix_s));
}

// Two instants a zone with daylight saving is in different halves of at, in any year of the board's life.
inline constexpr std::int64_t kProbeJanuary = 1768478400;  // 2026-01-15 12:00:00 UTC
inline constexpr std::int64_t kProbeJuly = 1784116800;     // 2026-07-15 12:00:00 UTC

// The earliest count of seconds a time server's answer can be taken for (2026-01-01 00:00:00 UTC, before this
// code was written): an answer older than that is a server with a wrong clock, and the board says no time
// before it says one that cannot be.
inline constexpr std::int64_t kNotBefore = 1767225600;

// --- whether to ask --------------------------------------------------------------------------------------

// What PUT /time-sync takes: `on` or `off`, with or without a line ending. A board keeps its time with a time
// server by default (planning/p4x-now-playing.md, stage 3); `off` is for a board that must contact none.
[[nodiscard]] constexpr std::optional<bool> parse_time_sync(std::string_view body) {
    body = detail::trimmed(body);
    if (body == "on") {
        return true;
    }
    if (body == "off") {
        return false;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr const char* time_sync_text(bool on) { return on ? "on" : "off"; }

// --- the words ---------------------------------------------------------------------------------------------

// How the clock writes the time: 24 hours (21:47) or 12 (9:47 PM), with the seconds or without. A person's own
// choice (PUT /clock-format), and the stored value is one number that must never be renumbered: bit 0 is the
// 12-hour clock and bit 1 the seconds, so the default, 0, is 24 hours and no seconds.
struct ClockFormat {
    bool twelve_hour = false;
    bool seconds = false;

    friend constexpr bool operator==(const ClockFormat&, const ClockFormat&) = default;
};

[[nodiscard]] constexpr std::int32_t clock_format_stored(const ClockFormat& format) {
    return (format.twelve_hour ? 1 : 0) | (format.seconds ? 2 : 0);
}

// Nothing for a number this build does not know (a partition written by another build).
[[nodiscard]] constexpr std::optional<ClockFormat> clock_format_from_stored(std::int32_t value) {
    if (value < 0 || value > 3) {
        return std::nullopt;
    }
    return ClockFormat{.twelve_hour = (value & 1) != 0, .seconds = (value & 2) != 0};
}

// What PUT /clock-format takes and GET reads back: `24` or `12`, then ` seconds` when the seconds are shown
// (`24`, `24 seconds`, `12`, `12 seconds`), with or without a line ending.
[[nodiscard]] constexpr std::optional<ClockFormat> parse_clock_format(std::string_view body) {
    body = detail::trimmed(body);
    ClockFormat out;
    if (body.starts_with("24")) {
        body.remove_prefix(2);
    } else if (body.starts_with("12")) {
        out.twelve_hour = true;
        body.remove_prefix(2);
    } else {
        return std::nullopt;
    }
    if (body.empty()) {
        return out;
    }
    if (body == " seconds") {
        out.seconds = true;
        return out;
    }
    return std::nullopt;
}

[[nodiscard]] inline std::string clock_format_text(const ClockFormat& format) {
    std::string out = format.twelve_hour ? "12" : "24";
    if (format.seconds) {
        out += " seconds";
    }
    return out;
}

// What the seam gives the screen: no time until a time server has answered, and never one the board made up.
struct Reading {
    bool known = false;
    LocalTime local;
    // A zone was set. Without one the time is UTC, and the screen says so.
    bool zone_set = false;
    // How the person who owns the board wants the time written. Not the clock's to know: the panel fills it in
    // from the setting when it reads the clock.
    ClockFormat format;

    friend constexpr bool operator==(const Reading&, const Reading&) = default;
};

// Minutes after local midnight, for the rules that go by the hour (the night schedule): nothing until the time is known.
[[nodiscard]] constexpr std::optional<int> minute_of_day(const Reading& reading) {
    if (!reading.known) {
        return std::nullopt;
    }
    return reading.local.hour * 60 + reading.local.minute;
}

// What the screen draws: the time (24 hours and no seconds unless the format says otherwise: the screen redraws
// when the minute does), what goes small beside it (the seconds, and AM or PM, when there are any), the date and
// the sentence for a board with no zone. Empty strings while the time is not known.
struct Face {
    std::string time;
    std::string suffix;
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
    // 12 hours has no leading zero and no hour 0: midnight is 12:05 AM and noon 12:00 PM.
    if (reading.format.twelve_hour) {
        const int hour = at.hour % 12 == 0 ? 12 : at.hour % 12;
        out.time = std::to_string(hour) + ":" + two(at.minute);
    } else {
        out.time = two(at.hour) + ":" + two(at.minute);
    }
    if (reading.format.seconds) {
        out.suffix = ":" + two(at.second);
    }
    if (reading.format.twelve_hour) {
        out.suffix += std::string(reading.format.seconds ? " " : "") + (at.hour < 12 ? "AM" : "PM");
    }
    const int weekday = at.weekday >= 0 && at.weekday < 7 ? at.weekday : 0;
    const int month = at.month >= 1 && at.month <= 12 ? at.month : 1;
    out.date = std::string(kWeekdays[weekday]) + " " + std::to_string(at.day) + " " + kMonths[month - 1];
    if (!reading.zone_set) {
        out.note = kUtcNote;
    }
    return out;
}

// "2026-10-21 08:09:44": the time as GET /status and the board's page write it, to the second.
[[nodiscard]] inline std::string local_text(const LocalTime& at) {
    const auto digits = [](int value, int width) {
        std::string text(static_cast<std::size_t>(width), '0');
        for (int i = width - 1; i >= 0; --i) {
            text[static_cast<std::size_t>(i)] = static_cast<char>('0' + value % 10);
            value /= 10;
        }
        return text;
    };
    return digits(at.year, 4) + "-" + digits(at.month, 2) + "-" + digits(at.day, 2) + " " + digits(at.hour, 2) +
           ":" + digits(at.minute, 2) + ":" + digits(at.second, 2);
}

}  // namespace iclforge::wallclock
