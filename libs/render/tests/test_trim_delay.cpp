// Per-output trim and delay (ac3/render/trim_delay.hpp).
//
// A delay line is where an off-by-one hides: a sample one early or one late
// sounds the same and moves a speaker 7 mm. So the delays are checked sample
// for sample, across block boundaries and delays longer than a block, and a
// change of delay is checked to drop what the old one held.

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "iclforge/render/trim_delay.hpp"

namespace {

using iclforge::render::TrimDelay;
using Catch::Approx;

// A ramp 1, 2, 3, ... per output, fed through in blocks of `block`, and what
// came out, concatenated.
std::vector<std::vector<float>> run_ramp(TrimDelay& stage, std::size_t outputs,
                                         std::size_t total, std::size_t block) {
    std::vector<std::vector<float>> result(outputs);
    std::vector<std::vector<float>> planes(outputs, std::vector<float>(block));
    std::vector<std::span<float>> spans(outputs);
    std::size_t next = 1;
    for (std::size_t done = 0; done < total; done += block) {
        const std::size_t n = std::min(block, total - done);
        for (std::size_t o = 0; o < outputs; ++o) {
            for (std::size_t k = 0; k < n; ++k) {
                planes[o][k] = static_cast<float>(next + k);
            }
            spans[o] = std::span<float>(planes[o].data(), n);
        }
        next += n;
        stage.process(spans);
        for (std::size_t o = 0; o < outputs; ++o) {
            const auto end = planes[o].begin() + static_cast<std::ptrdiff_t>(n);
            result[o].insert(result[o].end(), planes[o].begin(), end);
        }
    }
    return result;
}

}  // namespace

TEST_CASE("trim and delay are configured over the caller's storage", "[render][trim-delay]") {
    REQUIRE(TrimDelay::storage_floats(16, 960) == 15360);
    REQUIRE(TrimDelay::samples_for_ms(20.0, 48000) == 960);
    REQUIRE(TrimDelay::samples_for_ms(1.0, 44100) == 44);    // 44.1 rounds down
    REQUIRE(TrimDelay::samples_for_ms(0.99, 48000) == 48);   // 47.52 rounds up
    REQUIRE(TrimDelay::samples_for_ms(-3.0, 48000) == 0);
    REQUIRE(TrimDelay::samples_for_ms(std::numeric_limits<double>::quiet_NaN(), 48000) == 0);
    REQUIRE(TrimDelay::samples_for_ms(std::numeric_limits<double>::infinity(), 48000) == 0);

    std::vector<float> storage(TrimDelay::storage_floats(4, 100), 7.0F);
    TrimDelay stage;
    REQUIRE(stage.outputs() == 0);
    REQUIRE_FALSE(stage.configure(std::span<float>(storage.data(), 399), 4, 100));
    REQUIRE(stage.outputs() == 0);
    REQUIRE_FALSE(stage.configure(storage, TrimDelay::kMaxOutputs + 1, 0));
    REQUIRE(stage.configure(storage, 4, 100));
    REQUIRE(stage.outputs() == 4);
    REQUIRE(stage.max_delay() == 100);
    REQUIRE(storage[123] == 0.0F);  // the lines start silent
    REQUIRE_FALSE(stage.active());

    // No delay storage at all is a trim-only stage.
    TrimDelay trims_only;
    REQUIRE(trims_only.configure({}, 8, 0));
    REQUIRE_FALSE(trims_only.set_delay(0, 1));
    REQUIRE(trims_only.set_delay(0, 0));
    REQUIRE(trims_only.set_trim_db(7, -3.0));
}

TEST_CASE("a trim is a gain in dB, inside its range", "[render][trim-delay]") {
    std::vector<float> storage;
    TrimDelay stage;
    REQUIRE(stage.configure(storage, 3, 0));
    REQUIRE(stage.set_trim_db(0, 6.0206));
    REQUIRE(stage.set_trim_db(1, -12.0412));
    REQUIRE(stage.trim_db(0) == Approx(6.0206));
    REQUIRE(stage.active());

    std::array<float, 4> a = {0.25F, -0.25F, 0.5F, 0.0F};
    std::array<float, 4> b = {1.0F, 1.0F, 1.0F, 1.0F};
    std::array<float, 4> c = {0.3F, 0.3F, 0.3F, 0.3F};
    const std::array<std::span<float>, 3> spans = {a, b, c};
    stage.process(spans);
    REQUIRE(a[0] == Approx(0.5F).epsilon(1e-4));
    REQUIRE(a[1] == Approx(-0.5F).epsilon(1e-4));
    REQUIRE(b[2] == Approx(0.25F).epsilon(1e-4));
    REQUIRE(c[3] == 0.3F);  // 0 dB is exactly unity

    REQUIRE_FALSE(stage.set_trim_db(0, TrimDelay::kMaxTrimDb + 0.1));
    REQUIRE_FALSE(stage.set_trim_db(0, TrimDelay::kMinTrimDb - 0.1));
    REQUIRE_FALSE(stage.set_trim_db(0, std::numeric_limits<double>::quiet_NaN()));
    REQUIRE_FALSE(stage.set_trim_db(3, 0.0));
    REQUIRE(stage.trim_db(0) == Approx(6.0206));  // a refusal changes nothing
    REQUIRE(stage.set_trim_db(0, TrimDelay::kMaxTrimDb));
    REQUIRE(stage.set_trim_db(0, TrimDelay::kMinTrimDb));
}

TEST_CASE("a delay shifts an output by exactly its samples, across blocks",
          "[render][trim-delay]") {
    // Delays shorter than, equal to and longer than the 256-sample block, and
    // one output left undelayed beside them.
    const std::array<std::size_t, 5> delays = {0, 1, 255, 256, 700};
    std::vector<float> storage(TrimDelay::storage_floats(delays.size(), 960));
    TrimDelay stage;
    REQUIRE(stage.configure(storage, delays.size(), 960));
    for (std::size_t o = 0; o < delays.size(); ++o) {
        REQUIRE(stage.set_delay(o, delays[o]));
        REQUIRE(stage.delay(o) == delays[o]);
    }
    constexpr std::size_t kTotal = 256 * 8;
    for (const std::size_t block : {std::size_t{256}, std::size_t{100}, std::size_t{1}}) {
        CAPTURE(block);
        stage.reset();
        const auto result = run_ramp(stage, delays.size(), kTotal, block);
        for (std::size_t o = 0; o < delays.size(); ++o) {
            CAPTURE(o);
            const std::size_t d = delays[o];
            for (std::size_t k = 0; k < kTotal; ++k) {
                // Input sample k (0-based) is the value k + 1.
                const float expected = k < d ? 0.0F : static_cast<float>(k - d + 1);
                if (result[o][k] != expected) {
                    CAPTURE(k, result[o][k], expected);
                    FAIL("delayed output is off");
                }
            }
        }
    }
    REQUIRE_FALSE(stage.set_delay(0, 961));
    REQUIRE_FALSE(stage.set_delay(5, 1));
}

TEST_CASE("changing a delay drops what the old one held", "[render][trim-delay]") {
    std::vector<float> storage(TrimDelay::storage_floats(1, 64));
    TrimDelay stage;
    REQUIRE(stage.configure(storage, 1, 64));
    REQUIRE(stage.set_delay(0, 48));
    std::array<float, 32> block{};
    block.fill(1.0F);
    const std::array<std::span<float>, 1> spans = {block};
    stage.process(spans);  // 32 ones go into the line

    // Shorter: nothing of the old line comes out, just the new delay's silence.
    REQUIRE(stage.set_delay(0, 16));
    block.fill(2.0F);
    stage.process(spans);
    for (std::size_t k = 0; k < 16; ++k) {
        REQUIRE(block[k] == 0.0F);
    }
    for (std::size_t k = 16; k < 32; ++k) {
        REQUIRE(block[k] == 2.0F);
    }

    // Setting the same delay again is not a change, and keeps the line.
    REQUIRE(stage.set_delay(0, 16));
    block.fill(3.0F);
    stage.process(spans);
    for (std::size_t k = 0; k < 16; ++k) {
        REQUIRE(block[k] == 2.0F);
    }

    // reset() empties the lines and keeps the settings.
    stage.reset();
    REQUIRE(stage.delay(0) == 16);
    block.fill(4.0F);
    stage.process(spans);
    REQUIRE(block[0] == 0.0F);
    REQUIRE(block[16] == 4.0F);
}

TEST_CASE("trim applies after the delay, and outputs stay independent", "[render][trim-delay]") {
    std::vector<float> storage(TrimDelay::storage_floats(2, 8));
    TrimDelay stage;
    REQUIRE(stage.configure(storage, 2, 8));
    REQUIRE(stage.set_delay(0, 2));
    REQUIRE(stage.set_trim_db(0, -6.0206));
    std::array<float, 4> first = {1.0F, 1.0F, 1.0F, 1.0F};
    std::array<float, 4> second = {1.0F, 1.0F, 1.0F, 1.0F};
    const std::array<std::span<float>, 2> spans = {first, second};
    stage.process(spans);
    REQUIRE(first[0] == 0.0F);
    REQUIRE(first[1] == 0.0F);
    REQUIRE(first[2] == Approx(0.5F).epsilon(1e-4));
    REQUIRE(second == std::array<float, 4>{1.0F, 1.0F, 1.0F, 1.0F});

    // Fewer spans than outputs: the outputs given are processed and the
    // missing one is skipped, not read.
    std::array<float, 4> only = {2.0F, 2.0F, 2.0F, 2.0F};
    const std::array<std::span<float>, 1> one = {only};
    stage.process(one);
    REQUIRE(only[0] == Approx(0.5F).epsilon(1e-4));  // output 0's line: the last block's ones
    REQUIRE(only[2] == Approx(1.0F).epsilon(1e-4));  // then this block's twos, trimmed
}

TEST_CASE("process for some outputs trims those as process does and leaves the rest alone", "[render][trim-delay]") {
    std::vector<float> storage(TrimDelay::storage_floats(4, 10), 0.0F);
    TrimDelay stage;
    REQUIRE(stage.configure(storage, 4, 10));
    REQUIRE(stage.set_trim_db(0, -6.0));
    REQUIRE(stage.set_trim_db(2, 6.0));
    REQUIRE(stage.set_trim_db(3, -12.0));
    CHECK_FALSE(stage.delayed());

    const auto filled = [](std::size_t outputs) {
        std::vector<std::vector<float>> planes(outputs, std::vector<float>(8));
        for (std::size_t o = 0; o < outputs; ++o) {
            for (std::size_t k = 0; k < 8; ++k) {
                planes[o][k] = static_cast<float>((o + 1) * 10 + k);
            }
        }
        return planes;
    };
    auto everything = filled(4);
    std::vector<std::span<float>> all_spans(everything.begin(), everything.end());
    stage.process(all_spans);

    for (const std::uint32_t only : {0U, 0b0001U, 0b0110U, 0b1111U, 0xFFFFFFF0U}) {
        CAPTURE(only);
        auto some = filled(4);
        std::vector<std::span<float>> spans(some.begin(), some.end());
        stage.process(spans, only);
        const auto untouched = filled(4);
        for (std::size_t o = 0; o < 4; ++o) {
            CAPTURE(o);
            REQUIRE(some[o] == (((only >> o) & 1U) != 0 ? everything[o] : untouched[o]));
        }
    }

    // A delay is a line that moves with every block, which is why a caller asks delayed().
    REQUIRE(stage.set_delay(1, 3));
    CHECK(stage.delayed());
    REQUIRE(stage.set_delay(1, 0));
    CHECK_FALSE(stage.delayed());
    CHECK_FALSE(TrimDelay{}.delayed());
}

TEST_CASE("a ramped trim moves to its level over the next block, and a set one is there at once", "[render][trim-delay]") {
    std::vector<float> storage;
    TrimDelay stage;
    REQUIRE(stage.configure(storage, 2, 0));
    REQUIRE(stage.gain(0) == 1.0F);
    REQUIRE(stage.gain(5) == 1.0F);  // an output it does not have

    // set_trim_db: at once.
    REQUIRE(stage.set_trim_db(0, -6.0));
    REQUIRE(stage.ramp_trim_db(1, -6.0));
    REQUIRE(stage.gain(0) == stage.gain(1));
    REQUIRE(stage.gain(0) == Approx(0.501187).epsilon(1e-6));

    std::vector<float> a(256, 1.0F);
    std::vector<float> b(256, 1.0F);
    const std::array<std::span<float>, 2> planes = {std::span<float>(a), std::span<float>(b)};
    stage.process(planes);
    for (std::size_t k = 0; k < 256; ++k) {
        REQUIRE(a[k] == stage.gain(0));
        // From unity to the level in a straight line, the last sample of the block the level itself.
        REQUIRE(b[k] > stage.gain(1) - 1e-4F);
        REQUIRE(b[k] < 1.0F);
        if (k > 0) {
            REQUIRE(b[k] < b[k - 1]);
        }
    }
    REQUIRE(b[255] == Approx(stage.gain(1)).epsilon(1e-6));
    // The next block is steady.
    std::fill(a.begin(), a.end(), 1.0F);
    std::fill(b.begin(), b.end(), 1.0F);
    stage.process(planes);
    for (std::size_t k = 0; k < 256; ++k) {
        REQUIRE(b[k] == stage.gain(1));
    }

    // And back to unity: a ramp up, then nothing to do.
    REQUIRE(stage.ramp_trim_db(1, 0.0));
    std::fill(b.begin(), b.end(), 1.0F);
    stage.process(planes);
    REQUIRE(b[0] > stage.gain(0));
    REQUIRE(b[255] == Approx(1.0F).epsilon(1e-6));
    REQUIRE(stage.active());  // output 0 still has its trim
    REQUIRE(stage.ramp_trim_db(0, 0.0));
    REQUIRE(stage.active());  // a ramp has yet to run
    std::fill(a.begin(), a.end(), 1.0F);
    stage.process(planes);
    REQUIRE_FALSE(stage.active());

    // The refusals are set_trim_db's.
    REQUIRE_FALSE(stage.ramp_trim_db(0, 12.5));
    REQUIRE_FALSE(stage.ramp_trim_db(2, 0.0));
    REQUIRE_FALSE(stage.ramp_trim_db(0, std::numeric_limits<double>::quiet_NaN()));
}

TEST_CASE("a new stream ends a ramp under way, and the delayed outputs are named", "[render][trim-delay]") {
    std::vector<float> storage(TrimDelay::storage_floats(4, 16));
    TrimDelay stage;
    REQUIRE(stage.configure(storage, 4, 16));
    REQUIRE(stage.delayed_outputs() == 0);
    REQUIRE_FALSE(stage.delayed());
    REQUIRE(stage.set_delay(1, 5));
    REQUIRE(stage.set_delay(3, 16));
    REQUIRE(stage.delayed_outputs() == 0b1010U);
    REQUIRE(stage.delayed());
    REQUIRE(stage.set_delay(1, 0));
    REQUIRE(stage.delayed_outputs() == 0b1000U);

    REQUIRE(stage.ramp_trim_db(0, -12.0));
    stage.reset();
    std::vector<float> a(64, 1.0F);
    const std::array<std::span<float>, 1> planes = {std::span<float>(a)};
    stage.process(planes);
    for (const float sample : a) {
        REQUIRE(sample == stage.gain(0));
    }
}

TEST_CASE("a delayed output with nothing patched to it still plays out its line", "[render][trim-delay]") {
    // process(out, only) with the delayed outputs added to `only`: the tail of a line is not cut off by a block
    // in which the channel that fed it was silent.
    std::vector<float> storage(TrimDelay::storage_floats(2, 8));
    TrimDelay stage;
    REQUIRE(stage.configure(storage, 2, 8));
    REQUIRE(stage.set_delay(1, 4));
    std::vector<float> a(8, 0.0F);
    std::vector<float> b = {1, 2, 3, 4, 5, 6, 7, 8};
    const std::array<std::span<float>, 2> first = {std::span<float>(a), std::span<float>(b)};
    stage.process(first, 0b10U);
    REQUIRE(b == std::vector<float>({0, 0, 0, 0, 1, 2, 3, 4}));
    // The next block is silence: the line still holds 5 6 7 8.
    std::vector<float> silent(8, 0.0F);
    const std::array<std::span<float>, 2> second = {std::span<float>(a), std::span<float>(silent)};
    stage.process(second, stage.delayed_outputs());
    REQUIRE(silent == std::vector<float>({5, 6, 7, 8, 0, 0, 0, 0}));
}
