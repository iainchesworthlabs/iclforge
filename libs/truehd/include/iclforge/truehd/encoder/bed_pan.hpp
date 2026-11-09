#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "iclforge/truehd/export.hpp"

// The rule that turns an object's place into shares of a 7.1 bed, as the owner's DEE controls
// show it (libs/truehd/notes/feature-truehd-measured-bed-render.md). It is measured on the 7.1
// presentation of those streams, so it is a statement about what that bed holds, not about how any
// program arrives at it. Everything here is a pure function of its arguments.
//
// An object's place is the 6 bit code the object audio metadata carries: x 0 (left wall) to 62
// (right wall), y 0 (front wall) to 62 (back wall), z 0 (floor) to 15 (ceiling). The rule works on the
// code, never on a position between two codes (a master's float is rounded to the code first).
//
// The shares of the 8 rows of the bed (L R C LFE Ls Rs Lrs Rrs, the order substreams 0 to 2 decode
// them in; Ls and Rs are the side speakers, y 31, and Lrs and Rrs the rear ones, y 62):
//
//   rows     the place is bracketed in y between the front row (y 0: L at x 0, C at x 31, R at x 62),
//            the side row (y 31: Ls at x 0, Rs at x 62) and the rear row (y 62: Lrs and Rrs); the two
//            rows get cos and sin of pi/2 times the fraction of the way from one to the other.
//   columns  within a row, the same rule between the two speakers x lies between.
//   product  a place between rows is the outer product of the two splits, so the squares of the
//            shares add up to 1.
//   height   a 7.1 bed has no upper layer: z does not move the place (it plays where its floor
//            position is) and only enters the trim below.
//   trim     the only fixed gain between an object and the bed: T * w dB, with
//            w = min(1, y / 37.2 + max(0, (z - 3) / 12)) and T set by the layout. The height term is
//            a dead zone for z 0 to 3 and then one twelfth of T per code; it is MEASURED (below).
//   LFE      an object feeds no LFE. The LFE element is the bed's LFE row at unity, not filtered.
//
// What was measured: the shares and the trim on a grid of places along the front wall (ctl_e: x 0 to
// 62 at y 0) and along the left wall (ctl_k: y 0 to 62 at x 0), and one place near a corner
// (ctl_b: x 62, y 5). The height term of the trim was measured afterwards on the Dolby Reference
// Player's own renderer to 7.1.4, 5.1.4 and 7.1.2 (a centred front object, 12 element encodes, the z
// axis only): max(0, (z - 3) / 12) fits to 0.005 to 0.006 dB, where z / 15 is off by up to 0.6 dB, and
// the elevation pan there is exactly cos and sin (top / floor = tan(pi/2 * z / 15)); the 7.1 bed has
// no upper layer, so only the trim applies to it. What was not measured: the interior of the room,
// height away from a centred front object, and everything of size, zone, snap and screen reference.

namespace iclforge::truehd {

inline constexpr int kBedPanRows = 8;
inline constexpr int kBedPanMaxX = 62;
inline constexpr int kBedPanMaxY = 62;
inline constexpr int kBedPanMaxZ = 15;

// An object's place as the metadata codes it.
struct BedPosition {
    int x = 0;
    int y = 0;
    int z = 0;
    friend bool operator==(const BedPosition&, const BedPosition&) = default;
};

// The layouts the trim T is measured for (dB, for the layout the objects are rendered to).
enum class BedLayout : std::uint8_t {
    k3_1,
    k5_1,
    k5_1_2,
    k7_1,
    k9_1,
    k5_1_4,
    k7_1_2,
    k9_1_2,
    k7_1_4,
    k7_1_6,
    k9_1_4,
    k9_1_6,
};

// T for a layout: -4.5 dB for 3.1, -3.0 for 5.1, -2.25 for 5.1.2, -1.5 for 7.1, 9.1 and 5.1.4, -0.75
// for 7.1.2 and 9.1.2, and 0 for 7.1.4, 7.1.6, 9.1.4 and 9.1.6. The stream's bed is 7.1.
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT double bed_layout_trim_db(BedLayout layout) noexcept;

// The place a metadata value gives: x, y and z are the metadata's (0 to 1, z 0 at the floor), rounded
// to the code and held in its range. A z below 0 is the floor.
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT BedPosition bed_position_code(double x, double y,
                                                                   double z) noexcept;

// w of the trim: 0 at the front wall on the floor, 1 from y 37.2 (or from a z of 15) on; z 0 to 3
// adds nothing and each code above adds one twelfth. A code outside its range is held in it.
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT double bed_trim_weight(int y_code, int z_code) noexcept;

// The share of each of the 8 bed rows a place has, with the trim of `trim_db` (T) applied; the LFE
// row is 0. With trim_db 0 the squares of the shares add up to 1; with a trim they add up to
// 10^(T * w / 10).
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT std::array<double, kBedPanRows> bed_pan_gains(
    BedPosition position, double trim_db) noexcept;

// The same, with the trim of the 7.1 layout (the stream's own bed).
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT std::array<double, kBedPanRows> bed_pan_gains(
    BedPosition position) noexcept;

// A limiter for a bed that two or more objects have added up to something over full scale: the
// ceiling is 0 dBFS (1.0) and the look-ahead 32 samples. `samples` holds `channels` interleaved
// channels in full-scale units (1.0 is the largest value), a whole number of frames, and is changed
// in place. One gain is applied to every channel, so the image holds. The gain comes down ahead of a
// peak, over the look-ahead, so that no sample is over the ceiling, and goes back up as slowly; the
// signal is not delayed (the samples that follow the block, which a caller who limits block by block
// does not have, are taken as not over the ceiling). A signal that stays under the ceiling is left
// exactly as it is. Returns the number of frames whose gain was below 1. The attack and release
// shape is a choice of this library, not a measurement, and stays PROVISIONAL: a Dolby player is only
// known to hold the ceiling and look 32 samples ahead (the renderer showed no gain reduction in the
// one test that reached it, so the constants are unverified).
inline constexpr int kBedLimiterLookAhead = 32;
ICLFORGE_TRUEHD_EXPORT std::size_t limit_bed_peaks(std::span<double> samples, int channels,
                                                   int look_ahead = kBedLimiterLookAhead) noexcept;

}  // namespace iclforge::truehd
