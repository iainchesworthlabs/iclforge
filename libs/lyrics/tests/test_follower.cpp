#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

#include "iclforge/lyrics/follower.hpp"

// libs/lyrics/include/iclforge/lyrics/follower.hpp: the document a sink holds and the playhead the
// server's anchors move (planning/karaoke-phase2.md, W7d). The lines and words are the reader's and
// the cursor's, tested in test_lyrics.cpp; what is held here is which item an anchor is for, what a
// new document does to the old one, and what a sink would report.

namespace {

using iclforge::lyrics::Follower;

constexpr std::int64_t kSecond = 1'000'000;

const std::string kSong =
    "[00:01.00]Should auld acquaintance\n"
    "[00:03.00]And never brought to mind\n"
    "[00:06.00]<00:06.00>For <00:06.50>auld <00:07.00>lang <00:07.50>syne\n";

}  // namespace

TEST_CASE("follower: a document and its anchors give the line, as the sink's own clock has the sound",
          "[lyrics][follower]") {
    Follower follower;
    CHECK_FALSE(follower.has_lyrics());
    CHECK_FALSE(follower.line(0).has_value());

    REQUIRE(follower.set_document(7, kSong));
    CHECK(follower.has_lyrics());
    REQUIRE(follower.item().has_value());
    CHECK(*follower.item() == 7);
    // Held, with no anchor yet: before the first line.
    CHECK(follower.line(10 * kSecond) == -1);
    CHECK_FALSE(follower.position_ms(10 * kSecond).has_value());

    // The sound is 0 ms into the item at 10 s on the clock, playing.
    follower.anchor(7, 0, 10 * kSecond, 1.0);
    CHECK(follower.line(10 * kSecond) == -1);
    CHECK(follower.line(11 * kSecond) == 0);
    CHECK(follower.line(12 * kSecond + 500'000) == 0);
    CHECK(follower.line(13 * kSecond) == 1);
    CHECK(follower.position_ms(13 * kSecond) == 3000);

    // The word, in the word-timed line: 6.7 s into the item, which is "auld" (6.5 to 7.0 s).
    const auto at = follower.cursor(16 * kSecond + 700'000);
    REQUIRE(at.has_value());
    REQUIRE(at->line.has_value());
    CHECK(*at->line == 2);
    REQUIRE(at->word.has_value());
    CHECK(*at->word == 1);
}

TEST_CASE("follower: an anchor of another item is not this item's", "[lyrics][follower]") {
    Follower follower;
    REQUIRE(follower.set_document(7, kSong));
    follower.anchor(6, 0, 0, 1.0);
    CHECK_FALSE(follower.position_ms(5 * kSecond).has_value());
    follower.anchor(7, 0, 0, 1.0);
    CHECK(follower.position_ms(5 * kSecond) == 5000);

    // The next item's document takes the position away with the document: its own anchor is
    // waited for, and the one of the item that ended is ignored when it arrives late.
    REQUIRE(follower.set_document(8, "[00:02.00]Another song\n"));
    CHECK_FALSE(follower.position_ms(5 * kSecond).has_value());
    follower.anchor(7, 0, 0, 1.0);
    CHECK_FALSE(follower.position_ms(5 * kSecond).has_value());
    follower.anchor(8, 0, 0, 1.0);
    CHECK(follower.line(3 * kSecond) == 0);
}

TEST_CASE("follower: no lyrics, empty or refused, hold the item and no document", "[lyrics][follower]") {
    Follower follower;
    REQUIRE(follower.set_document(7, kSong));
    follower.anchor(7, 0, 0, 1.0);
    CHECK(follower.line(2 * kSecond) == 0);

    // An item with no lyrics: the server says so with an empty text.
    CHECK_FALSE(follower.set_document(8, ""));
    CHECK_FALSE(follower.has_lyrics());
    CHECK_FALSE(follower.line(2 * kSecond).has_value());
    REQUIRE(follower.item().has_value());
    CHECK(*follower.item() == 8);

    // Text with nothing in it that is lyrics, and a text past what the sink holds.
    CHECK_FALSE(follower.set_document(9, "   \n\n"));
    CHECK_FALSE(follower.has_lyrics());
    CHECK_FALSE(follower.set_document(10, kSong, kSong.size() - 1));
    CHECK_FALSE(follower.has_lyrics());
    CHECK(follower.set_document(11, kSong, kSong.size()));
    CHECK(follower.has_lyrics());
}

TEST_CASE("follower: a file with no times has a position and no line", "[lyrics][follower]") {
    Follower follower;
    REQUIRE(follower.set_document(7, "Should auld acquaintance\nAnd never brought to mind\n"));
    follower.anchor(7, 0, 0, 1.0);
    CHECK(follower.position_ms(5 * kSecond) == 5000);
    CHECK(follower.line(5 * kSecond) == -1);
}

TEST_CASE("follower: a seek clears the position and keeps the document, reset clears both",
          "[lyrics][follower]") {
    Follower follower;
    REQUIRE(follower.set_document(7, kSong));
    follower.anchor(7, 0, 0, 1.0);
    CHECK(follower.line(4 * kSecond) == 1);

    follower.clear_position();
    CHECK(follower.has_lyrics());
    CHECK_FALSE(follower.position_ms(4 * kSecond).has_value());
    CHECK(follower.line(4 * kSecond) == -1);
    // The stream starts again 1 s into the item, at 20 s on the clock.
    follower.anchor(7, 1000, 20 * kSecond, 1.0);
    CHECK(follower.line(20 * kSecond) == 0);

    follower.reset();
    CHECK_FALSE(follower.has_lyrics());
    CHECK_FALSE(follower.item().has_value());
    CHECK_FALSE(follower.line(20 * kSecond).has_value());
    // With no item held, an anchor is for none.
    follower.anchor(7, 0, 0, 1.0);
    CHECK_FALSE(follower.position_ms(1 * kSecond).has_value());
}
