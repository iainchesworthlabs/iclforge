#include "iclforge/adm/coordinates.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace iclforge::adm {

iclforge::adm::CartesianPosition polar_to_adm_cartesian(const iclforge::adm::PolarPosition& polar) {
    const double azimuth_rad = polar.azimuth_deg * std::numbers::pi / 180.0;
    const double elevation_rad = polar.elevation_deg * std::numbers::pi / 180.0;
    const double r = polar.distance;
    const double horizontal = r * std::cos(elevation_rad);
    // Clause 8: azimuth 0 = straight ahead (+Y), positive = left; X is right-positive, so
    // "positive azimuth" moves toward negative X. Elevation 0 = level, positive = up (+Z).
    return {.x = -horizontal * std::sin(azimuth_rad),
            .y = horizontal * std::cos(azimuth_rad),
            .z = r * std::sin(elevation_rad)};
}

iclforge::objects::oba::Position adm_cartesian_to_room(
    const iclforge::adm::CartesianPosition& cartesian) {
    return {.x = (cartesian.x + 1.0) / 2.0,
            .y = (1.0 - cartesian.y) / 2.0,
            .z = cartesian.z};
}

iclforge::adm::CartesianPosition room_to_adm_cartesian(
    const iclforge::objects::oba::Position& room) {
    return {.x = 2.0 * room.x - 1.0, .y = 1.0 - 2.0 * room.y, .z = room.z};
}

iclforge::objects::oba::Position iab_position_to_room(const iclforge::iab::Position& position) {
    // Direct passthrough - see coordinates.hpp's own comment on iab_position_to_room for why no
    // formula is needed: x/y already share oba::Position's convention exactly, and z is already
    // anchored at the same screen/ear-height zero, just never negative.
    return {.x = position.x, .y = position.y, .z = position.z};
}

iclforge::objects::oba::ObjectSize iab_spread_to_size(const iclforge::iab::ObjectSpread& spread) {
    if (spread.mode == iclforge::iab::ObjectSpreadMode::kNone) {
        return {};
    }
    return {.width = std::clamp(spread.x, 0.0, 1.0),
            .depth = std::clamp(spread.y, 0.0, 1.0),
            .height = std::clamp(spread.z, 0.0, 1.0)};
}

namespace {

// The include pattern of the seven horizontal zones, in the order screen left, screen centre,
// screen right, left wall, right wall, rear left, rear right.
using HorizontalPattern = std::array<bool, 7>;

struct Preset {
    iclforge::objects::oba::ZoneConstraint zone;
    HorizontalPattern pattern;
};

constexpr std::array<Preset, 6> kPresets{{
    {iclforge::objects::oba::ZoneConstraint::kNone, {true, true, true, true, true, true, true}},
    {iclforge::objects::oba::ZoneConstraint::kBackExcluded,
     {true, true, true, true, true, false, false}},
    {iclforge::objects::oba::ZoneConstraint::kSideExcluded,
     {true, true, true, false, false, true, true}},
    {iclforge::objects::oba::ZoneConstraint::kCentreAndBackOnly,
     {false, true, false, false, false, true, true}},
    {iclforge::objects::oba::ZoneConstraint::kScreenOnly,
     {true, true, true, false, false, false, false}},
    {iclforge::objects::oba::ZoneConstraint::kSurroundOnly,
     {false, false, false, true, true, true, true}},
}};

// Where a horizontal zone sits in the room plan, as the loudspeaker it is named for (the same
// positions bed_label_position() gives those labels). Only distances between zones are used.
[[nodiscard]] iclforge::objects::oba::Position zone_centre(std::size_t zone) {
    using iclforge::objects::oba::BedLabel;
    constexpr std::array<BedLabel, 7> kLabels{BedLabel::kL,  BedLabel::kC,  BedLabel::kR,
                                              BedLabel::kLs, BedLabel::kRs, BedLabel::kLb,
                                              BedLabel::kRb};
    return iclforge::objects::oba::bed_label_position(kLabels[zone]);
}

[[nodiscard]] bool covers(const HorizontalPattern& preset, const HorizontalPattern& wanted) {
    for (std::size_t i = 0; i < wanted.size(); ++i) {
        if (wanted[i] && !preset[i]) {
            return false;
        }
    }
    return true;
}

// How far a preset that covers `wanted` lets the object into zones the author excluded: for each
// such zone, its distance to the nearest zone the author included.
[[nodiscard]] double leak_distance(const HorizontalPattern& preset,
                                   const HorizontalPattern& wanted) {
    double total = 0.0;
    for (std::size_t z = 0; z < preset.size(); ++z) {
        if (!preset[z] || wanted[z]) {
            continue;
        }
        const auto from = zone_centre(z);
        double nearest = std::numeric_limits<double>::max();
        for (std::size_t w = 0; w < wanted.size(); ++w) {
            if (!wanted[w]) {
                continue;
            }
            const auto to = zone_centre(w);
            nearest = std::min(nearest, std::hypot(from.x - to.x, from.y - to.y));
        }
        total += nearest;
    }
    return total;
}

// A pattern that is exactly a preset maps to it. Any other pattern cannot be said in OAMD, which
// has the six presets and nothing finer, so it maps to the preset that excludes only zones the
// author excluded (it covers every included zone) and lets the object into the nearest extra zones;
// `exact` is false. kNone covers every pattern, so one no other preset covers gets kNone.
[[nodiscard]] IabZoneMapping match_preset(const HorizontalPattern& pattern, bool elevation) {
    IabZoneMapping mapping;
    mapping.enable_elevation = elevation;
    for (const auto& preset : kPresets) {
        if (preset.pattern == pattern) {
            mapping.zone = preset.zone;
            return mapping;
        }
    }
    mapping.exact = false;
    if (std::none_of(pattern.begin(), pattern.end(), [](bool included) { return included; })) {
        return mapping;  // no zone included: there is nothing to cover
    }
    double best = std::numeric_limits<double>::max();
    for (const auto& preset : kPresets) {
        if (!covers(preset.pattern, pattern)) {
            continue;
        }
        const double leak = leak_distance(preset.pattern, pattern);
        if (leak < best) {
            best = leak;
            mapping.zone = preset.zone;
        }
    }
    return mapping;
}

[[nodiscard]] bool included(double gain) { return gain >= kIabZoneIncludeThreshold; }

}  // namespace

IabZoneMapping iab_zones_to_constraint(const std::array<double, iclforge::iab::kZoneCount>& gains) {
    HorizontalPattern pattern{};
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        pattern[i] = included(gains[i]);
    }
    return match_preset(pattern, included(gains[7]) || included(gains[8]));
}

IabZoneMapping iab_zones19_to_constraint(const std::array<double, iclforge::iab::kZone19Count>& gains) {
    // Table 28 order: 0-2 base screen, 3-5 height screen, 6-8 base rear, 9-11 height rear, 12 base
    // left wall, 13 height left wall, 14 base right wall, 15 height right wall, 16-18 ceiling.
    // The presets treat the rear as one group, so a rear with any zone included asks for all of it,
    // and one with some zones in and some out cannot be said exactly.
    const bool rear_all = included(gains[6]) && included(gains[7]) && included(gains[8]);
    const bool rear_any = included(gains[6]) || included(gains[7]) || included(gains[8]);
    HorizontalPattern pattern{included(gains[0]),
                              included(gains[1]),
                              included(gains[2]),
                              included(gains[12]),
                              included(gains[14]),
                              rear_any,
                              rear_any};
    bool elevation = false;
    for (const std::size_t index : {3U, 4U, 5U, 9U, 10U, 11U, 13U, 15U, 16U, 17U, 18U}) {
        elevation = elevation || included(gains[index]);
    }
    IabZoneMapping mapping = match_preset(pattern, elevation);
    if (rear_any && !rear_all) {
        mapping.exact = false;
    }
    return mapping;
}

iclforge::objects::oba::Position adm_position_to_room(const iclforge::adm::Position& position) {
    return std::visit(
        [](const auto& p) -> iclforge::objects::oba::Position {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, iclforge::adm::PolarPosition>) {
                return adm_cartesian_to_room(polar_to_adm_cartesian(p));
            } else {
                static_assert(std::is_same_v<T, iclforge::adm::CartesianPosition>);
                return adm_cartesian_to_room(p);
            }
        },
        position);
}

namespace {

// TS 103 420 Table B.19. ZM3_SideRight's minX is printed 0.5611; its mirror ZM3_SideLeft's maxX is
// -0.51611, and every other left/right pair in the table is symmetric, so 0.51611 is used (see
// libs/adm/ERRATA.md). kZoneBoundTolerance covers either spelling when reading.
enum class Zone : std::uint8_t {
    kZm1,
    kZm2Left,
    kZm2Right,
    kZm3ScreenLeft,
    kZm3SideLeft,
    kZm3ScreenRight,
    kZm3SideRight,
    kZm4,
    kZm5,
    kZu,
    kZb,
};

struct ZoneRow {
    Zone zone;
    std::string_view label;
    double min_x, max_x, min_y, max_y, min_z, max_z;
};

constexpr std::array<ZoneRow, 11> kZoneTable{{
    {Zone::kZm1, "ZM1", -1, 1, -1, -0.41934, -0.49900, 0.49900},
    {Zone::kZm2Left, "ZM2_Left", -1, -0.75806, -0.41934, 0.83871, -0.49900, 0.49900},
    {Zone::kZm2Right, "ZM2_Right", 0.75806, 1, -0.41934, 0.83871, -0.49900, 0.49900},
    {Zone::kZm3ScreenLeft, "ZM3_ScreenLeft", -1, -0.16129, 0.5, 1, -0.49900, 0.49900},
    {Zone::kZm3SideLeft, "ZM3_SideLeft", -1, -0.51611, -0.70700, 0.49999, -0.49900, 0.49900},
    {Zone::kZm3ScreenRight, "ZM3_ScreenRight", 0.16129, 1, 0.5, 1, -0.49900, 0.49900},
    {Zone::kZm3SideRight, "ZM3_SideRight", 0.51611, 1, -0.70700, 0.49999, -0.49900, 0.49900},
    {Zone::kZm4, "ZM4", -1, 1, -1, 0.83871, -0.49900, 0.49900},
    {Zone::kZm5, "ZM5", -1, 1, 0.5, 1, -0.49900, 0.49900},
    {Zone::kZu, "ZU", -1, 1, -1, 1, 0.49950, 1},
    {Zone::kZb, "ZB", -1, 1, -1, 1, -1, -0.49950},
}};

[[nodiscard]] bool equal_ignoring_case(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               const auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
               return lower(x) == lower(y);
           });
}

[[nodiscard]] bool bounds_match(const iclforge::adm::ExclusionZone& zone, const ZoneRow& row) {
    const auto close = [](double a, double b) { return std::abs(a - b) <= kZoneBoundTolerance; };
    return zone.has_bounds && close(zone.min_x, row.min_x) && close(zone.max_x, row.max_x) &&
           close(zone.min_y, row.min_y) && close(zone.max_y, row.max_y) &&
           close(zone.min_z, row.min_z) && close(zone.max_z, row.max_z);
}

// A recognised zone, or nullptr.
[[nodiscard]] const ZoneRow* recognise(const iclforge::adm::ExclusionZone& zone) {
    for (const auto& row : kZoneTable) {
        if (!zone.label.empty() && equal_ignoring_case(zone.label, row.label)) {
            return &row;
        }
    }
    for (const auto& row : kZoneTable) {
        if (bounds_match(zone, row)) {
            return &row;
        }
    }
    return nullptr;
}

[[nodiscard]] constexpr std::uint32_t bit(Zone zone) {
    return 1U << static_cast<unsigned>(zone);
}

constexpr std::uint32_t kSideMask = bit(Zone::kZm2Left) | bit(Zone::kZm2Right);
constexpr std::uint32_t kCentreBackMask = bit(Zone::kZm3ScreenLeft) | bit(Zone::kZm3SideLeft) |
                                          bit(Zone::kZm3ScreenRight) | bit(Zone::kZm3SideRight);
constexpr std::uint32_t kTopBottomMask = bit(Zone::kZu) | bit(Zone::kZb);
constexpr std::uint32_t kHorizontalMask = bit(Zone::kZm1) | kSideMask | kCentreBackMask |
                                          bit(Zone::kZm4) | bit(Zone::kZm5);

[[nodiscard]] iclforge::adm::ExclusionZone to_exclusion_zone(Zone zone) {
    for (const auto& row : kZoneTable) {
        if (row.zone == zone) {
            return {.label = std::string(row.label),
                    .has_bounds = true,
                    .min_x = row.min_x,
                    .max_x = row.max_x,
                    .min_y = row.min_y,
                    .max_y = row.max_y,
                    .min_z = row.min_z,
                    .max_z = row.max_z};
        }
    }
    return {};
}

}  // namespace

AdmZoneMapping adm_zone_exclusion_to_constraint(std::span<const iclforge::adm::ExclusionZone> zones) {
    AdmZoneMapping mapping;
    std::uint32_t present = 0;
    for (const auto& zone : zones) {
        const auto* row = recognise(zone);
        if (row == nullptr) {
            mapping.exact = false;
            continue;
        }
        present |= bit(row->zone);
    }

    // Top-Bottom: both zones, or neither. One alone has no OAMD image, since b_enable_elevation is
    // a single switch.
    const auto top_bottom = present & kTopBottomMask;
    if (top_bottom == kTopBottomMask) {
        mapping.enable_elevation = false;
    } else if (top_bottom != 0) {
        mapping.exact = false;
    }

    const auto horizontal = present & kHorizontalMask;
    if (horizontal == 0) {
        return mapping;
    }
    if (horizontal == bit(Zone::kZm1)) {
        mapping.zone = iclforge::objects::oba::ZoneConstraint::kBackExcluded;
    } else if (horizontal == kSideMask) {
        mapping.zone = iclforge::objects::oba::ZoneConstraint::kSideExcluded;
    } else if (horizontal == kCentreBackMask) {
        mapping.zone = iclforge::objects::oba::ZoneConstraint::kCentreAndBackOnly;
    } else if (horizontal == bit(Zone::kZm4)) {
        mapping.zone = iclforge::objects::oba::ZoneConstraint::kScreenOnly;
    } else if (horizontal == bit(Zone::kZm5)) {
        mapping.zone = iclforge::objects::oba::ZoneConstraint::kSurroundOnly;
    } else {
        // Some other combination of horizontal zones: OAMD carries one preset or none.
        mapping.exact = false;
    }
    return mapping;
}

std::vector<iclforge::adm::ExclusionZone> constraint_to_adm_zone_exclusion(
    iclforge::objects::oba::ZoneConstraint zone, bool enable_elevation) {
    std::vector<iclforge::adm::ExclusionZone> out;
    switch (zone) {
        case iclforge::objects::oba::ZoneConstraint::kNone:
            break;
        case iclforge::objects::oba::ZoneConstraint::kBackExcluded:
            out.push_back(to_exclusion_zone(Zone::kZm1));
            break;
        case iclforge::objects::oba::ZoneConstraint::kSideExcluded:
            out.push_back(to_exclusion_zone(Zone::kZm2Left));
            out.push_back(to_exclusion_zone(Zone::kZm2Right));
            break;
        case iclforge::objects::oba::ZoneConstraint::kCentreAndBackOnly:
            out.push_back(to_exclusion_zone(Zone::kZm3ScreenLeft));
            out.push_back(to_exclusion_zone(Zone::kZm3SideLeft));
            out.push_back(to_exclusion_zone(Zone::kZm3ScreenRight));
            out.push_back(to_exclusion_zone(Zone::kZm3SideRight));
            break;
        case iclforge::objects::oba::ZoneConstraint::kScreenOnly:
            out.push_back(to_exclusion_zone(Zone::kZm4));
            break;
        case iclforge::objects::oba::ZoneConstraint::kSurroundOnly:
            out.push_back(to_exclusion_zone(Zone::kZm5));
            break;
    }
    if (!enable_elevation) {
        out.push_back(to_exclusion_zone(Zone::kZu));
        out.push_back(to_exclusion_zone(Zone::kZb));
    }
    return out;
}

}  // namespace iclforge::adm
