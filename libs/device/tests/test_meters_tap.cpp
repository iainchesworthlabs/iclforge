#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "iclforge/meters_tap.hpp"

namespace mt = iclforge::meters;

namespace {

// A clock that moves 3 us each time it is read: a block reads it twice, and spends 3.
std::uint64_t g_now = 0;
std::uint64_t fake_clock() noexcept {
    g_now += 3;
    return g_now;
}

// The tap is 32 KB of ring: never on a stack.
std::unique_ptr<mt::MeterTap> make_tap(mt::MeterTap::Clock clock = nullptr) { return std::make_unique<mt::MeterTap>(clock); }

// Blocks of `outputs` outputs at a constant level.
struct Feed {
    std::vector<std::vector<float>> data;
    std::vector<std::span<float>> views;

    Feed(std::size_t outputs, const std::vector<float>& levels, std::size_t frames) {
        data.assign(outputs, std::vector<float>(frames, 0.0F));
        for (std::size_t o = 0; o < outputs; ++o) {
            std::fill(data[o].begin(), data[o].end(), levels[o]);
            views.emplace_back(data[o]);
        }
    }
    std::span<const std::span<float>> span() { return std::span<const std::span<float>>(views.data(), views.size()); }
};

}  // namespace

TEST_CASE("the mix is divided by half of log2 of the outputs, rounded up", "[meters-tap]") {
    REQUIRE(mt::mix_shift(0) == 0);
    REQUIRE(mt::mix_shift(1) == 0);
    REQUIRE(mt::mix_shift(2) == 1);
    REQUIRE(mt::mix_shift(3) == 1);
    REQUIRE(mt::mix_shift(4) == 1);
    REQUIRE(mt::mix_shift(5) == 2);
    REQUIRE(mt::mix_shift(6) == 2);
    REQUIRE(mt::mix_shift(12) == 2);
    REQUIRE(mt::mix_shift(16) == 2);
    REQUIRE(mt::mix_shift(17) == 3);
}

TEST_CASE("the ring gives back the newest samples in the order they were written, across its end", "[meters-tap]") {
    auto ring = std::make_unique<mt::MonoRing>();
    std::array<std::int16_t, 8> out{};
    REQUIRE_FALSE(ring->read_newest(out));  // nothing written
    std::vector<std::int16_t> block(200);
    std::int16_t next = 0;
    // Write more than twice the capacity, a block at a time, then read the newest 8,192.
    std::uint64_t total = 0;
    while (total < mt::MonoRing::kCapacity * 2 + 50) {
        for (std::int16_t& v : block) {
            v = next++;
        }
        ring->write(block);
        total += block.size();
    }
    REQUIRE(ring->written() == total);
    std::vector<std::int16_t> newest(mt::kTapHistory);
    REQUIRE(ring->read_newest(newest));
    for (std::size_t i = 0; i < newest.size(); ++i) {
        REQUIRE(newest[i] == static_cast<std::int16_t>((total - newest.size() + i) & 0xFFFF));
    }
}

TEST_CASE("a copy the writer has overrun is torn, and one it has not is not", "[meters-tap]") {
    constexpr std::uint64_t kStart = 1000;
    // A reader that took kStart..kStart+8191 and finds the head where it was, or a little on: whole.
    REQUIRE_FALSE(mt::MonoRing::torn(kStart, kStart + mt::kTapHistory));
    REQUIRE_FALSE(mt::MonoRing::torn(kStart, kStart + mt::kTapHistory + 4000));
    // The writer a whole capacity past the first sample it copied (less the block it may be in the middle of): overrun.
    REQUIRE_FALSE(mt::MonoRing::torn(kStart, kStart + mt::MonoRing::kCapacity - mt::kTapBlockFrames));
    REQUIRE(mt::MonoRing::torn(kStart, kStart + mt::MonoRing::kCapacity - mt::kTapBlockFrames + 1));
    REQUIRE(mt::MonoRing::torn(kStart, kStart + mt::MonoRing::kCapacity * 2));
    // The ring is big enough for the copy to be that slow: 8,192 samples of 16,384 is 171 ms of the writer's progress.
    REQUIRE(mt::MonoRing::kCapacity - mt::kTapBlockFrames - mt::kTapHistory > 7000);
}

TEST_CASE("the tap writes the mix of the live outputs, scaled and rounded", "[meters-tap]") {
    auto tap = make_tap();
    // Two outputs at +0.5 and +0.25: the sum 0.75, halved (two outputs), full scale 32767: 12,287.6 -> 12,288.
    {
        Feed feed(2, {0.5F, 0.25F}, 256);
        tap->block(feed.span(), 256, 0b11);
    }
    // Fill the history: 8,192 samples is 32 blocks.
    for (int i = 0; i < 31; ++i) {
        Feed feed(2, {0.5F, 0.25F}, 256);
        tap->block(feed.span(), 256, 0b11);
    }
    std::array<std::int16_t, mt::kTapHistory> history{};
    REQUIRE(tap->read_history(history));
    for (const std::int16_t v : history) {
        REQUIRE(v == 12288);
    }
}

TEST_CASE("an output that is not live is not in the mix, and six outputs are divided by four", "[meters-tap]") {
    auto tap = make_tap();
    for (int i = 0; i < 32; ++i) {
        // Six outputs: one at 1.0, one at -0.5 that is not live (silent in fact, but the block holds the value to prove it is not read).
        Feed feed(6, {1.0F, -0.5F, 0.0F, 0.0F, 0.0F, 0.0F}, 256);
        tap->block(feed.span(), 256, 0b111101);
    }
    std::array<std::int16_t, mt::kTapHistory> history{};
    REQUIRE(tap->read_history(history));
    // 1.0 divided by 4 at 32767: 8,191.75 -> 8,192.
    REQUIRE(history.front() == 8192);
    REQUIRE(history.back() == 8192);
}

TEST_CASE("a sum past the range clips to it, and a negative one rounds away from zero", "[meters-tap]") {
    auto tap = make_tap();
    // One output (no division) at +2 and at -2: clips.
    for (int i = 0; i < 16; ++i) {
        Feed up(1, {2.0F}, 256);
        tap->block(up.span(), 256, 1);
    }
    for (int i = 0; i < 16; ++i) {
        Feed down(1, {-2.0F}, 256);
        tap->block(down.span(), 256, 1);
    }
    std::array<std::int16_t, mt::kTapHistory> history{};
    REQUIRE(tap->read_history(history));
    REQUIRE(history.front() == 32767);
    REQUIRE(history.back() == -32768);
}

TEST_CASE("each output's peak is published every 50 ms of audio, in dB below full scale", "[meters-tap]") {
    auto tap = make_tap();
    REQUIRE(tap->fast_levels().serial == 0);
    // 2,400 frames close a window; blocks of 256 get there on the tenth (2,560).
    for (int i = 0; i < 9; ++i) {
        Feed feed(3, {0.5F, 0.1F, 0.0F}, 256);
        tap->block(feed.span(), 256, 0b011);
    }
    REQUIRE(tap->fast_levels().serial == 0);
    Feed feed(3, {0.5F, 0.1F, 0.0F}, 256);
    tap->block(feed.span(), 256, 0b011);
    const mt::FastLevels levels = tap->fast_levels();
    REQUIRE(levels.serial == 1);
    REQUIRE(levels.outputs == 3);
    REQUIRE(std::fabs(levels.peak_db[0] - (-6.0206F)) < 1e-3F);
    REQUIRE(std::fabs(levels.peak_db[1] - (-20.0F)) < 1e-3F);
    REQUIRE(levels.peak_db[2] == mt::kTapSilentDb);
    // The next window starts from nothing: a quieter stream is read as quieter.
    for (int i = 0; i < 10; ++i) {
        Feed quiet(3, {0.05F, 0.0F, 0.0F}, 256);
        tap->block(quiet.span(), 256, 0b001);
    }
    const mt::FastLevels next = tap->fast_levels();
    REQUIRE(next.serial == 2);
    REQUIRE(std::fabs(next.peak_db[0] - (-26.0206F)) < 1e-3F);
    REQUIRE(next.peak_db[1] == mt::kTapSilentDb);
}

TEST_CASE("the tap counts and times what it does", "[meters-tap]") {
    g_now = 0;
    auto tap = make_tap(&fake_clock);
    for (int i = 0; i < 12; ++i) {
        Feed feed(2, {0.1F, 0.1F}, 256);
        tap->block(feed.span(), 256, 0b11);
    }
    mt::TapStats stats = tap->stats();
    REQUIRE(stats.blocks == 12);
    REQUIRE(stats.frames == 12 * 256);
    REQUIRE(stats.windows == 1);
    // Two reads of the clock a block, 3 us apart: 3 us a block.
    REQUIRE(stats.us_total == 12 * 3);
    REQUIRE(stats.us_max_block == 3);
    // A burst of 1,536 frames is six blocks of 256: 18 us on that clock.
    REQUIRE(std::fabs(stats.us_per_burst() - 18.0) < 1e-9);
    // Reads: too early (a short read), then enough, counted.
    std::array<std::int16_t, mt::kTapHistory> history{};
    REQUIRE_FALSE(tap->read_history(history));
    for (int i = 0; i < 20; ++i) {
        Feed feed(2, {0.1F, 0.1F}, 256);
        tap->block(feed.span(), 256, 0b11);
    }
    REQUIRE(tap->read_history(history));
    stats = tap->stats();
    REQUIRE(stats.reads == 2);
    REQUIRE(stats.short_reads == 1);
    REQUIRE(stats.torn == 0);
}

TEST_CASE("the tap is asked for only while the screen wants it", "[meters-tap]") {
    auto tap = make_tap();
    REQUIRE_FALSE(tap->wants_blocks());
    tap->set_wanted(true);
    REQUIRE(tap->wants_blocks());
    REQUIRE(tap->wanted());
    tap->set_wanted(false);
    REQUIRE_FALSE(tap->wants_blocks());
}
