#pragma once

// The time zones a board offers by name (planning/p4x-now-playing.md, stage 3 and the Preferences programme): a
// region and a city ("Australia/Melbourne") and the POSIX string that zone's rules end in
// (iclforge/wall_clock.hpp reads it; the board's C library applies it). The data is time_zone_data.hpp, written
// by tools/hearth/gen_time_zones.py from IANA's tzdata and checked in; this header is what is done with it.
// Standard library only, so that the host tests it (libs/device/tests/test_time_zones.cpp).
//
// What is stored is the string (`time_zone`, which the clock reads) and the name the person chose
// (`time_zone_name`): the string alone cannot say which city was chosen, because 89 strings stand for 420 names
// (Melbourne, Sydney, Hobart and Canberra are one rule). A name that no longer agrees with the string is not
// believed, and one the table knows is followed to the table's own rule, so that a firmware update that carries a
// government's new daylight saving rules moves the boards that chose that city and no others.

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "iclforge/time_zone_data.hpp"
#include "iclforge/wall_clock.hpp"

namespace iclforge::wallclock {

// The longest a zone's name is held in NVS: the longest in the table is 30.
inline constexpr std::size_t kTimeZoneNameMaxBytes = 39;

struct NamedZone {
    std::string_view name;   // "Australia/Melbourne"
    std::string_view posix;  // "AEST-10AEDT,M10.1.0,M4.1.0/3"

    friend constexpr bool operator==(const NamedZone&, const NamedZone&) = default;
};

// The table is sorted by name, the way a person looks a city up and the way the binary search below needs.
[[nodiscard]] constexpr std::size_t time_zone_count() { return data::kZoneCount; }

[[nodiscard]] constexpr NamedZone time_zone_at(std::size_t index) {
    return NamedZone{data::kZones[index].name, data::kZones[index].posix};
}

// A zone by its name, exactly as written (`Australia/Melbourne`, not `australia/melbourne`).
[[nodiscard]] constexpr std::optional<NamedZone> find_time_zone(std::string_view name) {
    std::size_t low = 0;
    std::size_t high = data::kZoneCount;
    while (low < high) {
        const std::size_t mid = low + (high - low) / 2;
        const std::string_view at = data::kZones[mid].name;
        if (at == name) {
            return time_zone_at(mid);
        }
        if (at < name) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return std::nullopt;
}

// --- regions and cities ------------------------------------------------------------------------------------------

// A name's region is what stands before its first slash; a name with none (UTC) is in "Other".
inline constexpr std::string_view kOtherRegion = "Other";

[[nodiscard]] constexpr std::string_view zone_region(std::string_view name) {
    const std::size_t slash = name.find('/');
    return slash == std::string_view::npos ? kOtherRegion : name.substr(0, slash);
}

// A region, as a run of the table: the names are sorted, so each region is one.
struct ZoneRegion {
    std::string_view name;
    std::size_t first = 0;
    std::size_t count = 0;
};

[[nodiscard]] constexpr std::size_t region_count() {
    std::size_t regions = 0;
    std::string_view previous;
    for (std::size_t i = 0; i < data::kZoneCount; ++i) {
        const std::string_view region = zone_region(data::kZones[i].name);
        if (i == 0 || region != previous) {
            ++regions;
            previous = region;
        }
    }
    return regions;
}

// The `index`-th region in table order (Africa, America, ..., Pacific, Other). An index past the last is empty.
[[nodiscard]] constexpr ZoneRegion region_at(std::size_t index) {
    ZoneRegion out;
    std::size_t started = 0;  // regions begun so far
    for (std::size_t i = 0; i < data::kZoneCount; ++i) {
        const std::string_view region = zone_region(data::kZones[i].name);
        const bool begins = i == 0 || region != zone_region(data::kZones[i - 1].name);
        if (begins) {
            if (started > index) {
                break;
            }
            ++started;
            if (started == index + 1) {
                out = ZoneRegion{.name = region, .first = i, .count = 0};
            }
        }
        if (started == index + 1) {
            ++out.count;
        }
    }
    return out;
}

// What a person reads for a name inside its region: the city, with underscores as spaces, and what lies between the
// region and the city in brackets after it (`Buenos Aires (Argentina)`, `Center (North Dakota)`, `Melbourne`).
// A name with no slash is itself.
[[nodiscard]] inline std::string zone_label(std::string_view name) {
    const std::size_t first = name.find('/');
    if (first == std::string_view::npos) {
        return std::string(name);
    }
    const std::size_t last = name.rfind('/');
    std::string city(name.substr(last + 1));
    std::replace(city.begin(), city.end(), '_', ' ');
    if (last == first) {
        return city;
    }
    std::string between(name.substr(first + 1, last - first - 1));
    std::replace(between.begin(), between.end(), '_', ' ');
    std::replace(between.begin(), between.end(), '/', ',');
    return city + " (" + between + ")";
}

// --- what PUT /timezone takes ---------------------------------------------------------------------------------------

// The zone as stored: the POSIX string the clock reads, and the name the person chose ("" for a string they typed).
struct ZoneChoice {
    std::string posix;
    std::string name;

    friend bool operator==(const ZoneChoice&, const ZoneChoice&) = default;
};

// What PUT /timezone takes: `none` (the zone taken away), a name from the table (`Australia/Melbourne`, which is
// stored with the string it stands for), or a POSIX string as ever. Nothing for a body that is none of them.
[[nodiscard]] inline std::optional<ZoneChoice> parse_zone_choice(std::string_view body) {
    body = detail::trimmed(body);
    if (const std::optional<NamedZone> named = find_time_zone(body)) {
        return ZoneChoice{std::string(named->posix), std::string(named->name)};
    }
    if (const std::optional<std::string> posix = parse_zone_setting(body)) {
        return ZoneChoice{*posix, std::string()};
    }
    return std::nullopt;
}

// What the board holds after reading its store: a name the table knows says the zone (its string is the table's, so
// that a release with a changed rule moves the boards that chose that city); a name it does not know, or none, is a
// string the person typed, and the name is dropped.
[[nodiscard]] inline ZoneChoice reconcile_zone(std::string_view stored_posix, std::string_view stored_name) {
    if (const std::optional<NamedZone> named = find_time_zone(stored_name)) {
        return ZoneChoice{std::string(named->posix), std::string(named->name)};
    }
    return ZoneChoice{std::string(stored_posix), std::string()};
}

// --- what a person reads --------------------------------------------------------------------------------------------
// A screen or a page never shows a POSIX string (the routes still take and report it, for tools): a zone is read as the
// place it is, or, for one that is in no table, as its offset.

// The place a POSIX string stands for, where the table holds it: for a string that several places share (Melbourne,
// Sydney, Hobart and Canberra are one) the place the generator prefers, the most populous of them
// (data::kPreferred, gen_time_zones.py's PREFERRED), and for a string one place has, that place. Nothing for a string
// the table does not hold. Exact: `AEST-10AEDT,M10.1.0,M4.1.0/3` and nothing near it.
[[nodiscard]] constexpr std::optional<NamedZone> find_time_zone_by_string(std::string_view posix) {
    if (posix.empty()) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < data::kPreferredCount; ++i) {
        if (std::string_view(data::kZones[data::kPreferred[i]].posix) == posix) {
            return time_zone_at(data::kPreferred[i]);
        }
    }
    for (std::size_t i = 0; i < data::kZoneCount; ++i) {
        if (std::string_view(data::kZones[i].posix) == posix) {
            return time_zone_at(i);
        }
    }
    return std::nullopt;
}

// The place a stored zone is: the one the person chose by name when the table knows the name, else the table's place for
// the string (a zone set through a page that kept only the string), else none.
[[nodiscard]] constexpr std::optional<NamedZone> zone_place(std::string_view posix, std::string_view name) {
    if (const std::optional<NamedZone> named = find_time_zone(name)) {
        return named;
    }
    return find_time_zone_by_string(posix);
}

// "Melbourne (Australia)": the city, and in brackets where it is. A city with a place between it and its region already
// says (`Buenos Aires (Argentina)`, `Center (North Dakota)`), and UTC is UTC.
[[nodiscard]] inline std::string zone_display(std::string_view name) {
    const std::string city = zone_label(name);
    const std::string_view region = zone_region(name);
    if (region == kOtherRegion || city.find(" (") != std::string::npos) {
        return city;
    }
    return city + " (" + std::string(region) + ")";
}

// "UTC+10:00", "UTC-03:30": an offset east of UTC, as people say it (a POSIX string counts hours west).
[[nodiscard]] inline std::string offset_text(int east_s) {
    const int minutes = (east_s < 0 ? -east_s : east_s) / 60;
    std::string out = "UTC";
    out += east_s < 0 ? '-' : '+';
    out += static_cast<char>('0' + minutes / 600 % 10);
    out += static_cast<char>('0' + minutes / 60 % 10);
    out += ':';
    out += static_cast<char>('0' + minutes % 60 / 10);
    out += static_cast<char>('0' + minutes % 10);
    return out;
}

// What a screen or a page says for a stored zone: the place (`Melbourne (Australia)`, from the name or, with none, from the
// string), `UTC` for none, and for a zone in no table its offset and what it is (`UTC+10:00 (custom)`, `UTC+10:00 (custom,
// with daylight saving)`). NEVER the string. `posix` may be `none` or empty.
[[nodiscard]] inline std::string zone_readable(std::string_view posix, std::string_view name = {}) {
    if (const std::optional<NamedZone> place = zone_place(posix, name)) {
        return zone_display(place->name);
    }
    if (posix.empty() || posix == kNoZone) {
        return "UTC";
    }
    const std::optional<ZoneRules> rules = parse_zone_rules(posix);
    if (!rules) {
        return "a zone this board does not read";
    }
    std::string out = offset_text(rules->offsets.std_east_s) + " (custom";
    if (rules->offsets.has_dst) {
        out += ", with daylight saving";
    }
    return out + ")";
}

}  // namespace iclforge::wallclock
