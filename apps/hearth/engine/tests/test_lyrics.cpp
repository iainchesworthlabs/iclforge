#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "platform/process.hpp"

#include "item_loader.hpp"
#include "lyrics.hpp"
#include "media_info.hpp"

// apps/hearth/engine/src/lyrics.cpp: the LRC reader, the lyrics file's place
// beside an item, and the cursor the lyric view asks on every frame
// (planning/karaoke.md, L1 to L6).

namespace {

namespace fs = std::filesystem;

using iclforge::hearth::LyricPosition;
using iclforge::hearth::Lyrics;
using iclforge::hearth::WordState;

Lyrics parsed(const std::string& text, std::optional<std::int64_t> duration = std::nullopt) {
    const auto lyrics = iclforge::hearth::parse_lyrics(text, duration);
    REQUIRE(lyrics.has_value());
    return *lyrics;
}

// A folder of this process's own under the test scratch directory.
class Folder {
public:
    Folder()
        : path_(fs::path{ICLFORGE_TEST_SCRATCH_DIR} /
                ("hearth_lyrics_" + iclforge::test::platform::process_id())) {
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

TEST_CASE("lyrics: lines, metadata and the last line's end", "[hearth][lyrics]") {
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

TEST_CASE("lyrics: the time forms and a tag that is not one", "[hearth][lyrics]") {
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

TEST_CASE("lyrics: several time tags put a line at each time, in order", "[hearth][lyrics]") {
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
          "[hearth][lyrics]") {
    const Lyrics sooner = parsed("[offset:+500]\n[00:10.00]x\n[00:00.10]y\n");
    CHECK(sooner.offset_ms == 500);
    REQUIRE(sooner.lines.size() == 2);
    CHECK(sooner.lines[0].start_ms == 0);  // 100 ms less 500, held at the start
    CHECK(sooner.lines[1].start_ms == 9500);

    const Lyrics later = parsed("[00:10.00]x\n[offset:-250]\n");
    REQUIRE(later.lines.size() == 1);
    CHECK(later.lines[0].start_ms == 10250);
}

TEST_CASE("lyrics: word tags give each word its time", "[hearth][lyrics]") {
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
          "[hearth][lyrics]") {
    const Lyrics lyrics = parsed("[00:05.00]Oh <00:05.50>yes\n[00:08.00]end\n");
    REQUIRE(lyrics.lines[0].words.size() == 2);
    CHECK(lyrics.lines[0].words[0].text == "Oh ");
    CHECK(lyrics.lines[0].words[0].start_ms == 5000);
    CHECK(lyrics.lines[0].words[0].end_ms == 5500);
    CHECK(lyrics.lines[0].words[1].text == "yes");
    CHECK(lyrics.lines[0].words[1].end_ms == 8000);
}

TEST_CASE("lyrics: a repeated line keeps its words' spacing in time", "[hearth][lyrics]") {
    const Lyrics lyrics = parsed("[00:10.00][00:30.00]<00:10.00>la <00:10.50>la\n[00:50.00]x\n");
    REQUIRE(lyrics.lines.size() == 3);
    CHECK(lyrics.lines[0].words[1].start_ms == 10500);
    REQUIRE(lyrics.lines[1].start_ms == 30000);
    CHECK(lyrics.lines[1].words[0].start_ms == 30000);
    CHECK(lyrics.lines[1].words[1].start_ms == 30500);
}

TEST_CASE("lyrics: a file with no times is plain text and is not followed", "[hearth][lyrics]") {
    const Lyrics lyrics = parsed("[ti:Plain]\nLine one\n\n[Chorus]\nLine two\n");
    CHECK_FALSE(lyrics.timed);
    CHECK(lyrics.title == "Plain");
    REQUIRE(lyrics.lines.size() == 3);
    CHECK(lyrics.lines[0].text == "Line one");
    CHECK(lyrics.lines[1].text == "[Chorus]");
    CHECK(lyrics.lines[2].text == "Line two");
    const LyricPosition where = iclforge::hearth::lyric_position(lyrics, 5000);
    CHECK_FALSE(where.line.has_value());
}

TEST_CASE("lyrics: a byte-order mark, and every kind of line ending", "[hearth][lyrics]") {
    const Lyrics lyrics = parsed("\xEF\xBB\xBF[00:01.00]a\r\n[00:02.00]b\r[00:03.00]c\n");
    REQUIRE(lyrics.lines.size() == 3);
    CHECK(lyrics.lines[0].text == "a");
    CHECK(lyrics.lines[1].text == "b");
    CHECK(lyrics.lines[2].text == "c");
    const Lyrics accents = parsed("[00:01.00]caf\xC3\xA9 \xE2\x80\x94 \xF0\x9F\x8E\xA4\n");
    CHECK(accents.lines[0].text == "caf\xC3\xA9 \xE2\x80\x94 \xF0\x9F\x8E\xA4");
}

TEST_CASE("lyrics: what is not lyrics says why", "[hearth][lyrics]") {
    const auto empty = iclforge::hearth::parse_lyrics("  \r\n \n");
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error().find("empty") != std::string::npos);

    // Latin-1 and a truncated or overlong sequence are not UTF-8, and are not
    // guessed at.
    for (const char* bad : {"[00:01.00]caf\xE9\n", "[00:01.00]\xE2\x82\n", "\xC0\x80[00:01.00]x\n",
                            "[00:01.00]\xED\xA0\x80\n"}) {
        const auto result = iclforge::hearth::parse_lyrics(bad);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("UTF-8") != std::string::npos);
    }

    const auto tags_only = iclforge::hearth::parse_lyrics("[ti:x]\n[ar:y]\n");
    REQUIRE_FALSE(tags_only.has_value());
    CHECK(tags_only.error().find("no lyrics") != std::string::npos);
}

TEST_CASE("lyrics: the cursor finds the line, the word and how far through it",
          "[hearth][lyrics]") {
    const Lyrics lyrics = parsed(kWordTimed);
    using iclforge::hearth::lyric_position;

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
        CHECK(iclforge::hearth::word_state(word, at.position_ms) == WordState::kSung);
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
          "[hearth][lyrics]") {
    const Lyrics lyrics = parsed(kWordTimed);
    using iclforge::hearth::lyric_position;

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
          "[hearth][lyrics]") {
    const iclforge::hearth::LyricWord word{.start_ms = 1000, .end_ms = 2000, .text = "x"};
    CHECK(iclforge::hearth::word_state(word, 999) == WordState::kNotYetSung);
    CHECK(iclforge::hearth::word_state(word, 1000) == WordState::kBeingSung);
    CHECK(iclforge::hearth::word_state(word, 1999) == WordState::kBeingSung);
    CHECK(iclforge::hearth::word_state(word, 2000) == WordState::kSung);
    const iclforge::hearth::LyricWord blink{.start_ms = 5000, .end_ms = 5000, .text = "y"};
    CHECK(iclforge::hearth::word_state(blink, 4999) == WordState::kNotYetSung);
    CHECK(iclforge::hearth::word_state(blink, 5000) == WordState::kSung);
}

TEST_CASE("lyrics: the file beside an item, in any case of the extension", "[hearth][lyrics]") {
    const Folder folder;
    folder.write("Song.LRC", "[00:01.00]hi\n");
    folder.write("other.txt", "x");
    folder.write("a.b.lrc", "[00:01.00]dots\n");

    const auto found = iclforge::hearth::find_lyrics_file((folder.path() / "Song.ac3").string());
    REQUIRE(found.has_value());
    // The same file, however this file system spells its name.
    CHECK(fs::equivalent(*found, folder.path() / "Song.LRC"));

    // A stem with a dot is the whole name less the last extension.
    const auto dotted = iclforge::hearth::find_lyrics_file((folder.path() / "a.b.mp4").string());
    REQUIRE(dotted.has_value());
    CHECK(dotted->filename().string() == "a.b.lrc");

    CHECK_FALSE(iclforge::hearth::find_lyrics_file((folder.path() / "other.ac3").string())
                    .has_value());
    CHECK_FALSE(iclforge::hearth::find_lyrics_file((folder.path() / "missing.ac3").string())
                    .has_value());

    const auto read = iclforge::hearth::read_lyrics(*found, 90000);
    REQUIRE(read.has_value());
    CHECK(read->lines.size() == 1);
    CHECK(read->lines[0].end_ms == 90000);

    const auto none = iclforge::hearth::read_lyrics(folder.path() / "nope.lrc");
    REQUIRE_FALSE(none.has_value());
    CHECK_FALSE(none.error().empty());
}

// The karaoke example's own words (examples/karaoke/auld-lang-syne/, made by
// tools/generators/gen_karaoke_example.py) read by this reader beside the stream they go with:
// found by the stream's name, timed to the syllable, and every word inside the stream's length.
// The file's other checks - that it is what the generator makes, that its tags are the vocal's
// notes - are tools/checks/test_gen_karaoke_example.py.
TEST_CASE("lyrics: the karaoke example's words are read and every word is inside its stream",
          "[hearth][lyrics][example]") {
    const fs::path stream = fs::path{ICLFORGE_KARAOKE_EXAMPLE_DIR} / "auld-lang-syne.ac3";
    const auto loaded = iclforge::hearth::ui::make_file_item_loader()(stream.string());
    REQUIRE(loaded.has_value());
    const auto info = iclforge::hearth::describe_media(stream.string(), *loaded);
    REQUIRE(info.sample_rate == 48000);
    const auto duration_ms =
        static_cast<std::int64_t>(info.played_samples() * 1000 / info.sample_rate);
    REQUIRE(duration_ms > 16000);

    const auto found = iclforge::hearth::find_lyrics_file(stream.string());
    REQUIRE(found.has_value());
    CHECK(found->filename().string() == "auld-lang-syne.lrc");
    const auto lyrics = iclforge::hearth::read_lyrics(*found, duration_ms);
    REQUIRE(lyrics.has_value());
    CHECK(lyrics->title == "Auld Lang Syne");
    CHECK(lyrics->timed);

    std::size_t sung = 0;
    std::size_t syllables = 0;
    std::int64_t previous = -1;
    for (const auto& line : lyrics->lines) {
        CHECK(line.start_ms >= previous);
        CHECK(line.end_ms <= duration_ms);
        previous = line.start_ms;
        if (line.text.empty()) {
            continue;  // the time that ends the last line
        }
        ++sung;
        CHECK(line.word_timed());
        for (const auto& word : line.words) {
            CHECK(word.start_ms >= line.start_ms);
            CHECK(word.end_ms >= word.start_ms);
            CHECK(word.end_ms <= duration_ms);
            ++syllables;
        }
    }
    CHECK(sung == 4);
    CHECK(syllables == 26);
    CHECK(lyrics->lines.front().text == "Should auld acquaintance be forgot,");
    CHECK(lyrics->lines.front().words.front().start_ms == 0);
    CHECK(lyrics->lines.back().end_ms == duration_ms);

    // The lines-only file is the same four lines, timed to the line and not to the word.
    const auto lines_only = iclforge::hearth::read_lyrics(
        fs::path{ICLFORGE_KARAOKE_EXAMPLE_DIR} / "auld-lang-syne-lines-only.lrc", duration_ms);
    REQUIRE(lines_only.has_value());
    CHECK(lines_only->timed);
    std::size_t texts = 0;
    for (const auto& line : lines_only->lines) {
        CHECK_FALSE(line.word_timed());
        texts += line.text.empty() ? 0U : 1U;
    }
    CHECK(texts == 4);
}
