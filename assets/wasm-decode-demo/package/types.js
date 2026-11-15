// Shared types for the package - the "typed" half of "typed ES module
// package": everything a consumer touches has a real interface instead of
// the `any` an untyped Embind wrapper would otherwise force on callers.
/** iclforge::ac3::DownmixTarget's own numeric order (core/downmix_target.hpp) - kept in sync by hand, there being only four values. */
export var DownmixTarget;
(function (DownmixTarget) {
    DownmixTarget[DownmixTarget["AsCoded"] = 0] = "AsCoded";
    DownmixTarget[DownmixTarget["LoRo"] = 1] = "LoRo";
    DownmixTarget[DownmixTarget["LtRt"] = 2] = "LtRt";
    DownmixTarget[DownmixTarget["Mono"] = 3] = "Mono";
})(DownmixTarget || (DownmixTarget = {}));
/**
 * Annex C's reproduction of a karaoke stream (bsmod 7 above 1/0: AC-3, or E-AC-3 of one independent
 * substream), in iclforge::ac3::KaraokeReproduction's
 * own numeric order. Applied to the fold, so it only changes anything for a stream that is karaoke.
 */
export var KaraokeReproduction;
(function (KaraokeReproduction) {
    /** The coded channels, as they are. */
    KaraokeReproduction[KaraokeReproduction["AsCoded"] = 0] = "AsCoded";
    /** L, C and R with the vocals at the stream's own level (Table C.2.2). */
    KaraokeReproduction[KaraokeReproduction["Aware"] = 1] = "Aware";
    /** The listener chooses which vocals are heard (Table C.2.3; see {@link KaraokeVocals}). */
    KaraokeReproduction[KaraokeReproduction["Capable"] = 2] = "Capable";
})(KaraokeReproduction || (KaraokeReproduction = {}));
/** Which of a karaoke stream's vocals the {@link KaraokeReproduction.Capable} reproduction plays. */
export var KaraokeVocals;
(function (KaraokeVocals) {
    KaraokeVocals[KaraokeVocals["None"] = 0] = "None";
    KaraokeVocals[KaraokeVocals["V1"] = 1] = "V1";
    KaraokeVocals[KaraokeVocals["V2"] = 2] = "V2";
    KaraokeVocals[KaraokeVocals["Both"] = 3] = "Both";
})(KaraokeVocals || (KaraokeVocals = {}));
//# sourceMappingURL=types.js.map