#pragma once

#include <optional>

#include "iclforge/ac3/core/eac3_tables.hpp"
#include "iclforge/ac3/core/types.hpp"
#include "iclforge/ac3/export.hpp"
#include "iclforge/base/loudness.hpp"

// Programme loudness to ITU-R BS.1770-4, and the dialnorm (§5.4.2.8) that
// follows from it. The meter is iclforge::base's (iclforge/base/loudness.hpp,
// since planning/consolidation.md's C6); what is AC-3's is how its channels are
// weighted - by acmod, or by E-AC-3 channel-map location - and dialnorm.
//
// A/52 defines dialnorm as how far "the average dialogue level is below
// digital 100 percent" and never says how to measure it — the standard is
// from 1995 and BS.1770 is from 2006, so in the spec's own era the answer was
// a trained listener with a level meter. Every modern delivery specification
// that fills the gap names BS.1770 gated loudness (ATSC A/85, EBU R 128), so
// that is what this encoder measures, and dialnorm is the negated integrated
// loudness rounded to the nearest dB. For material with no dialogue at all —
// music, effects — the integrated programme loudness IS the anchor A/85 asks
// for, so one measurement serves both cases.

namespace iclforge::ac3::meta {

// ITU-R BS.1770-5 (11/2023) Annex 3, "Extended loudness measurement algorithm
// for loudspeaker configurations of advanced sound systems", Table 4: the
// weighting coefficient Gi of a channel depends only on where that channel
// sits, given as an azimuth (theta) and an elevation (phi). Gi is
// 1.41 (+1.5 dB) when |phi| < 30 degrees AND 60 <= |theta| <= 120 degrees,
// and 1.00 (0 dB) everywhere else - including every upper-layer position,
// which is outside the |phi| < 30 row entirely. Annex 3 changes nothing else
// about the algorithm: "First, second and fourth stages of the algorithm
// (filtering and gating procedure) are the same as in the algorithm for the
// 3/2 multichannel format".
//
// Each Table E2.5 location below is placed at the BS.2051 loudspeaker label
// it stands for and its weight read off Table 4. Table 5 of the same Annex
// then tabulates that weight per BS.2051 configuration, which gives a second
// and independent check on every value: M+000/M±030 and M±SC at 1.00,
// M±060/M±090/M±110 at 1.41, M±135/M+180 at 1.00, and every U/T/B entry at
// 1.00.
//
// std::nullopt for the two LFE-type locations. That is not a zero weight:
// Annex 3 weights "each channel except the LFE channels", so an LFE-type
// channel is not a term in the sum at all - which is also why true peak,
// which does measure it, reads it from a separate path.
//
// NOTE for anyone cross-checking against another meter: ffmpeg's ebur128 does
// NOT implement Annex 3. Probed one channel at a time with a 997 Hz tone, it
// weights a 7.1 layout's BACK surrounds (M±135) at 1.41, the same as its side
// surrounds - i.e. it generalises Annex 1's Table 3 by channel NAME rather
// than by position, so anything called a surround gets +1.5 dB. Table 4 and
// Table 5 both put M±135 at 1.00, and this function follows the standard, so
// the two disagree by 1.5 dB on those two channels by design. On 5.1, where
// the only surrounds are M±110, the two agree exactly - measured to within
// 0.02 dB, which is what makes ffmpeg a usable oracle for the Annex 1 path
// and not for this one.
[[nodiscard]] ICLFORGE_AC3_EXPORT std::optional<double> position_weight(
    eac3::chanmap::Location location);

class ICLFORGE_AC3_EXPORT LoudnessMeter : public base::LoudnessMeter {
   public:
    // BS.1770 Annex 1's basic algorithm for the 3/2 multichannel system:
    // channel weights follow its Table 3 - unity for the front channels,
    // +1.5 dB for the surrounds, and the LFE excluded outright - keyed on the
    // Table 5.8 acmod, whose coded order push() then expects: the coded
    // channels in AC-3 order with LFE last, matching the encoder's own input
    // convention.
    //
    // Table 3 names exactly five channels (L, R, C, Ls, Rs), so the lone
    // surround of 2/1 and 3/1 is not in it. This constructor reads that
    // surround as the surround FIELD collapsed to one channel - the pair's
    // own +1.5 dB - which is what A/52's own downmix does with it (it feeds
    // both surround outputs). The Annex 3 constructor below reaches the other
    // answer for the same coded channel, because there it is Table E2.5's Cs,
    // a discrete rear centre at 180 degrees, and Table 4 puts 180 degrees
    // outside the +1.5 dB sector. Both are faithful to their own algorithm;
    // the two algorithms genuinely differ here, and that is the only layout
    // for which they do.
    LoudnessMeter(SampleRate rate, Acmod acmod, bool lfe);

    // BS.1770-5 Annex 3's extended algorithm over a rendered Table E2.5
    // layout - the wide layouts (7.1, 5.1.2, 5.1.4, 7.1.4) an acmod cannot
    // name, because a dependent substream's height/wide/rear channels are not
    // members of Table 5.8 at all. `layout` is the rendered program's channel
    // order exactly as Eac3Decoder::decode_access_unit reports it, and push()
    // expects spans in that same order.
    //
    // For any layout whose full-bandwidth channels are all Table 5.8's own
    // (mono through 5.1), this agrees with the acmod constructor above
    // channel for channel - Ls/Rs are M±110, squarely inside Table 4's
    // +1.5 dB sector, which is where Table 3's 1.41 came from in the first
    // place. 1+1 dual mono has no layout to pass here: it is two unrelated
    // programmes rather than one soundfield, so it keeps its own two meters.
    LoudnessMeter(SampleRate rate, const eac3::chanmap::Layout& layout);
};

// §5.4.2.8: dialnorm is how many dB dialogue sits below digital 100%, valid
// 1..31. A programme louder than −1 LKFS or quieter than −31 clamps; the
// clamp at 31 is why a stream that never measured anything says 31, and why
// 31 is a poor default rather than a neutral one.
[[nodiscard]] ICLFORGE_AC3_EXPORT int dialnorm_from_lkfs(double lkfs);

}  // namespace iclforge::ac3::meta
