#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "platform/process.hpp"

#include "iclforge/lyrics/lyrics.hpp"
#include "iclforge/lyrics/timing.hpp"

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

// ---- a duet's singer (planning/karaoke-phase2.md, W11) -----------------------------------------

TEST_CASE("lyrics: a line's singer is the marker at its start, and not part of its text",
          "[lyrics][duet]") {
    using iclforge::lyrics::Singer;
    const Lyrics lyrics = parsed(
        "[00:01.00]V1: Leave her, Johnny\n"
        "[00:03.00]V2:Leave her\n"
        "[00:05.00]V1+V2: Oh leave her, Johnny, leave her\n"
        "[00:07.00]It's time for us to leave her\n");
    REQUIRE(lyrics.lines.size() == 4);
    CHECK(lyrics.lines[0].singer == Singer::kFirst);
    CHECK(lyrics.lines[0].text == "Leave her, Johnny");
    CHECK(lyrics.lines[1].singer == Singer::kSecond);
    CHECK(lyrics.lines[1].text == "Leave her");
    CHECK(lyrics.lines[2].singer == Singer::kBoth);
    CHECK(lyrics.lines[2].text == "Oh leave her, Johnny, leave her");
    // A line that says nothing is for nobody in particular, a solo's every line.
    CHECK(lyrics.lines[3].singer == Singer::kUnmarked);
    CHECK(lyrics.lines[3].text == "It's time for us to leave her");
    CHECK(iclforge::lyrics::has_singers(lyrics));
    CHECK_FALSE(iclforge::lyrics::has_singers(parsed("[00:01.00]Should auld acquaintance\n")));
}

TEST_CASE("lyrics: the marker's spellings, and what is only text", "[lyrics][duet]") {
    using iclforge::lyrics::Singer;
    const Lyrics lyrics = parsed(
        "[00:01.00]v1: lower case\n"
        "[00:02.00]  V2 :  spaced out\n"
        "[00:03.00]v2+v1: the other order\n"
        "[00:04.00]V1 + V2: spaces round the plus\n"
        "[00:05.00]V3: not a singer\n"
        "[00:06.00]V1 is the first vocal\n"
        "[00:07.00]Sing V1: not at the start\n"
        "[00:08.00]V1+V1: not two singers\n"
        "[00:09.00]V1:\n");
    REQUIRE(lyrics.lines.size() == 9);
    CHECK(lyrics.lines[0].singer == Singer::kFirst);
    CHECK(lyrics.lines[0].text == "lower case");
    CHECK(lyrics.lines[1].singer == Singer::kSecond);
    CHECK(lyrics.lines[1].text == "spaced out");
    CHECK(lyrics.lines[2].singer == Singer::kBoth);
    CHECK(lyrics.lines[3].singer == Singer::kBoth);
    CHECK(lyrics.lines[3].text == "spaces round the plus");
    for (std::size_t i = 4; i <= 7; ++i) {
        CHECK(lyrics.lines[i].singer == Singer::kUnmarked);
    }
    CHECK(lyrics.lines[4].text == "V3: not a singer");
    CHECK(lyrics.lines[5].text == "V1 is the first vocal");
    CHECK(lyrics.lines[6].text == "Sing V1: not at the start");
    CHECK(lyrics.lines[7].text == "V1+V1: not two singers");
    // The marker alone is a line with a singer and no words, as a gap with a time.
    CHECK(lyrics.lines[8].singer == Singer::kFirst);
    CHECK(lyrics.lines[8].text.empty());
}

TEST_CASE("lyrics: the marker comes before the word tags, or after the first, and is not a word",
          "[lyrics][duet]") {
    using iclforge::lyrics::Singer;
    const Lyrics lyrics = parsed(
        "[00:10.00]V2: <00:10.00>Leave <00:10.50>her <00:11.00>Johnny\n"
        "[00:20.00]<00:20.00>V1: Leave <00:20.50>her\n"
        "[00:30.00][00:40.00]V1+V2: Chorus\n");
    REQUIRE(lyrics.lines.size() == 4);
    CHECK(lyrics.lines[0].singer == Singer::kSecond);
    CHECK(lyrics.lines[0].text == "Leave her Johnny");
    REQUIRE(lyrics.lines[0].words.size() == 3);
    CHECK(lyrics.lines[0].words[0].text == "Leave ");
    CHECK(lyrics.lines[0].words[0].start_ms == 10000);
    CHECK(lyrics.lines[1].singer == Singer::kFirst);
    CHECK(lyrics.lines[1].text == "Leave her");
    REQUIRE(lyrics.lines[1].words.size() == 2);
    CHECK(lyrics.lines[1].words[0].text == "Leave ");
    // A repeated line is the same singer each time.
    CHECK(lyrics.lines[2].singer == Singer::kBoth);
    CHECK(lyrics.lines[3].singer == Singer::kBoth);
}

TEST_CASE("lyrics: a plain file may carry the marker too, and the names are id tags",
          "[lyrics][duet]") {
    using iclforge::lyrics::Singer;
    const Lyrics lyrics = parsed(
        "[ti:Leave Her, Johnny]\n[v1:Shantyman]\n[v2:Crew]\n"
        "V1: Oh, the times are hard\n"
        "V2: Leave her, Johnny\n");
    CHECK_FALSE(lyrics.timed);
    CHECK(lyrics.first_singer_name == "Shantyman");
    CHECK(lyrics.second_singer_name == "Crew");
    CHECK(lyrics.title == "Leave Her, Johnny");
    REQUIRE(lyrics.lines.size() == 2);
    CHECK(lyrics.lines[0].singer == Singer::kFirst);
    CHECK(lyrics.lines[0].text == "Oh, the times are hard");
    CHECK(lyrics.lines[1].singer == Singer::kSecond);
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

// ---- MIDI karaoke (planning/karaoke-phase2.md, W13e) --------------------------------------------

namespace {

// A Standard MIDI File built by hand, for the karaoke reader's tests: every byte is a number.
std::string midi_byte(int value) { return std::string(1, static_cast<char>(value)); }

std::string midi_vlq(std::uint32_t value) {
    std::string out = midi_byte(static_cast<int>(value & 0x7F));
    value >>= 7U;
    while (value > 0) {
        out.insert(out.begin(), static_cast<char>(0x80 | (value & 0x7F)));
        value >>= 7U;
    }
    return out;
}

// delta, FF, type, length, payload
std::string midi_meta(std::uint32_t delta, int type, const std::string& payload) {
    return midi_vlq(delta) + midi_byte(0xFF) + midi_byte(type) + midi_vlq(static_cast<std::uint32_t>(payload.size())) +
           payload;
}

std::string midi_text(std::uint32_t delta, const std::string& text, int type = 0x01) {
    return midi_meta(delta, type, text);
}

std::string midi_tempo(std::uint32_t delta, std::uint32_t micros) {
    return midi_meta(delta, 0x51,
                     midi_byte(static_cast<int>(micros >> 16U)) + midi_byte(static_cast<int>((micros >> 8U) & 0xFF)) +
                         midi_byte(static_cast<int>(micros & 0xFF)));
}

std::string midi_track(const std::string& events) {
    const std::string body = events + midi_vlq(0) + midi_byte(0xFF) + midi_byte(0x2F) + midi_byte(0);
    const auto length = static_cast<std::uint32_t>(body.size());
    return std::string("MTrk") + midi_byte(static_cast<int>(length >> 24U)) +
           midi_byte(static_cast<int>((length >> 16U) & 0xFF)) + midi_byte(static_cast<int>((length >> 8U) & 0xFF)) +
           midi_byte(static_cast<int>(length & 0xFF)) + body;
}

std::string midi_file(const std::vector<std::string>& tracks, int division = 500, int format = 1) {
    std::string out = "MThd";
    out += midi_byte(0) + midi_byte(0) + midi_byte(0) + midi_byte(6);
    out += midi_byte(0) + midi_byte(format);
    out += midi_byte(0) + midi_byte(static_cast<int>(tracks.size()));
    out += midi_byte(division >> 8) + midi_byte(division & 0xFF);
    for (const std::string& track : tracks) {
        out += track;
    }
    return out;
}

// "Words": the track name a karaoke file's syllables are on.
std::string midi_words_track(const std::string& events) {
    return midi_track(midi_meta(0, 0x03, "Words") + events);
}

// A SMPTE time division: the frame rate negated (two's complement) in the high byte, and the ticks in a
// frame in the low one.
int smpte_division(int frames_per_second, int ticks_per_frame) {
    return ((256 - frames_per_second) << 8) | ticks_per_frame;
}

}  // namespace

TEST_CASE("midi karaoke: syllables are words with their ticks in milliseconds, and a slash begins a line",
          "[lyrics][midi]") {
    // Division 500 and the default tempo (500000 microseconds a quarter): one tick is one millisecond.
    const std::string words = midi_words_track(
        midi_text(0, "@KMIDI KARAOKE FILE") + midi_text(0, "@TLeave Her, Johnny") + midi_text(0, "@TTraditional") +
        midi_text(0, "@LENGL") + midi_text(1000, "Oh") + midi_text(300, " the") + midi_text(300, " times") +
        midi_text(400, "/Are") + midi_text(300, " hard"));
    const auto lyrics = iclforge::lyrics::parse_midi_karaoke(midi_file({midi_track(""), words}));
    REQUIRE(lyrics.has_value());
    CHECK(lyrics->timed);
    CHECK(lyrics->title == "Leave Her, Johnny");
    CHECK(lyrics->artist == "Traditional");
    REQUIRE(lyrics->lines.size() == 2);

    const auto& first = lyrics->lines[0];
    CHECK(first.text == "Oh the times");
    CHECK(first.start_ms == 1000);
    CHECK(first.end_ms == 2000);
    REQUIRE(first.words.size() == 3);
    // The space that began a syllable is the end of the one before, where a view expects it.
    CHECK(first.words[0].text == "Oh ");
    CHECK(first.words[0].start_ms == 1000);
    CHECK(first.words[0].end_ms == 1300);
    CHECK(first.words[1].text == "the ");
    CHECK(first.words[1].end_ms == 1600);
    CHECK(first.words[2].text == "times");
    CHECK(first.words[2].start_ms == 1600);
    CHECK(first.words[2].end_ms == 2000);

    const auto& second = lyrics->lines[1];
    CHECK(second.text == "Are hard");
    CHECK(second.start_ms == 2000);
    REQUIRE(second.words.size() == 2);
    CHECK(second.words[1].start_ms == 2300);
    // The last line has no line after it: it ends a while on, as the other readers' do.
    CHECK(second.end_ms == 2300 + 5000);
}

TEST_CASE("midi karaoke: a tempo change moves every later tick", "[lyrics][midi]") {
    // At tick 2000 the tempo doubles in speed (250000 microseconds a quarter): half a millisecond a
    // tick from there. The tempo may be on any track; here it is on the first, as format 1 has it.
    const std::string conductor = midi_track(midi_tempo(0, 500000) + midi_tempo(2000, 250000));
    const std::string words = midi_words_track(midi_text(1000, "A") + midi_text(1000, " b") + midi_text(2000, " c"));
    const auto lyrics = iclforge::lyrics::parse_midi_karaoke(midi_file({conductor, words}));
    REQUIRE(lyrics.has_value());
    REQUIRE(lyrics->lines.size() == 1);
    const auto& line = lyrics->lines[0];
    REQUIRE(line.words.size() == 3);
    CHECK(line.words[0].start_ms == 1000);
    // Tick 2000 is where the change is: still the old rate up to it.
    CHECK(line.words[1].start_ms == 2000);
    // Tick 4000 is 2000 ticks at 0.5 ms after it.
    CHECK(line.words[2].start_ms == 3000);
}

TEST_CASE("midi karaoke: a SMPTE time division gives every tick a fixed length, whatever the tempo",
          "[lyrics][midi][smpte]") {
    // 25 frames a second and 40 ticks a frame: 1000 ticks a second, so a tick is a millisecond, as
    // with division 500 and the default tempo in the first case, and the same words give the same
    // times. A tempo event names a quarter note, which SMPTE time has none of: it moves nothing.
    const std::string conductor = midi_track(midi_tempo(0, 250000) + midi_tempo(1500, 1000000));
    const std::string words = midi_words_track(
        midi_text(1000, "Oh") + midi_text(300, " the") + midi_text(300, " times") + midi_text(400, "/Are") +
        midi_text(300, " hard"));
    const auto lyrics = iclforge::lyrics::parse_midi_karaoke(midi_file({conductor, words}, smpte_division(25, 40)));
    REQUIRE(lyrics.has_value());
    CHECK(lyrics->timed);
    REQUIRE(lyrics->lines.size() == 2);
    const auto& first = lyrics->lines[0];
    CHECK(first.text == "Oh the times");
    CHECK(first.start_ms == 1000);
    CHECK(first.end_ms == 2000);
    REQUIRE(first.words.size() == 3);
    CHECK(first.words[0].end_ms == 1300);
    CHECK(first.words[1].end_ms == 1600);
    CHECK(first.words[2].start_ms == 1600);
    const auto& second = lyrics->lines[1];
    CHECK(second.start_ms == 2000);
    REQUIRE(second.words.size() == 2);
    CHECK(second.words[1].start_ms == 2300);
}

TEST_CASE("midi karaoke: each SMPTE frame rate is its own number of ticks a second", "[lyrics][midi][smpte]") {
    // One syllable at a tick, and the line it starts is the time of that tick.
    const auto start_of = [](int division, std::uint32_t tick) {
        const std::string words = midi_words_track(midi_text(tick, "Sing"));
        const auto lyrics = iclforge::lyrics::parse_midi_karaoke(midi_file({words}, division));
        REQUIRE(lyrics.has_value());
        REQUIRE(lyrics->lines.size() == 1);
        return lyrics->lines[0].start_ms;
    };
    // 24 a second and 10 ticks a frame: 240 ticks a second.
    CHECK(start_of(smpte_division(24, 10), 240) == 1000);
    CHECK(start_of(smpte_division(24, 10), 600) == 2500);
    // 25 and 4: 100 ticks a second.
    CHECK(start_of(smpte_division(25, 4), 100) == 1000);
    // 30 and 80: 2400 a second.
    CHECK(start_of(smpte_division(30, 80), 2400) == 1000);
    CHECK(start_of(smpte_division(30, 80), 12000) == 5000);
    // "29" is the 29.97 frames a second of NTSC, 30000 over 1001, and not 30: 100 ticks a frame is
    // 2997.003 ticks a second, so 3000 ticks are a thousandth of a second MORE than a second, and
    // 30000 are 10.01 seconds, where 30 frames a second would have them at exactly 1 and 10.
    CHECK(start_of(smpte_division(29, 100), 3000) == 1001);
    CHECK(start_of(smpte_division(29, 100), 30000) == 10010);
    CHECK(start_of(smpte_division(30, 100), 30000) == 10000);
    // The frame size is the low byte whole: 255 ticks a frame at 25 is 6375 ticks a second.
    CHECK(start_of(smpte_division(25, 255), 6375) == 1000);
}

TEST_CASE("midi karaoke: a SMPTE tick is rounded to the nearest millisecond", "[lyrics][midi][smpte]") {
    const auto start_of = [](std::uint32_t tick) {
        const std::string words = midi_words_track(midi_text(tick, "Sing"));
        const auto lyrics =
            iclforge::lyrics::parse_midi_karaoke(midi_file({words}, smpte_division(25, 3)));
        REQUIRE(lyrics.has_value());
        return lyrics->lines[0].start_ms;
    };
    // 75 ticks a second, so a tick is 13.33 milliseconds: 13.33, 26.67, 40, 66.67, 1000.
    CHECK(start_of(1) == 13);
    CHECK(start_of(2) == 27);
    CHECK(start_of(3) == 40);
    CHECK(start_of(5) == 67);
    CHECK(start_of(75) == 1000);
}

TEST_CASE("midi karaoke: a SMPTE time far into a file is not wrapped by a 32-bit count", "[lyrics][midi][smpte]") {
    // The largest delta a variable-length quantity holds, 0x0FFFFFFF ticks, at 24 ticks a second:
    // 11,184,810.625 seconds, which is past what 32 bits of milliseconds hold.
    const std::string words = midi_words_track(midi_text(0x0FFFFFFFU, "Late"));
    const auto lyrics = iclforge::lyrics::parse_midi_karaoke(midi_file({words}, smpte_division(24, 1)));
    REQUIRE(lyrics.has_value());
    CHECK(lyrics->lines[0].start_ms == 11184810625LL);
}

TEST_CASE("midi karaoke: a SMPTE rate that is not defined, and a frame of no ticks, say why", "[lyrics][midi][smpte]") {
    const std::string words = midi_words_track(midi_text(0, "Hello"));
    // 32 frames a second is none of the four.
    const auto unknown = iclforge::lyrics::parse_midi_karaoke(midi_file({words}, smpte_division(32, 40)));
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().find("SMPTE") != std::string::npos);
    CHECK(unknown.error().find("29.97") != std::string::npos);
    // So is 23, and so is 60.
    CHECK_FALSE(iclforge::lyrics::parse_midi_karaoke(midi_file({words}, smpte_division(23, 40))).has_value());
    CHECK_FALSE(iclforge::lyrics::parse_midi_karaoke(midi_file({words}, smpte_division(60, 40))).has_value());
    // A frame of no ticks has no length: the file is damaged, not "at an unknown rate".
    const auto empty_frame = iclforge::lyrics::parse_midi_karaoke(midi_file({words}, smpte_division(25, 0)));
    REQUIRE_FALSE(empty_frame.has_value());
    CHECK(empty_frame.error() == "The MIDI file is damaged.");
    // A frame of no ticks at a rate that is not one is damaged too, and no division by it is done.
    CHECK(iclforge::lyrics::parse_midi_karaoke(midi_file({words}, smpte_division(32, 0))).error() ==
          "The MIDI file is damaged.");
}

TEST_CASE("midi karaoke: the words are the Words track's, else the Lyric events, else the Text events",
          "[lyrics][midi]") {
    // Lyric events (type 5) beat Text events (type 1) when there is no Words track.
    const std::string with_lyric_events = midi_track(
        midi_text(0, "A comment, not the words") + midi_text(500, "Hello", 0x05) + midi_text(500, " world", 0x05));
    const auto lyric = iclforge::lyrics::parse_midi_karaoke(midi_file({with_lyric_events}));
    REQUIRE(lyric.has_value());
    CHECK(lyric->lines[0].text == "Hello world");

    // Text events only, on a track that is not called Words.
    const std::string text_only = midi_track(midi_text(100, "Only") + midi_text(100, " text"));
    const auto text = iclforge::lyrics::parse_midi_karaoke(midi_file({text_only}));
    REQUIRE(text.has_value());
    CHECK(text->lines[0].text == "Only text");

    // The Words track wins over Lyric events elsewhere, and "words" is the name in any case.
    const std::string other = midi_track(midi_text(0, "Not these", 0x05));
    const std::string named = midi_track(midi_meta(0, 0x03, "WORDS") + midi_text(50, "These"));
    const auto chosen = iclforge::lyrics::parse_midi_karaoke(midi_file({other, named}));
    REQUIRE(chosen.has_value());
    CHECK(chosen->lines[0].text == "These");
}

TEST_CASE("midi karaoke: a backslash and a line end begin lines too, and the music is skipped over",
          "[lyrics][midi]") {
    const std::string backslash = midi_byte(92);
    const std::string line_feed = midi_byte(10);
    // Notes in running status, a program change, a pitch bend and a system exclusive message
    // between the syllables: none of it is words, and all of it has to be stepped over exactly.
    const std::string music = midi_vlq(0) + midi_byte(0x90) + midi_byte(60) + midi_byte(64) +  // note on
                              midi_vlq(10) + midi_byte(62) + midi_byte(64) +                    // running status
                              midi_vlq(0) + midi_byte(0xC0) + midi_byte(5) +                    // program change
                              midi_vlq(0) + midi_byte(0xE0) + midi_byte(0) + midi_byte(64) +    // pitch bend
                              midi_vlq(0) + midi_byte(0xF0) + midi_vlq(3) + midi_byte(1) + midi_byte(2) + midi_byte(0xF7) +
                              // channel pressure: one data byte, and the last thing before the next syllable
                              midi_vlq(0) + midi_byte(0xD0) + midi_byte(30);
    const std::string words = midi_words_track(
        midi_text(100, "One") + music + midi_text(100, backslash + "Two") + midi_text(100, line_feed + "Three") +
        midi_text(100, "/") + midi_text(100, "Four"));
    const auto lyrics = iclforge::lyrics::parse_midi_karaoke(midi_file({words}, 1000, 0));
    REQUIRE(lyrics.has_value());
    REQUIRE(lyrics->lines.size() == 4);
    CHECK(lyrics->lines[0].text == "One");
    CHECK(lyrics->lines[1].text == "Two");
    CHECK(lyrics->lines[2].text == "Three");
    CHECK(lyrics->lines[3].text == "Four");
    // Division 1000 at the default tempo is half a millisecond a tick.
    CHECK(lyrics->lines[0].start_ms == 50);
}

TEST_CASE("midi karaoke: what cannot be read says why", "[lyrics][midi]") {
    CHECK(iclforge::lyrics::parse_midi_karaoke("[00:01.00]Not MIDI").error() == "The file is not a MIDI file.");

    // No words, and a file cut short. (SMPTE time is read: see the cases above.)
    const std::string words = midi_words_track(midi_text(0, "Hello"));
    CHECK(iclforge::lyrics::parse_midi_karaoke(midi_file({midi_track(midi_text(0, "@TTitle only"))})).error() ==
          "The MIDI file has no words in it.");
    const std::string whole = midi_file({words});
    CHECK(iclforge::lyrics::parse_midi_karaoke(whole.substr(0, whole.size() - 6)).error() ==
          "The MIDI file is damaged.");

    // Words that are Latin-1 are a code page, and are not guessed at: the reason says what to do.
    const std::string latin1 = midi_words_track(midi_text(0, "Caf" + midi_byte(0xE9)));
    const auto coded = iclforge::lyrics::parse_midi_karaoke(midi_file({latin1}));
    REQUIRE_FALSE(coded.has_value());
    CHECK(coded.error().find("not UTF-8") != std::string::npos);
    // The same word in UTF-8 reads, and so does Hebrew.
    const std::string utf8 = midi_words_track(midi_text(0, "Caf" + midi_byte(0xC3) + midi_byte(0xA9)) +
                                              midi_text(100, " " + midi_byte(0xD7) + midi_byte(0xA9) + midi_byte(0xD7) +
                                                                midi_byte(0x9C) + midi_byte(0xD7) + midi_byte(0x95) +
                                                                midi_byte(0xD7) + midi_byte(0x9D)));
    const auto fine = iclforge::lyrics::parse_midi_karaoke(midi_file({utf8}));
    REQUIRE(fine.has_value());
    CHECK(fine->lines[0].words.size() == 2);
}

TEST_CASE("midi karaoke: words in the common code pages are refused by name, not read as other text",
          "[lyrics][midi][encoding]") {
    // What a karaoke file written before UTF-8 was usual holds: the reader checks every syllable as
    // UTF-8 and refuses the file at the first that is not (planning/karaoke-phase2.md, D8). Each of
    // these is the first syllable of a file whose other syllables are plain.
    const auto bytes = [](std::initializer_list<int> values) {
        std::string out;
        for (const int value : values) {
            out += midi_byte(value);
        }
        return out;
    };
    const std::vector<std::pair<const char*, std::string>> syllables = {
        // Windows-1252: a curly quote and an e with an acute, in a word.
        {"windows-1252", "Caf" + bytes({0xE9}) + " " + bytes({0x93}) + "late" + bytes({0x94})},
        // Latin-1: a Spanish ene and a German sharp s.
        {"latin-1", "Ma" + bytes({0xF1}) + "ana, Stra" + bytes({0xDF}) + "e"},
        // Shift-JIS: "arigatou" in hiragana.
        {"shift-jis", bytes({0x82, 0xA0, 0x82, 0xE8, 0x82, 0xAA, 0x82, 0xC6, 0x82, 0xA4})},
        // EUC-JP: the same word.
        {"euc-jp", bytes({0xA4, 0xA2, 0xA4, 0xEA, 0xA4, 0xAC, 0xA4, 0xC8, 0xA4, 0xA6})},
        // GBK: "ni hao".
        {"gbk", bytes({0xC4, 0xE3, 0xBA, 0xC3})},
        // Windows-1251: Russian "privet".
        {"windows-1251", bytes({0xCF, 0xF0, 0xE8, 0xE2, 0xE5, 0xF2})},
        // Windows-1255: Hebrew "shalom".
        {"windows-1255", bytes({0xF9, 0xEC, 0xE5, 0xED})},
    };
    for (const auto& [name, text] : syllables) {
        CAPTURE(name);
        const std::string words = midi_words_track(midi_text(0, "Plain") + midi_text(100, " " + text) +
                                                   midi_text(100, " words"));
        const auto lyrics = iclforge::lyrics::parse_midi_karaoke(midi_file({words}));
        REQUIRE_FALSE(lyrics.has_value());
        CHECK(lyrics.error().find("not UTF-8") != std::string::npos);
    }
}

TEST_CASE("midi karaoke: only the title, the artist and the words have to be UTF-8, not the other @ lines",
          "[lyrics][midi][encoding]") {
    const auto bytes = [](std::initializer_list<int> values) {
        std::string out;
        for (const int value : values) {
            out += midi_byte(value);
        }
        return out;
    };
    // A copyright line, a key line and a language line with a Latin-1 copyright sign and e acute in
    // them: the reader never shows them, so they do not refuse a file whose words are plain.
    const std::string words = midi_words_track(
        midi_text(0, "@KMIDI KARAOKE FILE" + bytes({0xE9})) + midi_text(0, "@TCafe Song") +
        midi_text(0, "@TTraditional") + midi_text(0, "@I" + bytes({0xA9}) + " 1985 Caf" + bytes({0xE9})) +
        midi_text(0, "@LFRAN") + midi_text(100, "Sing") + midi_text(100, " along"));
    const auto lyrics = iclforge::lyrics::parse_midi_karaoke(midi_file({words}));
    REQUIRE(lyrics.has_value());
    CHECK(lyrics->title == "Cafe Song");
    CHECK(lyrics->artist == "Traditional");
    REQUIRE(lyrics->lines.size() == 1);
    CHECK(lyrics->lines[0].text == "Sing along");

    // The title and the artist are kept, so they are checked: the file is refused, naming UTF-8.
    for (const bool artist : {false, true}) {
        const std::string first = artist ? std::string{"@TA Title"} : "@TCaf" + bytes({0xE9});
        const std::string second = artist ? "@TCaf" + bytes({0xE9}) : std::string{"@TTraditional"};
        const std::string refused = midi_words_track(midi_text(0, first) + midi_text(0, second) +
                                                     midi_text(100, "Sing") + midi_text(100, " along"));
        const auto result = iclforge::lyrics::parse_midi_karaoke(midi_file({refused}));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("not UTF-8") != std::string::npos);
    }
}

TEST_CASE("midi karaoke: a file is read as MIDI by its header, and a .kar is found beside an item",
          "[lyrics][midi]") {
    const std::string file = midi_file({midi_words_track(midi_text(200, "Sing") + midi_text(200, " along"))});
    const auto detected = iclforge::lyrics::parse_timed_text(file);
    REQUIRE(detected.has_value());
    CHECK(detected->lines[0].text == "Sing along");

    Folder folder;
    folder.write("song.mp3", "x");
    folder.write("song.kar", file);
    const std::string item = (folder.path() / "song.mp3").string();
    const auto found = iclforge::lyrics::find_lyrics_file(item);
    REQUIRE(found.has_value());
    CHECK(found->extension().string() == ".kar");
    const auto read = iclforge::lyrics::read_lyrics(*found);
    REQUIRE(read.has_value());
    CHECK(read->lines[0].words[1].start_ms == 400);
    // The other formats come first where a stem has more than one.
    folder.write("song.srt", "1" + midi_byte(10) + "00:00:01,000 --> 00:00:02,000" + midi_byte(10) + "Subtitles" + midi_byte(10));
    CHECK(iclforge::lyrics::find_lyrics_file(item)->extension().string() == ".srt");
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

// ---- who sings an ASS line, and which of the lines at one time is read -----------------------------

TEST_CASE("ass: two actors on the sung lines are the two singers, in the order they first appear",
          "[lyrics][ass][duet]") {
    // The second singer's line is written first in the file and is later in time: the first vocal
    // is the one who sings first.
    const std::string text = kAssHeader +
        "Dialogue: 0,0:00:03.00,0:00:05.00,Default,Ben,0,0,0,,{\\k100}Hi {\\k100}there\n"
        "Dialogue: 0,0:00:01.00,0:00:03.00,Default,Anna,0,0,0,,{\\k100}Hel{\\k100}lo\n"
        "Dialogue: 0,0:00:05.00,0:00:07.00,Default,Anna,0,0,0,,{\\k200}Bye\n"
        "Dialogue: 0,0:00:08.00,0:00:09.00,Default,Anna,0,0,0,,Spoken by Anna, not sung\n";
    const auto lyrics = iclforge::lyrics::parse_ass(text);
    REQUIRE(lyrics.has_value());
    CHECK(lyrics->first_singer_name == "Anna");
    CHECK(lyrics->second_singer_name == "Ben");
    REQUIRE(lyrics->lines.size() == 4);
    using iclforge::lyrics::Singer;
    CHECK(lyrics->lines[0].text == "Hello");
    CHECK(lyrics->lines[0].singer == Singer::kFirst);
    CHECK(lyrics->lines[1].text == "Hi there");
    CHECK(lyrics->lines[1].singer == Singer::kSecond);
    CHECK(lyrics->lines[2].singer == Singer::kFirst);
    // A line with no karaoke tags is not sung, whatever actor it names: nobody in particular.
    CHECK(lyrics->lines[3].singer == Singer::kUnmarked);
    CHECK(iclforge::lyrics::has_singers(*lyrics));

    // The markers are what is written, and read back.
    const std::string written = iclforge::lyrics::write_lrc(*lyrics);
    CHECK(written.find("V1: ") != std::string::npos);
    CHECK(written.find("V2: ") != std::string::npos);
    CHECK(written.find("[v1:Anna]") != std::string::npos);
}

TEST_CASE("ass: two styles on the sung lines are the two singers when no actors are named",
          "[lyrics][ass][duet]") {
    const std::string text = kAssHeader +
        "Dialogue: 0,0:00:01.00,0:00:03.00,Male,,0,0,0,,{\\k100}Hel{\\k100}lo\n"
        "Dialogue: 0,0:00:03.00,0:00:05.00,Female,,0,0,0,,{\\k100}Hi {\\k100}there\n"
        "Dialogue: 0,0:00:05.00,0:00:07.00,Male,,0,0,0,,{\\k200}Bye\n";
    const auto lyrics = iclforge::lyrics::parse_ass(text);
    REQUIRE(lyrics.has_value());
    REQUIRE(lyrics->lines.size() == 3);
    using iclforge::lyrics::Singer;
    CHECK(lyrics->lines[0].singer == Singer::kFirst);
    CHECK(lyrics->lines[1].singer == Singer::kSecond);
    CHECK(lyrics->lines[2].singer == Singer::kFirst);
    // A style is the author's own label, and is not shown as a singer's name.
    CHECK(lyrics->first_singer_name.empty());
    CHECK(lyrics->second_singer_name.empty());
}

TEST_CASE("ass: actors decide before styles, and a sung line with no actor is for nobody in particular",
          "[lyrics][ass][duet]") {
    // Three styles, which alone would mark nobody; two actors, which are clear.
    const std::string text = kAssHeader +
        "Dialogue: 0,0:00:01.00,0:00:03.00,Verse,Anna,0,0,0,,{\\k100}One{\\k100}a\n"
        "Dialogue: 0,0:00:03.00,0:00:05.00,Verse2,Ben,0,0,0,,{\\k100}Two{\\k100}b\n"
        "Dialogue: 0,0:00:05.00,0:00:07.00,Chorus,,0,0,0,,{\\k100}All{\\k100}in\n";
    const auto lyrics = iclforge::lyrics::parse_ass(text);
    REQUIRE(lyrics.has_value());
    REQUIRE(lyrics->lines.size() == 3);
    using iclforge::lyrics::Singer;
    CHECK(lyrics->lines[0].singer == Singer::kFirst);
    CHECK(lyrics->lines[1].singer == Singer::kSecond);
    CHECK(lyrics->lines[2].singer == Singer::kUnmarked);
}

TEST_CASE("ass: one singer, or three, marks nobody, because it is not clear who is who",
          "[lyrics][ass][duet]") {
    using iclforge::lyrics::Singer;
    const auto none_marked = [](const std::string& text) {
        const auto lyrics = iclforge::lyrics::parse_ass(text);
        REQUIRE(lyrics.has_value());
        for (const auto& line : lyrics->lines) {
            CHECK(line.singer == Singer::kUnmarked);
        }
        CHECK(lyrics->first_singer_name.empty());
        CHECK_FALSE(iclforge::lyrics::has_singers(*lyrics));
    };
    // One style and one actor.
    none_marked(kAssHeader +
                "Dialogue: 0,0:00:01.00,0:00:03.00,Default,Anna,0,0,0,,{\\k100}A{\\k100}b\n"
                "Dialogue: 0,0:00:03.00,0:00:05.00,Default,Anna,0,0,0,,{\\k100}C{\\k100}d\n");
    // Three styles, no actors.
    none_marked(kAssHeader +
                "Dialogue: 0,0:00:01.00,0:00:03.00,Lead,,0,0,0,,{\\k100}A{\\k100}b\n"
                "Dialogue: 0,0:00:03.00,0:00:05.00,Harmony,,0,0,0,,{\\k100}C{\\k100}d\n"
                "Dialogue: 0,0:00:05.00,0:00:07.00,Both,,0,0,0,,{\\k100}E{\\k100}f\n");
    // Three actors.
    none_marked(kAssHeader +
                "Dialogue: 0,0:00:01.00,0:00:03.00,Default,Anna,0,0,0,,{\\k100}A{\\k100}b\n"
                "Dialogue: 0,0:00:03.00,0:00:05.00,Default,Ben,0,0,0,,{\\k100}C{\\k100}d\n"
                "Dialogue: 0,0:00:05.00,0:00:07.00,Default,Cara,0,0,0,,{\\k100}E{\\k100}f\n");
    // A second style that is not sung does not make a duet: a translation is not a singer.
    none_marked(kAssHeader +
                "Dialogue: 0,0:00:01.00,0:00:03.00,Romaji,,0,0,0,,{\\k100}A{\\k100}b\n"
                "Dialogue: 0,0:00:07.00,0:00:09.00,Translation,,0,0,0,,Thank you\n");
}

TEST_CASE("ass: a translation at the sung line's time is not read, and one with a time of its own is",
          "[lyrics][ass][styles]") {
    const std::string text = kAssHeader +
        "Dialogue: 0,0:00:01.00,0:00:03.00,Romaji,,0,0,0,,{\\k100}Ari{\\k100}gatou\n"
        "Dialogue: 0,0:00:01.00,0:00:03.00,Translation,,0,0,0,,Thank you\n"
        "Dialogue: 0,0:00:05.00,0:00:07.00,Translation,,0,0,0,,A line of its own\n"
        "Dialogue: 0,0:00:09.00,0:00:12.00,Romaji,,0,0,0,,{\\k100}Sa{\\k100}yonara\n"
        "Dialogue: 0,0:00:09.00,0:00:11.00,Translation,,0,0,0,,Goodbye\n";
    const auto lyrics = iclforge::lyrics::parse_ass(text);
    REQUIRE(lyrics.has_value());
    REQUIRE(lyrics->lines.size() == 4);
    CHECK(lyrics->lines[0].text == "Arigatou");
    CHECK(lyrics->lines[0].word_timed());
    CHECK(lyrics->lines[1].text == "A line of its own");
    // At the same start but not the same end is not the same time: both are read.
    CHECK(lyrics->lines[2].text == "Sayonara");
    CHECK(lyrics->lines[3].text == "Goodbye");
}

TEST_CASE("ass: a line written on two layers is one line, and lines of one style at one time are all read",
          "[lyrics][ass][styles]") {
    // An outline under a fill: the same line twice, on layers 0 and 1.
    const auto layered = iclforge::lyrics::parse_ass(kAssHeader +
        "Dialogue: 0,0:00:01.00,0:00:03.00,Default,,0,0,0,,{\\k100}Hel{\\k100}lo\n"
        "Dialogue: 1,0:00:01.00,0:00:03.00,Default,,0,0,0,,{\\k100}Hel{\\k100}lo\n");
    REQUIRE(layered.has_value());
    CHECK(layered->lines.size() == 1);

    // A top line and a bottom line: two lines of one style at one time, which are two lines.
    const auto stacked = iclforge::lyrics::parse_ass(kAssHeader +
        "Dialogue: 0,0:00:01.00,0:00:03.00,Default,,0,0,0,,{\\an8}Top line\n"
        "Dialogue: 0,0:00:01.00,0:00:03.00,Default,,0,0,0,,Bottom line\n");
    REQUIRE(stacked.has_value());
    CHECK(stacked->lines.size() == 2);

    // The same words in the same style at a different end are not a repeat.
    const auto longer = iclforge::lyrics::parse_ass(kAssHeader +
        "Dialogue: 0,0:00:01.00,0:00:03.00,Default,,0,0,0,,Same words\n"
        "Dialogue: 0,0:00:01.00,0:00:04.00,Default,,0,0,0,,Same words\n");
    REQUIRE(longer.has_value());
    CHECK(longer->lines.size() == 2);
}

TEST_CASE("ass: with no sung line among them, the lines at one time are read in the file's commonest style",
          "[lyrics][ass][styles]") {
    // English is the file's commonest style (3 events to Spanish's 2): where the two share a time,
    // English is read. A time with Spanish alone is read in Spanish.
    const std::string text = kAssHeader +
        "Dialogue: 0,0:00:01.00,0:00:03.00,English,,0,0,0,,Hello there\n"
        "Dialogue: 0,0:00:01.00,0:00:03.00,Spanish,,0,0,0,,Hola\n"
        "Dialogue: 0,0:00:04.00,0:00:06.00,English,,0,0,0,,Goodbye\n"
        "Dialogue: 0,0:00:04.00,0:00:06.00,Spanish,,0,0,0,,Adios\n"
        "Dialogue: 0,0:00:07.00,0:00:09.00,English,,0,0,0,,Again\n";
    const auto lyrics = iclforge::lyrics::parse_ass(text);
    REQUIRE(lyrics.has_value());
    REQUIRE(lyrics->lines.size() == 3);
    CHECK(lyrics->lines[0].text == "Hello there");
    CHECK(lyrics->lines[1].text == "Goodbye");
    CHECK(lyrics->lines[2].text == "Again");
}

// ---- a lyrics file beside a stream (place_lyrics_beside) --------------------------------------

namespace {

std::string read_text_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

}  // namespace

TEST_CASE("place_lyrics_beside writes the stream's own .lrc from a SubRip file, gaps marked",
          "[lyrics][place]") {
    const Folder folder;
    const fs::path srt = folder.write(
        "words.srt",
        "1\n00:00:01,000 --> 00:00:03,000\nOne\n\n2\n00:00:05,000 --> 00:00:07,000\nTwo\n");
    const fs::path stream = folder.path() / "song.ac3";

    const auto placed = iclforge::lyrics::place_lyrics_beside(srt, stream);
    REQUIRE(placed.status == iclforge::lyrics::PlacementStatus::kWritten);
    CHECK(placed.path == folder.path() / "song.lrc");
    CHECK(read_text_file(placed.path) == "[00:01.00]One\n[00:03.00]\n[00:05.00]Two\n");
    // What a player looks for beside the stream is found by name, and is not the half-written file.
    const auto found = iclforge::lyrics::find_lyrics_file(stream.string());
    REQUIRE(found.has_value());
    CHECK(*found == placed.path);
    CHECK_FALSE(fs::exists(folder.path() / "song.lrc.part"));
}

TEST_CASE("place_lyrics_beside keeps an ASS script's word times and a duet's singers",
          "[lyrics][place]") {
    const Folder folder;
    const fs::path ass = folder.write(
        "script.ass",
        "[Script Info]\nScriptType: v4.00+\n\n[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:01.00,0:00:03.00,Default,,0,0,0,,{\\k100}Hel{\\k100}lo\n");
    const auto placed =
        iclforge::lyrics::place_lyrics_beside(ass, folder.path() / "karaoke.ac3");
    REQUIRE(placed.status == iclforge::lyrics::PlacementStatus::kWritten);
    // The syllables are words with their own times: "Hel" at 1.00 s and "lo" at 2.00 s.
    CHECK(read_text_file(placed.path) == "[00:01.00]<00:01.00>Hel<00:02.00>lo\n");

    const fs::path duet = folder.write("duet.lrc", "[v1:Anna]\n[v2:Ben]\n[00:01.00]V1: Hello\n[00:03.00]V2: Hi\n");
    const auto second = iclforge::lyrics::place_lyrics_beside(duet, folder.path() / "pair.mp4");
    REQUIRE(second.status == iclforge::lyrics::PlacementStatus::kWritten);
    CHECK(second.path == folder.path() / "pair.lrc");
    const auto reread = iclforge::lyrics::read_lyrics(second.path);
    REQUIRE(reread.has_value());
    CHECK(reread->first_singer_name == "Anna");
    REQUIRE(reread->lines.size() == 2);
    CHECK(reread->lines[1].singer == iclforge::lyrics::Singer::kSecond);
}

TEST_CASE("place_lyrics_beside never overwrites a file of that name", "[lyrics][place]") {
    const Folder folder;
    const fs::path srt = folder.write("words.srt", "1\n00:00:01,000 --> 00:00:03,000\nNew\n");
    const fs::path existing = folder.write("song.lrc", "[00:00.00]Mine\n");
    const fs::path stream = folder.path() / "song.ac3";

    const auto placed = iclforge::lyrics::place_lyrics_beside(srt, stream);
    CHECK(placed.status == iclforge::lyrics::PlacementStatus::kExists);
    CHECK(placed.path == existing);
    CHECK(read_text_file(existing) == "[00:00.00]Mine\n");

    // Giving the file that is already there is the same answer, and it is left as it was.
    const auto itself = iclforge::lyrics::place_lyrics_beside(existing, stream);
    CHECK(itself.status == iclforge::lyrics::PlacementStatus::kExists);
    CHECK(read_text_file(existing) == "[00:00.00]Mine\n");
}

TEST_CASE("place_lyrics_beside says why a file is not lyrics, and writes nothing",
          "[lyrics][place]") {
    const Folder folder;
    const fs::path stream = folder.path() / "song.ac3";

    // Latin-1: a code page is named and not guessed at.
    const fs::path latin1 = folder.write("cafe.lrc", "[00:01.00]Caf\xE9\n");
    const auto coded = iclforge::lyrics::place_lyrics_beside(latin1, stream);
    CHECK(coded.status == iclforge::lyrics::PlacementStatus::kUnreadable);
    CHECK(coded.error.find("not UTF-8") != std::string::npos);

    // A file that is not there.
    const auto missing = iclforge::lyrics::place_lyrics_beside(folder.path() / "nothing.lrc", stream);
    CHECK(missing.status == iclforge::lyrics::PlacementStatus::kUnreadable);
    CHECK_FALSE(missing.error.empty());

    CHECK_FALSE(fs::exists(folder.path() / "song.lrc"));
    CHECK_FALSE(fs::exists(folder.path() / "song.lrc.part"));
}

// ---- a code page, read with a decoder the program supplies ---------------------------------------

namespace {

// Latin-1: each byte is the code point of that number. `context` counts the calls when given.
bool latin1_decoder(void* context, std::string_view bytes, std::string& utf8) {
    if (context != nullptr) {
        ++*static_cast<int*>(context);
    }
    for (const char c : bytes) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x80) {
            utf8.push_back(c);
        } else {
            utf8.push_back(static_cast<char>(0xC0U | (byte >> 6U)));
            utf8.push_back(static_cast<char>(0x80U | (byte & 0x3FU)));
        }
    }
    return true;
}

// A code page that does not have the bytes it is given.
bool refusing_decoder(void*, std::string_view, std::string&) { return false; }

// A decoder that says it converted and hands back something that is not UTF-8.
bool broken_decoder(void*, std::string_view, std::string& utf8) {
    utf8 = std::string(1, static_cast<char>(0xFF));
    return true;
}

}  // namespace

TEST_CASE("encoding: a file in a code page is read with the decoder the program gave, and only then",
          "[lyrics][encoding]") {
    const std::string latin1 = "[ti:Caf\xE9]\n[00:01.00]Caf\xE9 au lait\n";
    int calls = 0;
    const iclforge::lyrics::TextEncoding encoding{.decoder = latin1_decoder, .context = &calls};

    // Without a decoder it is refused with the reason, as before: a code page is never guessed at.
    const auto refused = iclforge::lyrics::parse_lyrics(latin1);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == "The lyrics file is not UTF-8 or UTF-16.");

    const auto read = iclforge::lyrics::parse_lyrics(latin1, std::nullopt, encoding);
    REQUIRE(read.has_value());
    CHECK(calls == 1);
    CHECK(read->title == "Caf\xC3\xA9");
    REQUIRE(read->lines.size() == 1);
    // The e acute is U+00E9, which is C3 A9 in UTF-8, not the one byte E9 the file held.
    CHECK(read->lines[0].text == "Caf\xC3\xA9 au lait");
}

TEST_CASE("encoding: a file that is UTF-8 or UTF-16 is read as it always was, whatever decoder is given",
          "[lyrics][encoding]") {
    int calls = 0;
    const iclforge::lyrics::TextEncoding encoding{.decoder = latin1_decoder, .context = &calls};

    const auto utf8 = iclforge::lyrics::parse_lyrics("[00:01.00]Caf\xC3\xA9\n", std::nullopt, encoding);
    REQUIRE(utf8.has_value());
    CHECK(utf8->lines[0].text == "Caf\xC3\xA9");

    // With a byte-order mark, in either order.
    const std::string utf8_mark = "\xEF\xBB\xBF[00:01.00]Caf\xC3\xA9\n";
    CHECK(iclforge::lyrics::parse_lyrics(utf8_mark, std::nullopt, encoding)->lines[0].text == "Caf\xC3\xA9");
    std::string utf16 = "\xFF\xFE";
    for (const char c : std::string{"[00:01.00]Hi\n"}) {
        utf16.push_back(c);
        utf16.push_back('\0');
    }
    CHECK(iclforge::lyrics::parse_lyrics(utf16, std::nullopt, encoding)->lines[0].text == "Hi");

    // The decoder was never asked.
    CHECK(calls == 0);
}

TEST_CASE("encoding: a decoder that cannot read the bytes, or hands back what is not UTF-8, is an error",
          "[lyrics][encoding]") {
    const std::string latin1 = "[00:01.00]Caf\xE9\n";
    const auto refusing = iclforge::lyrics::parse_lyrics(
        latin1, std::nullopt, iclforge::lyrics::TextEncoding{.decoder = refusing_decoder});
    REQUIRE_FALSE(refusing.has_value());
    CHECK(refusing.error() == "The lyrics file is not valid in the code page it was read as.");

    const auto broken = iclforge::lyrics::parse_lyrics(
        latin1, std::nullopt, iclforge::lyrics::TextEncoding{.decoder = broken_decoder});
    REQUIRE_FALSE(broken.has_value());
    CHECK(broken.error() == "The lyrics file is not valid in the code page it was read as.");
}

TEST_CASE("encoding: SubRip, ASS and whatever parse_timed_text is given are read in the code page too",
          "[lyrics][encoding]") {
    const iclforge::lyrics::TextEncoding encoding{.decoder = latin1_decoder};
    const std::string srt = "1\n00:00:01,000 --> 00:00:03,000\nCaf\xE9\n";
    const auto subrip = iclforge::lyrics::parse_subrip(srt, std::nullopt, encoding);
    REQUIRE(subrip.has_value());
    CHECK(subrip->lines[0].text == "Caf\xC3\xA9");

    const std::string ass =
        "[Script Info]\nScriptType: v4.00+\n\n[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:01.00,0:00:03.00,Default,,0,0,0,,{\\k100}Caf{\\k100}\xE9\n";
    const auto script = iclforge::lyrics::parse_ass(ass, std::nullopt, encoding);
    REQUIRE(script.has_value());
    CHECK(script->lines[0].text == "Caf\xC3\xA9");

    // Whichever it is, told by what it holds after it has been decoded.
    const auto told_srt = iclforge::lyrics::parse_timed_text(srt, std::nullopt, encoding);
    REQUIRE(told_srt.has_value());
    CHECK(told_srt->lines[0].text == "Caf\xC3\xA9");
    const auto told_ass = iclforge::lyrics::parse_timed_text(ass, std::nullopt, encoding);
    REQUIRE(told_ass.has_value());
    CHECK(told_ass->lines[0].word_timed());
    CHECK_FALSE(iclforge::lyrics::parse_timed_text(srt).has_value());
}

TEST_CASE("encoding: a MIDI karaoke file's syllables, title and artist are read in the code page, and the other @ lines are not",
          "[lyrics][midi][encoding]") {
    int calls = 0;
    const iclforge::lyrics::TextEncoding encoding{.decoder = latin1_decoder, .context = &calls};
    const std::string e_acute = midi_byte(0xE9);
    const std::string words = midi_words_track(
        midi_text(0, "@TCaf" + e_acute) + midi_text(0, "@TD" + e_acute + "j" + e_acute) +
        midi_text(0, "@I" + midi_byte(0xA9) + " 1985") + midi_text(100, "Caf" + e_acute) +
        midi_text(100, " au") + midi_text(100, " lait"));

    // Refused without it, as before.
    CHECK_FALSE(iclforge::lyrics::parse_midi_karaoke(midi_file({words})).has_value());

    const auto lyrics = iclforge::lyrics::parse_midi_karaoke(midi_file({words}), std::nullopt, encoding);
    REQUIRE(lyrics.has_value());
    CHECK(lyrics->title == "Caf\xC3\xA9");
    CHECK(lyrics->artist == "D\xC3\xA9j\xC3\xA9");
    REQUIRE(lyrics->lines.size() == 1);
    CHECK(lyrics->lines[0].text == "Caf\xC3\xA9 au lait");
    // The copyright line is never shown and was never asked of the decoder: two names and one syllable
    // are the bytes that were not UTF-8.
    CHECK(calls == 3);

    // And told by its header, with the decoder carried through, when it is given as any lyrics file.
    const auto told = iclforge::lyrics::parse_timed_text(midi_file({words}), std::nullopt, encoding);
    REQUIRE(told.has_value());
    CHECK(told->lines[0].text == "Caf\xC3\xA9 au lait");

    // A syllable the page does not have is an error naming the page, not a word of another script.
    const auto refused = iclforge::lyrics::parse_midi_karaoke(
        midi_file({words}), std::nullopt, iclforge::lyrics::TextEncoding{.decoder = refusing_decoder});
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == "The words of the MIDI file are not valid in the code page they were read as.");
}

TEST_CASE("encoding: read_lyrics and place_lyrics_beside take the decoder, and write UTF-8", "[lyrics][encoding]") {
    const Folder folder;
    const fs::path lrc = folder.write("cafe.lrc", "[00:01.00]Caf\xE9\n");
    const iclforge::lyrics::TextEncoding encoding{.decoder = latin1_decoder};

    CHECK_FALSE(iclforge::lyrics::read_lyrics(lrc).has_value());
    const auto read = iclforge::lyrics::read_lyrics(lrc, std::nullopt, encoding);
    REQUIRE(read.has_value());
    CHECK(read->lines[0].text == "Caf\xC3\xA9");

    const fs::path stream = folder.path() / "song.ac3";
    const auto unplaced = iclforge::lyrics::place_lyrics_beside(lrc, stream);
    CHECK(unplaced.status == iclforge::lyrics::PlacementStatus::kUnreadable);
    CHECK_FALSE(fs::exists(folder.path() / "song.lrc"));

    const auto placed = iclforge::lyrics::place_lyrics_beside(lrc, stream, std::nullopt, encoding);
    REQUIRE(placed.status == iclforge::lyrics::PlacementStatus::kWritten);
    // What a player reads: UTF-8, the e acute as C3 A9.
    CHECK(read_text_file(placed.path) == "[00:01.00]Caf\xC3\xA9\n");
}

// ---- a lyrics file converted to UTF-8 LRC where the caller says (convert_lyrics_file) ------------

TEST_CASE("convert_lyrics_file writes UTF-8 LRC from a file in a code page, read in the page named",
          "[lyrics][convert]") {
    const Folder folder;
    const fs::path latin1 = folder.write("cafe.lrc", "[00:01.00]Caf\xE9\n[00:03.00]Cr\xE8me\n");
    const fs::path out = folder.path() / "cafe-utf8.lrc";
    int calls = 0;
    const iclforge::lyrics::TextEncoding encoding{.decoder = latin1_decoder, .context = &calls};

    // Without the page it is refused by name, and nothing is written.
    const auto refused = iclforge::lyrics::convert_lyrics_file(latin1, out);
    CHECK(refused.status == iclforge::lyrics::PlacementStatus::kUnreadable);
    CHECK(iclforge::lyrics::is_code_page_refusal(refused.error));
    CHECK_FALSE(fs::exists(out));
    CHECK_FALSE(fs::exists(folder.path() / "cafe-utf8.lrc.part"));

    const auto written = iclforge::lyrics::convert_lyrics_file(latin1, out, std::nullopt, encoding);
    REQUIRE(written.status == iclforge::lyrics::PlacementStatus::kWritten);
    CHECK(written.path == out);
    CHECK(read_text_file(out) == "[00:01.00]Caf\xC3\xA9\n[00:03.00]Cr\xC3\xA8me\n");
    CHECK(calls > 0);
    // The file converted is left as it was.
    CHECK(read_text_file(latin1) == "[00:01.00]Caf\xE9\n[00:03.00]Cr\xE8me\n");
}

TEST_CASE("convert_lyrics_file leaves a file there alone unless told to replace it",
          "[lyrics][convert]") {
    const Folder folder;
    const fs::path srt = folder.write("words.srt", "1\n00:00:01,000 --> 00:00:03,000\nNew\n");
    const fs::path existing = folder.write("mine.lrc", "[00:00.00]Mine\n");

    const auto kept = iclforge::lyrics::convert_lyrics_file(srt, existing);
    CHECK(kept.status == iclforge::lyrics::PlacementStatus::kExists);
    CHECK(read_text_file(existing) == "[00:00.00]Mine\n");

    const auto replaced = iclforge::lyrics::convert_lyrics_file(srt, existing, std::nullopt, {}, true);
    CHECK(replaced.status == iclforge::lyrics::PlacementStatus::kWritten);
    CHECK(read_text_file(existing) == "[00:01.00]New\n");
}

TEST_CASE("convert_lyrics_file never writes over the file it reads, even when told to replace",
          "[lyrics][convert]") {
    const Folder folder;
    const fs::path source = folder.write("song.lrc", "[00:01.00]Caf\xE9\n");
    int calls = 0;
    const iclforge::lyrics::TextEncoding encoding{.decoder = latin1_decoder, .context = &calls};

    const auto same = iclforge::lyrics::convert_lyrics_file(source, source, std::nullopt, encoding, true);
    CHECK(same.status == iclforge::lyrics::PlacementStatus::kFailed);
    CHECK_FALSE(same.error.empty());
    CHECK(read_text_file(source) == "[00:01.00]Caf\xE9\n");
    CHECK_FALSE(fs::exists(folder.path() / "song.lrc.part"));
}

TEST_CASE("convert_lyrics_file says a file that is not there is not lyrics, and writes nothing",
          "[lyrics][convert]") {
    const Folder folder;
    const fs::path out = folder.path() / "out.lrc";
    const auto missing = iclforge::lyrics::convert_lyrics_file(folder.path() / "nothing.lrc", out);
    CHECK(missing.status == iclforge::lyrics::PlacementStatus::kUnreadable);
    CHECK_FALSE(missing.error.empty());
    CHECK_FALSE(fs::exists(out));
}

TEST_CASE("is_code_page_refusal is the refusal for want of a page, and nothing else",
          "[lyrics][convert]") {
    using iclforge::lyrics::is_code_page_refusal;
    // A text file in a code page, an ASS script and a MIDI karaoke file, none given a decoder.
    const auto lrc = iclforge::lyrics::parse_lyrics("[00:01.00]Caf\xE9\n");
    REQUIRE_FALSE(lrc.has_value());
    CHECK(is_code_page_refusal(lrc.error()));

    const auto ass = iclforge::lyrics::parse_ass(
        "[Events]\nFormat: Start, End, Text\nDialogue: 0:00:01.00,0:00:02.00,Caf\xE9\n");
    REQUIRE_FALSE(ass.has_value());
    CHECK(is_code_page_refusal(ass.error()));

    const auto midi = iclforge::lyrics::parse_midi_karaoke(
        midi_file({midi_words_track(midi_text(100, "Caf" + midi_byte(0xE9)))}));
    REQUIRE_FALSE(midi.has_value());
    CHECK(is_code_page_refusal(midi.error()));

    // Not that: empty, or text that did not read in the page it was given.
    const auto empty = iclforge::lyrics::parse_lyrics("   \n");
    REQUIRE_FALSE(empty.has_value());
    CHECK_FALSE(is_code_page_refusal(empty.error()));

    const iclforge::lyrics::TextEncoding refusing{.decoder = refusing_decoder, .context = nullptr};
    const auto wrong_page = iclforge::lyrics::parse_lyrics("[00:01.00]Caf\xE9\n", std::nullopt, refusing);
    REQUIRE_FALSE(wrong_page.has_value());
    CHECK_FALSE(is_code_page_refusal(wrong_page.error()));

    CHECK_FALSE(is_code_page_refusal(""));
}

TEST_CASE("code_page_remedy names the command and the key that answer a code page", "[lyrics][convert]") {
    const std::string_view remedy = iclforge::lyrics::code_page_remedy();
    CHECK(remedy.find("forge lyrics-to-lrc") != std::string_view::npos);
    CHECK(remedy.find("lyrics-encoding=") != std::string_view::npos);
    // A sentence of its own, to follow a refusal's.
    CHECK(remedy.ends_with('.'));
}

TEST_CASE("with_code_page_remedy follows a code page refusal and nothing else", "[lyrics][convert]") {
    using iclforge::lyrics::with_code_page_remedy;
    const auto refused = iclforge::lyrics::parse_lyrics("[00:01.00]Caf\xE9\n");
    REQUIRE_FALSE(refused.has_value());
    const std::string shown = with_code_page_remedy(refused.error());
    CHECK(shown.starts_with(refused.error()));
    CHECK(shown.size() > refused.error().size());
    CHECK(shown.ends_with(std::string{iclforge::lyrics::code_page_remedy()}));

    const auto empty = iclforge::lyrics::parse_lyrics("   \n");
    REQUIRE_FALSE(empty.has_value());
    CHECK(with_code_page_remedy(empty.error()) == empty.error());
    CHECK(with_code_page_remedy("") == "");
}

// ---- a script made with Aegisub's karaoke templater --------------------------------------------
// The templater (kara-templater.lua) takes the timed karaoke on lines whose Effect is empty or has
// "karaoke" in it, writes its effects as Dialogue lines with Effect "fx", and keeps the line it made
// them from as a comment with Effect "karaoke". Template and code lines are comments of its own.

namespace {

// A script in the shape the templater leaves: its definitions, the original timed line as a comment,
// and one effect line per syllable.
constexpr const char* kTemplatedScript = R"ass([Script Info]
ScriptType: v4.00+

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,template syl all,{\pos($x,$y)\fad(50,50)}
Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,code once,count = 0
Comment: 1,0:00:01.00,0:00:04.00,Default,,0,0,0,karaoke,{\k50}Hel{\k50}lo {\k100}world
Dialogue: 2,0:00:00.50,0:00:01.50,Default,,0,0,0,fx,{\pos(100,100)\fad(50,50)}Hel
Dialogue: 2,0:00:01.00,0:00:02.00,Default,,0,0,0,fx,{\pos(140,100)\fad(50,50)}lo
Dialogue: 2,0:00:02.00,0:00:04.50,Default,,0,0,0,fx,{\pos(180,100)\fad(50,50)}world
)ass";

}  // namespace

TEST_CASE("ass templater: the commented karaoke line is the lyric, and its effect lines are not",
          "[lyrics][ass]") {
    const auto lyrics = iclforge::lyrics::parse_ass(kTemplatedScript);
    REQUIRE(lyrics.has_value());
    REQUIRE(lyrics->lines.size() == 1);
    const auto& line = lyrics->lines[0];
    CHECK(line.text == "Hello world");
    CHECK(line.start_ms == 1000);
    CHECK(line.end_ms == 4000);
    REQUIRE(line.words.size() == 3);
    CHECK(line.words[0].text == "Hel");
    CHECK(line.words[0].start_ms == 1000);
    CHECK(line.words[0].end_ms == 1500);
    CHECK(line.words[2].text == "world");
    CHECK(line.words[2].start_ms == 2000);
}

TEST_CASE("ass templater: other dialogue stays, and other comments and definitions are not read",
          "[lyrics][ass]") {
    // A title card with no effect is a line of its own; a note commented out with no effect is not.
    const std::string text = std::string(kTemplatedScript) +
                             "Dialogue: 0,0:00:05.00,0:00:06.00,Default,,0,0,0,,Thanks for singing\n"
                             "Comment: 0,0:00:07.00,0:00:08.00,Default,,0,0,0,,A note to the author\n";
    const auto lyrics = iclforge::lyrics::parse_ass(text);
    REQUIRE(lyrics.has_value());
    REQUIRE(lyrics->lines.size() == 2);
    CHECK(lyrics->lines[0].text == "Hello world");
    CHECK(lyrics->lines[1].text == "Thanks for singing");
}

TEST_CASE("ass templater: effect lines with no source to be made from are read as they always were",
          "[lyrics][ass]") {
    // Nothing is left to prefer, so the effect lines are the lines: as before this rule.
    const std::string text =
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,fx,One\n"
        "Dialogue: 0,0:00:03.00,0:00:04.00,Default,,0,0,0,fx,Two\n";
    const auto lyrics = iclforge::lyrics::parse_ass(text);
    REQUIRE(lyrics.has_value());
    REQUIRE(lyrics->lines.size() == 2);
    CHECK(lyrics->lines[0].text == "One");
    CHECK(lyrics->lines[1].text == "Two");
}

TEST_CASE("ass templater: a source still on a dialogue line is read and its effects are not",
          "[lyrics][ass]") {
    // Not yet commented out (Aegisub does that once a template has applied): Effect "Karaoke", in any
    // case, marks the source, and "FX" the effects.
    const std::string text =
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 1,0:00:01.00,0:00:03.00,Default,,0,0,0,Karaoke,{\\k100}Sing {\\k100}along\n"
        "Dialogue: 2,0:00:01.00,0:00:02.00,Default,,0,0,0,FX,{\\pos(10,10)}Sing\n"
        "Dialogue: 2,0:00:02.00,0:00:03.00,Default,,0,0,0,FX,{\\pos(90,10)}along\n";
    const auto lyrics = iclforge::lyrics::parse_ass(text);
    REQUIRE(lyrics.has_value());
    REQUIRE(lyrics->lines.size() == 1);
    CHECK(lyrics->lines[0].text == "Sing along");
    CHECK(lyrics->lines[0].words.size() == 2);
}

TEST_CASE("ass templater: a definition is never the source, whatever its effect says",
          "[lyrics][ass]") {
    // "template" and "code" and "mixin" are the templater's own words for a comment, even when the
    // rest of the effect mentions karaoke; the file's one real line is the Dialogue with no effect.
    const std::string text =
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,template karaoke all,{\\fad(10,10)}\n"
        "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,code karaoke,x = 1\n"
        "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mixin karaoke,{\\blur2}\n"
        "Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,Just a line\n";
    const auto lyrics = iclforge::lyrics::parse_ass(text);
    REQUIRE(lyrics.has_value());
    REQUIRE(lyrics->lines.size() == 1);
    CHECK(lyrics->lines[0].text == "Just a line");
}

TEST_CASE("ass templater: a script whose format names no effect column reads as it always did",
          "[lyrics][ass]") {
    // Without an Effect column nothing can be told, so a comment is a comment and nothing is dropped.
    const std::string text =
        "[Events]\n"
        "Format: Start, End, Text\n"
        "Comment: 0:00:01.00,0:00:02.00,Not read\n"
        "Dialogue: 0:00:03.00,0:00:04.00,Read\n";
    const auto lyrics = iclforge::lyrics::parse_ass(text);
    REQUIRE(lyrics.has_value());
    REQUIRE(lyrics->lines.size() == 1);
    CHECK(lyrics->lines[0].text == "Read");
}

TEST_CASE("ass templater: parse_timed_text reads a templated script by what is in it", "[lyrics][ass]") {
    const auto lyrics = iclforge::lyrics::parse_timed_text(kTemplatedScript);
    REQUIRE(lyrics.has_value());
    REQUIRE(lyrics->lines.size() == 1);
    CHECK(lyrics->lines[0].text == "Hello world");
}

TEST_CASE("ass templater: with no Format line the Effect is the v4+ order's ninth column",
          "[lyrics][ass]") {
    const std::string text =
        "[Events]\n"
        "Comment: 1,0:00:01.00,0:00:03.00,Default,,0,0,0,karaoke,{\\k100}No {\\k100}format\n"
        "Dialogue: 2,0:00:01.00,0:00:02.00,Default,,0,0,0,fx,{\\pos(10,10)}No\n"
        "Dialogue: 2,0:00:02.00,0:00:03.00,Default,,0,0,0,fx,{\\pos(90,10)}format\n";
    const auto lyrics = iclforge::lyrics::parse_ass(text);
    REQUIRE(lyrics.has_value());
    REQUIRE(lyrics->lines.size() == 1);
    CHECK(lyrics->lines[0].text == "No format");
}

TEST_CASE("ass templater: an effect that only has karaoke in it is a source, as Aegisub reads it",
          "[lyrics][ass]") {
    const std::string text =
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Comment: 1,0:00:01.00,0:00:03.00,Default,,0,0,0,syl karaoke line,{\\k100}Any {\\k100}effect\n"
        "Dialogue: 2,0:00:01.00,0:00:02.00,Default,,0,0,0,fx,{\\pos(10,10)}Any\n"
        "Dialogue: 2,0:00:02.00,0:00:03.00,Default,,0,0,0,fx,{\\pos(90,10)}effect\n";
    const auto lyrics = iclforge::lyrics::parse_ass(text);
    REQUIRE(lyrics.has_value());
    REQUIRE(lyrics->lines.size() == 1);
    CHECK(lyrics->lines[0].text == "Any effect");
}
