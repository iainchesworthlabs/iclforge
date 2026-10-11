#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "iclforge/base/layout.hpp"
#include "iclforge/render/export.hpp"

// The spatial/object layer: applications place and move mono sources around
// the listener; the renderer turns the scene into a 5.1 channel bed that
// feeds the AC-3 encoder (or any other sink — nothing here knows about
// AC-3 except the bed's channel order).
//
// Design:
// - 2D pairwise amplitude panning (VBAP on the horizontal ring — 5.1 has no
//   height): pick the adjacent speaker pair around the target azimuth, solve
//   the 2x2 system, clamp, and normalize to Σg² = 1 (energy preservation).
// - Speaker geometry per ITU-R BS.775: C 0°, L +30°, R −30°, SL +110°,
//   SR −110° (azimuth counterclockwise from front, degrees).
// - Objects never feed the LFE implicitly; an explicit lfe_send exists.
// - Automation is clocked at the 256-sample block: targets set between
//   blocks, applied with per-sample linear ramps (no zipper noise).
// - The render path performs no allocation.

namespace iclforge::spatial {

inline constexpr int kBedChannels = 5;  // AC-3 3/2 order: L, C, R, SL, SR
inline constexpr int kBlockSamples = 256;

// The ITU-R BS.775 ring, in AC-3 3/2 channel order, degrees counterclockwise
// from front. Everything spatial — the panner here, and the soundfield
// analysis the front ends draw — is defined against this one array so the
// geometry cannot drift between them.
inline constexpr std::array<double, kBedChannels> kSpeakerAzimuthDeg = {
    30.0,    // L
    0.0,     // C
    -30.0,   // R
    110.0,   // SL
    -110.0,  // SR
};

// Per-speaker gains for one source direction, AC-3 3/2 channel order.
using PanGains = std::array<double, kBedChannels>;

// Energy-normalized pairwise pan of a direction (degrees, CCW from front,
// any value; normalized internally) onto the 5.1 ring.
[[nodiscard]] ICLFORGE_RENDER_EXPORT PanGains pan_azimuth(double azimuth_deg);

// The same pan onto an ARBITRARY horizontal ring, which is what any layout
// wider than 5.1 needs: 7.1 puts its side surrounds at 90° and its rears at
// 150°, so a source at 110° belongs to a different pair there than it does on
// the 5.1 ring. `ring_azimuth_deg` may be in any order and any range; `gains`
// takes one entry per ring member and is OVERWRITTEN, with Sum(g^2) == 1 for a
// non-empty ring.
//
// Two speakers more than 180° apart leave an arc no pair can enclose - the
// hole behind a front-only pair being the obvious case, where the VBAP system
// is singular and both gains solve negative. Across such an arc this
// crossfades at constant power instead, which agrees with the pairwise
// solution at both edges and never drops the source into silence.
ICLFORGE_RENDER_EXPORT void pan_ring(double azimuth_deg, std::span<const double> ring_azimuth_deg,
                              std::span<double> gains);

// The same pan, addressed by a room-anchored position instead of an angle:
// x runs 0 at the left wall to 1 at the right and y 0 at the front wall to 1
// at the back, which is TS 103 420 §4.2.1's system. A source at the exact
// centre of the room has no direction at all and stays at the front.
//
// There is no z. A 5.1 ring has no height speakers, so elevation cannot be
// rendered and a raised source folds onto the ring at its azimuth, at full
// level - a legacy 5.1 decoder has to hear everything, or backward
// compatibility means nothing. The height survives in the object metadata
// instead, which is the entire reason the object layer exists.
//
// The consequence is worth stating plainly: two sources at the same azimuth
// and different heights get IDENTICAL bed gains, and nothing downstream can
// tell them apart from the bed alone.
[[nodiscard]] ICLFORGE_RENDER_EXPORT PanGains pan_room(double x, double y);

// --- arbitrary-layout, height-aware panning ---------------------------------
//
// Everything above targets the fixed 5.1 ring. A source with real elevation -
// a Table E2.5 height location, or an object whose z lifts it toward the
// ceiling - needs a second, upper ring and a crossfade between the two, which
// is what iclforge::ac3::plan's channel-layout renderer already built to move a bed's
// channels between differently-shaped layouts (5.1 to 7.1.4 and back). It is
// promoted here rather than duplicated because IO12's object-based loudness
// measurement needs the identical geometry: an object panned onto a wide
// layout by its own position must agree with what the encoder itself would
// have rendered that position to.

// A source or speaker direction: azimuth counterclockwise from front (ITU-R
// BS.775, any range), elevation above the listener's plane in degrees (0 on
// the ring, positive toward the ceiling).
struct Direction {
    double azimuth_deg = 0.0;
    double elevation_deg = 0.0;
};

// The nominal elevation of the upper layer - TS 103 420 renders heights well
// above the ring, and 45 degrees is the conventional Atmos ceiling angle.
inline constexpr double kHeightElevationDeg = 45.0;
// Everything at or above this counts as the upper layer. Half way to the
// nominal height angle, so no real location is ambiguous.
inline constexpr double kHeightThresholdDeg = kHeightElevationDeg / 2.0;
// A gain below this is not a quiet signal, it is arithmetic (cos(pi/2) lands
// near 6e-17 rather than on zero).
inline constexpr double kNegligibleGain = 1e-9;

// Where a Table E2.5 location sits, in the same (azimuth, elevation) terms as
// `Direction` above. `has_rears`/`has_side_discrete` disambiguate the two
// locations whose direction depends on what else is in the same layout - see
// the .cpp for why: without a discrete rear pair, Ls/Rs sit at the 5.1 ring's
// own +-110 degrees; with one, they move to the side (+-90) and the rear pair
// takes +-135/180 instead. The two LFE-type locations have no direction and
// return {0, 0}; a caller that means to pan a source should exclude them
// first (see PanTargets below), since an LFE-type entry here says nothing
// about where the LFE speaker is.
[[nodiscard]] ICLFORGE_RENDER_EXPORT Direction direction_of(base::Location location, bool has_rears,
                                                     bool has_side_discrete);

// A layout's full-bandwidth locations and the direction each one sits at,
// LFE-type locations excluded - the set pan_direction below actually spreads
// a source over.
struct PanTargets {
    std::vector<base::Location> locations;
    std::vector<Direction> directions;

    // Where a location sits in this set, or -1 if it takes no panned audio.
    [[nodiscard]] int index_of(base::Location location) const {
        for (std::size_t i = 0; i < locations.size(); ++i) {
            if (locations[i] == location) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }
};

[[nodiscard]] ICLFORGE_RENDER_EXPORT PanTargets pan_targets(std::span<const base::Location> locations);

// One source direction spread over a target speaker set. Two rings - the
// listener's plane and the ceiling - each panned by azimuth (via pan_ring
// above), crossfaded by elevation at constant power. `gains` is sized to
// `targets` and OVERWRITTEN.
//
// A target with no upper layer takes the whole source at full level rather
// than a cosine-attenuated share: a 5.1 ring has no height speakers, and a
// legacy decoder has to hear everything or backward compatibility means
// nothing - the same rule pan_room states for the 5.1 bed.
ICLFORGE_RENDER_EXPORT void pan_direction(Direction source, std::span<const Direction> targets,
                                    std::span<double> gains);

// The direction an object-audio-metadata room position (TS 103 420 §4.2.1:
// x/y in [0, 1] as pan_room above, z -1 at the floor to +1 at the ceiling)
// sits at, for panning it with pan_direction rather than folding it onto the
// flat 5.1 ring the way pan_room does.
//
// Azimuth is exactly pan_room's own atan2(left, forward) - the two must agree
// or an object would point one way in a 5.1 fold and another in a wider
// render. Elevation reads z against the horizontal distance from room centre,
// atan2(z, horizontal): an object at the room's centre height (z = 0)
// measures 0 degrees whatever its azimuth, matching a DynamicObject's own
// default position, and one directly overhead (x = y = 0.5, z = 1) measures a
// full 90 regardless of the elevation angle this file otherwise treats as
// "the ceiling" - the two are independent numbers that only happen to agree
// at kHeightElevationDeg for a source at the room's outer edge, which is
// where every named height location in Table E2.5 sits.
[[nodiscard]] ICLFORGE_RENDER_EXPORT Direction position_direction(double x, double y, double z);

// --- zone constraints (TS 103 420 §5.2.6, Annex A.2) -------------------------
//
// An object can carry a constraint on WHERE a renderer may put it: which of
// the horizontal zones, and whether the Top-Bottom one. The standard defines
// the zones by the speakers in them (Table A.7), so the rule is exact for a
// speaker layout and needs no geometry of the renderer's own. `zone_bit::`
// values are OR-ed into the mask speaker_zones() returns.
namespace zone_bit {
inline constexpr unsigned kScreen = 1U << 0;
inline constexpr unsigned kSide = 1U << 1;
inline constexpr unsigned kSurround = 1U << 2;
inline constexpr unsigned kBack = 1U << 3;
inline constexpr unsigned kTopBottom = 1U << 4;
// Not a zone: the centre speaker, the one Table A.7's note 1 adds to the back
// zone for the centre-and-back preset. Carried in the same mask so a speaker
// is one byte.
inline constexpr unsigned kCentre = 1U << 5;
}  // namespace zone_bit

// Table A.7: the zones the speaker at `location` is in, with kCentre set for
// the centre speaker. A surround pair
// (Ls/Rs) is Surround and, by the table's note 2, Back in a 5.X layout or
// smaller and Side in a larger one: `wide_layout` says which, true when the
// layout also has rear surrounds, discrete side surrounds or wides.
// LFE-type locations are in no zone - they take no panned audio.
[[nodiscard]] ICLFORGE_RENDER_EXPORT unsigned speaker_zones(base::Location location,
                                                            bool wide_layout);

// The same for a speaker placed by angle alone, which Table A.7 cannot name:
// above the listener's plane is Top-Bottom; on it, within the front arc
// Screen, abeam Side, and behind Back, with every horizontal speaker beyond
// the front also Surround - the room's own sense of the words.
[[nodiscard]] ICLFORGE_RENDER_EXPORT unsigned direction_zones(Direction direction);

// Tables 20 and 21: whether a speaker in `zones` may take an object whose
// zone_constraints_idx is `zone_constraints_idx` (0 none, 1 back excluded,
// 2 side excluded, 3 centre-and-back only, 4 screen only, 5 surround only;
// the reserved 6 and 7 constrain nothing) and whose b_enable_elevation is
// `enable_elevation`. A Top-Bottom speaker answers to the elevation flag
// alone - the table makes that zone independent of the horizontal ones. The
// centre-and-back preset also takes the speaker marked zone_bit::kCentre
// (Table A.7's note 1).
[[nodiscard]] ICLFORGE_RENDER_EXPORT bool zone_admits(unsigned zones, int zone_constraints_idx,
                                                      bool enable_elevation);

// What an object asks of the renderer beyond where it is (§5.2): every field
// at its default is the point source with no constraint the panner has always
// rendered, which is how a caller tells there is nothing to apply.
struct ObjectConstraints {
    int zone_constraints_idx = 0;  // Table 20
    bool enable_elevation = true;  // Table 21
    bool snap = false;             // §5.2.5, channel lock
    // §5.2.2, each in [0, 1] of the room's own extent along that axis (the
    // room is 2 units high, z -1 to +1, so a height of 1 is the whole of it).
    double width = 0.0;
    double depth = 0.0;
    double height = 0.0;

    [[nodiscard]] bool is_default() const {
        return zone_constraints_idx == 0 && enable_elevation && !snap && width == 0.0 &&
               depth == 0.0 && height == 0.0;
    }
};

// One object's gains over `targets`, a room-anchored position (§4.2.1) and its
// constraints applied. `zones[i]` is the zone mask of targets[i] (speaker_zones
// or direction_zones), and `gains` is sized to `targets` and OVERWRITTEN with
// sum of squares 1.
//
//   ZONES take the speakers the object may not use out of the set it is
//   panned over, so it still sounds at full level - it moves, it does not get
//   quieter. When no speaker satisfies the constraint it cannot be honoured
//   and the object is panned over all of them.
//   SNAP is the nearest of the speakers left, at unit gain.
//   EXTENT is the cuboid the size describes, centred on the position,
//   sampled at the centre and both ends of each axis it extends along (27
//   points at most), each point panned and their powers averaged - more
//   speakers at the same level, never louder. Snap wins over extent.
//
// With default constraints this is pan_direction of position_direction.
ICLFORGE_RENDER_EXPORT void pan_constrained(double x, double y, double z,
                                            const ObjectConstraints& constraints,
                                            std::span<const Direction> targets,
                                            std::span<const std::uint8_t> zones,
                                            std::span<double> gains);

struct ObjectState {
    double azimuth_deg = 0.0;
    double gain = 1.0;      // linear
    double lfe_send = 0.0;  // linear; the only way an object reaches the LFE
};

// Renders mono objects into a 5.1 bed (5 fullbw channels + LFE), one
// 256-sample block at a time, ramping each object's channel gains linearly
// from the previous block's values to the current targets.
class ICLFORGE_RENDER_EXPORT BedRenderer {
   public:
    // Returns the object's index. Call before rendering starts (allocates).
    std::size_t add_object(const ObjectState& initial);

    void set_target(std::size_t object, const ObjectState& target);

    // audio: one 256-sample mono span per object, same order as add_object.
    // bed: 6 spans (L, C, R, SL, SR, LFE) of 256 samples each, OVERWRITTEN.
    void render_block(std::span<const std::span<const float>> audio,
                      std::span<const std::span<float>> bed);

   private:
    struct Slot {
        ObjectState target;
        PanGains current_gains{};
        double current_lfe = 0.0;
        bool primed = false;  // first block jumps to target instead of ramping
    };
    std::vector<Slot> slots_;
};

}  // namespace iclforge::spatial
