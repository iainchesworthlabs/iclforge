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
