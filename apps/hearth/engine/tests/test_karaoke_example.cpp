#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "iclforge/lyrics/lyrics.hpp"
#include "item_loader.hpp"
#include "media_info.hpp"

// The karaoke example read by the reader of libs/lyrics through Hearth's own file loader: the part
// of the old test_lyrics.cpp that needs the application (item_loader.hpp, media_info.hpp), so it
// stays with Hearth when the reader moved out of its engine.

namespace {

namespace fs = std::filesystem;

}  // namespace

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

    const auto found = iclforge::lyrics::find_lyrics_file(stream.string());
    REQUIRE(found.has_value());
    CHECK(found->filename().string() == "auld-lang-syne.lrc");
    const auto lyrics = iclforge::lyrics::read_lyrics(*found, duration_ms);
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
    const auto lines_only = iclforge::lyrics::read_lyrics(
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

// The duet example's words (examples/karaoke/leave-her-johnny/, made by
// tools/generators/gen_karaoke_duet_example.py), read the same way. The format has no marker for
// which singer has a line, so what is held is what any lyric file is: found by the stream's name,
// timed to the syllable, every word inside the stream, four lines. Which singer has which is the
// example's README, and that the singers do alternate is tools/checks/test_gen_karaoke_duet_example.py.
TEST_CASE("lyrics: the duet example's words are read and every word is inside its stream",
          "[hearth][lyrics][example]") {
    const fs::path stream = fs::path{ICLFORGE_KARAOKE_DUET_EXAMPLE_DIR} / "leave-her-johnny.ac3";
    const auto loaded = iclforge::hearth::ui::make_file_item_loader()(stream.string());
    REQUIRE(loaded.has_value());
    const auto info = iclforge::hearth::describe_media(stream.string(), *loaded);
    REQUIRE(info.sample_rate == 48000);
    const auto duration_ms =
        static_cast<std::int64_t>(info.played_samples() * 1000 / info.sample_rate);
    REQUIRE(duration_ms == 19200);

    const auto found = iclforge::lyrics::find_lyrics_file(stream.string());
    REQUIRE(found.has_value());
    CHECK(found->filename().string() == "leave-her-johnny.lrc");
    const auto lyrics = iclforge::lyrics::read_lyrics(*found, duration_ms);
    REQUIRE(lyrics.has_value());
    CHECK(lyrics->title == "Leave Her, Johnny");
    CHECK(lyrics->timed);

    std::size_t sung = 0;
    std::size_t syllables = 0;
    std::int64_t previous = -1;
    for (const auto& line : lyrics->lines) {
        CHECK(line.start_ms >= previous);
        previous = line.start_ms;
        if (line.text.empty()) {
            // The time that ends the last line. Here it is the stream's own length, to the
            // sample, and a line that starts at the end is given the reader's default length
            // (kLastLineMs) past it: nothing is sung in it, so it is not held to the stream.
            CHECK(line.start_ms == duration_ms);
            continue;
        }
        ++sung;
        CHECK(line.end_ms <= duration_ms);
        CHECK(line.word_timed());
        for (const auto& word : line.words) {
            CHECK(word.start_ms >= line.start_ms);
            CHECK(word.end_ms >= word.start_ms);
            CHECK(word.end_ms <= duration_ms);
            ++syllables;
        }
    }
    CHECK(sung == 4);
    CHECK(syllables == 30);
    CHECK(lyrics->lines.front().words.front().start_ms == 0);
    // The crew answers on the second line, which starts 5.4 s in: the verse's pick-up and
    // two bars, 18 eighths of 0.3 s. The last sung line ends with the music and the stream.
    CHECK(lyrics->lines[1].start_ms == 5400);
    CHECK(lyrics->lines[3].end_ms == duration_ms);
}
