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
    // Every sixteenth block is timed in parts: none of these twelve.
    REQUIRE(stats.sampled_blocks == 0);
    // A burst of 1,536 frames is six blocks of 256: 18 us on that clock.
    REQUIRE(std::fabs(stats.us_per_burst() - 18.0) < 1e-9);
    // A view that opens reads its own longest block, not the board's.
    tap->reset_max();
    REQUIRE(tap->stats().us_max_block == 0);
    REQUIRE(tap->stats().blocks == 12);
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

TEST_CASE("every sixteenth block is timed in its parts, and the parts add up to what a block took, less the bookkeeping", "[meters-tap]") {
    g_now = 0;
    auto tap = make_tap(&fake_clock);
    for (int i = 0; i < 64; ++i) {
        Feed feed(2, {0.1F, 0.1F}, 256);
        tap->block(feed.span(), 256, 0b11);
    }
    const mt::TapStats stats = tap->stats();
    REQUIRE(stats.blocks == 64);
    REQUIRE(stats.sampled_blocks == 4);
    // On a clock that moves 3 us a read: the clock's own cost is one read (3), the pass one (3 more than the read after it began), the pack one.
    CHECK(stats.us_clock == 4 * 3);
    CHECK(stats.us_pass == 4 * 3);
    CHECK(stats.us_pack == 4 * 3);
    CHECK(std::fabs(stats.us_per_clock_read() - 3.0) < 1e-9);
    CHECK(std::fabs(stats.part_per_burst(stats.us_pass) - 18.0) < 1e-9);  // six blocks of 3 us
    mt::TapStats none;
    CHECK(none.part_per_burst(5) == 0.0);
    CHECK(none.us_per_clock_read() == 0.0);
}

namespace {

// The first version's loops, kept as what the tap is held to: an output at a time, a mix cleared first, the sum clamped and rounded away from zero by a
// branch, the peaks turned to dB when a window closes. The tap now takes four outputs at a time and converts into the ring in place; the numbers must not move.
struct FirstVersion {
    std::array<float, mt::kTapMaxOutputs> peak{};
    std::size_t window_frames = 0;
    std::vector<std::int16_t> mix16;
    std::array<float, mt::kTapMaxOutputs> published{};
    std::size_t published_outputs = 0;
    std::uint32_t serial = 0;

    void block(std::span<const std::span<float>> outputs, std::size_t frames, std::uint32_t live) {
        frames = std::min(frames, mt::kTapBlockFrames);
        std::array<float, mt::kTapBlockFrames> mix{};
        const std::size_t count = std::min(outputs.size(), mt::kTapMaxOutputs);
        for (std::size_t o = 0; o < count; ++o) {
            if (((live >> o) & 1U) == 0) {
                continue;
            }
            float p = peak[o];
            for (std::size_t k = 0; k < frames; ++k) {
                const float x = outputs[o][k];
                mix[k] += x;
                p = std::max(p, std::fabs(x));
            }
            peak[o] = p;
        }
        const float scale = 32767.0F / static_cast<float>(1U << mt::mix_shift(count));
        for (std::size_t k = 0; k < frames; ++k) {
            const float v = std::clamp(mix[k] * scale, -32768.0F, 32767.0F);
            mix16.push_back(static_cast<std::int16_t>(v >= 0.0F ? v + 0.5F : v - 0.5F));
        }
        window_frames += frames;
        if (window_frames >= mt::kFastWindowFrames) {
            for (std::size_t o = 0; o < count; ++o) {
                published[o] = peak[o] <= 1e-6F ? mt::kTapSilentDb : 20.0F * std::log10(peak[o]);
                peak[o] = 0.0F;
            }
            published_outputs = count;
            ++serial;
            window_frames = 0;
        }
    }
};

// A repeatable run of numbers (xorshift), so that a failure is the same failure on every machine.
struct Noise {
    std::uint32_t state = 0x9E3779B9U;
    std::uint32_t next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
    // Uniform in [-1.2, 1.2), with a sample in eight on a grid of eighths (so that sums of two land on halves of a step) and one in sixty-four a hot one that clips.
    float sample() {
        const std::uint32_t r = next();
        if ((r & 63U) == 0) {
            return ((r >> 8) & 1U) != 0 ? 3.0F : -3.0F;
        }
        if ((r & 7U) == 0) {
            return static_cast<float>(static_cast<int>((r >> 8) % 17U) - 8) / 8.0F;
        }
        return (static_cast<float>(r >> 8) / 8388608.0F - 1.0F) * 1.2F;
    }
};

}  // namespace

TEST_CASE("the tap makes the numbers the first version made, for any count of outputs, any outputs live, and a block of any length", "[meters-tap]") {
    for (const std::size_t outputs : {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 12U, 16U}) {
        const std::uint32_t all = outputs == 32 ? 0xFFFFFFFFU : ((1U << outputs) - 1U);
        for (const std::uint32_t live : {all, all & 0x5555U, all & 0x2U, std::uint32_t{0}, all & ~1U}) {
            for (const std::size_t frames : {std::size_t{256}, std::size_t{100}, std::size_t{1}}) {
                CAPTURE(outputs, live, frames);
                auto tap = make_tap();
                FirstVersion first;
                Noise noise;
                noise.state ^= static_cast<std::uint32_t>(outputs * 977U + live * 31U + frames);
                // Enough blocks to fill the history and close windows (a block of one frame needs 8,192 of them).
                const std::size_t blocks = frames == 256 ? 40 : frames == 100 ? 90 : 8300;
                for (std::size_t b = 0; b < blocks; ++b) {
                    std::vector<std::vector<float>> data(outputs, std::vector<float>(frames));
                    std::vector<std::span<float>> views;
                    for (auto& channel : data) {
                        for (float& v : channel) {
                            v = noise.sample();
                        }
                        views.emplace_back(channel);
                    }
                    const std::span<const std::span<float>> all_views(views.data(), views.size());
                    tap->block(all_views, frames, live);
                    first.block(all_views, frames, live);
                }
                std::array<std::int16_t, mt::kTapHistory> history{};
                REQUIRE(tap->read_history(history));
                const std::size_t base = first.mix16.size() - mt::kTapHistory;
                bool same = true;
                for (std::size_t i = 0; i < mt::kTapHistory && same; ++i) {
                    same = history[i] == first.mix16[base + i];
                }
                REQUIRE(same);
                const mt::FastLevels levels = tap->fast_levels();
                REQUIRE(levels.serial == first.serial);
                REQUIRE(levels.outputs == first.published_outputs);
                for (std::size_t o = 0; o < levels.outputs; ++o) {
                    REQUIRE(levels.peak_db[o] == first.published[o]);
                }
            }
        }
    }
}

TEST_CASE("a sum that is not a number is a number the meter can draw", "[meters-tap]") {
    auto tap = make_tap();
    const float nan = std::nanf("");
    for (int i = 0; i < 32; ++i) {
        Feed feed(2, {nan, 0.5F}, 256);
        tap->block(feed.span(), 256, 0b11);
    }
    std::array<std::int16_t, mt::kTapHistory> history{};
    REQUIRE(tap->read_history(history));
    // Whatever it is, it is one of the 16 bits and the same for every sample, and the peak of the number that is not one is not the window's.
    REQUIRE(history.front() == history.back());
    const mt::FastLevels levels = tap->fast_levels();
    REQUIRE(levels.serial >= 1);
    REQUIRE(std::fabs(levels.peak_db[1] - (-6.0206F)) < 1e-3F);
    REQUIRE(levels.peak_db[0] == mt::kTapSilentDb);
}

TEST_CASE("the ring reads across the wrap of its 32-bit count, and says what it did not have", "[meters-tap]") {
    // 256 samples before the count wraps.
    auto ring = std::make_unique<mt::MonoRing>(0xFFFFFF00U);
    std::vector<std::int16_t> block(200);
    std::int16_t next = 0;
    for (int i = 0; i < 3; ++i) {
        for (std::int16_t& v : block) {
            v = next++;
        }
        ring->write(block);
    }
    // 600 samples in, 344 past the wrap: the newest 300 are 300..599, and they cross it.
    REQUIRE(ring->written() == 344);
    std::vector<std::int16_t> newest(300);
    REQUIRE(ring->read_newest(newest));
    for (std::size_t i = 0; i < newest.size(); ++i) {
        REQUIRE(newest[i] == static_cast<std::int16_t>(300 + i));
    }
    // More than has been written since the wrap is a short read, for the 170 ms it takes to write that many.
    std::vector<std::int16_t> more(400);
    REQUIRE_FALSE(ring->read_newest(more));
    // Torn is a difference of counts, so it does not mind the wrap either.
    constexpr std::uint64_t kStart = 0xFFFFFF00ULL;
    REQUIRE_FALSE(mt::MonoRing::torn(kStart, kStart + mt::MonoRing::kCapacity - mt::kTapBlockFrames));
    REQUIRE(mt::MonoRing::torn(kStart, kStart + mt::MonoRing::kCapacity - mt::kTapBlockFrames + 1));
}

TEST_CASE("the tap's totals are the sums of what each look drained, and a look does not lose or repeat any", "[meters-tap]") {
    g_now = 0;
    auto tap = make_tap(&fake_clock);
    // Fifteen blocks in all: the sixteenth is the one that is timed in parts, and costs more on this clock.
    for (int i = 0; i < 9; ++i) {
        Feed feed(2, {0.1F, 0.1F}, 256);
        tap->block(feed.span(), 256, 0b11);
    }
    const mt::TapStats first = tap->stats();
    REQUIRE(first.blocks == 9);
    const mt::TapStats again = tap->stats();
    REQUIRE(again.blocks == 9);
    REQUIRE(again.frames == 9 * 256);
    REQUIRE(again.us_total == 9 * 3);
    for (int i = 0; i < 6; ++i) {
        Feed feed(2, {0.1F, 0.1F}, 256);
        tap->block(feed.span(), 256, 0b11);
    }
    const mt::TapStats last = tap->stats();
    REQUIRE(last.blocks == 15);
    REQUIRE(last.frames == 15 * 256);
    REQUIRE(last.us_total == 15 * 3);
    REQUIRE(std::fabs(last.us_per_burst() - 18.0) < 1e-9);
}
