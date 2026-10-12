#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "iclforge/pitch/reference.hpp"

// A pitch score (planning/karaoke-microphone.md, D10): for the stretches where a reference melody
// has a note, how much of that time the singer's pitch was within a tolerance of it, with octaves
// ignored. It is not a judgement of singing: it hears nothing but pitch, and it is exactly as good
// as the reference it is given.
//
// The scorer does no time alignment of its own. The caller knows where in the item each of the
// singer's frames was sung (the capture time, the heard position, the declared latencies) and says
// so in `add()`; what the scorer adds is the arithmetic, and an estimate of how early or late the
// singer is, so that a person can move the offset. Standard library only.

namespace iclforge::pitch {

struct ScoreOptions {
    // The most a frame may be from the reference, in cents, to count as a hit. 100 is a semitone.
    double tolerance_cents = 100.0;
    // Octaves do not matter: a voice an octave below the melody is right.
    bool fold_octaves = true;
    // A constant shift of the whole performance that is allowed (singing in another key), in
    // cents, taken off every deviation before it is judged.
    double key_shift_cents = 0.0;
};

// The counts over a stretch of the item.
struct Tally {
    // Grid points inside the stretch where the reference has a note.
    std::uint32_t counted = 0;
    // Of those, how many the singer was voiced at.
    std::uint32_t sung = 0;
    // Of those, how many were within the tolerance.
    std::uint32_t hit = 0;
    // The mean signed deviation of the sung points, in cents after folding and the key shift:
    // positive is sharp. 0 when nothing was sung.
    double mean_cents = 0.0;

    // False for a stretch with no reference note in it: it has no score, which is not a score of 0.
    [[nodiscard]] bool scored() const { return counted > 0; }
    // hit / counted, 0 to 1; 0 when nothing was counted.
    [[nodiscard]] double fraction() const {
        return counted == 0 ? 0.0 : static_cast<double>(hit) / static_cast<double>(counted);
    }
    // sung / counted: how much of the reference's voiced time the singer was voiced for.
    [[nodiscard]] double coverage() const {
        return counted == 0 ? 0.0 : static_cast<double>(sung) / static_cast<double>(counted);
    }
};

class Scorer {
   public:
    explicit Scorer(Contour reference, const ScoreOptions& options = {});

    // The singer's pitch at a position in the item, in milliseconds on the reference's timeline:
    // `hz` of 0 says the singer was not voiced. A point is replaced by a later answer for the same
    // grid point, so a seek or a retry is harmless; a position outside the reference is ignored.
    void add(std::int64_t item_ms, float hz);

    // Forgets every answer of the singer, keeping the reference and the options.
    void clear();

    void set_options(const ScoreOptions& options) { options_ = options; }
    [[nodiscard]] const ScoreOptions& options() const { return options_; }
    [[nodiscard]] const Contour& reference() const { return reference_; }

    // The counts for [start_ms, end_ms): a line, or the whole item with total().
    [[nodiscard]] Tally tally(std::int64_t start_ms, std::int64_t end_ms) const;
    [[nodiscard]] Tally total() const;

    // How early or late the singer is, in milliseconds, within +-`max_lag_ms`: the shift of the
    // singer's contour against the reference that gives the most hits. Positive is late. Nothing
    // until there are enough points (20 counted and 10 hits at the best shift) to say.
    [[nodiscard]] std::optional<std::int32_t> best_lag_ms(std::int32_t max_lag_ms = 300) const;

    // The median deviation of the sung points, in cents after folding and before any key shift:
    // what `key_shift_cents` would have to be for "score against my key". Nothing until 20 sung
    // points.
    [[nodiscard]] std::optional<double> key_offset_cents() const;

   private:
    // -1 is "no answer for this grid point", 0 "not voiced", anything else Hz.
    static constexpr float kUnset = -1.0F;

    [[nodiscard]] double deviation(float reference_hz, float sung_hz) const;

    Contour reference_;
    ScoreOptions options_;
    std::vector<float> sung_;
};

}  // namespace iclforge::pitch
