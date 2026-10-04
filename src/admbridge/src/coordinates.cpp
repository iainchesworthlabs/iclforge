#include "iclforge/admbridge/coordinates.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <type_traits>
#include <variant>

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

iclforge::oba::ObjectSize iab_spread_to_size(const iclforge::iab::ObjectSpread& spread) {
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
    iclforge::oba::ZoneConstraint zone;
    HorizontalPattern pattern;
};

constexpr std::array<Preset, 6> kPresets{{
    {iclforge::oba::ZoneConstraint::kNone, {true, true, true, true, true, true, true}},
    {iclforge::oba::ZoneConstraint::kBackExcluded, {true, true, true, true, true, false, false}},
    {iclforge::oba::ZoneConstraint::kSideExcluded, {true, true, true, false, false, true, true}},
    {iclforge::oba::ZoneConstraint::kCentreAndBackOnly, {false, true, false, false, false, true, true}},
    {iclforge::oba::ZoneConstraint::kScreenOnly, {true, true, true, false, false, false, false}},
    {iclforge::oba::ZoneConstraint::kSurroundOnly, {false, false, false, true, true, true, true}},
}};

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
    const bool rear = included(gains[6]) && included(gains[7]) && included(gains[8]);
    HorizontalPattern pattern{included(gains[0]), included(gains[1]), included(gains[2]), included(gains[12]),
                              included(gains[14]), rear, rear};
    // A rear group with some zones in and some out matches no preset.
    const bool rear_mixed = !rear && (included(gains[6]) || included(gains[7]) || included(gains[8]));
    bool elevation = false;
    for (const std::size_t index : {3U, 4U, 5U, 9U, 10U, 11U, 13U, 15U, 16U, 17U, 18U}) {
        elevation = elevation || included(gains[index]);
    }
    IabZoneMapping mapping = match_preset(pattern, elevation);
    if (rear_mixed) {
        mapping.zone = iclforge::oba::ZoneConstraint::kNone;
        mapping.exact = false;
    }
    return mapping;
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

}  // namespace iclforge::admbridge
