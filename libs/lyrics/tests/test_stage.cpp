#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "iclforge/lyrics/follower.hpp"
#include "iclforge/lyrics/lyrics.hpp"
#include "iclforge/lyrics/stage.hpp"

// libs/lyrics/include/iclforge/lyrics/stage.hpp: what a screen shows of a song at one position
// (planning/karaoke-p4x-panel.md, D2). Each expectation is the arithmetic of the format worked out
// from the file below, not what the code happens to answer.

namespace {

using iclforge::lyrics::Lyrics;
using iclforge::lyrics::Singer;
using iclforge::lyrics::Stage;
using iclforge::lyrics::StageMode;
using iclforge::lyrics::StageOptions;

// Line 0  10.0 s to 12.0 s, three words at 10.0 / 10.5 / 11.0 (the last runs to the line's end)
// Line 1  12.0 s to 14.0 s, two words at 12.0 / 12.5
// Line 2  14.0 s to 30.0 s, a line that only marks a time: sixteen seconds of nothing being sung
// Line 3  30.0 s to 33.0 s, four words at 30.0 / 30.5 / 31.0 / 31.5
// Line 4  33.0 s to 36.0 s, timed as a line only
// Line 5  36.0 s to 41.0 s (five seconds after it starts, there being no length)
constexpr const char* kSong =
    "[ti:Stage test]\n"
    "[00:10.00]<00:10.00>Should <00:10.50>auld <00:11.00>acquaintance\n"
    "[00:12.00]<00:12.00>be <00:12.50>forgot\n"
    "[00:14.00]\n"
    "[00:30.00]<00:30.00>For <00:30.50>auld <00:31.00>lang <00:31.50>syne\n"
    "[00:33.00]Line timed only\n"
    "[00:36.00]Last line\n";

Lyrics song() {
    const auto parsed = iclforge::lyrics::parse_lyrics(kSong);
    REQUIRE(parsed.has_value());
    return *parsed;
}

std::vector<std::size_t> before(const Stage& stage) {
    return {stage.before.begin(), stage.before.begin() + static_cast<std::ptrdiff_t>(stage.before_count)};
}
std::vector<std::size_t> after(const Stage& stage) {
    return {stage.after.begin(), stage.after.begin() + static_cast<std::ptrdiff_t>(stage.after_count)};
}

}  // namespace

TEST_CASE("stage: no document and a document with no times", "[lyrics][stage]") {
    CHECK(iclforge::lyrics::make_stage(nullptr, 1000).mode == StageMode::kNoLyrics);

    const auto plain = iclforge::lyrics::parse_lyrics("Should auld acquaintance\nbe forgot\n");
    REQUIRE(plain.has_value());
    REQUIRE_FALSE(plain->timed);
    // Plain text is shown whole and is not followed, whatever the position.
    CHECK(iclforge::lyrics::make_stage(&*plain, 5000).mode == StageMode::kPlain);
    CHECK(iclforge::lyrics::make_stage(&*plain, std::nullopt).mode == StageMode::kPlain);
}

TEST_CASE("stage: timed lyrics with no position yet show the first lines", "[lyrics][stage]") {
    const Lyrics lyrics = song();
    const Stage stage = iclforge::lyrics::make_stage(&lyrics, std::nullopt);
    CHECK(stage.mode == StageMode::kWaiting);
    CHECK(before(stage).empty());
    // Two lines after, the marker line (2) is not a row.
    CHECK(after(stage) == std::vector<std::size_t>{0, 1});

    StageOptions three;
    three.lines_after = 3;
    CHECK(after(iclforge::lyrics::make_stage(&lyrics, std::nullopt, 0, three)) ==
          std::vector<std::size_t>{0, 1, 3});
}

TEST_CASE("stage: before the first line is a gap with the count-in", "[lyrics][stage]") {
    const Lyrics lyrics = song();

    Stage stage = iclforge::lyrics::make_stage(&lyrics, 5000);
    REQUIRE(stage.mode == StageMode::kGap);
    CHECK(stage.next_line_in_ms == 5000);
    // From the start of the item to the first line.
    CHECK(stage.gap_ms == 10000);
    CHECK(stage.instrumental);
    // Five seconds to go: more than the four dots, so all four are lit.
    CHECK(stage.count_in == 4);
    CHECK(before(stage).empty());
    CHECK(after(stage) == std::vector<std::size_t>{0, 1});

    // 1.5 s to go: ceil(1.5) = two dots still lit; the last second shows one.
    stage = iclforge::lyrics::make_stage(&lyrics, 8500);
    CHECK(stage.next_line_in_ms == 1500);
    CHECK(stage.count_in == 2);
    stage = iclforge::lyrics::make_stage(&lyrics, 9500);
    CHECK(stage.next_line_in_ms == 500);
    CHECK(stage.count_in == 1);
}

TEST_CASE("stage: a word-timed line names the word, the fill and the words round it", "[lyrics][stage]") {
    const Lyrics lyrics = song();

    // 10.75 s: the second word (10.5 to 11.0), half way through it.
    Stage stage = iclforge::lyrics::make_stage(&lyrics, 10750);
    REQUIRE(stage.mode == StageMode::kLine);
    CHECK(stage.line == 0);
    CHECK(stage.word_timed);
    CHECK(stage.word == 1);
    CHECK(stage.words_sung == 1);
    CHECK(stage.fraction == Catch::Approx(0.5));
    CHECK(before(stage).empty());
    // Line 2 only marks a time, so the next text after line 1 is line 3.
    CHECK(after(stage) == std::vector<std::size_t>{1, 3});
    CHECK(stage.position_ms == 10750);

    // The last word runs to the line's end: 11.5 s is the middle of 11.0 to 12.0.
    stage = iclforge::lyrics::make_stage(&lyrics, 11500);
    CHECK(stage.word == 2);
    CHECK(stage.words_sung == 2);
    CHECK(stage.fraction == Catch::Approx(0.5));

    // 31.2 s in line 3: the third word (31.0 to 31.5), 0.4 of the way, two sung, and a row before.
    stage = iclforge::lyrics::make_stage(&lyrics, 31200);
    REQUIRE(stage.mode == StageMode::kLine);
    CHECK(stage.line == 3);
    CHECK(stage.word == 2);
    CHECK(stage.words_sung == 2);
    CHECK(stage.fraction == Catch::Approx(0.4));
    // The marker line is skipped going back as well: line 1 is the line before line 3.
    CHECK(before(stage) == std::vector<std::size_t>{1});
    CHECK(after(stage) == std::vector<std::size_t>{4, 5});
}

TEST_CASE("stage: a line timed as a line names no word", "[lyrics][stage]") {
    const Lyrics lyrics = song();
    const Stage stage = iclforge::lyrics::make_stage(&lyrics, 34000);
    REQUIRE(stage.mode == StageMode::kLine);
    CHECK(stage.line == 4);
    CHECK_FALSE(stage.word_timed);
    CHECK_FALSE(stage.word.has_value());
    CHECK(stage.words_sung == 0);
    CHECK(stage.fraction == 0.0);
    CHECK(before(stage) == std::vector<std::size_t>{3});
    CHECK(after(stage) == std::vector<std::size_t>{5});
}

TEST_CASE("stage: the marker line is an instrumental gap", "[lyrics][stage]") {
    const Lyrics lyrics = song();
    // 14.5 s: line 2 has begun, it has no text, and line 3 is 15.5 s away.
    const Stage stage = iclforge::lyrics::make_stage(&lyrics, 14500);
    REQUIRE(stage.mode == StageMode::kGap);
    CHECK(stage.line == 2);
    CHECK(stage.next_line_in_ms == 15500);
    // From the marker's own start (14.0 s) to 30.0 s.
    CHECK(stage.gap_ms == 16000);
    CHECK(stage.instrumental);
    CHECK(stage.count_in == 4);
    // The line before it is what has just been sung.
    CHECK(before(stage) == std::vector<std::size_t>{1});
    CHECK(after(stage) == std::vector<std::size_t>{3, 4});

    // Two seconds out the count-in is two dots.
    CHECK(iclforge::lyrics::make_stage(&lyrics, 28000).count_in == 2);
}

TEST_CASE("stage: after the last line nothing follows", "[lyrics][stage]") {
    const Lyrics lyrics = song();
    // Line 5 ends at 41.0 s.
    Stage stage = iclforge::lyrics::make_stage(&lyrics, 37000);
    REQUIRE(stage.mode == StageMode::kLine);
    CHECK(stage.line == 5);
    CHECK(after(stage).empty());

    stage = iclforge::lyrics::make_stage(&lyrics, 45000);
    REQUIRE(stage.mode == StageMode::kGap);
    CHECK_FALSE(stage.next_line_in_ms.has_value());
    CHECK_FALSE(stage.gap_ms.has_value());
    CHECK_FALSE(stage.instrumental);
    CHECK(stage.count_in == 0);
    // The line that ended is the row before.
    CHECK(before(stage) == std::vector<std::size_t>{5});
    CHECK(after(stage).empty());
}

TEST_CASE("stage: the delay moves the lyrics later and the position with it", "[lyrics][stage]") {
    const Lyrics lyrics = song();
    // 10.75 s with 500 ms of delay is 10.25 s in the song: the first word, half way.
    const Stage stage = iclforge::lyrics::make_stage(&lyrics, 10750, 500);
    REQUIRE(stage.mode == StageMode::kLine);
    CHECK(stage.position_ms == 10250);
    CHECK(stage.word == 0);
    CHECK(stage.words_sung == 0);
    CHECK(stage.fraction == Catch::Approx(0.5));

    // A negative delay brings them sooner: 9.8 s with -300 ms is 10.1 s.
    const Stage sooner = iclforge::lyrics::make_stage(&lyrics, 9800, -300);
    REQUIRE(sooner.mode == StageMode::kLine);
    CHECK(sooner.word == 0);
}

TEST_CASE("stage: a short gap between lines is not an instrumental break", "[lyrics][stage]") {
    // A subtitle file's cues leave breaths between them: nothing is being sung, and no count-in is
    // drawn for a second.
    const auto cues = iclforge::lyrics::parse_subrip(
        "1\n00:00:10,000 --> 00:00:12,000\nFirst\n\n2\n00:00:12,600 --> 00:00:14,000\nSecond\n");
    REQUIRE(cues.has_value());
    const Stage stage = iclforge::lyrics::make_stage(&*cues, 12300);
    REQUIRE(stage.mode == StageMode::kGap);
    CHECK(stage.line == 0);
    CHECK(stage.next_line_in_ms == 300);
    // From the cue's end (12.0 s) to the next cue's start (12.6 s).
    CHECK(stage.gap_ms == 600);
    CHECK_FALSE(stage.instrumental);
    CHECK(stage.count_in == 0);
    CHECK(before(stage) == std::vector<std::size_t>{0});
    CHECK(after(stage) == std::vector<std::size_t>{1});
}

TEST_CASE("stage: a duet's line carries its singer", "[lyrics][stage]") {
    const auto duet = iclforge::lyrics::parse_lyrics(
        "[00:10.00]V1: Leave her, Johnny\n[00:14.00]V2: Leave her\n[00:18.00]V1+V2: Leave her, Johnny\n");
    REQUIRE(duet.has_value());
    CHECK(iclforge::lyrics::make_stage(&*duet, 11000).singer == Singer::kFirst);
    CHECK(iclforge::lyrics::make_stage(&*duet, 15000).singer == Singer::kSecond);
    CHECK(iclforge::lyrics::make_stage(&*duet, 19000).singer == Singer::kBoth);
    // Nothing is sung before the first line, so there is nobody's line.
    CHECK(iclforge::lyrics::make_stage(&*duet, 1000).singer == Singer::kUnmarked);
}

TEST_CASE("stage: the context asked for is the context given, and no more than the array holds",
          "[lyrics][stage]") {
    const Lyrics lyrics = song();
    StageOptions none;
    none.lines_before = 0;
    none.lines_after = 0;
    const Stage bare = iclforge::lyrics::make_stage(&lyrics, 31200, 0, none);
    CHECK(bare.before_count == 0);
    CHECK(bare.after_count == 0);

    StageOptions many;
    many.lines_before = 100;
    many.lines_after = 100;
    const Stage wide = iclforge::lyrics::make_stage(&lyrics, 34000, 0, many);
    // Line 4 has the text lines 3, 1 and 0 before it and one, line 5, after.
    CHECK(before(wide) == std::vector<std::size_t>{0, 1, 3});
    CHECK(after(wide) == std::vector<std::size_t>{5});
}

TEST_CASE("stage: the follower answers for the sound's own clock", "[lyrics][stage][follower]") {
    iclforge::lyrics::Follower follower;
    constexpr std::int64_t kSecond = 1'000'000;
    // Nothing held yet.
    CHECK(follower.stage(0).mode == StageMode::kNoLyrics);

    REQUIRE(follower.set_document(7, kSong));
    // Held, with no anchor: waiting, with the first lines to show.
    CHECK(follower.stage(0).mode == StageMode::kWaiting);

    // The sound is at 10.0 s at server time 100 s, playing at speed 1.
    follower.anchor(7, 10'000, 100 * kSecond, 1.0);
    const Stage stage = follower.stage(100 * kSecond + 750'000);
    REQUIRE(stage.mode == StageMode::kLine);
    CHECK(stage.word == 1);
    CHECK(stage.fraction == Catch::Approx(0.5));

    // A delay is the screen's own: the same answer as the free function with the same delay.
    CHECK(follower.stage(100 * kSecond + 750'000, 500).word == 0);
}

TEST_CASE("stage: a line's direction is its first strong character's", "[lyrics][stage][direction]") {
    // Hebrew "shalom", Yiddish "a gutn tog" and Arabic "marhaba", as UTF-8 bytes, so that nothing
    // depends on the compiler's source encoding. Each escape is followed by a backslash or a quote,
    // never by a hex digit, which a hex escape would swallow.
    const std::string shalom = "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D";
    const std::string gutn_tog =
        "\xD7\x90\xD6\xB7 \xD7\x92\xD7\x95\xD7\x98\xD7\x9F \xD7\x98\xD7\x90\xD6\xB8\xD7\x92";
    const std::string marhaba = "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7";
    const std::string note = "\xE2\x99\xAA";
    const std::string privet = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";

    using iclforge::lyrics::text_is_right_to_left;
    CHECK(text_is_right_to_left(shalom));
    CHECK(text_is_right_to_left(gutn_tog));
    CHECK(text_is_right_to_left(marhaba));
    CHECK_FALSE(text_is_right_to_left("Should auld acquaintance"));
    CHECK_FALSE(text_is_right_to_left(""));
    // The first STRONG character decides: digits, spaces, punctuation and a music note are skipped...
    CHECK(text_is_right_to_left("  12, " + note + " " + shalom));
    CHECK(text_is_right_to_left("(" + shalom + ") and Latin"));
    // ...and a Latin word first makes the line left to right whatever follows it.
    CHECK_FALSE(text_is_right_to_left("Hello " + shalom));
    // Cyrillic is left to right; bytes that are not UTF-8 are skipped, not strong.
    CHECK_FALSE(text_is_right_to_left(privet));
    CHECK(text_is_right_to_left(std::string("\xFF\xFE") + shalom));
    // A truncated sequence at the end is not read past the end.
    CHECK_FALSE(text_is_right_to_left("abc\xD7"));
    CHECK_FALSE(text_is_right_to_left("\xD7"));
}
