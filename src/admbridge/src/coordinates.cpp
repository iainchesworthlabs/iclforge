#include "iclforge/admbridge/coordinates.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace iclforge::admbridge {

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

iclforge::oba::Position adm_cartesian_to_room(const iclforge::adm::CartesianPosition& cartesian) {
    return {.x = (cartesian.x + 1.0) / 2.0,
            .y = (1.0 - cartesian.y) / 2.0,
            .z = cartesian.z};
}

iclforge::adm::CartesianPosition room_to_adm_cartesian(const iclforge::oba::Position& room) {
    return {.x = 2.0 * room.x - 1.0, .y = 1.0 - 2.0 * room.y, .z = room.z};
}

iclforge::oba::Position iab_position_to_room(const iclforge::iab::Position& position) {
    // Direct passthrough - see coordinates.hpp's own comment on iab_position_to_room for why no
    // formula is needed: x/y already share oba::Position's convention exactly, and z is already
    // anchored at the same screen/ear-height zero, just never negative.
    return {.x = position.x, .y = position.y, .z = position.z};
}

iclforge::oba::Position adm_position_to_room(const iclforge::adm::Position& position) {
    return std::visit(
        [](const auto& p) -> iclforge::oba::Position {
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
// src/admbridge/ERRATA.md). kZoneBoundTolerance covers either spelling when reading.
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
        mapping.zone = iclforge::oba::ZoneConstraint::kBackExcluded;
    } else if (horizontal == kSideMask) {
        mapping.zone = iclforge::oba::ZoneConstraint::kSideExcluded;
    } else if (horizontal == kCentreBackMask) {
        mapping.zone = iclforge::oba::ZoneConstraint::kCentreAndBackOnly;
    } else if (horizontal == bit(Zone::kZm4)) {
        mapping.zone = iclforge::oba::ZoneConstraint::kScreenOnly;
    } else if (horizontal == bit(Zone::kZm5)) {
        mapping.zone = iclforge::oba::ZoneConstraint::kSurroundOnly;
    } else {
        // Some other combination of horizontal zones: OAMD carries one preset or none.
        mapping.exact = false;
    }
    return mapping;
}

std::vector<iclforge::adm::ExclusionZone> constraint_to_adm_zone_exclusion(
    iclforge::oba::ZoneConstraint zone, bool enable_elevation) {
    std::vector<iclforge::adm::ExclusionZone> out;
    switch (zone) {
        case iclforge::oba::ZoneConstraint::kNone:
            break;
        case iclforge::oba::ZoneConstraint::kBackExcluded:
            out.push_back(to_exclusion_zone(Zone::kZm1));
            break;
        case iclforge::oba::ZoneConstraint::kSideExcluded:
            out.push_back(to_exclusion_zone(Zone::kZm2Left));
            out.push_back(to_exclusion_zone(Zone::kZm2Right));
            break;
        case iclforge::oba::ZoneConstraint::kCentreAndBackOnly:
            out.push_back(to_exclusion_zone(Zone::kZm3ScreenLeft));
            out.push_back(to_exclusion_zone(Zone::kZm3SideLeft));
            out.push_back(to_exclusion_zone(Zone::kZm3ScreenRight));
            out.push_back(to_exclusion_zone(Zone::kZm3SideRight));
            break;
        case iclforge::oba::ZoneConstraint::kScreenOnly:
            out.push_back(to_exclusion_zone(Zone::kZm4));
            break;
        case iclforge::oba::ZoneConstraint::kSurroundOnly:
            out.push_back(to_exclusion_zone(Zone::kZm5));
            break;
    }
    if (!enable_elevation) {
        out.push_back(to_exclusion_zone(Zone::kZu));
        out.push_back(to_exclusion_zone(Zone::kZb));
    }
    return out;
}

}  // namespace iclforge::admbridge
