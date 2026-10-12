#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "iclforge/lyrics/lyrics.hpp"

// Giving lyrics their times, and writing them down (planning/karaoke-phase2.md, W6 and
// planning/karaoke-phase2-findings.md, "W3: word timing"): what a tap editor edits and saves.
//
// The editor has two passes over one stream. Lines: a key press stamps the next line's start as the
// stream plays, which alone turns an untimed lyric into a line-timed file. Words: each line's words
// start at an estimate by their letters, drawn as a guess, and a press replaces a guess with a time.
// Everything here is the data side of that, Qt-free and clock-free: a position in milliseconds is an
// argument, and the caller took it from the engine's heard position and took its reaction time off.
//
// The functions edit a Lyrics in place and keep its invariants (the reader's too, so that what is
// written reads back as what was edited):
//
//   - a time they set is held to the hundredth of a second, the file's own resolution, so that what
//     is held is what is written;
//   - lines are in text order and their starts never go backwards;
//   - a line's end is the next line's start, and the last line's the item's duration, its length,
//     or a gap past its start, as the reader decides it;
//   - a word starts at or after its line's start and at or after the word before it, and ends where
//     the next one starts or the line does.

namespace iclforge::lyrics {

// `lyrics` as `.lrc` text: UTF-8 without a byte-order mark, one line to a line, in the order held.
//
//   [ti:] [ar:] [al:] [by:] [length:mm:ss]   the id tags that are set
//   [v1:name] [v2:name]                      the duet's two singers' names, when set
//   [mm:ss.xx]text                           a timed line
//   [mm:ss.xx]V1: text                       a line of a duet, V1:, V2: or V1+V2: first
//   [mm:ss.xx]<mm:ss.xx>word <mm:ss.xx>word  a word-timed line, its words as they are spaced
//   [mm:ss.xx]                               a time that only marks an end
//   text                                     a line of an untimed file
//
// A time is written to the hundredth of a second, rounded to the nearest, which is the format's own
// resolution. The times are written as they are held: an `[offset:]` is never written, because the
// reader applied it. A line whose text begins `[` followed by something shaped like a time would be
// read back as a time, which LRC has no way to escape; nothing is done about it here.
//
// What a file's lines end at is not written by default: an LRC line ends where the next begins, which
// is what the reader gives a line it has no end for. A SubRip cue or an ASS event has an end of its
// own, and there is nothing sung between it and the next line; `mark_gaps` writes that.
struct LrcWriteOptions {
    // Where a timed line ends before the next one starts, write a timed line with no text at its end,
    // as an LRC file marks an instrumental gap, so that a reader of the file knows the line is over.
    // A line that ends where the next begins (every line of a plain LRC file) gets none, nor does
    // one whose end is not a hundredth of a second past its start. The last line's end is not
    // written: a reader decides that from the item's length.
    bool mark_gaps = false;
};

[[nodiscard]] std::string write_lrc(const Lyrics& lyrics, const LrcWriteOptions& options = {});

// A line's words with times spread across the line's span in proportion to their letters: what a
// person starts from and corrects, and never what is shown as sung. Empty for a line with no text.
// A word's text keeps the spacing after it, so the words joined are the line's text.
//
// "Letters" counts code points other than spaces and punctuation of ASCII, and other than the
// combining marks (the points and cantillation of Hebrew, which Yiddish uses, and the general
// combining diacritics): a word with its vowel points is as long as the word without them. A word
// of no letters (a dash) counts as one so that it has a time of its own.
[[nodiscard]] std::vector<LyricWord> estimate_words(const LyricLine& line);

// Where line `line` starts, from a press. Returns false, changing nothing, for a line that does not
// exist. The line is held no earlier than the one before it, and the lines after it are moved
// forward as far as they must be to stay in order (a line that was already later is not touched).
// The file becomes `timed`. Words that the line had are kept, moved by the same amount, so that a
// nudge of the line carries its words with it.
bool set_line_start(Lyrics& lyrics, std::size_t line, std::int64_t start_ms);

// Where word `word` of line `line` starts, from a press, held between the word before it and the one
// after (or the line's own start and end). False, changing nothing, for a word that does not exist.
bool set_word_start(Lyrics& lyrics, std::size_t line, std::size_t word, std::int64_t start_ms);

// Who sings a line (W11). False, changing nothing, for a line that does not exist. kUnmarked
// takes the marker off. Nothing about the line's time or text changes, so the marker is only
// written (write_lrc) and read back.
bool set_line_singer(Lyrics& lyrics, std::size_t line, Singer singer);

// Gives a line that has no word times the estimate (estimate_words) over its span. False when the
// line does not exist, has no text, or already has word times, which an estimate must not replace.
bool estimate_line(Lyrics& lyrics, std::size_t line);

// Recomputes every line's and word's end from the starts, as the reader does for a file: a line ends
// where the next begins, the last line at `duration_ms` when it is known, else at the file's length,
// else five seconds on. Call it after a set of edits; each function above leaves the ends right for
// the line it touched, and this makes them right for all.
void settle_ends(Lyrics& lyrics, std::optional<std::int64_t> duration_ms = std::nullopt);

}  // namespace iclforge::lyrics
