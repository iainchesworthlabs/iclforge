#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// What a listener chooses about a karaoke stream, with no codec in it.
//
// A karaoke stream carries a backing (L and R), a guide melody (M) and one or two vocals (V1, V2).
// ETSI TS 102 366 Annex C defines it for AC-3 and the E-AC-3 decoder takes the same reading (the
// reproduction is ours by analogy there: planning/karaoke-eac3.md, D1); an AC-4 mapping would
// choose a presentation and a dialogue gain for the same choice (planning/karaoke-ac4-options.md).
// The types are here, header-only, so that every one of them names the same thing: the output
// stages of libs/ac3, the settings and the wire's converters, and a later AC-4 resolver. Nothing in
// this header is exported, and iclforge::ac3 re-exports the names, so no caller changed.

namespace iclforge::base {

// How a karaoke stream is reproduced. What the choices mean for AC-3 and E-AC-3 is Annex C's
// (C.2.3.1 Table C.2.2, C.2.3.2 Table C.2.3), written out beside OutputConfig in
// iclforge/ac3/decoder/output.hpp.
enum class KaraokeReproduction : std::uint8_t {
    // A karaoke stream goes through the decoder like any other. The default: the decoders are a
    // check on the encoder first, and nothing here re-routes a programme unasked.
    kOff,
    // The karaoke-AWARE decoder: L, C and R, the melody in the centre and the vocals at the
    // stream's own surround level (the programme provider's choice). Ignored for a stream that is
    // not karaoke.
    kAware,
    // The karaoke-CAPABLE decoder: as kAware, with the vocals the listener chose (KaraokeVocals),
    // and the listener's levels on the three parts.
    kCapable,
};

// Which of a karaoke stream's vocal channels the listener hears (KaraokeReproduction::kCapable).
// A choice of a vocal the stream does not code (V2 of a stream with one vocal) reproduces none.
enum class KaraokeVocals : std::uint8_t {
    kNone,
    kV1,
    kV2,
    kBoth,
};

// What a person-facing level control for a karaoke part spans, in dB against Table C.2.3 (a
// decoder's linear gains are the same levels). The bottom of the span is off - a singer who pulls
// the melody all the way down wants it gone, not 24 dB quieter - and the top is 12 dB over the
// table. Hearth's Karaoke page, its settings, the wire's karaoke_*_db keys and a sink's decoder all
// use this one span and this one conversion, so a level means the same on every one of them.
inline constexpr double kKaraokeMinLevelDb = -24.0;
inline constexpr double kKaraokeMaxLevelDb = 12.0;

// A level in dB held to that span. A value that is not a number is 0 dB, the table's: the gains
// below mute one, and a stray NaN from a settings file must not silence the melody.
[[nodiscard]] inline double karaoke_level_db(double db) {
    return std::isnan(db) ? 0.0 : std::clamp(db, kKaraokeMinLevelDb, kKaraokeMaxLevelDb);
}

// That level as the linear gain a decoder's output stage takes: 0.0 (off) at the bottom of the
// span, and exactly 1.0 for 0 dB.
[[nodiscard]] inline double karaoke_gain_from_db(double db) {
    const double held = karaoke_level_db(db);
    return held <= kKaraokeMinLevelDb ? 0.0 : std::pow(10.0, held / 20.0);
}

}  // namespace iclforge::base
