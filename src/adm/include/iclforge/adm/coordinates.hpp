#pragma once

#include <array>
#include <span>
#include <vector>

#include "iclforge/adm/export.hpp"
#include "iclforge/objects/oamd.hpp"
#include "iclforge/adm/model.hpp"
#include "iclforge/iab/model.hpp"

// Coordinate conversion between the two position systems Recommendation ITU-R BS.2076-2 (10/2019)
// Annex 1 defines for audioBlockFormat (Tables 15-17, and Clause 8 "Coordinate system" for the
// sign conventions those tables' column headings alone don't spell out) and
// iclforge::objects::oba::Position's own room-anchored convention (ac3/oba/oamd.hpp, ETSI TS 103 420
// clause 4.2.1).
//
// Clause 8, verified directly against the published Recommendation text (not transcribed from
// any secondary source):
//
//   "Azimuth - angle in the horizontal plane with 0 degrees as straight ahead, and positive
//   angles to the left (or anti-clockwise) when viewed from above."
//   "Elevation - angle in the vertical plane with 0 degrees horizontally ahead, and positive
//   angles going up."
//   "Distance - a normalized distance, where 1.0 is assumed to be the default radius of the
//   sphere."
//   "X - left to right, with positive values to the right."
//   "Y - front to back, with positive values to the front."
//   "Z - top to bottom, with positive values to the top."
//
// (Table 16 adds the range: "the values 1.0 and -1.0 are on the surface of the cube.")
//
// iclforge::objects::oba::Position (oamd.hpp): x runs 0 (left wall) to 1 (right wall), y runs 0 (front wall)
// to 1 (back wall), z runs -1 (floor) to +1 (ceiling) - left-handed, normalized to the room cuboid,
// with (0.5, 0, 0) the centre of the front wall.
//
// polar_to_adm_cartesian() turns a polar/spherical position into the same right-positive/
// front-positive/top-positive point BS.2076-2's own Cartesian axes describe, via the ordinary
// physics spherical-to-Cartesian conversion adapted for azimuth's "positive = left" sign (BS.
// 2076-2 does not itself spell out this intermediate step - Tables 15 and 16 are presented as two
// independent, alternative ways to say the same thing, not as one derived from the other - so the
// exact formula below is this module's own, checked two ways: (1) against Clause 8's stated axis
// directions at the cardinal points (0 deg azimuth = straight ahead = +Y; +90 deg azimuth = left
// = -X, since X is right-positive; +90 deg elevation = up = +Z), and (2) empirically, in this
// module's own tests, against the existing ring-position constants this project's
// tests/ac3/oba/test_atmos_motion.cpp already hardcodes (kL/kR/kSR) - converting BS.2076-2's own
// M+030/ M-030/M-110 speaker-label azimuths (Annex A common definitions) through this formula
// reproduces those exact room coordinates.
//
// adm_cartesian_to_room() then rescales that point from BS.2076-2's [-1, 1] unit cube (both axes
// signed, origin at the room's centre) onto iclforge::objects::oba::Position's own [0, 1] (x, y) / [-1, 1]
// (z) convention (origin off-centre on x/y, centred on z) - a pure affine remap, not a design
// choice: x_room = (x_adm + 1) / 2, y_room = (1 - y_adm) / 2 (BS.2076-2's Y is front-positive,
// oba's y is front-zero/back-one, hence the sign flip), z_room = z_adm (both top-positive, both
// already
// [-1, 1] - no rescale needed).
//
// One genuine, documented judgement call: BS.2076-2 nowhere equates a polar position's unit
// SPHERE (distance = 1.0 on its surface) with a Cartesian position's unit CUBE (|x|, |y| or |z| =
// 1.0 on its surface) - they coincide only exactly on each axis (e.g. straight ahead at distance
// 1.0 sits on both the sphere's and the cube's front face), not off-axis (e.g. a 45-degree-azimuth
// object at distance 1.0 sits well inside the cube's own surface, at Euclidean distance 1.0 from
// centre rather than at the cube's own corner). This module treats the polar-derived point as a
// Cartesian point of the same coordinates without renormalizing for that difference - the
// simplest reading, and the one that reproduces the existing kL/kR/kSR ring constants exactly for
// on-axis azimuths, which is the case that matters for real DirectSpeakers content (every
// standard loudspeaker position BS.2076-2's own Annex A common definitions use is on-axis: pure
// left/right, pure front/back, or pure up/down combinations).
namespace iclforge::adm {

// BS.2076-2 Clause 8's polar convention to the same right/front/top-positive point its own
// Cartesian axes describe. See this header's own top comment for the full derivation and the
// three independent checks performed against it.
[[nodiscard]] ICLFORGE_ADM_EXPORT iclforge::adm::CartesianPosition polar_to_adm_cartesian(
    const iclforge::adm::PolarPosition& polar);

// BS.2076-2's [-1, 1] unit-cube Cartesian convention to iclforge::objects::oba::Position's [0, 1]/[0, 1]/
// [-1, 1] room-anchored one. Pure affine remap - see this header's own top comment.
[[nodiscard]] ICLFORGE_ADM_EXPORT iclforge::objects::oba::Position adm_cartesian_to_room(
    const iclforge::adm::CartesianPosition& cartesian);

// Dispatches on iclforge::adm::Position's own variant (ac3adm/model.hpp: PolarPosition or
// CartesianPosition, selected by AudioBlockFormat::cartesian) and converts whichever alternative
// is actually present straight to room coordinates.
[[nodiscard]] ICLFORGE_ADM_EXPORT iclforge::objects::oba::Position adm_position_to_room(
    const iclforge::adm::Position& position);

// The write-direction inverse of adm_cartesian_to_room() above, for the JOC ->
// ADM BWF writer: x_adm = 2*x_room - 1, y_adm = 1 - 2*y_room, z_adm = z_room - the algebraic
// inverse of the affine remap this header's own top comment derives, not a second, independently
// checked formula. This writer only ever emits cartesian ADM (the Dolby Atmos Master ADM Profile's
// own shape), so unlike the read side there is no matching room_to_adm_polar()/room_position_to_adm()
// pair - a caller wanting a polar master would need one, and none of this project's own writers do.
[[nodiscard]] ICLFORGE_ADM_EXPORT iclforge::adm::CartesianPosition room_to_adm_cartesian(
    const iclforge::objects::oba::Position& room);

// SMPTE ST 2098-2:2022 §11.1's unit cube to iclforge::objects::oba::Position's own room-anchored convention
// - for IAB reader bridge, phase 3 ("atmos-iab", mapping the IAB bed/object graph onto this same
// ObjectPath layer). Unlike BS.2076-2's Cartesian convention above, this needs no formula at all:
// §11.1 defines IAB's x ("0 corresponds to left wall... 1 corresponds to right wall") and y ("0
// corresponds to front wall... 1 corresponds to back wall") identically to oba::Position's own
// [0, 1]/[0, 1] x/y (oamd.hpp §4.2.1: "x runs 0 at the left wall to 1 at the right, y 0 at the
// front wall to 1 at the back"), and z - "z=0 corresponds to a horizontal plane at... the height
// of the main screen Loudspeakers, the side and rear surround Loudspeakers; z=1 corresponds to
// the ceiling" - anchors its zero at exactly the same screen/ear-height reference oba::Position's
// own z=0 does (oamd.hpp: "the centre of the front wall is (0.5, 0, 0)"), just never expressing
// anything below it: IAB's [0, 1] is the upper half of oba::Position's own [-1 floor, +1 ceiling]
// range. This is this bridge's own judgement call, not a clause either standard states outright
// (no clause equates the two specs' height references directly) - the same status
// coordinates.hpp's own "one genuine, documented judgement call" above has for ADM, stated
// plainly rather than asserted as spec fact. See iab_bridge.cpp's own top comment for where this
// is used; the spread and zone mappings follow below.
[[nodiscard]] ICLFORGE_ADM_EXPORT iclforge::objects::oba::Position iab_position_to_room(
    const iclforge::iab::Position& position);

// SMPTE ST 2098-2:2022 §10.5.16-17: ObjectSpread is the extent of the object on each axis, as a
// fraction of the unit cube ("+/- 0.5 * ObjectSpread from the object's position"), 0 for a point
// source. ETSI TS 103 420 §5.6.1.2 codes object_width, object_depth and object_height as the same
// normalized [0, 1] extent on the room's x, y and z axes, so IAB's spread on x, y and z is
// width, depth and height. The same rename the ADM bridge makes for BS.2076-2's width, depth and
// height. This is this bridge's own reading; neither clause equates the two scales.
[[nodiscard]] ICLFORGE_ADM_EXPORT iclforge::objects::oba::ObjectSize iab_spread_to_size(
    const iclforge::iab::ObjectSpread& spread);

// A zone control mapped onto TS 103 420's zone constraints.
struct IabZoneMapping {
    iclforge::objects::oba::ZoneConstraint zone = iclforge::objects::oba::ZoneConstraint::kNone;
    bool enable_elevation = true;
    // False when the gains matched none of Table 20's six presets, so `zone` is kNone: the object
    // is left unconstrained rather than approximated by a preset that excludes the wrong zones.
    bool exact = true;
};

// A zone gain at or above this counts as the zone being included. TS 103 420 Table 20 only
// includes or excludes a zone; ST 2098-2 gives each zone a gain from 0 to 1.
inline constexpr double kIabZoneIncludeThreshold = 0.5;

// ST 2098-2 §10.5.11-14 Table 24's nine zones (screen left/centre/right, left wall, right wall,
// rear left, rear right, overhead left, overhead right) onto TS 103 420 §5.6.1.6 Table 20 and
// Table 21. The horizontal zones map to a preset only when their include/exclude pattern is
// exactly that preset's:
//   none               every horizontal zone
//   back excluded      all but the two rear zones
//   side excluded      all but the two wall zones
//   centre and back    screen centre and the two rear zones
//   screen only        the three screen zones
//   surround only      the two wall zones and the two rear zones
// The overhead zones set b_enable_elevation: on when either is included.
[[nodiscard]] ICLFORGE_ADM_EXPORT IabZoneMapping iab_zones_to_constraint(
    const std::array<double, iclforge::iab::kZoneCount>& gains);

// The same mapping for ObjectZoneDefinition19's 19 zones (§10.6 Table 28), which replace the nine
// zones when present. The base layer zones carry the horizontal pattern - screen from zones 0-2,
// the left and right walls from 12 and 14, the rear from 6-8, a rear or screen group counting as
// included only when all its zones are - and every height layer or ceiling zone feeds
// b_enable_elevation.
[[nodiscard]] ICLFORGE_ADM_EXPORT IabZoneMapping iab_zones19_to_constraint(
    const std::array<double, iclforge::iab::kZone19Count>& gains);

// ETSI TS 103 420 V1.2.1 Annex B.2.6, Tables B.18 and B.19: OAMD's zone constraint (§5.6.1.6,
// Tables 20 and 21) expressed as an ADM zoneExclusion, and back. ADM lists the zones that are
// EXCLUDED; OAMD names the one horizontal zone group that is kept (or dropped) plus a separate
// Top-Bottom switch. The two meet exactly only for the presets Table B.18 lists:
//
//   ZM1                                  back excluded            kBackExcluded
//   ZM2_Left + ZM2_Right                 side excluded            kSideExcluded
//   ZM3_ScreenLeft/_SideLeft/_ScreenRight/_SideRight
//                                        all but centre-and-back  kCentreAndBackOnly
//   ZM4                                  all but screen           kScreenOnly
//   ZM5                                  all but surround         kSurroundOnly
//   ZU + ZB                              Top-Bottom excluded      enable_elevation = false
//
// A zone is recognised by its label ("ZM1", "ZM2_Left", ...) or, failing that, by its six
// Cartesian bounds matching Table B.19 within kZoneBoundTolerance. `exact` is false when any zone
// was not recognised, or the recognised ones are not one of the combinations above (an arbitrary
// box, only ZU without ZB, two different horizontal presets at once): what could be mapped is
// still returned, the rest is dropped, and the caller decides whether to warn.
struct ICLFORGE_ADM_EXPORT AdmZoneMapping {
    iclforge::objects::oba::ZoneConstraint zone = iclforge::objects::oba::ZoneConstraint::kNone;
    bool enable_elevation = true;
    bool exact = true;
};

inline constexpr double kZoneBoundTolerance = 0.05;

[[nodiscard]] ICLFORGE_ADM_EXPORT AdmZoneMapping adm_zone_exclusion_to_constraint(
    std::span<const iclforge::adm::ExclusionZone> zones);

// The write direction: the zoneExclusion that says what `zone` and `enable_elevation` say. Empty
// for kNone with elevation enabled, which is the ADM default (no zoneExclusion element).
[[nodiscard]] ICLFORGE_ADM_EXPORT std::vector<iclforge::adm::ExclusionZone>
constraint_to_adm_zone_exclusion(iclforge::objects::oba::ZoneConstraint zone, bool enable_elevation);

}  // namespace iclforge::adm
