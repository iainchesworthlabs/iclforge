#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "iclforge/pitch/tracker.hpp"

// A reference melody: what a singer's pitch is compared with (planning/karaoke-microphone.md, D9;
// findings F4). It is a contour on a fixed grid, so that a reference from a decoded guide-melody
// channel and a reference from MIDI notes are the same thing to the scorer.
//
// Standard library only.

namespace iclforge::pitch {

// The pitch of a melody, one value every `hop_ms` from time 0: Hz, or 0 where nothing is sung or
// played. The default grid is the tracker's own hop.
struct Contour {
    std::uint32_t hop_ms = 10;
    std::vector<float> hz;

    [[nodiscard]] bool empty() const { return hz.empty(); }
    // The end of the contour, in milliseconds.
    [[nodiscard]] std::int64_t duration_ms() const {
        return static_cast<std::int64_t>(hz.size()) * static_cast<std::int64_t>(hop_ms);
    }
    // How many grid points are voiced.
    [[nodiscard]] std::size_t voiced_frames() const;
};

// A note: a stretch of one pitch. `midi` is a fractional MIDI note number (69 is 440 Hz).
struct Note {
    std::int64_t start_ms = 0;
    std::int64_t end_ms = 0;
    double midi = 0.0;
};

struct ContourOptions {
    // The tracker's range and gates, as TrackerConfig has them. A guide melody is clean, so the
    // clarity floor is the default.
    float min_hz = 70.0F;
    float max_hz = 1100.0F;
    float clarity_min = 0.6F;
    float level_floor_db = -55.0F;
    std::uint32_t hop_ms = 10;
};

// The contour of one channel of audio at `sample_rate`: the tracker's answers placed on the grid by
// their centre time. Empty when the rate or the options cannot be tracked.
[[nodiscard]] Contour contour_of_signal(std::span<const float> mono, std::uint32_t sample_rate,
                                        const ContourOptions& options = {});

// A contour rendered from notes (a MIDI melody): each grid point inside a note is that note's
// pitch. A note that overlaps an earlier one wins from its start. `duration_ms` pads the contour to
// a length (the item's), and 0 ends it at the last note.
[[nodiscard]] Contour contour_of_notes(std::span<const Note> notes, std::uint32_t hop_ms = 10,
                                       std::int64_t duration_ms = 0);

struct NoteOptions {
    // A run of voiced points is one note while neighbours stay within this many cents of the run's
    // running median.
    double split_cents = 70.0;
    // A note shorter than this is dropped (a tracker's flicker, a consonant).
    std::int64_t min_ms = 60;
    // An unvoiced gap this short does not end a note.
    std::int64_t bridge_ms = 30;
};

// The notes of a contour: runs of voiced points of about one pitch, each with its median pitch.
[[nodiscard]] std::vector<Note> segment_notes(const Contour& contour,
                                              const NoteOptions& options = {});

}  // namespace iclforge::pitch
