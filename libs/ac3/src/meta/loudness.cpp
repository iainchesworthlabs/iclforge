#include "iclforge/ac3/meta/loudness.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

#include "iclforge/ac3/core/eac3_tables.hpp"
#include "iclforge/ac3/core/types.hpp"
#include "iclforge/base/loudness.hpp"

namespace iclforge::ac3::meta {

namespace {

using base::kSurroundWeight;

[[nodiscard]] std::vector<std::optional<double>> acmod_weights(Acmod acmod, bool lfe) {
    const int fullbw = fullbw_channel_count(acmod);
    // Table 5.8 codes the full-bandwidth channels first and the LFE last, so
    // the loudness terms are simply slots 0..fullbw-1.
    std::vector<std::optional<double>> weights(static_cast<std::size_t>(fullbw), 1.0);
    // Which coded positions are surrounds depends on acmod (Table 5.8): 2/1
    // and 3/1 end with a single S, 2/2 and 3/2 end with Ls and Rs, and no
    // other mode has any. In every case they are the LAST coded channels, so
    // the count is the only thing that varies.
    const std::size_t surrounds = [acmod]() -> std::size_t {
        switch (acmod) {
            case Acmod::k2_1:
            case Acmod::k3_1:
                return 1;
            case Acmod::k2_2:
            case Acmod::k3_2:
                return 2;
            default:
                return 0;
        }
    }();
    // Clamped rather than subtracted outright. Table 5.8 guarantees a mode is
    // at least as wide as its own surround count - 2/1 codes three channels,
    // 3/2 five - but at -O3 GCC cannot see through fullbw_channel_count() to
    // prove `weights` is even non-empty, and -Werror=null-dereference fires
    // on the indexing if it cannot. std::min costs nothing and makes the
    // bound something the compiler can check rather than something it has to
    // take on trust.
    for (std::size_t i = weights.size() - std::min(surrounds, weights.size());
         i < weights.size(); ++i) {
        weights[i] = kSurroundWeight;
    }
    if (lfe) {
        weights.emplace_back(std::nullopt);
    }
    return weights;
}

[[nodiscard]] std::vector<std::optional<double>> layout_weights(
    const eac3::chanmap::Layout& layout) {
    std::vector<std::optional<double>> weights;
    weights.reserve(static_cast<std::size_t>(std::max(layout.count, 0)));
    for (int slot = 0; slot < layout.count; ++slot) {
        weights.push_back(position_weight(layout[slot]));
    }
    return weights;
}

}  // namespace

std::optional<double> position_weight(eac3::chanmap::Location location) {
    using Location = eac3::chanmap::Location;
    switch (location) {
        // Annex 3 weights "each channel except the LFE channels", so an
        // LFE-type location is not a term in the sum at all.
        case Location::kLfe:
        case Location::kLfe2:
            return std::nullopt;

        // Table 4's one non-unity cell: 60 <= |theta| <= 120 at |phi| < 30.
        // Table 5 confirms all three pairs at 1.41 - M±110 (Ls/Rs, the 5.1
        // surrounds Annex 1's own Table 3 already weighted 1.41, which is why
        // a 5.1 layout measures the same through either algorithm), M±090
        // (Lsd/Rsd, the direct-radiating side surrounds a 7.1 layout uses)
        // and M±060 (Lw/Rw, the wides).
        //
        // Ls/Rs and Lsd/Rsd are robust to the exact angle assumed: anywhere
        // from 90 to 110 degrees is inside the sector. Lw/Rw sit right on its
        // 60-degree edge, which Table 4 includes ("60 <= |theta|") and Table
        // 5's M±060 row then states outright at 1.41.
        case Location::kLeftSurround:
        case Location::kRightSurround:
        case Location::kLsd:
        case Location::kRsd:
        case Location::kLw:
        case Location::kRw:
            return kSurroundWeight;

        // Everything else is unity. That is three different cells of Table 4,
        // listed together because the answer is the same and a switch with
        // three identical branches is worse to read than one:
        //
        //   |theta| < 60 (first column)      - M+000 (C), M±030 (L/R) and
        //                                      M±SC (Lc/Rc, the "screen" pair
        //                                      inboard of L/R).
        //   120 < |theta| <= 180 (third)     - M±135 (Lrs/Rrs, the 7.1 rear
        //                                      pair) and M+180 (Cs). So
        //                                      widening 5.1 to 7.1 adds two
        //                                      channels that are NOT
        //                                      surround-weighted, whatever
        //                                      their names suggest.
        //   |phi| >= 30 (the "else" row)     - every upper-layer and top
        //                                      position, whatever its azimuth:
        //                                      U+000/U±030/U±045/U±090/U±110/
        //                                      U±135/U+180 and T+000 are all
        //                                      1.00 in Table 5, so no height
        //                                      channel is ever
        //                                      surround-weighted. Robust to
        //                                      the exact elevation assumed,
        //                                      since any plausible height
        //                                      angle is at or above 30
        //                                      degrees and the row spans the
        //                                      whole azimuth circle.
        case Location::kLeft:
        case Location::kCentre:
        case Location::kRight:
        case Location::kLc:
        case Location::kRc:
        case Location::kLrs:
        case Location::kRrs:
        case Location::kCs:
        case Location::kVhl:
        case Location::kVhr:
        case Location::kVhc:
        case Location::kLts:
        case Location::kRts:
        case Location::kTs:
            return 1.0;
    }
    return std::nullopt;
}

LoudnessMeter::LoudnessMeter(SampleRate rate, Acmod acmod, bool lfe)
    : base::LoudnessMeter(sample_rate_hz(rate), acmod_weights(acmod, lfe)) {}

LoudnessMeter::LoudnessMeter(SampleRate rate, const eac3::chanmap::Layout& layout)
    : base::LoudnessMeter(sample_rate_hz(rate), layout_weights(layout)) {}

int dialnorm_from_lkfs(double lkfs) {
    const auto value = static_cast<int>(std::lround(-lkfs));
    return std::clamp(value, 1, 31);
}

}  // namespace iclforge::ac3::meta
