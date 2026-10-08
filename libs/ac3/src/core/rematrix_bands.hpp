#pragma once

#include <array>

namespace iclforge::ac3 {

// §7.5, Table 7.25 "Rematrix Banding Table A": [low, high] inclusive bin
// bounds of the (at most) four 2/0 rematrixing bands, coupling not in use.
// Coupling active shortens the table from the end (Tables 7.26-7.28 are the
// same four ranges with the last one or two dropped and the new last one's
// high bound clamped to where coupling begins) rather than moving any
// boundary, so this one table plus a band COUNT is everything either format
// needs - E-AC-3 reuses it unchanged: Annex E §3.3's "Modifications to
// Previously Defined Parameters" touches nrematbd (how many of these bands
// are active, given coupling/enhanced-coupling/spectral extension) but never
// the ranges themselves.
inline constexpr std::array<std::array<int, 2>, 4> kRematrixBands = {{
    {13, 24}, {25, 36}, {37, 60}, {61, 252},
}};

}  // namespace iclforge::ac3
