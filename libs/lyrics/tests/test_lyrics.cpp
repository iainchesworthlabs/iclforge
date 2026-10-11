#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "platform/process.hpp"

#include "iclforge/lyrics/lyrics.hpp"

// libs/lyrics/src/lyrics.cpp: the LRC reader, the lyrics file's place
// beside an item, and the cursor the lyric view asks on every frame
// (planning/karaoke.md, L1 to L6).

namespace {

namespace fs = std::filesystem;

using iclforge::lyrics::LyricPosition;
using iclforge::lyrics::Lyrics;
using iclforge::lyrics::WordState;

Lyrics parsed(const std::string& text, std::optional<std::int64_t> duration = std::nullopt) {
    const auto lyrics = iclforge::lyrics::parse_lyrics(text, duration);
    REQUIRE(lyrics.has_value());
    return *lyrics;
}

// A folder of this process's own under the test scratch directory.
class Folder {
public:
    Folder()
        : path_(fs::path{ICLFORGE_TEST_SCRATCH_DIR} /
                ("lyrics_" + iclforge::test::platform::process_id())) {
        fs::remove_all(path_);
        fs::create_directories(path_);
    }
    ~Folder() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    Folder(const Folder&) = delete;
    Folder& operator=(const Folder&) = delete;

    fs::path write(const std::string& name, const std::string& text) const {
        const fs::path file = path_ / name;
        std::ofstream out(file, std::ios::binary);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        REQUIRE(out.good());
        return file;
    }

    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

// Two lines, the first with word tags and a closing tag, the second without.
const char* const kWordTimed =
    "[00:12.00]<00:12.00>Should <00:12.50>auld <00:13.20>acquaintance <00:14.00>be "
    "<00:14.40>forgot,<00:15.00>\n"
    "[00:16.00]And never\n";

}  // namespace

TEST_CASE("lyrics: lines, metadata and the last line's end", "[lyrics]") {
    const Lyrics lyrics = parsed(
        "[ti:Auld Lang Syne]\n[ar:Traditional]\n[al:Songs]\n[by:me]\n[length:3:42]\n"
        "[00:12.50]Should auld acquaintance be forgot,\n"
        "[00:17.00]And never brought to mind?\n"
        "[00:21.3]And auld lang syne?\n");
    CHECK(lyrics.title == "Auld Lang Syne");
    CHECK(lyrics.artist == "Traditional");
    CHECK(lyrics.album == "Songs");
    CHECK(lyrics.author == "me");
    REQUIRE(lyrics.length_ms.has_value());
    CHECK(*lyrics.length_ms == 222000);
    CHECK(lyrics.timed);
    REQUIRE(lyrics.lines.size() == 3);
    CHECK(lyrics.lines[0].start_ms == 12500);
    CHECK(lyrics.lines[0].end_ms == 17000);
    CHECK(lyrics.lines[0].text == "Should auld acquaintance be forgot,");
    CHECK_FALSE(lyrics.lines[0].word_timed());
    CHECK(lyrics.lines[1].start_ms == 17000);
    CHECK(lyrics.lines[2].start_ms == 21300);
    // No duration given: the file's own length ends the last line.
    CHECK(lyrics.lines[2].end_ms == 222000);

    // The item's duration, when it is known, comes first.
    const Lyrics with_duration = parsed("[00:10.00]a\n[00:20.00]b\n", 60000);
    CHECK(with_duration.lines[1].end_ms == 60000);
    // And with neither, five seconds.
    const Lyrics alone = parsed("[00:10.00]a\n[00:20.00]b\n");
    CHECK(alone.lines[1].end_ms == 25000);
}

TEST_CASE("lyrics: the time forms and a tag that is not one", "[lyrics]") {
    const Lyrics lyrics = parsed(
        "[00:05]whole\n[1:02.5]tenth\n[01:03.34]hundredth\n[01:04.345]milli\n[01:05:30]colon\n"
        "[00:61.00]seconds out of range\n[Chorus]not a time\n");
    REQUIRE(lyrics.lines.size() == 5);
    CHECK(lyrics.lines[0].start_ms == 5000);
    CHECK(lyrics.lines[1].start_ms == 62500);
    CHECK(lyrics.lines[2].start_ms == 63340);
    CHECK(lyrics.lines[3].start_ms == 64345);
    CHECK(lyrics.lines[4].start_ms == 65300);
    // In a timed file a line without a time is not shown.
    for (const auto& line : lyrics.lines) {
        CHECK(line.text.find("not a time") == std::string::npos);
        CHECK(line.text.find("out of range") == std::string::npos);
    }
}

TEST_CASE("lyrics: several time tags put a line at each time, in order", "[lyrics]") {
    const Lyrics lyrics = parsed("[00:10.00][00:40.00]Chorus\n[00:20.00]Verse\n");
    REQUIRE(lyrics.lines.size() == 3);
    CHECK(lyrics.lines[0].text == "Chorus");
    CHECK(lyrics.lines[0].start_ms == 10000);
    CHECK(lyrics.lines[0].end_ms == 20000);
    CHECK(lyrics.lines[1].text == "Verse");
    CHECK(lyrics.lines[2].text == "Chorus");
    CHECK(lyrics.lines[2].start_ms == 40000);
}

TEST_CASE("lyrics: an offset moves every time, and a positive one makes lyrics sooner",
          "[lyrics]") {
    const Lyrics sooner = parsed("[offset:+500]\n[00:10.00]x\n[00:00.10]y\n");
    CHECK(sooner.offset_ms == 500);
    REQUIRE(sooner.lines.size() == 2);
    CHECK(sooner.lines[0].start_ms == 0);  // 100 ms less 500, held at the start
    CHECK(sooner.lines[1].start_ms == 9500);

    const Lyrics later = parsed("[00:10.00]x\n[offset:-250]\n");
    REQUIRE(later.lines.size() == 1);
    CHECK(later.lines[0].start_ms == 10250);
}

TEST_CASE("lyrics: word tags give each word its time", "[lyrics]") {
    const Lyrics lyrics = parsed(kWordTimed);
    REQUIRE(lyrics.lines.size() == 2);
    const auto& line = lyrics.lines[0];
    REQUIRE(line.word_timed());
    REQUIRE(line.words.size() == 5);
    CHECK(line.text == "Should auld acquaintance be forgot,");
    // The tags are never text, and a word keeps the spacing written after it.
    CHECK(line.words[0].text == "Should ");
    CHECK(line.words[2].text == "acquaintance ");
    CHECK(line.words[4].text == "forgot,");
    CHECK(line.words[0].start_ms == 12000);
    CHECK(line.words[0].end_ms == 12500);
    CHECK(line.words[2].start_ms == 13200);
    CHECK(line.words[2].end_ms == 14000);
    // A closing tag ends the last word; the line runs on to the next one.
    CHECK(line.words[4].start_ms == 14400);
    CHECK(line.words[4].end_ms == 15000);
    CHECK(line.end_ms == 16000);
    // The next line has times and no word tags: it is not word-timed.
    CHECK_FALSE(lyrics.lines[1].word_timed());
    CHECK(lyrics.lines[1].text == "And never");
}

TEST_CASE("lyrics: text before a line's first word tag starts with the line",
          "[lyrics]") {
    const Lyrics lyrics = parsed("[00:05.00]Oh <00:05.50>yes\n[00:08.00]end\n");
    REQUIRE(lyrics.lines[0].words.size() == 2);
    CHECK(lyrics.lines[0].words[0].text == "Oh ");
    CHECK(lyrics.lines[0].words[0].start_ms == 5000);
    CHECK(lyrics.lines[0].words[0].end_ms == 5500);
    CHECK(lyrics.lines[0].words[1].text == "yes");
    CHECK(lyrics.lines[0].words[1].end_ms == 8000);
}

TEST_CASE("lyrics: a repeated line keeps its words' spacing in time", "[lyrics]") {
    const Lyrics lyrics = parsed("[00:10.00][00:30.00]<00:10.00>la <00:10.50>la\n[00:50.00]x\n");
    REQUIRE(lyrics.lines.size() == 3);
    CHECK(lyrics.lines[0].words[1].start_ms == 10500);
    REQUIRE(lyrics.lines[1].start_ms == 30000);
    CHECK(lyrics.lines[1].words[0].start_ms == 30000);
    CHECK(lyrics.lines[1].words[1].start_ms == 30500);
}

TEST_CASE("lyrics: a file with no times is plain text and is not followed", "[lyrics]") {
    const Lyrics lyrics = parsed("[ti:Plain]\nLine one\n\n[Chorus]\nLine two\n");
    CHECK_FALSE(lyrics.timed);
    CHECK(lyrics.title == "Plain");
    REQUIRE(lyrics.lines.size() == 3);
    CHECK(lyrics.lines[0].text == "Line one");
    CHECK(lyrics.lines[1].text == "[Chorus]");
    CHECK(lyrics.lines[2].text == "Line two");
    const LyricPosition where = iclforge::lyrics::lyric_position(lyrics, 5000);
    CHECK_FALSE(where.line.has_value());
}

TEST_CASE("lyrics: a byte-order mark, and every kind of line ending", "[lyrics]") {
    const Lyrics lyrics = parsed("\xEF\xBB\xBF[00:01.00]a\r\n[00:02.00]b\r[00:03.00]c\n");
    REQUIRE(lyrics.lines.size() == 3);
    CHECK(lyrics.lines[0].text == "a");
    CHECK(lyrics.lines[1].text == "b");
    CHECK(lyrics.lines[2].text == "c");
    const Lyrics accents = parsed("[00:01.00]caf\xC3\xA9 \xE2\x80\x94 \xF0\x9F\x8E\xA4\n");
    CHECK(accents.lines[0].text == "caf\xC3\xA9 \xE2\x80\x94 \xF0\x9F\x8E\xA4");
}

TEST_CASE("lyrics: what is not lyrics says why", "[lyrics]") {
    const auto empty = iclforge::lyrics::parse_lyrics("  \r\n \n");
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error().find("empty") != std::string::npos);

    // Latin-1 and a truncated or overlong sequence are not UTF-8, and are not
    // guessed at.
    for (const char* bad : {"[00:01.00]caf\xE9\n", "[00:01.00]\xE2\x82\n", "\xC0\x80[00:01.00]x\n",
                            "[00:01.00]\xED\xA0\x80\n"}) {
        const auto result = iclforge::lyrics::parse_lyrics(bad);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("UTF-8") != std::string::npos);
    }

    const auto tags_only = iclforge::lyrics::parse_lyrics("[ti:x]\n[ar:y]\n");
    REQUIRE_FALSE(tags_only.has_value());
    CHECK(tags_only.error().find("no lyrics") != std::string::npos);
}

// ---- SubRip (planning/karaoke-phase2.md, W13c) -------------------------------------------------

TEST_CASE("subrip: cues are lines that end where their cue does, with the gap between them",
          "[lyrics][subrip]") {
    const std::string text =
        "1\n00:00:12,000 --> 00:00:15,500\nShould auld acquaintance\nbe forgot\n\n"
        "2\n00:00:20,250 --> 00:00:22,000\nAnd never brought to mind?\n";
    const auto lyrics = iclforge::lyrics::parse_subrip(text);
    REQUIRE(lyrics.has_value());
    CHECK(lyrics->timed);
    REQUIRE(lyrics->lines.size() == 2);
    CHECK(lyrics->lines[0].start_ms == 12000);
    CHECK(lyrics->lines[0].end_ms == 15500);
    // A cue's text lines are one line of lyrics.
    CHECK(lyrics->lines[0].text == "Should auld acquaintance be forgot");
    CHECK_FALSE(lyrics->lines[0].word_timed());
    CHECK(lyrics->lines[1].start_ms == 20250);
    CHECK(lyrics->lines[1].end_ms == 22000);

    // The gap between the cues is nothing being sung, which an LRC file cannot say.
    const auto in_gap = iclforge::lyrics::lyric_position(*lyrics, 17000);
    REQUIRE(in_gap.line.has_value());
    CHECK(*in_gap.line == 0);
    CHECK_FALSE(in_gap.in_line);
    const auto in_cue = iclforge::lyrics::lyric_position(*lyrics, 21000);
    REQUIRE(in_cue.line.has_value());
    CHECK(*in_cue.line == 1);
    CHECK(in_cue.in_line);
}

TEST_CASE("subrip: styling and position tags are dropped, and what is not a tag stays",
          "[lyrics][subrip]") {
    const std::string text =
        "1\n00:00:01,000 --> 00:00:02,000 X1:63 X2:223 Y1:43 Y2:58\n"
        "<i>Should</i> <b>auld</b> {\\an8}acquaintance <font color=\"#ff0000\">be</font>\n"
        "\n"
        "2\n00:00:03,000 --> 00:00:04,000\n"
        "Mind 1 < 2 and 3 > 2 {\\i1}fine\n";
    const auto lyrics = iclforge::lyrics::parse_subrip(text);
    REQUIRE(lyrics.has_value());
    REQUIRE(lyrics->lines.size() == 2);
    CHECK(lyrics->lines[0].text == "Should auld acquaintance be");
    CHECK(lyrics->lines[0].end_ms == 2000);
    // A lone angle bracket is a character; the override block is not.
    CHECK(lyrics->lines[1].text == "Mind 1 < 2 and 3 > 2 fine");
}

TEST_CASE("subrip: numbers are not needed, a dot is a comma, and cues out of order are put in order",
          "[lyrics][subrip]") {
    const std::string text =
        "00:01:00.5 --> 00:01:02.25\nLate\n\n"
        "00:00:05,000 --> 00:00:06,000\nEarly\n\n"
        "\n\n"
        "00:10:00,000 --> 00:10:01,000\nVery late\n";
    const auto lyrics = iclforge::lyrics::parse_subrip(text);
    REQUIRE(lyrics.has_value());
    REQUIRE(lyrics->lines.size() == 3);
    CHECK(lyrics->lines[0].text == "Early");
    CHECK(lyrics->lines[1].text == "Late");
    // ".5" is half a second and ".25" a quarter.
    CHECK(lyrics->lines[1].start_ms == 60500);
    CHECK(lyrics->lines[1].end_ms == 62250);
    CHECK(lyrics->lines[2].start_ms == 600000);
}

TEST_CASE("subrip: a cue with no blank line after it, a cue that ends before it starts, and one with no text",
          "[lyrics][subrip]") {
    const std::string text =
        "00:00:01,000 --> 00:00:02,000\nOne\n"
        "00:00:03,000 --> 00:00:02,000\nBackwards\n"
        "00:00:05,000 --> 00:00:06,000\n\n"
        "00:00:07,000 --> 00:00:08,000\nLast\n";
    const auto lyrics = iclforge::lyrics::parse_subrip(text);
    REQUIRE(lyrics.has_value());
    REQUIRE(lyrics->lines.size() == 4);
    CHECK(lyrics->lines[0].text == "One");
    CHECK(lyrics->lines[0].end_ms == 2000);
    // An end before the start is the start.
    CHECK(lyrics->lines[1].start_ms == 3000);
    CHECK(lyrics->lines[1].end_ms == 3000);
    // A cue with no text is a time that marks nothing, kept as an empty line.
    CHECK(lyrics->lines[2].text.empty());
    CHECK(lyrics->lines[3].text == "Last");
}

TEST_CASE("subrip: the encodings are the LRC reader's, and what is not subtitles says so",
          "[lyrics][subrip]") {
    const std::string cue = "1\n00:00:01,000 --> 00:00:02,000\nShalom \xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D\n";
    // A UTF-8 byte-order mark, and CR LF line ends.
    std::string crlf = "\xEF\xBB\xBF";
    for (const char c : cue) {
        if (c == '\n') {
            crlf += "\r\n";
        } else {
            crlf.push_back(c);
        }
    }
    const auto with_mark = iclforge::lyrics::parse_subrip(crlf);
    REQUIRE(with_mark.has_value());
    REQUIRE(with_mark->lines.size() == 1);
    CHECK(with_mark->lines[0].text == "Shalom \xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D");

    // UTF-16 with a mark, in both byte orders.
    const auto utf16 = [](const std::string& ascii, bool big_endian) {
        std::string out = big_endian ? std::string{"\xFE\xFF"} : std::string{"\xFF\xFE"};
        for (const char c : ascii) {
            out.push_back(big_endian ? '\0' : c);
            out.push_back(big_endian ? c : '\0');
        }
        return out;
    };
    const std::string ascii = "1\n00:00:01,000 --> 00:00:02,000\nHello\n";
    for (const bool big : {false, true}) {
        const auto read = iclforge::lyrics::parse_subrip(utf16(ascii, big));
        REQUIRE(read.has_value());
        CHECK(read->lines[0].text == "Hello");
    }

    CHECK(iclforge::lyrics::parse_subrip("[00:01.00]Not subtitles\n").error() ==
          "The subtitle file has no cues in it.");
    CHECK(iclforge::lyrics::parse_subrip("   \n").error() == "The lyrics file is empty.");
    CHECK(iclforge::lyrics::parse_subrip("\xFF\x00\xFF").has_value() == false);
}

TEST_CASE("subrip: a file is read as subtitles or as LRC by what it holds", "[lyrics][subrip]") {
    const auto srt = iclforge::lyrics::parse_timed_text("1\n00:00:01,000 --> 00:00:02,000\nHello there\n");
    REQUIRE(srt.has_value());
    REQUIRE(srt->lines.size() == 1);
    CHECK(srt->lines[0].end_ms == 2000);
    const auto lrc = iclforge::lyrics::parse_timed_text("[00:01.00]Hello there\n[00:03.00]Again\n");
    REQUIRE(lrc.has_value());
    REQUIRE(lrc->lines.size() == 2);
    // An LRC line's end is the next line's start: it has no end of its own.
    CHECK(lrc->lines[0].end_ms == 3000);
    // A lyric that has an arrow in it is not a cue.
    const auto arrow = iclforge::lyrics::parse_timed_text("[00:01.00]go --> there now\n");
    REQUIRE(arrow.has_value());
    CHECK(arrow->lines[0].text == "go --> there now");
    // Neither, and the reader's own reason.
    CHECK(iclforge::lyrics::parse_timed_text("").error() == "The lyrics file is empty.");
}

TEST_CASE("subrip: the file beside an item is its .lrc, and its .srt where there is no .lrc",
          "[lyrics][subrip]") {
    Folder folder;
    folder.write("song.mp3", "x");
    CHECK_FALSE(iclforge::lyrics::find_lyrics_file((folder.path() / "song.mp3").string()).has_value());
    folder.write("song.SRT", "1\n00:00:01,000 --> 00:00:02,000\nHello\n");
    const auto subtitles = iclforge::lyrics::find_lyrics_file((folder.path() / "song.mp3").string());
    REQUIRE(subtitles.has_value());
    // The extension in any case, as for the .lrc; on a case-insensitive file system the usual
    // spelling is what comes back, so the name is compared in lower case.
    std::string name = subtitles->filename().string();
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    CHECK(name == "song.srt");
    // A file read by its path is read as what it is.
    const auto read = iclforge::lyrics::read_lyrics(*subtitles);
    REQUIRE(read.has_value());
    CHECK(read->lines[0].text == "Hello");
    // The .lrc is the one when there are both.
    folder.write("song.lrc", "[00:01.00]From the lrc\n");
    const auto both = iclforge::lyrics::find_lyrics_file((folder.path() / "song.mp3").string());
    REQUIRE(both.has_value());
    CHECK(both->filename().string() == "song.lrc");
}

// ---- Advanced SubStation Alpha (planning/karaoke-phase2.md, W13d) -------------------------------

namespace {

// A script with the usual sections and one karaoke event and one plain one.
const std::string kAssHeader =
    "[Script Info]\nTitle: Song\nScriptType: v4.00+\n\n"
    "[V4+ Styles]\nFormat: Name, Fontname\nStyle: Default,Arial\n\n"
    "[Events]\n"
    "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";

}  // namespace

TEST_CASE("ass: karaoke tags give each syllable its start and end", "[lyrics][ass]") {
    const std::string text = kAssHeader +
        "Dialogue: 0,0:00:12.00,0:00:15.50,Default,,0,0,0,,{\\k50}Should {\\k30}auld {\\k80}acquaint{\\k20}ance\n"
        "Dialogue: 0,0:00:20.00,0:00:22.00,Default,,0,0,0,,And never brought to mind\n";
    const auto lyrics = iclforge::lyrics::parse_ass(text);
    REQUIRE(lyrics.has_value());
    CHECK(lyrics->timed);
    REQUIRE(lyrics->lines.size() == 2);

    const auto& first = lyrics->lines[0];
    CHECK(first.start_ms == 12000);
    CHECK(first.end_ms == 15500);
    CHECK(first.text == "Should auld acquaintance");
    REQUIRE(first.words.size() == 4);
    CHECK(first.words[0].text == "Should ");
    CHECK(first.words[0].start_ms == 12000);
    CHECK(first.words[0].end_ms == 12500);
    CHECK(first.words[1].text == "auld ");
    CHECK(first.words[1].start_ms == 12500);
    CHECK(first.words[1].end_ms == 12800);
    // A word is a syllable: the fill moves by syllable.
    CHECK(first.words[2].text == "acquaint");
    CHECK(first.words[2].start_ms == 12800);
    CHECK(first.words[2].end_ms == 13600);
    CHECK(first.words[3].text == "ance");
    CHECK(first.words[3].start_ms == 13600);
    CHECK(first.words[3].end_ms == 13800);

    // An event with no karaoke tags is a line, with its own end and no words.
    CHECK(lyrics->lines[1].text == "And never brought to mind");
    CHECK_FALSE(lyrics->lines[1].word_timed());
    CHECK(lyrics->lines[1].end_ms == 22000);

    // The cursor is the word's: a quarter of the way through "auld" at 12575 ms.
    const auto at = iclforge::lyrics::lyric_position(*lyrics, 12575);
    REQUIRE(at.line.has_value());
    CHECK(*at.line == 0);
    REQUIRE(at.word.has_value());
    CHECK(*at.word == 1);
    CHECK(at.fraction == Catch::Approx(0.25));
}

TEST_CASE("ass: the other karaoke tags are the same duration, and text before the first is a word of no length",
          "[lyrics][ass]") {
    const std::string text = kAssHeader +
        "Dialogue: 0,0:00:01.00,0:00:03.00,Default,,0,0,0,,{\\kf40}A {\\K30}B {\\ko20}C\n"
        "Dialogue: 0,0:00:05.00,0:00:07.00,Default,,0,0,0,,Hi {\\k50}there {\\k0}{\\k25}friend\n";
    const auto lyrics = iclforge::lyrics::parse_ass(text);
    REQUIRE(lyrics.has_value());
    REQUIRE(lyrics->lines.size() == 2);
    const auto& tags = lyrics->lines[0];
    REQUIRE(tags.words.size() == 3);
    CHECK(tags.words[0].start_ms == 1000);
    CHECK(tags.words[0].end_ms == 1400);
    CHECK(tags.words[1].start_ms == 1400);
    CHECK(tags.words[1].end_ms == 1700);
    CHECK(tags.words[2].start_ms == 1700);
    CHECK(tags.words[2].end_ms == 1900);

    const auto& lead = lyrics->lines[1];
    REQUIRE(lead.words.size() == 3);
    CHECK(lead.text == "Hi there friend");
    // "Hi " has no tag of its own: it starts with the line and runs to the next word.
    CHECK(lead.words[0].text == "Hi ");
    CHECK(lead.words[0].start_ms == 5000);
    CHECK(lead.words[0].end_ms == 5000);
    CHECK(lead.words[1].text == "there ");
    CHECK(lead.words[1].start_ms == 5000);
    CHECK(lead.words[1].end_ms == 5500);
    // A {\\k0} between two words has no length and no text.
    CHECK(lead.words[2].text == "friend");
    CHECK(lead.words[2].start_ms == 5500);
    CHECK(lead.words[2].end_ms == 5750);

    // A last word of no length runs to the end of its line, so that it can be filled.
    const auto closing = iclforge::lyrics::parse_ass(
        "[Events]\nDialogue: 0,0:00:01.00,0:00:04.00,Default,,0,0,0,,{\\k50}One {\\k0}two\n");
    REQUIRE(closing.has_value());
    REQUIRE(closing->lines[0].words.size() == 2);
    CHECK(closing->lines[0].words[1].start_ms == 1500);
    CHECK(closing->lines[0].words[1].end_ms == 4000);
}

TEST_CASE("ass: override blocks, line breaks, drawings and comments", "[lyrics][ass]") {
    const std::string text = kAssHeader +
        "Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,{\\an8}{\\pos(10,20)}Hello, there\\Nfriend\n"
        "Dialogue: 0,0:00:03.00,0:00:04.00,Default,,0,0,0,,{\\p1}m 0 0 l 10 10 l 0 10{\\p0}\n"
        "Comment: 0,0:00:05.00,0:00:06.00,Default,,0,0,0,,not a lyric\n"
        "Dialogue: 0,0:00:07.00,0:00:08.00,Default,,0,0,0,,Two\\hspaces\\nhere\n";
    const auto lyrics = iclforge::lyrics::parse_ass(text);
    REQUIRE(lyrics.has_value());
    REQUIRE(lyrics->lines.size() == 2);
    // The commas of the text are the text's: it is the rest of the line.
    CHECK(lyrics->lines[0].text == "Hello, there friend");
    CHECK(lyrics->lines[1].text == "Two spaces here");
}

TEST_CASE("ass: the columns are the file's own Format line", "[lyrics][ass]") {
    const std::string text =
        "[Events]\nFormat: Start, End, Text\n"
        "Dialogue: 0:00:09.50,0:00:10.00,Late, but there\n"
        "Dialogue: 1:02:03.45,1:02:04.00,An hour in\n"
        "Dialogue: 0:00:01.5,0:00:02.00,Early\n";
    const auto lyrics = iclforge::lyrics::parse_ass(text);
    REQUIRE(lyrics.has_value());
    REQUIRE(lyrics->lines.size() == 3);
    // In order of start, with the hour and the fractions as the format writes them.
    CHECK(lyrics->lines[0].text == "Early");
    CHECK(lyrics->lines[0].start_ms == 1500);
    CHECK(lyrics->lines[1].text == "Late, but there");
    CHECK(lyrics->lines[1].start_ms == 9500);
    CHECK(lyrics->lines[2].start_ms == 3723450);

    // No Format line at all: the v4+ order.
    const auto bare = iclforge::lyrics::parse_ass(
        "[Events]\nDialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,No format line\n");
    REQUIRE(bare.has_value());
    CHECK(bare->lines[0].text == "No format line");
}

TEST_CASE("ass: what is not a script says so, and a file is read as one by what it holds",
          "[lyrics][ass]") {
    CHECK(iclforge::lyrics::parse_ass("[Script Info]\nTitle: Nothing\n").error() ==
          "The subtitle file has no dialogue in it.");
    CHECK(iclforge::lyrics::parse_ass("").error() == "The lyrics file is empty.");

    // UTF-16 with a mark, as an editor that saves "Unicode" writes it.
    const std::string ascii = kAssHeader + "Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,{\\k100}Hello\n";
    std::string utf16 = std::string{"\xFF\xFE"};
    for (const char c : ascii) {
        utf16.push_back(c);
        utf16.push_back('\0');
    }
    const auto wide = iclforge::lyrics::parse_ass(utf16);
    REQUIRE(wide.has_value());
    CHECK(wide->lines[0].text == "Hello");
    const auto detected = iclforge::lyrics::parse_timed_text(
        kAssHeader + "Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,{\\k100}Hello\n");
    REQUIRE(detected.has_value());
    REQUIRE(detected->lines.size() == 1);
    CHECK(detected->lines[0].word_timed());
    // An LRC lyric that says "Dialogue:" in its words is not a script.
    const auto lrc = iclforge::lyrics::parse_timed_text("[00:01.00]Dialogue: a play in three acts\n");
    REQUIRE(lrc.has_value());
    CHECK(lrc->lines[0].text == "Dialogue: a play in three acts");
}

TEST_CASE("ass: the file beside an item is its .lrc, then its .srt, then its .ass", "[lyrics][ass]") {
    Folder folder;
    folder.write("song.mp3", "x");
    folder.write("song.ass", kAssHeader +
        "Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,{\\k100}From the script\n");
    const std::string item = (folder.path() / "song.mp3").string();
    const auto script = iclforge::lyrics::find_lyrics_file(item);
    REQUIRE(script.has_value());
    CHECK(script->extension().string() == ".ass");
    const auto read = iclforge::lyrics::read_lyrics(*script);
    REQUIRE(read.has_value());
    CHECK(read->lines[0].text == "From the script");
    CHECK(read->lines[0].word_timed());
    folder.write("song.srt", "1\n00:00:01,000 --> 00:00:02,000\nFrom the subtitles\n");
    CHECK(iclforge::lyrics::find_lyrics_file(item)->extension().string() == ".srt");
    folder.write("song.lrc", "[00:01.00]From the lrc\n");
    CHECK(iclforge::lyrics::find_lyrics_file(item)->extension().string() == ".lrc");
}

TEST_CASE("lyrics: the cursor finds the line, the word and how far through it",
          "[lyrics]") {
    const Lyrics lyrics = parsed(kWordTimed);
    using iclforge::lyrics::lyric_position;

    // Before anything has started.
    CHECK_FALSE(lyric_position(lyrics, 11999).line.has_value());

    // The first word, from its start.
    LyricPosition at = lyric_position(lyrics, 12000);
    REQUIRE(at.line.has_value());
    CHECK(*at.line == 0);
    CHECK(at.in_line);
    REQUIRE(at.word.has_value());
    CHECK(*at.word == 0);
    CHECK(at.fraction == Catch::Approx(0.0));

    // Half way through it, and then the next word at its start.
    at = lyric_position(lyrics, 12250);
    REQUIRE(at.word.has_value());
    CHECK(*at.word == 0);
    CHECK(at.fraction == Catch::Approx(0.5));
    at = lyric_position(lyrics, 12500);
    REQUIRE(at.word.has_value());
    CHECK(*at.word == 1);
    CHECK(at.fraction == Catch::Approx(0.0));

    // The last word of the line, nearly done.
    at = lyric_position(lyrics, 14999);
    REQUIRE(at.word.has_value());
    CHECK(*at.word == 4);
    CHECK(at.fraction == Catch::Approx(599.0 / 600.0));

    // After the closing tag: still the line, no word being sung, all sung.
    at = lyric_position(lyrics, 15500);
    REQUIRE(at.line.has_value());
    CHECK(*at.line == 0);
    CHECK(at.in_line);
    CHECK_FALSE(at.word.has_value());
    for (const auto& word : lyrics.lines[0].words) {
        CHECK(iclforge::lyrics::word_state(word, at.position_ms) == WordState::kSung);
    }

    // The next line has no word times: the line, and no word.
    at = lyric_position(lyrics, 16000);
    REQUIRE(at.line.has_value());
    CHECK(*at.line == 1);
    CHECK_FALSE(at.word.has_value());

    // Past the last line's end.
    at = lyric_position(lyrics, 16000 + 5000);
    CHECK(*at.line == 1);
    CHECK_FALSE(at.in_line);
}

TEST_CASE("lyrics: a seek is only another position, and the delay shifts it",
          "[lyrics]") {
    const Lyrics lyrics = parsed(kWordTimed);
    using iclforge::lyrics::lyric_position;

    const LyricPosition late = lyric_position(lyrics, 15500);
    const LyricPosition back = lyric_position(lyrics, 12250);
    REQUIRE(back.word.has_value());
    CHECK(*back.word == 0);
    CHECK(back.fraction == Catch::Approx(0.5));
    // Asking again for the first answers as it did.
    CHECK(lyric_position(lyrics, 15500).in_line == late.in_line);

    // A delay of 250 ms shows what was at 12000 when the position is 12250.
    const LyricPosition delayed = lyric_position(lyrics, 12250, 250);
    REQUIRE(delayed.word.has_value());
    CHECK(*delayed.word == 0);
    CHECK(delayed.fraction == Catch::Approx(0.0));
    CHECK(delayed.position_ms == 12000);
    // And a negative one shows them sooner.
    const LyricPosition sooner = lyric_position(lyrics, 11800, -250);
    REQUIRE(sooner.line.has_value());
    CHECK(*sooner.line == 0);
}

TEST_CASE("lyrics: a word is sung from its end, and a zero-length one from its start",
          "[lyrics]") {
    const iclforge::lyrics::LyricWord word{.start_ms = 1000, .end_ms = 2000, .text = "x"};
    CHECK(iclforge::lyrics::word_state(word, 999) == WordState::kNotYetSung);
    CHECK(iclforge::lyrics::word_state(word, 1000) == WordState::kBeingSung);
    CHECK(iclforge::lyrics::word_state(word, 1999) == WordState::kBeingSung);
    CHECK(iclforge::lyrics::word_state(word, 2000) == WordState::kSung);
    const iclforge::lyrics::LyricWord blink{.start_ms = 5000, .end_ms = 5000, .text = "y"};
    CHECK(iclforge::lyrics::word_state(blink, 4999) == WordState::kNotYetSung);
    CHECK(iclforge::lyrics::word_state(blink, 5000) == WordState::kSung);
}

TEST_CASE("lyrics: the file beside an item, in any case of the extension", "[lyrics]") {
    const Folder folder;
    folder.write("Song.LRC", "[00:01.00]hi\n");
    folder.write("other.txt", "x");
    folder.write("a.b.lrc", "[00:01.00]dots\n");

    const auto found = iclforge::lyrics::find_lyrics_file((folder.path() / "Song.ac3").string());
    REQUIRE(found.has_value());
    // The same file, however this file system spells its name.
    CHECK(fs::equivalent(*found, folder.path() / "Song.LRC"));

    // A stem with a dot is the whole name less the last extension.
    const auto dotted = iclforge::lyrics::find_lyrics_file((folder.path() / "a.b.mp4").string());
    REQUIRE(dotted.has_value());
    CHECK(dotted->filename().string() == "a.b.lrc");

    CHECK_FALSE(iclforge::lyrics::find_lyrics_file((folder.path() / "other.ac3").string())
                    .has_value());
    CHECK_FALSE(iclforge::lyrics::find_lyrics_file((folder.path() / "missing.ac3").string())
                    .has_value());

    const auto read = iclforge::lyrics::read_lyrics(*found, 90000);
    REQUIRE(read.has_value());
    CHECK(read->lines.size() == 1);
    CHECK(read->lines[0].end_ms == 90000);

    const auto none = iclforge::lyrics::read_lyrics(folder.path() / "nope.lrc");
    REQUIRE_FALSE(none.has_value());
    CHECK_FALSE(none.error().empty());
}

// ---- UTF-16, Hebrew and Yiddish (planning/karaoke-phase2.md, D8) ---------------------------------
//
// The Hebrew and Yiddish lines are generated here, so there is no licence question; the UTF-8 each
// is expected to become is written out in bytes and not produced by the code under test.

namespace {

// `text` as UTF-16 bytes, with a byte-order mark unless told not to.
std::string utf16_bytes(const std::u16string& text, bool big_endian, bool bom = true) {
    std::string out;
    const auto put = [&](char16_t unit) {
        const auto high = static_cast<char>(static_cast<unsigned>(unit) >> 8U);
        const auto low = static_cast<char>(static_cast<unsigned>(unit) & 0xFFU);
        if (big_endian) {
            out += high;
            out += low;
        } else {
            out += low;
            out += high;
        }
    };
    if (bom) {
        put(0xFEFF);
    }
    for (const char16_t unit : text) {
        put(unit);
    }
    return out;
}

// "shalom olam" (hello world), and Yiddish "vos" with its pasekh-alef: a ligature and a point.
const std::u16string kHebrewLine = u"[00:01.00]שלום עולם\n";
const std::string kHebrewUtf8 = "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D \xD7\xA2\xD7\x95\xD7\x9C\xD7\x9D";
const std::u16string kYiddishLine = u"[00:03.00]װאָס זאָגסט\n";
const std::string kYiddishUtf8 =
    "\xD7\xB0\xD7\x90\xD6\xB8\xD7\xA1 \xD7\x96\xD7\x90\xD6\xB8\xD7\x92\xD7\xA1\xD7\x98";

}  // namespace

TEST_CASE("lyrics: UTF-16 with a byte-order mark, in either order, reads as the same UTF-8",
          "[lyrics][utf16]") {
    const std::u16string both = kHebrewLine + kYiddishLine;
    for (const bool big_endian : {false, true}) {
        const auto lyrics = iclforge::lyrics::parse_lyrics(utf16_bytes(both, big_endian));
        REQUIRE(lyrics.has_value());
        REQUIRE(lyrics->lines.size() >= 2);
        CHECK(lyrics->timed);
        CHECK(lyrics->lines[0].text == kHebrewUtf8);
        CHECK(lyrics->lines[0].start_ms == 1000);
        CHECK(lyrics->lines[1].text == kYiddishUtf8);
        CHECK(lyrics->lines[1].start_ms == 3000);
    }
}

TEST_CASE("lyrics: UTF-8 Hebrew reads the same, with and without a byte-order mark",
          "[lyrics][utf16]") {
    const std::string line = "[00:01.00]" + kHebrewUtf8 + "\n";
    const auto plain = iclforge::lyrics::parse_lyrics(line);
    const auto marked = iclforge::lyrics::parse_lyrics("\xEF\xBB\xBF" + line);
    REQUIRE(plain.has_value());
    REQUIRE(marked.has_value());
    CHECK(plain->lines[0].text == kHebrewUtf8);
    CHECK(marked->lines[0].text == kHebrewUtf8);
}

TEST_CASE("lyrics: a line's last letter is not trimmed as white space, whatever the locale",
          "[lyrics][utf16]") {
    // U+05E0 (nun) is D7 A0: 0xA0 is white space in a Latin-1 locale's table, and the line's text
    // ends in that byte. Trailing spaces and tabs are still trimmed.
    const auto lyrics = iclforge::lyrics::parse_lyrics("[00:01.00]\xD7\xA9\xD7\xA0 \t\r\n");
    REQUIRE(lyrics.has_value());
    CHECK(lyrics->lines[0].text == "\xD7\xA9\xD7\xA0");
}

TEST_CASE("lyrics: right-to-left words keep their timing and their spacing in UTF-16",
          "[lyrics][utf16]") {
    const std::u16string line =
        u"[00:10.00]<00:10.00>שלום <00:10.60>עולם\n";
    const auto lyrics = iclforge::lyrics::parse_lyrics(utf16_bytes(line, true), 20000);
    REQUIRE(lyrics.has_value());
    REQUIRE(lyrics->lines.size() == 1);
    const auto& words = lyrics->lines[0].words;
    REQUIRE(words.size() == 2);
    CHECK(words[0].text == "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D ");
    CHECK(words[0].start_ms == 10000);
    CHECK(words[0].end_ms == 10600);
    CHECK(words[1].text == "\xD7\xA2\xD7\x95\xD7\x9C\xD7\x9D");
    CHECK(words[1].start_ms == 10600);
}

TEST_CASE("lyrics: a code point outside the BMP survives a UTF-16 surrogate pair", "[lyrics][utf16]") {
    // U+1F3A4 is D83C DFA4 in UTF-16 and F0 9F 8E A4 in UTF-8.
    const auto lyrics =
        iclforge::lyrics::parse_lyrics(utf16_bytes(u"[00:01.00]\U0001F3A4 la\n", false));
    REQUIRE(lyrics.has_value());
    CHECK(lyrics->lines[0].text == "\xF0\x9F\x8E\xA4 la");
}

TEST_CASE("lyrics: UTF-16 that is not UTF-16, and a code page, say so and are not guessed at",
          "[lyrics][utf16]") {
    // An odd number of bytes, a high surrogate with nothing after it, a low one on its own.
    const std::string odd = utf16_bytes(u"[00:01.00]a\n", false) + "x";
    CHECK_FALSE(iclforge::lyrics::parse_lyrics(odd).has_value());
    const std::u16string lone_high = u"[00:01.00]a\xD83C";
    CHECK_FALSE(iclforge::lyrics::parse_lyrics(utf16_bytes(lone_high, false)).has_value());
    const std::u16string lone_low = u"[00:01.00]a\xDFA4 b\n";
    CHECK_FALSE(iclforge::lyrics::parse_lyrics(utf16_bytes(lone_low, true)).has_value());

    // UTF-32 is refused by name.
    const std::string utf32("\xFF\xFE\x00\x00[\x00\x00\x00", 8);
    const auto refused = iclforge::lyrics::parse_lyrics(utf32);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().find("UTF-32") != std::string::npos);

    // Windows-1255, the Hebrew code page most old files are in: not UTF-8, so refused, and the
    // sentence names both encodings that would have been read.
    const auto cp1255 = iclforge::lyrics::parse_lyrics("[00:01.00]\xF9\xEC\xE5\xED\n");
    REQUIRE_FALSE(cp1255.has_value());
    CHECK(cp1255.error().find("UTF-8 or UTF-16") != std::string::npos);
}

TEST_CASE("lyrics: a UTF-16 file on disk is read, and its time is the item's", "[lyrics][utf16]") {
    const Folder folder;
    const fs::path file = folder.write("Hebrew.lrc", utf16_bytes(kHebrewLine, false));
    const auto lyrics = iclforge::lyrics::read_lyrics(file, 5000);
    REQUIRE(lyrics.has_value());
    REQUIRE(lyrics->lines.size() == 1);
    CHECK(lyrics->lines[0].text == kHebrewUtf8);
    CHECK(lyrics->lines[0].end_ms == 5000);
}
