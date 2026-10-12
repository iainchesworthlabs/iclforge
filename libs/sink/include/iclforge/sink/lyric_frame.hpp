#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "iclforge/lyrics/lyrics.hpp"
#include "iclforge/lyrics/stage.hpp"

// What a screen draws of a song's words at one moment, as values with the text in them
// (planning/hearth-android.md, phase 2; planning/hearth-sink-screen.md, S2 to S8).
//
// lyrics::Stage names indices into a document and keeps no text, which suits a board that holds the
// document once and draws from it. An Android view is on the other side of JNI and holds no
// document, so a frame is the Stage with the strings it names copied in: a handful of rows, built
// from the document the core holds and handed over once a frame. Nothing here is a clock or a
// document; make_lyric_frame() is a function of its arguments.

namespace iclforge::sink {

// One row of text on the stage.
struct LyricRow {
    std::string text;
    // A duet's singer for the line, kUnmarked for a solo.
    lyrics::Singer singer = lyrics::Singer::kUnmarked;
    // The line's own direction, by its first strong letter (SS4), whatever the UI language is.
    bool right_to_left = false;
};

struct LyricFrame {
    // The item the document is for, or -1 when the sink holds none.
    std::int64_t item = -1;
    // A stream is playing: the screen says "no lyrics" for a document that is not there, after its
    // grace (S7), and says nothing while the sink is idle.
    bool streaming = false;

    lyrics::StageMode mode = lyrics::StageMode::kNoLyrics;
    // Where in the item the stage is, less the singer's delay.
    std::int64_t position_ms = 0;

    // The document's tags, for the header: empty where the file has none.
    std::string title;
    std::string artist;
    std::string album;
    // The `[v1:]` and `[v2:]` names of a duet's two singers, and whether any line has a singer.
    std::string first_singer;
    std::string second_singer;
    bool duet = false;
    // The file has times (S2 to S5), or is plain text (S6).
    bool timed = false;
    std::optional<std::int64_t> length_ms{};

    // kLine: the line being sung, and its words with the spacing written after each, which joined
    // are its text. `words` is empty for a line-timed one (S5): nothing is invented for it.
    LyricRow current;
    bool word_timed = false;
    std::vector<std::string> words;
    // kLine and word_timed: the words in front of `word` are sung, `word` is being sung (none
    // between two words) and the rest are not yet.
    std::size_t words_sung = 0;
    std::optional<std::size_t> word{};
    // How far through `word`, from 0 up to but not including 1, by time.
    double fraction = 0.0;

    // The lines that carry text before and after the current one, oldest and soonest first.
    std::vector<LyricRow> before;
    std::vector<LyricRow> after;
    // kPlain: every line, shown whole and not followed (S6).
    std::vector<LyricRow> plain;

    // kGap: the time to the next line, how long the gap is in all, whether it is an instrumental
    // break (S4) and the count-in's dots still lit.
    std::optional<std::int64_t> next_line_in_ms{};
    std::optional<std::int64_t> gap_ms{};
    bool instrumental = false;
    std::size_t count_in = 0;

    // "Line 3 of 12": the current line's place among the lines that have text, and how many there
    // are; both 0 when there is nothing to count.
    std::size_t line_number = 0;
    std::size_t line_count = 0;
};

// The rows a screen is given: one line before the current one and three after it (the panel draws two
// after on its small scale, a TV draws all three; planning/hearth-sink-screen.md, Layout). One place,
// so that the app, its instrumented tests and the checked-in frames ask for the same.
inline constexpr lyrics::StageOptions kScreenStageOptions{.lines_before = 1, .lines_after = 3};

// The frame for `lyrics` (null: none held, of item `item` if the sink knows which) at `stage`, which
// make_stage() answered for the same document. `streaming` is whether a stream is playing.
[[nodiscard]] LyricFrame make_lyric_frame(std::optional<std::int64_t> item, const lyrics::Lyrics* lyrics,
                                          const lyrics::Stage& stage, bool streaming);

// The frame as one line of JSON for a layer that has no C++ types (the Android app's JNI): the same
// fields, in the same names, with the rows as arrays of objects. Strings are escaped to the letter
// of RFC 8259 and left in UTF-8.
[[nodiscard]] std::string lyric_frame_json(const LyricFrame& frame);

}  // namespace iclforge::sink
