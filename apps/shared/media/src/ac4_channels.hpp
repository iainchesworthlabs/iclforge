#pragma once

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <optional>
#include <span>
#include <vector>

#include "iclforge/ac3/core/eac3_tables.hpp"
#include "iclforge/ac3/core/tables.hpp"
#include "iclforge/ac4/decoder/decoder.hpp"

// AC-4's channels in WAV files, for forge's `decode` and `ac4-encode` and
// forge-gui's AC-4 pages alike: a WAV file holds them in the WAVEFORMATEXTENSIBLE
// speaker order the E-AC-3 path writes (plan::wav_order: FL FR FC LFE BL BR,
// then SL SR and the top front pair), with Ls and Rs at SL and SR, Lb and Rb at
// BL and BR, the top back pair at TBL and TBR, and Lw and Rw, which that order
// has no place for, last.
// So 5.1's surrounds take the fifth and sixth channels, as E-AC-3's do, and
// 5.1.4 and 7.1.4 come out as DEE takes them in. The top side pair of an X.2
// layout, which the order has no place for either, takes the top front pair's
// places, which an X.2 layout leaves empty. 22.2's other channels take the
// order's places where it has one (Tc, Tfc, Tbc and Cb at TC, TFC, TBC and BC,
// and the 9.X.4 screen pair at FLC and FRC), and its second LFE and bottom
// channels go last with Lw and Rw, in the order the decoder writes them. A
// layout with both top pairs, 22.2, has Tsl and Tsr share the top front pair's
// ranks, so those four keep the decoder's order among themselves.

namespace iclforge::apps {

[[nodiscard]] inline int ac4_wav_rank(iclforge::ac4::Speaker speaker) {
    switch (speaker) {
        case iclforge::ac4::Speaker::kLeft:
            return 0;
        case iclforge::ac4::Speaker::kRight:
            return 1;
        case iclforge::ac4::Speaker::kCentre:
            return 2;
        case iclforge::ac4::Speaker::kLfe:
            return 3;
        case iclforge::ac4::Speaker::kLeftBack:
            return 4;
        case iclforge::ac4::Speaker::kRightBack:
            return 5;
        case iclforge::ac4::Speaker::kLeftSurround:
            return 9;
        case iclforge::ac4::Speaker::kRightSurround:
            return 10;
        case iclforge::ac4::Speaker::kTopFrontLeft:
        case iclforge::ac4::Speaker::kTopSideLeft:
            return 12;
        case iclforge::ac4::Speaker::kTopFrontRight:
        case iclforge::ac4::Speaker::kTopSideRight:
            return 14;
        case iclforge::ac4::Speaker::kTopBackLeft:
            return 15;
        case iclforge::ac4::Speaker::kTopBackRight:
            return 17;
        case iclforge::ac4::Speaker::kLeftScreen:
            return 6;
        case iclforge::ac4::Speaker::kRightScreen:
            return 7;
        case iclforge::ac4::Speaker::kCentreBack:
            return 8;
        case iclforge::ac4::Speaker::kTopCentre:
            return 11;
        case iclforge::ac4::Speaker::kTopFrontCentre:
            return 13;
        case iclforge::ac4::Speaker::kTopBackCentre:
            return 16;
        default:
            return 99;
    }
}

// The indices of `speakers` ordered by `rank`, ties kept in their order.
template <typename Rank>
[[nodiscard]] std::vector<std::size_t> ac4_order(std::span<const iclforge::ac4::Speaker> speakers, Rank rank) {
    std::vector<std::size_t> order(speakers.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::ranges::stable_sort(order, {}, [&](std::size_t c) { return rank(speakers[c]); });
    return order;
}

// The level and loudness meters' order, A/52's: L C R Ls Rs, the LFE, then any
// other.
[[nodiscard]] inline int ac4_meter_rank(iclforge::ac4::Speaker speaker) {
    switch (speaker) {
        case iclforge::ac4::Speaker::kLeft:
            return 0;
        case iclforge::ac4::Speaker::kCentre:
            return 1;
        case iclforge::ac4::Speaker::kRight:
            return 2;
        case iclforge::ac4::Speaker::kLeftSurround:
            return 3;
        case iclforge::ac4::Speaker::kRightSurround:
            return 4;
        case iclforge::ac4::Speaker::kLfe:
            return 5;
        default:
            return 99;
    }
}

// The coding mode that names an AC-4 layout's bed for a meter: 1/0, 2/0, 3/0
// or 3/2; a 7.X layout's last pair is metered past it.
[[nodiscard]] inline iclforge::ac3::Acmod ac4_bed_acmod(
    std::span<const iclforge::ac4::Speaker> speakers) {
    const auto has = [&](iclforge::ac4::Speaker s) {
        return std::ranges::find(speakers, s) != speakers.end();
    };
    if (has(iclforge::ac4::Speaker::kLeftSurround)) {
        return iclforge::ac3::Acmod::k3_2;
    }
    if (has(iclforge::ac4::Speaker::kLeft)) {
        return has(iclforge::ac4::Speaker::kCentre) ? iclforge::ac3::Acmod::k3_0
                                                    : iclforge::ac3::Acmod::k2_0;
    }
    return iclforge::ac3::Acmod::k1_0;
}

// Where an AC-4 speaker is among A/52's Table E2.5 locations, for a meter that
// weights channels by where they are (BS.1770-5 Annex 3) and for placing a
// decoded presentation on an AC-3 or E-AC-3 layout: Lb and Rb are the rear
// surrounds, Lw and Rw the wides, the top front pair the vertical heights, the
// top back and top side pairs the top surrounds (Table E2.5 has one pair for
// both, as the object renderer places them), and the second LFE LFE2. 22.2's
// Tfc is the vertical height centre, Tc and Tbc the top surround, and Cb the
// centre surround. Its bottom channels (Bfl, Bfr, Bfc) and 9.X.4's screen pair
// (Lscr, Rscr) have no location in the table that the standard names: nothing.
[[nodiscard]] inline std::optional<iclforge::ac3::eac3::chanmap::Location> ac4_location(
    iclforge::ac4::Speaker speaker) {
    using L = iclforge::ac3::eac3::chanmap::Location;
    switch (speaker) {
        case iclforge::ac4::Speaker::kLeft:
            return L::kLeft;
        case iclforge::ac4::Speaker::kRight:
            return L::kRight;
        case iclforge::ac4::Speaker::kCentre:
            return L::kCentre;
        case iclforge::ac4::Speaker::kLfe:
            return L::kLfe;
        case iclforge::ac4::Speaker::kLeftSurround:
            return L::kLeftSurround;
        case iclforge::ac4::Speaker::kRightSurround:
            return L::kRightSurround;
        case iclforge::ac4::Speaker::kLeftBack:
            return L::kLrs;
        case iclforge::ac4::Speaker::kRightBack:
            return L::kRrs;
        case iclforge::ac4::Speaker::kLeftWide:
            return L::kLw;
        case iclforge::ac4::Speaker::kRightWide:
            return L::kRw;
        case iclforge::ac4::Speaker::kTopFrontLeft:
            return L::kVhl;
        case iclforge::ac4::Speaker::kTopFrontRight:
            return L::kVhr;
        case iclforge::ac4::Speaker::kTopBackLeft:
        case iclforge::ac4::Speaker::kTopSideLeft:
            return L::kLts;
        case iclforge::ac4::Speaker::kTopBackRight:
        case iclforge::ac4::Speaker::kTopSideRight:
            return L::kRts;
        case iclforge::ac4::Speaker::kLfe2:
            return L::kLfe2;
        case iclforge::ac4::Speaker::kTopFrontCentre:
            return L::kVhc;
        case iclforge::ac4::Speaker::kTopCentre:
        case iclforge::ac4::Speaker::kTopBackCentre:
            return L::kTs;
        case iclforge::ac4::Speaker::kCentreBack:
            return L::kCs;
        case iclforge::ac4::Speaker::kLeftScreen:
        case iclforge::ac4::Speaker::kRightScreen:
        case iclforge::ac4::Speaker::kBottomFrontLeft:
        case iclforge::ac4::Speaker::kBottomFrontRight:
        case iclforge::ac4::Speaker::kBottomFrontCentre:
            return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace iclforge::apps
