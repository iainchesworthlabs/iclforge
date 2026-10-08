#pragma once

#include <cstdint>

namespace iclforge::base {

// Where a channel is meant to be heard, as a list of speakers rather than a codec's coding of
// them: the names of TS 103 190-1 clause D.1 and TS 103 190-2 clause A.3, those of the channel
// modes of Part 1 Table 88, and the immersive layouts' (Part 2 Table A.27). AC-4 decodes to it
// (iclforge::ac4::Speaker). E-AC-3's channel map codes its locations otherwise, in bit order
// (iclforge::base::Location, layout.hpp), and names pairs this list does not.
enum class Speaker : std::uint8_t {
    kLeft,
    kRight,
    kCentre,
    kLfe,            // Low-Frequency Effects
    kLeftSurround,   // Left Side/Surround, Ls: a side speaker in the 7.X modes
    kRightSurround,  // Right Side/Surround, Rs
    kLeftBack,       // Lb, in 7.X 3/4/0 and 7.X.4
    kRightBack,      // Rb
    kLeftWide,       // Lw, in 7.X 5/2/0
    kRightWide,      // Rw
    kTopFrontLeft,   // Tfl, in 7.X 3/2/2 and the X.4 layouts
    kTopFrontRight,  // Tfr
    kTopBackLeft,    // Tbl, in the X.4 layouts
    kTopBackRight,   // Tbr
    kTopSideLeft,    // Tsl, the top pair of the X.2 layouts: 5.X.2, the core layout
    kTopSideRight,   // Tsr
    kLfe2,           // the second LFE a bed can assign (Part 2 Tables 64 and 65)
    // Part 2 Table A.27's other speakers, added after the ones above so that
    // their values keep their meaning: the 9.X.4 layouts' screen pair, and the
    // 22.2 layout's centre, top and bottom channels.
    kLeftScreen,         // Lscr, the left screen edge speaker in 9.X.4
    kRightScreen,        // Rscr
    kTopFrontCentre,     // Tfc, in 22.2
    kTopBackCentre,      // Tbc
    kTopCentre,          // Tc
    kBottomFrontLeft,    // Bfl
    kBottomFrontRight,   // Bfr
    kBottomFrontCentre,  // Bfc
    kCentreBack,         // Cb
};

}  // namespace iclforge::base
