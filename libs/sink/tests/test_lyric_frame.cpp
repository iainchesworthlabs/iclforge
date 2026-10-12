#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "iclforge/lyrics/lyrics.hpp"
#include "iclforge/lyrics/stage.hpp"
#include "iclforge/sink/lyric_frame.hpp"

// What a screen is handed: the Stage of a document at a position, with the strings it names. The
// documents are the repository's karaoke examples (examples/karaoke) written out, so that the cases
// need no files; the screen's own rules (the length steps, the grace, the colours) are the view's.

namespace {

namespace lyr = iclforge::lyrics;
using iclforge::sink::LyricFrame;

constexpr std::string_view kAuldLangSyne =
    "[ti:Auld Lang Syne]\n"
    "[ar:Traditional; words by Robert Burns, 1788]\n"
    "[al:iclforge karaoke example]\n"
    "[length:00:16]\n"
    "[00:00.00]<00:00.00>Should <00:00.50>auld <00:01.25>ac<00:01.50>quain<00:02.00>tance <00:02.50>be "
    "<00:03.25>for<00:03.50>got,\n"
    "[00:04.00]<00:04.00>And <00:04.50>ne<00:05.25>ver <00:05.50>brought <00:06.00>to <00:06.50>mind?\n"
    "[00:12.00]<00:12.00>Should <00:12.50>auld <00:13.25>ac<00:13.50>quain<00:14.00>tance <00:14.50>be "
    "<00:15.25>for<00:15.50>got,\n"
    "[00:16.50]\n";

constexpr std::string_view kLeaveHerJohnny =
    "[ti:Leave Her, Johnny]\n"
    "[ar:Traditional]\n"
    "[v1:Shantyman]\n"
    "[v2:Crew]\n"
    "[length:00:19]\n"
    "[00:00.00]V1: <00:00.00>I <00:00.60>thought <00:01.20>I <00:01.80>heard <00:02.70>the <00:03.00>Old "
    "<00:03.60>Man <00:04.20>say,\n"
    "[00:05.40]V2: <00:05.40>Leave <00:06.00>her, <00:06.60>John<00:07.20>ny, <00:07.80>leave "
    "<00:08.40>her!\n"
    "[00:09.60]\n";

constexpr std::string_view kLinesOnly =
    "[ti:Auld Lang Syne]\n"
    "[00:00.00]Should auld acquaintance be forgot,\n"
    "[00:04.00]And never brought to mind?\n"
    "[00:08.00]Should auld acquaintance be forgot,\n"
    "[00:12.00]And auld lang syne!\n"
    "[00:16.50]\n";

lyr::Lyrics parsed(std::string_view text) {
    auto lyrics = lyr::parse_lyrics(text);
    REQUIRE(lyrics.has_value());
    return std::move(*lyrics);
}

LyricFrame frame_at(const lyr::Lyrics& lyrics, std::optional<std::int64_t> position_ms, std::int64_t delay_ms = 0,
                    bool streaming = true) {
    const lyr::Stage stage = lyr::make_stage(&lyrics, position_ms, delay_ms);
    return iclforge::sink::make_lyric_frame(std::int64_t{7}, &lyrics, stage, streaming);
}

std::string joined(const LyricFrame& frame) {
    std::string text;
    for (const std::string& word : frame.words) {
        text += word;
    }
    return text;
}

}  // namespace

TEST_CASE("lyric frame: a word-timed line names its words, the one being sung and how far through it", "[sink][lyrics]") {
    const lyr::Lyrics lyrics = parsed(kAuldLangSyne);
    // 2.2 s: "Should auld ac|quain|tance" - "tance" began at 2.0 s and "be" begins at 2.5 s.
    const LyricFrame frame = frame_at(lyrics, 2200);
    CHECK(frame.item == 7);
    CHECK(frame.streaming);
    CHECK(frame.mode == lyr::StageMode::kLine);
    CHECK(frame.title == "Auld Lang Syne");
    CHECK(frame.artist == "Traditional; words by Robert Burns, 1788");
    CHECK(frame.timed);
    CHECK_FALSE(frame.duet);

    CHECK(frame.current.text == "Should auld acquaintance be forgot,");
    CHECK_FALSE(frame.current.right_to_left);
    REQUIRE(frame.word_timed);
    CHECK(joined(frame) == frame.current.text);
    REQUIRE(frame.words.size() == 8);
    CHECK(frame.words_sung == 4);
    REQUIRE(frame.word.has_value());
    CHECK(*frame.word == 4);
    CHECK(frame.words[*frame.word] == "tance ");
    CHECK(frame.fraction > 0.35);
    CHECK(frame.fraction < 0.45);

    CHECK(frame.before.empty());
    REQUIRE(frame.after.size() == 2);
    CHECK(frame.after[0].text == "And never brought to mind?");
    CHECK(frame.after[1].text == "Should auld acquaintance be forgot,");
    CHECK(frame.line_number == 1);
    CHECK(frame.line_count == 3);
    CHECK(frame.length_ms == 16000);
}

TEST_CASE("lyric frame: the singer's delay shows the words later", "[sink][lyrics]") {
    const lyr::Lyrics lyrics = parsed(kAuldLangSyne);
    // 4.2 s is "And" of the second line; a delay of 500 ms shows the lyrics 500 ms later, so the
    // stage is where the words were at 3.7 s: still the first line, on "got,".
    const LyricFrame on_time = frame_at(lyrics, 4200);
    CHECK(on_time.current.text == "And never brought to mind?");
    const LyricFrame delayed = frame_at(lyrics, 4200, 500);
    CHECK(delayed.current.text == "Should auld acquaintance be forgot,");
    CHECK(delayed.position_ms == 3700);
}

TEST_CASE("lyric frame: a duet's line carries its singer and the names the file gave", "[sink][lyrics]") {
    const lyr::Lyrics lyrics = parsed(kLeaveHerJohnny);
    const LyricFrame frame = frame_at(lyrics, 6700);
    CHECK(frame.mode == lyr::StageMode::kLine);
    CHECK(frame.duet);
    CHECK(frame.first_singer == "Shantyman");
    CHECK(frame.second_singer == "Crew");
    CHECK(frame.current.singer == lyr::Singer::kSecond);
    CHECK(frame.current.text == "Leave her, Johnny, leave her!");
    REQUIRE(frame.before.size() == 1);
    CHECK(frame.before[0].singer == lyr::Singer::kFirst);
}

TEST_CASE("lyric frame: a line-timed file shows the line whole and names no word", "[sink][lyrics]") {
    const lyr::Lyrics lyrics = parsed(kLinesOnly);
    const LyricFrame frame = frame_at(lyrics, 4500);
    CHECK(frame.mode == lyr::StageMode::kLine);
    CHECK(frame.current.text == "And never brought to mind?");
    CHECK_FALSE(frame.word_timed);
    CHECK(frame.words.empty());
    CHECK_FALSE(frame.word.has_value());
    CHECK(frame.line_number == 2);
    CHECK(frame.line_count == 4);
}

TEST_CASE("lyric frame: a gap between lines says how long until the next and counts in", "[sink][lyrics]") {
    // A line that only marks a time ends the second line at 7.0 s; the third begins at 12.0 s: a 5 s
    // instrumental break.
    const lyr::Lyrics lyrics = parsed(
        "[00:00.00]<00:00.00>Should <00:00.50>auld <00:01.25>ac<00:01.50>quain<00:02.00>tance "
        "<00:02.50>be <00:03.25>for<00:03.50>got,\n"
        "[00:04.00]<00:04.00>And <00:04.50>ne<00:05.25>ver <00:05.50>brought <00:06.00>to <00:06.50>mind?\n"
        "[00:07.00]\n"
        "[00:12.00]<00:12.00>Should <00:12.50>auld <00:13.25>ac<00:13.50>quain<00:14.00>tance "
        "<00:14.50>be <00:15.25>for<00:15.50>got,\n"
        "[00:16.50]\n");
    const LyricFrame frame = frame_at(lyrics, 9000);
    CHECK(frame.mode == lyr::StageMode::kGap);
    CHECK(frame.instrumental);
    REQUIRE(frame.next_line_in_ms.has_value());
    CHECK(*frame.next_line_in_ms == 3000);
    REQUIRE(frame.gap_ms.has_value());
    CHECK(*frame.gap_ms >= 4000);
    CHECK(frame.count_in >= 1);
    CHECK(frame.count_in <= 4);
    REQUIRE_FALSE(frame.before.empty());
    CHECK(frame.before.back().text == "And never brought to mind?");
    REQUIRE_FALSE(frame.after.empty());
    CHECK(frame.after.front().text == "Should auld acquaintance be forgot,");
    CHECK(frame.current.text.empty());
}

TEST_CASE("lyric frame: a plain file is every line, shown whole and not followed", "[sink][lyrics]") {
    const lyr::Lyrics lyrics = parsed("Should auld acquaintance be forgot,\nAnd never brought to mind?\n");
    const LyricFrame frame = frame_at(lyrics, 1000);
    CHECK(frame.mode == lyr::StageMode::kPlain);
    CHECK_FALSE(frame.timed);
    REQUIRE(frame.plain.size() == 2);
    CHECK(frame.plain[1].text == "And never brought to mind?");
    CHECK(frame.current.text.empty());
    CHECK(frame.before.empty());
    CHECK(frame.after.empty());
}

TEST_CASE("lyric frame: a line's direction is its own, by its first strong letter", "[sink][lyrics]") {
    const lyr::Lyrics lyrics = parsed(
        "[00:00.00]\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D \xD7\xA2\xD7\x95\xD7\x9C\xD7\x9D\n"
        "[00:04.00]Hello world\n"
        "[00:08.00]\n");
    const LyricFrame hebrew = frame_at(lyrics, 1000);
    CHECK(hebrew.current.right_to_left);
    REQUIRE(hebrew.after.size() == 1);
    CHECK_FALSE(hebrew.after[0].right_to_left);
    const LyricFrame latin = frame_at(lyrics, 5000);
    CHECK_FALSE(latin.current.right_to_left);
    REQUIRE(latin.before.size() == 1);
    CHECK(latin.before[0].right_to_left);
}

TEST_CASE("lyric frame: before an anchor has come due the first lines are waiting, and without a document there are none",
          "[sink][lyrics]") {
    const lyr::Lyrics lyrics = parsed(kAuldLangSyne);
    const LyricFrame waiting = frame_at(lyrics, std::nullopt);
    CHECK(waiting.mode == lyr::StageMode::kWaiting);
    REQUIRE_FALSE(waiting.after.empty());
    CHECK(waiting.after[0].text == "Should auld acquaintance be forgot,");

    const lyr::Stage none = lyr::make_stage(nullptr, std::nullopt);
    const LyricFrame absent = iclforge::sink::make_lyric_frame(std::nullopt, nullptr, none, true);
    CHECK(absent.item == -1);
    CHECK(absent.mode == lyr::StageMode::kNoLyrics);
    CHECK(absent.streaming);
    CHECK(absent.title.empty());
}

TEST_CASE("lyric frame: the JSON a layer without C++ types is given escapes what it must and names the same fields",
          "[sink][lyrics]") {
    const lyr::Lyrics lyrics = parsed("[ti:A \"title\"]\n[00:00.00]He said \"hi\"\\ and\tleft\n[00:04.00]\n");
    const LyricFrame frame = frame_at(lyrics, 1000);
    const std::string json = iclforge::sink::lyric_frame_json(frame);
    CHECK(json.front() == '{');
    CHECK(json.back() == '}');
    CHECK(json.find("\"mode\":\"line\"") != std::string::npos);
    CHECK(json.find("\"title\":\"A \\\"title\\\"\"") != std::string::npos);
    CHECK(json.find("\"text\":\"He said \\\"hi\\\"\\\\ and\\tleft\"") != std::string::npos);
    CHECK(json.find("\"item\":7") != std::string::npos);
    CHECK(json.find("\"streaming\":true") != std::string::npos);
    CHECK(json.find("\"word\":null") != std::string::npos);

    const lyr::Stage none = lyr::make_stage(nullptr, std::nullopt);
    const std::string empty = iclforge::sink::lyric_frame_json(iclforge::sink::make_lyric_frame(std::nullopt, nullptr, none, false));
    CHECK(empty.find("\"mode\":\"none\"") != std::string::npos);
    CHECK(empty.find("\"item\":-1") != std::string::npos);
}
