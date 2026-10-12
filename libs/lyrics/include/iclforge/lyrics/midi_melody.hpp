#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

// The melody of a MIDI karaoke file (`.kar`, a Standard MIDI File): the notes of the one track and
// channel that carries the tune, as a reference for a singer's pitch
// (planning/karaoke-microphone.md, D9; findings F4). parse_midi_karaoke() in lyrics.hpp reads the
// words and ignores every note; this reads the notes and uses the words only to find which of them
// is the melody.
//
// A karaoke file does not say which track is the tune: by convention it is one track of its own,
// but the convention is not a rule. What is checkable is the thing the convention is for: a
// syllable begins where a sung note does. The candidate (a track and channel, percussion
// excluded) whose note starts coincide with the most syllable starts is the melody, and the share
// of syllables it matches is returned so that a page can show how sure that is. A file whose best
// candidate matches under half the syllables, or that has no words, has no reference chosen: that
// is an error with a reason, not a guess.
//
// Standard library only.

namespace iclforge::lyrics {

struct MelodyNote {
    std::int64_t start_ms = 0;
    std::int64_t end_ms = 0;
    // The MIDI note number: 69 is A4, 440 Hz.
    int pitch = 0;
};

struct Melody {
    // In order of start. Notes of the one track and channel; where they overlap, the order is the
    // file's.
    std::vector<MelodyNote> notes;
    // The chosen track (counted from 0) and channel (0 to 15).
    std::size_t track = 0;
    int channel = 0;
    // How many of the file's syllables have a note of this melody starting within the tolerance,
    // 0 to 1, and how many syllables the file has.
    double match = 0.0;
    std::size_t syllables = 0;
};

// `tolerance_ms` is how far a syllable may be from the note it is sung on and still match: files
// are hand-timed and a syllable is often a tick or two off its note.
[[nodiscard]] std::expected<Melody, std::string> read_midi_melody(std::string_view bytes,
                                                                  std::int64_t tolerance_ms = 60);

}  // namespace iclforge::lyrics
