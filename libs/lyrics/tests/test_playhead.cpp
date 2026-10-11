#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>

#include "iclforge/lyrics/playhead.hpp"

// libs/lyrics/include/iclforge/lyrics/playhead.hpp: where in an item the sound is, from the anchors a
// server sends (planning/karaoke-phase2.md, W7d). The sums are exact (a speed of 1 or 0), so each
// expectation is the arithmetic of the format and not what the code happens to give.

namespace {

using iclforge::lyrics::Playhead;

constexpr std::int64_t kSecond = 1'000'000;

}  // namespace

TEST_CASE("playhead: nothing before an anchor has come due", "[lyrics][playhead]") {
    Playhead head;
    CHECK(head.empty());
    CHECK_FALSE(head.position_ms(0).has_value());
    CHECK_FALSE(head.position_ms(5 * kSecond).has_value());

    head.anchor(1000, 5 * kSecond, 1.0);
    CHECK_FALSE(head.empty());
    // Before its time the anchor says nothing; at its time it is where it said.
    CHECK_FALSE(head.position_ms(5 * kSecond - 1).has_value());
    CHECK(head.position_ms(5 * kSecond) == 1000);
}

TEST_CASE("playhead: playing moves with the clock and held does not", "[lyrics][playhead]") {
    Playhead head;
    head.anchor(1000, 5 * kSecond, 1.0);
    CHECK(head.position_ms(5 * kSecond + 500'000) == 1500);
    CHECK(head.position_ms(8 * kSecond) == 4000);

    Playhead held;
    held.anchor(2000, 1 * kSecond, 0.0);
    CHECK(held.position_ms(1 * kSecond) == 2000);
    CHECK(held.position_ms(90 * kSecond) == 2000);

    // A time is rounded to the millisecond, not cut.
    Playhead rounded;
    rounded.anchor(0, 0, 1.0);
    CHECK(rounded.position_ms(1'499) == 1);
    CHECK(rounded.position_ms(1'500) == 2);
    CHECK(rounded.position_ms(2'400) == 2);
}

TEST_CASE("playhead: a hold sent for the end of what is buffered applies when the clock gets there",
          "[lyrics][playhead]") {
    Playhead head;
    head.anchor(0, 0, 1.0);
    // The server has sent audio to 4 s and then paused: a hold at 4 s, time 4 s.
    head.anchor(4000, 4 * kSecond, 0.0);
    CHECK(head.position_ms(3 * kSecond) == 3000);
    CHECK(head.position_ms(4 * kSecond - 1'000) == 3999);
    CHECK(head.position_ms(4 * kSecond) == 4000);
    CHECK(head.position_ms(10 * kSecond) == 4000);

    // And resumed at 10 s from where it stood.
    head.anchor(4000, 10 * kSecond, 1.0);
    CHECK(head.position_ms(9 * kSecond) == 4000);
    CHECK(head.position_ms(12 * kSecond) == 6000);
}

TEST_CASE("playhead: a seek clears and the new anchor starts the words when the sound does",
          "[lyrics][playhead]") {
    Playhead head;
    head.anchor(0, 0, 1.0);
    CHECK(head.position_ms(2 * kSecond) == 2000);

    head.clear();
    CHECK(head.empty());
    CHECK_FALSE(head.position_ms(2 * kSecond).has_value());
    // The new stream's first chunk plays at 3 s from the clock, at 60 s into the item.
    head.anchor(60'000, 3 * kSecond, 1.0);
    CHECK_FALSE(head.position_ms(2 * kSecond).has_value());
    CHECK(head.position_ms(3 * kSecond) == 60'000);
    CHECK(head.position_ms(4 * kSecond) == 61'000);
}

TEST_CASE("playhead: anchors in any order, and one at an anchor's time replaces it", "[lyrics][playhead]") {
    Playhead head;
    head.anchor(8000, 8 * kSecond, 1.0);
    head.anchor(0, 0, 1.0);
    head.anchor(4000, 4 * kSecond, 0.0);
    CHECK(head.position_ms(2 * kSecond) == 2000);
    CHECK(head.position_ms(6 * kSecond) == 4000);
    CHECK(head.position_ms(9 * kSecond) == 9000);

    // A correction at the same time takes the place of the one there.
    head.anchor(4500, 4 * kSecond, 0.0);
    CHECK(head.position_ms(6 * kSecond) == 4500);
}

TEST_CASE("playhead: a negative speed is held and the position is never below 0", "[lyrics][playhead]") {
    Playhead head;
    head.anchor(1000, 0, -1.0);
    CHECK(head.position_ms(5 * kSecond) == 1000);
}

TEST_CASE("playhead: past its room the oldest anchor goes and the newest still stands", "[lyrics][playhead]") {
    Playhead head;
    // One a second for twenty seconds, the item moving at 1: more than the playhead holds.
    for (std::int64_t second = 0; second < 20; ++second) {
        head.anchor(second * 1000, second * kSecond, 1.0);
    }
    CHECK(head.position_ms(19 * kSecond + 250'000) == 19'250);
    // The ones kept are the latest sixteen, so a time in the oldest of them still answers.
    CHECK(head.position_ms(4 * kSecond) == 4000);
    // One older than everything kept, sent late, goes in at the front and pushes the oldest out.
    head.anchor(500, 500'000, 1.0);
    CHECK(head.position_ms(19 * kSecond) == 19'000);
}
