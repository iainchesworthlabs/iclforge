#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>

#include "iclforge/lyrics/lyrics.hpp"
#include "iclforge/lyrics/timing.hpp"

// libs/lyrics/src/timing.cpp: what a tap editor edits and saves (planning/karaoke-phase2.md, W6).
// The writer is held to the reader (a file written reads back as what was written), and the editing
// functions to the invariants the reader itself keeps.

namespace {

using iclforge::lyrics::Lyrics;
using iclforge::lyrics::LyricLine;
using iclforge::lyrics::LyricWord;

Lyrics parsed(const std::string& text, std::optional<std::int64_t> duration = std::nullopt) {
    const auto lyrics = iclforge::lyrics::parse_lyrics(text, duration);
    REQUIRE(lyrics.has_value());
    return *lyrics;
}

// The words of a line as "start:text" for a compact comparison.
std::string show_words(const LyricLine& line) {
    std::string out;
    for (const LyricWord& word : line.words) {
        out += std::to_string(word.start_ms) + ":" + word.text + "|";
    }
    return out;
}

// Hebrew "shalom olam" and "shalom" with its points (shin dot, qamats, vav holam).
const std::string kShalom = "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D";
const std::string kShalomPointed = "\xD7\xA9\xD7\x81\xD7\x9C\xD6\xB8\xD7\x95\xD6\xB9\xD7\x9D";
const std::string kOlam = "\xD7\xA2\xD7\x95\xD7\x9C\xD7\x9D";

}  // namespace

TEST_CASE("timing: what is written reads back as what was written", "[lyrics][timing]") {
    const std::string source =
        "[ti:Auld Lang Syne]\n[ar:Traditional]\n[length:01:30]\n"
        "[00:12.00]<00:12.00>Should <00:12.50>auld <00:13.20>acquaintance\n"
        "[00:15.00]And never brought to mind?\n"
        "[00:20.00]\n";
    const Lyrics first = parsed(source, 90000);
    const std::string written = iclforge::lyrics::write_lrc(first);
    const Lyrics second = parsed(written, 90000);

    CHECK(second.title == "Auld Lang Syne");
    CHECK(second.artist == "Traditional");
    CHECK(second.length_ms == 90000);
    CHECK(second.timed);
    REQUIRE(second.lines.size() == first.lines.size());
    for (std::size_t i = 0; i < first.lines.size(); ++i) {
        CHECK(second.lines[i].start_ms == first.lines[i].start_ms);
        CHECK(second.lines[i].end_ms == first.lines[i].end_ms);
        CHECK(second.lines[i].text == first.lines[i].text);
        CHECK(show_words(second.lines[i]) == show_words(first.lines[i]));
    }
    // A second write is the first: nothing drifts.
    CHECK(iclforge::lyrics::write_lrc(second) == written);
}

TEST_CASE("timing: a time is written to the nearest hundredth, and an untimed file is text",
          "[lyrics][timing]") {
    Lyrics lyrics = parsed("one\ntwo\nthree\n");
    REQUIRE_FALSE(lyrics.timed);
    // An untimed file is written as it is: lines of text, no tags.
    CHECK(iclforge::lyrics::write_lrc(lyrics) == "one\ntwo\nthree\n");

    REQUIRE(iclforge::lyrics::set_line_start(lyrics, 0, 1234));
    REQUIRE(iclforge::lyrics::set_line_start(lyrics, 1, 61236));
    REQUIRE(iclforge::lyrics::set_line_start(lyrics, 2, 601005));
    const std::string written = iclforge::lyrics::write_lrc(lyrics);
    CHECK(written == "[00:01.23]one\n[01:01.24]two\n[10:01.01]three\n");
}

TEST_CASE("timing: a time is held to the hundredth, so that what is held is what is written",
          "[lyrics][timing]") {
    Lyrics lyrics = parsed("a\nb\n");
    REQUIRE(iclforge::lyrics::set_line_start(lyrics, 0, 1234));
    CHECK(lyrics.lines[0].start_ms == 1230);
    REQUIRE(iclforge::lyrics::set_line_start(lyrics, 1, 5005));
    CHECK(lyrics.lines[1].start_ms == 5010);  // half up
    REQUIRE(iclforge::lyrics::set_line_start(lyrics, 0, -300));
    CHECK(lyrics.lines[0].start_ms == 0);
}

TEST_CASE("timing: a line's start is held in order, and later lines are pushed, not shuffled",
          "[lyrics][timing]") {
    Lyrics lyrics = parsed("a\nb\nc\nd\n");
    REQUIRE(iclforge::lyrics::set_line_start(lyrics, 0, 1000));
    REQUIRE(iclforge::lyrics::set_line_start(lyrics, 1, 4000));
    REQUIRE(iclforge::lyrics::set_line_start(lyrics, 3, 9000));
    CHECK(lyrics.timed);
    // A press earlier than the line before is held at that line's start, not before it.
    REQUIRE(iclforge::lyrics::set_line_start(lyrics, 2, 500));
    CHECK(lyrics.lines[2].start_ms == 4000);
    // Moving a line later pushes the ones that would be earlier, and leaves a later one alone.
    REQUIRE(iclforge::lyrics::set_line_start(lyrics, 1, 12000));
    CHECK(lyrics.lines[1].start_ms == 12000);
    CHECK(lyrics.lines[2].start_ms == 12000);
    CHECK(lyrics.lines[3].start_ms == 12000);
    // The one before's end is where this begins.
    CHECK(lyrics.lines[0].end_ms == 12000);
    // A line that is not there is refused and nothing moves.
    CHECK_FALSE(iclforge::lyrics::set_line_start(lyrics, 4, 1));
    CHECK(lyrics.lines[0].start_ms == 1000);
}

TEST_CASE("timing: moving a line moves its words with it", "[lyrics][timing]") {
    Lyrics lyrics = parsed("[00:10.00]<00:10.00>one <00:11.00>two\n[00:20.00]x\n", 30000);
    REQUIRE(iclforge::lyrics::set_line_start(lyrics, 0, 12000));
    CHECK(lyrics.lines[0].start_ms == 12000);
    CHECK(show_words(lyrics.lines[0]) == "12000:one |13000:two|");
}

TEST_CASE("timing: an estimate spreads a line's words by their letters", "[lyrics][timing]") {
    LyricLine line;
    line.start_ms = 1000;
    line.end_ms = 7000;
    line.text = "aa bbbb";
    const auto words = iclforge::lyrics::estimate_words(line);
    REQUIRE(words.size() == 2);
    // Two letters and four, over six seconds: a third and two thirds.
    CHECK(words[0].text == "aa ");
    CHECK(words[0].start_ms == 1000);
    CHECK(words[0].end_ms == 3000);
    CHECK(words[1].text == "bbbb");
    CHECK(words[1].start_ms == 3000);
    CHECK(words[1].end_ms == 7000);
    // The words joined are the line.
    CHECK(words[0].text + words[1].text == line.text);

    CHECK(iclforge::lyrics::estimate_words(LyricLine{}).empty());
}

TEST_CASE("timing: punctuation does not lengthen a word, and a word of none is not silent",
          "[lyrics][timing]") {
    LyricLine line;
    line.start_ms = 0;
    line.end_ms = 4000;
    // "hi," (two letters) and "-" (none): the dash counts as one, so it has a time of its own.
    line.text = "hi, - ok";
    const auto words = iclforge::lyrics::estimate_words(line);
    REQUIRE(words.size() == 3);
    CHECK(words[0].text == "hi, ");
    CHECK(words[1].text == "- ");
    CHECK(words[2].text == "ok");
    CHECK(words[0].start_ms == 0);
    CHECK(words[1].start_ms == 1600);  // 2 of 5
    CHECK(words[2].start_ms == 2400);  // 3 of 5
    CHECK(words[2].end_ms == 4000);
}

TEST_CASE("timing: Hebrew's points do not lengthen a word, and the text is kept whole",
          "[lyrics][timing]") {
    LyricLine plain;
    plain.start_ms = 0;
    plain.end_ms = 8000;
    plain.text = kShalom + " " + kOlam;
    LyricLine pointed = plain;
    pointed.text = kShalomPointed + " " + kOlam;

    const auto a = iclforge::lyrics::estimate_words(plain);
    const auto b = iclforge::lyrics::estimate_words(pointed);
    REQUIRE(a.size() == 2);
    REQUIRE(b.size() == 2);
    // Four letters each: the same split with or without the points.
    CHECK(a[1].start_ms == 4000);
    CHECK(b[1].start_ms == 4000);
    CHECK(b[0].text + b[1].text == pointed.text);
}

TEST_CASE("timing: a word's start is held between its neighbours", "[lyrics][timing]") {
    Lyrics lyrics = parsed("[00:10.00]one two three\n[00:20.00]x\n", 30000);
    REQUIRE(iclforge::lyrics::estimate_line(lyrics, 0));
    REQUIRE(lyrics.lines[0].words.size() == 3);
    const auto first = lyrics.lines[0].words[0].start_ms;
    CHECK(first == 10000);

    // Pressed far too early: held at the line's start, not before it.
    REQUIRE(iclforge::lyrics::set_word_start(lyrics, 0, 1, 2000));
    CHECK(lyrics.lines[0].words[1].start_ms == 10000);
    // Pressed past the word after it: held at that word's start.
    const auto third = lyrics.lines[0].words[2].start_ms;
    REQUIRE(iclforge::lyrics::set_word_start(lyrics, 0, 1, 99000));
    CHECK(lyrics.lines[0].words[1].start_ms == third);
    // In between it is taken, and the ends follow.
    REQUIRE(iclforge::lyrics::set_word_start(lyrics, 0, 1, 11000));
    CHECK(lyrics.lines[0].words[1].start_ms == 11000);
    REQUIRE(iclforge::lyrics::set_word_start(lyrics, 0, 2, 12500));
    CHECK(lyrics.lines[0].words[2].start_ms == 12500);
    CHECK(lyrics.lines[0].words[1].end_ms == 12500);
    CHECK(lyrics.lines[0].words[2].end_ms == 20000);

    CHECK_FALSE(iclforge::lyrics::set_word_start(lyrics, 0, 3, 1));
    CHECK_FALSE(iclforge::lyrics::set_word_start(lyrics, 5, 0, 1));
}

TEST_CASE("timing: an estimate is made once, and not for text that is not there",
          "[lyrics][timing]") {
    Lyrics lyrics = parsed("[00:01.00]hello world\n[00:05.00]\n[00:09.00]<00:09.00>x\n", 12000);
    CHECK(iclforge::lyrics::estimate_line(lyrics, 0));
    // It has words now, and a person's corrections are not replaced by another estimate.
    CHECK_FALSE(iclforge::lyrics::estimate_line(lyrics, 0));
    // A marker line has no text; a line with tapped words keeps them.
    CHECK_FALSE(iclforge::lyrics::estimate_line(lyrics, 1));
    CHECK_FALSE(iclforge::lyrics::estimate_line(lyrics, 2));
    CHECK_FALSE(iclforge::lyrics::estimate_line(lyrics, 9));
}

TEST_CASE("timing: the last line's end is the duration, the length, or a gap on", "[lyrics][timing]") {
    Lyrics lyrics = parsed("a\nb\n");
    REQUIRE(iclforge::lyrics::set_line_start(lyrics, 0, 1000));
    REQUIRE(iclforge::lyrics::set_line_start(lyrics, 1, 4000));

    iclforge::lyrics::settle_ends(lyrics);
    CHECK(lyrics.lines[0].end_ms == 4000);
    CHECK(lyrics.lines[1].end_ms == 9000);  // five seconds on

    lyrics.length_ms = 20000;
    iclforge::lyrics::settle_ends(lyrics);
    CHECK(lyrics.lines[1].end_ms == 20000);

    iclforge::lyrics::settle_ends(lyrics, 30000);
    CHECK(lyrics.lines[1].end_ms == 30000);
}

TEST_CASE("timing: the whole tap pass, from plain text to a file that reads back", "[lyrics][timing]") {
    // Plain text with no times: a Hebrew line and a Yiddish one, then an English one.
    const std::string text =
        kShalom + " " + kOlam + "\n\xD7\xB0\xD7\x90\xD6\xB8\xD7\xA1 \xD7\x96\xD7\x90\xD6\xB8\xD7\x92\n"
        "Should auld acquaintance\n";
    Lyrics lyrics = parsed(text);
    REQUIRE_FALSE(lyrics.timed);
    REQUIRE(lyrics.lines.size() == 3);

    // The line pass.
    REQUIRE(iclforge::lyrics::set_line_start(lyrics, 0, 1000));
    REQUIRE(iclforge::lyrics::set_line_start(lyrics, 1, 5000));
    REQUIRE(iclforge::lyrics::set_line_start(lyrics, 2, 9000));
    // The word pass: an estimate everywhere, then one correction.
    for (std::size_t i = 0; i < 3; ++i) {
        REQUIRE(iclforge::lyrics::estimate_line(lyrics, i));
    }
    REQUIRE(iclforge::lyrics::set_word_start(lyrics, 2, 1, 10200));
    iclforge::lyrics::settle_ends(lyrics, 15000);

    const std::string written = iclforge::lyrics::write_lrc(lyrics);
    const Lyrics back = parsed(written, 15000);
    REQUIRE(back.timed);
    REQUIRE(back.lines.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(back.lines[i].start_ms == lyrics.lines[i].start_ms);
        CHECK(back.lines[i].text == lyrics.lines[i].text);
        REQUIRE(back.lines[i].word_timed());
        CHECK(show_words(back.lines[i]) == show_words(lyrics.lines[i]));
    }
    // The corrected word is where it was put (to the hundredth), and the line text is unchanged.
    CHECK(back.lines[2].words[1].start_ms == 10200);
    CHECK(back.lines[2].text == "Should auld acquaintance");
    CHECK(back.lines[2].end_ms == 15000);
}
