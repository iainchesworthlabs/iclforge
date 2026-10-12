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

TEST_CASE("timing: a duet's singers and their names are written and read back", "[lyrics][timing][duet]") {
    using iclforge::lyrics::Singer;
    const std::string source =
        "[ti:Leave Her, Johnny]\n[v1:Shantyman]\n[v2:Crew]\n"
        "[00:01.00]V1: <00:01.00>Oh <00:01.40>the times\n"
        "[00:03.00]V2: Leave her, Johnny\n"
        "[00:05.00]V1+V2: Leave her\n"
        "[00:07.00]No one in particular\n";
    const Lyrics first = parsed(source, 20000);
    const std::string written = iclforge::lyrics::write_lrc(first);
    // The marker is the first thing after the time, before any word tag, and the names are tags.
    CHECK(written.find("[v1:Shantyman]\n[v2:Crew]\n") != std::string::npos);
    CHECK(written.find("[00:01.00]V1: <00:01.00>Oh ") != std::string::npos);
    CHECK(written.find("[00:03.00]V2: Leave her, Johnny\n") != std::string::npos);
    CHECK(written.find("[00:05.00]V1+V2: Leave her\n") != std::string::npos);
    CHECK(written.find("[00:07.00]No one in particular\n") != std::string::npos);

    const Lyrics second = parsed(written, 20000);
    REQUIRE(second.lines.size() == first.lines.size());
    for (std::size_t i = 0; i < first.lines.size(); ++i) {
        CHECK(second.lines[i].singer == first.lines[i].singer);
        CHECK(second.lines[i].text == first.lines[i].text);
        CHECK(second.lines[i].start_ms == first.lines[i].start_ms);
    }
    CHECK(second.first_singer_name == "Shantyman");
    CHECK(second.second_singer_name == "Crew");
    // And written once more it is the same text.
    CHECK(iclforge::lyrics::write_lrc(second) == written);
}

TEST_CASE("timing: the singer of a line is set, and an untimed duet keeps its markers",
          "[lyrics][timing][duet]") {
    using iclforge::lyrics::Singer;
    Lyrics lyrics = parsed("First line\nSecond line\nThird line\n");
    CHECK_FALSE(iclforge::lyrics::has_singers(lyrics));
    CHECK(iclforge::lyrics::set_line_singer(lyrics, 1, Singer::kSecond));
    CHECK(iclforge::lyrics::set_line_singer(lyrics, 0, Singer::kBoth));
    CHECK_FALSE(iclforge::lyrics::set_line_singer(lyrics, 3, Singer::kFirst));
    CHECK(iclforge::lyrics::has_singers(lyrics));
    // Untimed: the text with its markers, which is what a person marked up before timing it.
    CHECK(iclforge::lyrics::write_lrc(lyrics) == "V1+V2: First line\nV2: Second line\nThird line\n");
    // Timing the lines (the tap pass) leaves who sings each alone, and the file reads back so.
    CHECK(iclforge::lyrics::set_line_start(lyrics, 0, 1000));
    CHECK(iclforge::lyrics::set_line_start(lyrics, 1, 3000));
    CHECK(iclforge::lyrics::set_line_start(lyrics, 2, 5000));
    const Lyrics back = parsed(iclforge::lyrics::write_lrc(lyrics));
    REQUIRE(back.lines.size() == 3);
    CHECK(back.lines[0].singer == Singer::kBoth);
    CHECK(back.lines[1].singer == Singer::kSecond);
    CHECK(back.lines[2].singer == Singer::kUnmarked);
    CHECK(back.lines[1].start_ms == 3000);
    // Taking the marker off is setting kUnmarked.
    CHECK(iclforge::lyrics::set_line_singer(lyrics, 1, Singer::kUnmarked));
    const std::string cleared = iclforge::lyrics::write_lrc(lyrics);
    CHECK(cleared.find("]V2:") == std::string::npos);
    // (The first line is still both, whose marker has a V2 in it.)
    CHECK(cleared.find("[00:01.00]V1+V2: First line\n") != std::string::npos);
    CHECK(cleared.find("[00:03.00]Second line\n") != std::string::npos);
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

TEST_CASE("timing: a gap between cues is written as a time with no text when asked, and reads back as an end",
          "[lyrics][timing][gaps]") {
    // A SubRip file has an end for each cue. The second and third cues touch, so nothing is sung
    // between them; the first and second have two seconds of silence between them.
    const auto subrip = iclforge::lyrics::parse_subrip(
        "1\n00:00:01,000 --> 00:00:03,000\nOne\n\n"
        "2\n00:00:05,000 --> 00:00:07,000\nTwo\n\n"
        "3\n00:00:07,000 --> 00:00:09,000\nThree\n");
    REQUIRE(subrip.has_value());

    // By default an LRC line ends where the next begins, so the gap is not written.
    CHECK(iclforge::lyrics::write_lrc(*subrip) == "[00:01.00]One\n[00:05.00]Two\n[00:07.00]Three\n");

    const std::string marked =
        iclforge::lyrics::write_lrc(*subrip, iclforge::lyrics::LrcWriteOptions{.mark_gaps = true});
    // The gap after One is marked; Two runs into Three, and the last line's end is not written.
    CHECK(marked == "[00:01.00]One\n[00:03.00]\n[00:05.00]Two\n[00:07.00]Three\n");

    // Read back, One ends where its cue did, and not at the next line's start.
    const Lyrics again = parsed(marked);
    REQUIRE(again.lines.size() == 4);
    CHECK(again.lines[0].text == "One");
    CHECK(again.lines[0].end_ms == 3000);
    CHECK(again.lines[1].text.empty());
    CHECK(again.lines[2].text == "Two");
    CHECK(again.lines[2].end_ms == 7000);
}

TEST_CASE("timing: marking gaps leaves an LRC file as it was", "[lyrics][timing][gaps]") {
    // Each line of an LRC file ends where the next begins, so there is no gap to mark.
    const std::string source = "[00:01.00]One\n[00:03.00]Two\n[00:05.00]Three\n";
    const Lyrics lyrics = parsed(source);
    CHECK(iclforge::lyrics::write_lrc(lyrics, iclforge::lyrics::LrcWriteOptions{.mark_gaps = true}) ==
          iclforge::lyrics::write_lrc(lyrics));

    // An end that is not a hundredth of a second past the start is not a gap either, and a line
    // that is only a time is not given another.
    Lyrics built;
    built.timed = true;
    built.lines.push_back({.start_ms = 1000, .end_ms = 1004, .text = "Blink"});
    built.lines.push_back({.start_ms = 3000, .end_ms = 4000, .text = ""});
    built.lines.push_back({.start_ms = 6000, .end_ms = 7000, .text = "After"});
    CHECK(iclforge::lyrics::write_lrc(built, iclforge::lyrics::LrcWriteOptions{.mark_gaps = true}) ==
          "[00:01.00]Blink\n[00:03.00]\n[00:06.00]After\n");
}

