#pragma once

// The time zones a board offers by name (planning/p4x-now-playing.md, stage 3 and the Preferences programme): a
// region and a city ("Australia/Melbourne") and the POSIX string that zone's rules end in
// (iclforge/wall_clock.hpp reads it; the board's C library applies it). The data is time_zone_data.hpp, written
// by tools/hearth/gen_time_zones.py from IANA's tzdata and checked in; this header is what is done with it.
// Standard library only, so that the host tests it (libs/device/tests/test_time_zones.cpp).
//
// What is stored is the string (`time_zone`, which the clock reads) and the name the person chose
// (`time_zone_name`): the string alone cannot say which city was chosen, because 88 strings stand for 420 names
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

}  // namespace iclforge::wallclock
