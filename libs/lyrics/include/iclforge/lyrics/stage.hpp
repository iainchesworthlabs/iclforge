#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "iclforge/lyrics/lyrics.hpp"

// What a screen shows of a song's words at one position (planning/karaoke-p4x-panel.md, D2): which
// lines are the past, the current and the next, which word is being sung and how far through it is,
// and, between lines, how long until the next one. The desktop's lyric view works this out in QML
// beside its controller; a sink's panel needs the same answer in C++ with no Qt, and a test needs it
// with no screen at all.
//
// A Stage holds INDICES into the document and a few numbers, never text: a board holds the document
// once and the drawing reads the strings from it. It has no heap in it (the rows are fixed arrays),
// so one per frame costs nothing a board has to count.
//
// Nothing here keeps a clock or a document: make_stage() is a function of its arguments, as
// lyric_position() is, and a seek, a pause and a stop are only a different position.

namespace iclforge::lyrics {

// The most lines either side of the current one that a Stage names. StageOptions asks for fewer.
inline constexpr std::size_t kStageMaxContext = 4;

enum class StageMode : std::uint8_t {
    // No document is held: nothing to draw but "no lyrics".
    kNoLyrics,
    // A file with no times (plain text): shown whole, not followed.
    kPlain,
    // Timed lyrics, and no position yet (before the first anchor has come due): the first lines are
    // `after`, so a singer sees what is coming before the sound starts.
    kWaiting,
    // Timed, with a position, and nothing is being sung: before the first line, after the last, on a
    // line that only marks a time, or in the space between two lines.
    kGap,
    // Timed, and `line` is being sung.
    kLine,
};

struct StageOptions {
    // Lines of text shown before the current one and after it (at most kStageMaxContext each). The
    // desktop view shows one and two, and a full-screen view two and three.
    std::size_t lines_before = 1;
    std::size_t lines_after = 2;
    // A gap at least this long is an instrumental break: the view shows it as one, with the count-in.
    // A shorter one (the breath between two subtitle cues) shows no current line and no count-in.
    std::int64_t instrumental_min_ms = 4000;
    // The count-in's dots, one a second, the last second of each lit one going out at the time.
    std::size_t count_in_dots = 4;
};

struct Stage {
    StageMode mode = StageMode::kNoLyrics;
    // The position the stage is for: the one asked, less the delay. Zero for kNoLyrics and kPlain.
    std::int64_t position_ms = 0;

    // kLine: the line being sung; kGap on a line that only marks a time: that line.
    std::size_t line = 0;
    // kLine: the line has word times. When it has none the whole line is the emphasis and no word is
    // named (no times are invented, planning/karaoke.md, decision 3).
    bool word_timed = false;
    // kLine and word_timed: the words in front of the one being sung are sung ([0, words_sung)), the
    // one being sung is `word` (none between two words), and the rest are not yet.
    std::size_t words_sung = 0;
    std::optional<std::size_t> word{};
    // How far through `word`, from 0 up to but not including 1, by time.
    double fraction = 0.0;
    // The line's singer (a duet's marker); kUnmarked for a solo and in every other mode.
    Singer singer = Singer::kUnmarked;

    // The lines that carry text before and after the current one, oldest and soonest first: lines
    // that only mark a time are not rows. In kGap `before` ends with the line that has just ended.
    std::array<std::size_t, kStageMaxContext> before{};
    std::size_t before_count = 0;
    std::array<std::size_t, kStageMaxContext> after{};
    std::size_t after_count = 0;

    // kGap: milliseconds to the start of the next line with text, and none after the last line.
    std::optional<std::int64_t> next_line_in_ms{};
    // kGap: how long the gap is in all (from the end of the line before, or the start of a line that
    // marks a time, or 0 before the first line, to the next line's start); none after the last line.
    std::optional<std::int64_t> gap_ms{};
    // kGap: the gap is an instrumental break (gap_ms >= StageOptions::instrumental_min_ms).
    bool instrumental = false;
    // kGap and instrumental: the dots still lit, from count_in_dots down to 1 as the next line
    // approaches. 0 outside an instrumental break and when nothing follows it.
    std::size_t count_in = 0;
};

// The stage of `lyrics` (null: none held) at `position_ms`, which is nullopt before the first anchor
// has come due; `delay_ms` is the singer's nudge, positive showing the lyrics later (as
// lyric_position's). The document is read, not kept: the Stage is valid for it until it is replaced.
[[nodiscard]] Stage make_stage(const Lyrics* lyrics, std::optional<std::int64_t> position_ms,
                               std::int64_t delay_ms = 0, const StageOptions& options = {});

// Whether `text` (UTF-8) reads right to left: its first strong character is Hebrew, Arabic, Syriac,
// Thaana or N'Ko, which is the Unicode bidirectional algorithm's rule for a paragraph's base direction
// (P2, P3) over those scripts. Digits, punctuation, spaces, symbols and bytes that are not UTF-8 are
// skipped; text with no strong character reads left to right. A screen sets the direction of a line
// from this, and runs a word's fill from its right edge.
[[nodiscard]] bool text_is_right_to_left(std::string_view text);

// A run of code points, `first` to `last` inclusive: what a font file carries, as a screen lists it.
struct CodePointRange {
    char32_t first = 0;
    char32_t last = 0;
};

// Whether every code point of `text` (UTF-8) is one the fonts can draw: in `ranges` (sorted by
// `first`, not overlapping) or a control character, which draws nothing. A byte sequence that is not
// UTF-8 is not covered. A screen that finds a line is not covered shows a sentence in its place,
// where the font would draw a missing-glyph box for each letter.
[[nodiscard]] bool text_is_covered(std::string_view text, std::span<const CodePointRange> ranges);

}  // namespace iclforge::lyrics
