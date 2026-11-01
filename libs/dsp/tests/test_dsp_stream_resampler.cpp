#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <vector>

#include "iclforge/dsp/resampler.hpp"
#include "iclforge/dsp/stream_resampler.hpp"

// The streaming converter is resample()'s filter kept running between blocks. Three things hold it
// to that: the output does not depend on how the input is cut into blocks; a sine comes out as the
// same sine, later by the filter's delay and no other change (which is what a linear-phase filter
// does to it); and, where the delay is a whole number of output frames, the output is the offline
// converter's, that many frames on.

namespace {

struct Ratio {
    std::uint32_t from;
    std::uint32_t to;
};

constexpr std::array<Ratio, 6> kRatios{Ratio{96000, 48000}, Ratio{192000, 48000},
                                       Ratio{88200, 44100}, Ratio{44100, 48000},
                                       Ratio{88200, 48000}, Ratio{48000, 96000}};

std::vector<float> sine(std::size_t frames, std::uint32_t rate, double hz) {
    std::vector<float> out(frames);
    for (std::size_t n = 0; n < frames; ++n) {
        out[n] = static_cast<float>(
            0.5 * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) / rate));
    }
    return out;
}

std::vector<float> noise(std::size_t frames) {
    std::vector<float> out(frames);
    std::uint32_t state = 12345U;
    for (float& sample : out) {
        state = state * 1664525U + 1013904223U;
        sample = static_cast<float>(static_cast<double>(state >> 8U) / 8388608.0 - 1.0) * 0.4F;
    }
    return out;
}

std::vector<float> run_in_blocks(iclforge::dsp::StreamResampler& stream,
                                 const std::vector<float>& input,
                                 std::span<const std::size_t> sizes) {
    std::vector<std::vector<float>> out;
    std::size_t at = 0;
    for (std::size_t step = 0; at < input.size(); ++step) {
        const std::size_t block = std::min(sizes[step % sizes.size()], input.size() - at);
        const std::array<std::span<const float>, 1> in{
            std::span<const float>{input}.subspan(at, block)};
        const std::size_t before = out.empty() ? 0 : out[0].size();
        const std::size_t promised = stream.outputs_for(block);
        stream.process(in, block, out);
        REQUIRE(out[0].size() - before == promised);
        at += block;
    }
    return out.empty() ? std::vector<float>{} : out[0];
}

}  // namespace

TEST_CASE("a stream resampler's output does not depend on how the input is cut",
          "[dsp][resampler][stream]") {
    for (const Ratio r : kRatios) {
        CAPTURE(r.from, r.to);
        const std::vector<float> input = noise(20000);
        const std::array<std::size_t, 1> whole{input.size()};
        const std::array<std::size_t, 3> odd{1, 7, 1000};
        iclforge::dsp::StreamResampler one_shot{r.from, r.to, 1};
        iclforge::dsp::StreamResampler pieces{r.from, r.to, 1};
        const std::vector<float> a = run_in_blocks(one_shot, input, whole);
        const std::vector<float> b = run_in_blocks(pieces, input, odd);
        // After n input frames there are floor(n * to / from) outputs.
        REQUIRE(a.size() == input.size() * r.to / r.from);
        CHECK(a == b);
    }
}

TEST_CASE("a stream resampler passes a sine on, later by its delay and no other change",
          "[dsp][resampler][stream]") {
    for (const Ratio r : kRatios) {
        CAPTURE(r.from, r.to);
        constexpr double kHz = 1000.0;
        const std::vector<float> input = sine(24000, r.from, kHz);
        iclforge::dsp::StreamResampler stream{r.from, r.to, 1};
        const std::array<std::size_t, 1> sizes{480};
        const std::vector<float> out = run_in_blocks(stream, input, sizes);
        REQUIRE(out.size() > 2000);
        double worst = 0.0;
        // Past the filter's start-up, which is the silence it takes the input to be before it.
        for (std::size_t n = 1000; n < out.size(); ++n) {
            const double seconds = static_cast<double>(n) / r.to - stream.delay() / r.from;
            const double want = 0.5 * std::sin(2.0 * std::numbers::pi * kHz * seconds);
            worst = std::max(worst, std::abs(static_cast<double>(out[n]) - want));
        }
        CHECK(worst < 1e-4);
    }
}

TEST_CASE("a stream resampler is the offline converter's output, a whole delay on",
          "[dsp][resampler][stream]") {
    // The ratios whose delay is a whole number of output frames.
    for (const Ratio r : {Ratio{96000, 48000}, Ratio{88200, 44100}, Ratio{48000, 96000}}) {
        CAPTURE(r.from, r.to);
        const std::vector<float> input = noise(12000);
        const std::vector<float> whole = iclforge::dsp::resample(input, r.from, r.to);
        iclforge::dsp::StreamResampler stream{r.from, r.to, 1};
        const std::array<std::size_t, 2> sizes{333, 64};
        const std::vector<float> out = run_in_blocks(stream, input, sizes);
        const double delay_out = stream.delay() * r.to / r.from;
        REQUIRE(delay_out == std::floor(delay_out));
        const auto shift = static_cast<std::size_t>(delay_out);
        double worst = 0.0;
        for (std::size_t n = shift; n < out.size(); ++n) {
            worst = std::max(worst, std::abs(static_cast<double>(out[n]) -
                                             static_cast<double>(whole[n - shift])));
        }
        CHECK(worst < 1e-6);
    }
}

TEST_CASE("a stream resampler keeps its channels apart and treats a missing one as silence",
          "[dsp][resampler][stream]") {
    const std::vector<float> a = sine(4800, 96000, 700.0);
    const std::vector<float> b = sine(4800, 96000, 3100.0);
    iclforge::dsp::StreamResampler pair{96000, 48000, 2};
    iclforge::dsp::StreamResampler single_a{96000, 48000, 1};
    std::vector<std::vector<float>> both;
    std::vector<std::vector<float>> only_a;
    const std::array<std::span<const float>, 2> in{a, b};
    pair.process(in, a.size(), both);
    single_a.process(std::span<const std::span<const float>>{in}.first(1), a.size(), only_a);
    REQUIRE(both.size() == 2);
    CHECK(both[0] == only_a[0]);
    CHECK(both[0] != both[1]);

    // One channel given of two: the other converts silence.
    iclforge::dsp::StreamResampler half{96000, 48000, 2};
    std::vector<std::vector<float>> got;
    half.process(std::span<const std::span<const float>>{in}.first(1), a.size(), got);
    CHECK(got[0] == only_a[0]);
    CHECK(std::ranges::all_of(got[1], [](float v) { return v == 0.0F; }));
}

TEST_CASE("a stream resampler that is reset starts the stream again", "[dsp][resampler][stream]") {
    const std::vector<float> input = noise(3000);
    iclforge::dsp::StreamResampler stream{96000, 48000, 1};
    const std::array<std::span<const float>, 1> in{input};
    std::vector<std::vector<float>> first;
    stream.process(in, input.size(), first);
    stream.reset();
    std::vector<std::vector<float>> second;
    stream.process(in, input.size(), second);
    CHECK(first == second);
    // The output lags the input by the filter's delay, which is a few tens of frames.
    CHECK(stream.delay() > 10.0);
    CHECK(stream.delay() < 400.0);
}

TEST_CASE("a stream resampler at equal rates is an identity", "[dsp][resampler][stream]") {
    const std::vector<float> input = noise(1000);
    iclforge::dsp::StreamResampler stream{48000, 48000, 1};
    const std::array<std::span<const float>, 1> in{input};
    std::vector<std::vector<float>> out;
    stream.process(in, input.size(), out);
    REQUIRE(out[0].size() == input.size());
    CHECK(out[0] == input);
}
