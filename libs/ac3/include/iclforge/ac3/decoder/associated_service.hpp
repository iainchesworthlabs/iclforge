#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "iclforge/ac3/core/eac3_tables.hpp"
#include "iclforge/ac3/decoder/decoder.hpp"
#include "iclforge/ac3/decoder/output.hpp"
#include "iclforge/ac3/export.hpp"

// The receiver's side of §E3.10 "Control of Program Mixing": combining a main
// programme with an associated service - a visually impaired description, a
// commentary, a voice-over - after both have been decoded.
//
// Annex E carries the control data and leaves the mixer to the receiver. This
// component is that mixer, and it sits after the decoder rather than inside it:
// a main and an associated programme are two decodes of two independent
// substreams (or of two streams), each through its own Eac3Decoder, and what
// joins them is PCM plus the mixmdate group each decode reported in
// DecodedAccessUnit::mixing.
//
// What it applies, and where each rule comes from:
//
//   * pgmscl / pgmscl2 (§E3.10.1) scale the programme that carries them. The
//     main's own scale is applied to the main, the associated's to the
//     associated, and a dual-mono (1+1) associated service scales Ch2 by pgmscl2.
//   * extpgmscl (§E3.10.2) scales the OTHER programme: the associated
//     service's extpgmscl is the usual way a broadcaster ducks the main under
//     a description track, and the main's extpgmscl scales the associated one.
//     The two rules are symmetric and the gains add in dB, which is what
//     "combined with" in §E3.10.6 says for the per-channel scales below.
//   * extpgmlscl, extpgmcscl, extpgmrscl, extpgmlsscl, extpgmrsscl,
//     extpgmlfescl and the two auxiliary channel scales (mixdef 3's mixdata2e,
//     §E3.10.6) trim individual channels of the other programme, on top of
//     extpgmscl. dmixscl (§E3.10.7) replaces them when that programme reached
//     the mixer already folded to two channels, because the channels they name
//     no longer exist; it is ignored otherwise.
//   * panmean / panmean2 (§E3.10.8) place a MONO associated service among the
//     main's channels by Tables E3.15 (stereo), E3.16 and E3.17 (5.1). A
//     stream that sends no pan is "center". A main with only a centre channel
//     takes the service unpanned.
//
// What it deliberately does not apply, because Annex E gives it no processing
// to apply:
//
//   * premixcmpsel, drcsrc and premixcmpscl (§E2.3.1.19-21). The text says
//     "decoders are not required to use them" and that the compression model
//     they were written for "is not supported by the E-AC-3 mixing model".
//   * spchdat and the speech enhancement tree (§E2.3.1.45-51), which §E2.3.1.45
//     calls "placeholders for as yet undefined data".
//   * blkmixcfginfo (§E2.3.1.59-61) and the reserved paninfo bits, which have
//     no semantics at all.
//
// They remain decoded and reported (DecodedAccessUnit::mixing); none changes
// the audio, and a test holds that to be exactly so.
//
// Beyond the spec: Tables E3.15-E3.17 cover a mono service over a stereo or a
// 5.1 main and nothing else. A wider or narrower main (3.0, 2.2, 7.1, 5.1.4)
// is served by the 5.1 tables with the seats it lacks folded into the ones it
// has, and every channel that shares a seat (Ls and Lrs, say) splitting that
// seat's power; channels at heights and the LFEs receive none of the service.
// A stereo or wider associated service is not panned - it has a soundfield of
// its own - and is routed to the main's channel at the same location.
//
// Each programme's levels are those its decoder produced: the mixer applies the
// metadata's gains and nothing else, so dialnorm normalisation, dynrng/compr
// and the §7.8 fold are whatever each DecoderConfig asked for. The programmes
// are summed without a limiter; a sum past full scale is the caller's to meet.

namespace iclforge::ac3 {

struct AssociatedServiceMixConfig {
    // The fold each programme's decoder was configured with
    // (DecoderConfig::output.target). A folded DecodedAccessUnit holds the fold's
    // channels - L and R for Lo/Ro and Lt/Rt, C for mono - while its `layout`
    // still names what was rendered before it, so the mixer is told rather
    // than left to guess which of the two `channels` holds.
    DownmixTarget main_fold = DownmixTarget::kAsCoded;
    DownmixTarget associated_fold = DownmixTarget::kAsCoded;
    // The listener's own level for the associated service, in dB, added to the
    // stream's gains - the receiver's "description volume" control. 0 leaves
    // the stream's mix as authored.
    double associated_trim_db = 0.0;
};

enum class MixError : std::uint8_t {
    kSampleRateMismatch,    // the two programmes are at different sample rates
    kFrameLengthMismatch,   // different samples per channel, or ragged channels
    kChannelCountMismatch,  // a unit's channels disagree with its layout and fold
    kUnmixableMain,         // 1+1 dual mono as the main: two programmes, no soundfield
    kUnmixableAssociated,   // an associated service with no channels to mix
};

[[nodiscard]] ICLFORGE_AC3_EXPORT std::string_view describe(MixError error);

// What one mix() applied, for a caller that reports it. A gain is in dB, and
// -infinity where the stream muted it.
struct AssociatedServiceMixResult {
    // pgmscl of the main plus extpgmscl of the associated service: the level
    // every main channel took before any per-channel trim.
    double main_gain_db = 0.0;
    // pgmscl of the associated service plus extpgmscl of the main plus the
    // configured trim. Of Ch1 for a dual-mono associated service.
    double associated_gain_db = 0.0;
    // The pan position a mono associated service was placed at (0..239,
    // 1.5-degree steps clockwise from the centre), std::nullopt when it was
    // not panned: a stereo or wider service, or a main with a lone centre.
    std::optional<int> panmean = std::nullopt;
};

// The Table E2.5 location of each channel of `unit.channels`, given the fold the
// unit's decoder was configured with. Empty for 1+1 dual mono, whose channels
// are Ch1 and Ch2 and have no location. A folded unit reports the fold's own
// channels (L and R, or C), whatever its `layout` says.
[[nodiscard]] ICLFORGE_AC3_EXPORT std::vector<eac3::chanmap::Location> output_locations(
    const DecodedAccessUnit& unit, DownmixTarget fold);

// How much of a mono service panned to `panmean` each of `locations` receives,
// as a linear amplitude per location, in the same order. panmean is §E2.3.1.54's
// index, 0..239; the reserved 240..255 and anything outside the range is taken
// as the centre. The weights are constant power: for the layouts the tables
// cover, they square to one across the channels.
[[nodiscard]] ICLFORGE_AC3_EXPORT std::vector<double> pan_weights(
    int panmean, std::span<const eac3::chanmap::Location> locations);

// One instance per pair of programmes: it keeps the gains of the previous
// access unit so that a change of pgmscl, extpgmscl or pan - an audio
// description's fade in and out is exactly that - ramps over the first block of
// the unit rather than stepping.
class ICLFORGE_AC3_EXPORT AssociatedServiceMixer {
   public:
    AssociatedServiceMixer() = default;
    explicit AssociatedServiceMixer(const AssociatedServiceMixConfig& config) : config_(config) {}

    // Adds `associated` into `main`'s channels in place, with the gains and pan
    // §E3.10 gives. Both units are one frame period of their programmes; their
    // channels must be the same length and their sample rates equal. `main` is
    // unchanged on an error.
    [[nodiscard]] std::expected<AssociatedServiceMixResult, MixError> mix(
        DecodedAccessUnit& main, const DecodedAccessUnit& associated);

    // Forget the previous unit's gains, as at the start of a new stream or
    // after a seek: the next mix() applies its gains at once.
    void reset();

   private:
    AssociatedServiceMixConfig config_{};
    // Per main channel: the gain applied to it last time.
    std::vector<double> main_gains_;
    // Per associated source, per main channel, row-major: the weight last time.
    std::vector<double> source_gains_;
    std::size_t main_channels_ = 0;
    std::size_t sources_ = 0;
    bool primed_ = false;
};

}  // namespace iclforge::ac3
