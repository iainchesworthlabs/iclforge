#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <vector>

#include "iclforge/pitch/synth.hpp"
#include "iclforge/pitch/tracker.hpp"
#include "sanitized.hpp"

// libs/pitch/src/tracker.cpp: the McLeod Pitch Method over a window, its decimator and its gates
// (planning/karaoke-microphone.md, D7; findings F5).

namespace {

using iclforge::pitch::cents_between;
using iclforge::pitch::Frame;
using iclforge::pitch::Tracker;
using iclforge::pitch::TrackerConfig;
using iclforge::pitch::VoiceNote;
using iclforge::pitch::VoiceOptions;

std::vector<float> sine(std::uint32_t rate, double hz, double seconds, double amplitude) {
    std::vector<float> x(static_cast<std::size_t>(seconds * rate));
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = static_cast<float>(
            amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate));
    }
    return x;
}

std::vector<Frame> track(const TrackerConfig& config, const std::vector<float>& samples) {
    auto tracker = Tracker::make(config);
    REQUIRE(tracker.has_value());
    std::vector<Frame> frames;
    tracker->push(samples, frames);
    return frames;
}

double voiced_fraction(const std::vector<Frame>& frames) {
    if (frames.empty()) {
        return 0.0;
    }
    const auto voiced =
        std::count_if(frames.begin(), frames.end(), [](const Frame& f) { return f.voiced(); });
    return static_cast<double>(voiced) / static_cast<double>(frames.size());
}

// Where a stream of white-ish noise comes from: a fixed LCG, so that a run is the same on every
// platform.
std::vector<float> noise(std::size_t n, double amplitude) {
    std::vector<float> x(n);
    std::uint32_t state = 12345U;
    for (float& v : x) {
        state = state * 1664525U + 1013904223U;
        v = static_cast<float>(amplitude * (static_cast<double>(state >> 8) / 8388608.0 - 1.0));
    }
    return x;
}

}  // namespace

TEST_CASE("pitch: a sine is tracked to within a couple of cents", "[pitch][tracker]") {
    for (const double hz : {110.0, 220.0, 440.0, 880.0, 1000.0}) {
        const TrackerConfig config;
        const auto frames = track(config, sine(16000, hz, 1.0, 0.3));
        REQUIRE(frames.size() > 50);
        CHECK(voiced_fraction(frames) > 0.99);
        std::vector<double> errors;
        for (const Frame& f : frames) {
            if (f.voiced()) {
                errors.push_back(std::fabs(cents_between(hz, static_cast<double>(f.hz))));
            }
        }
        std::sort(errors.begin(), errors.end());
        INFO("hz " << hz);
        CHECK(errors[errors.size() / 2] < 2.0);
        CHECK(errors.back() < 6.0);
    }
}

TEST_CASE("pitch: silence, noise and a very quiet tone are not voiced", "[pitch][tracker]") {
    const TrackerConfig config;
    {
        const auto frames = track(config, std::vector<float>(16000, 0.0F));
        REQUIRE(!frames.empty());
        CHECK(voiced_fraction(frames) == 0.0);
        CHECK(frames.front().level_db <= -100.0F);
    }
    {
        const auto frames = track(config, noise(16000 * 2, 0.2));
        REQUIRE(!frames.empty());
        CHECK(voiced_fraction(frames) < 0.05);
    }
    {
        // -70 dBFS: periodic, and under the level floor.
        const auto frames = track(config, sine(16000, 440.0, 1.0, 0.0003));
        REQUIRE(!frames.empty());
        CHECK(voiced_fraction(frames) == 0.0);
    }
}

TEST_CASE("pitch: a tone below the range is not reported as another pitch", "[pitch][tracker]") {
    const auto frames = track(TrackerConfig{}, sine(16000, 40.0, 1.0, 0.3));
    REQUIRE(!frames.empty());
    CHECK(voiced_fraction(frames) == 0.0);
}

TEST_CASE("pitch: frames come a hop apart, the first once a whole window has been seen",
          "[pitch][tracker]") {
    const auto frames = track(TrackerConfig{}, sine(16000, 330.0, 1.0, 0.3));
    REQUIRE(frames.size() > 10);
    // A 64 ms window is 1,024 samples at 16 kHz; its centre is sample 512, and a hop is 160.
    CHECK(frames.front().centre_sample == 512);
    for (std::size_t i = 1; i < frames.size(); ++i) {
        CHECK(frames[i].centre_sample - frames[i - 1].centre_sample == 160);
    }
}

TEST_CASE("pitch: any chunk size gives the same frames", "[pitch][tracker]") {
    VoiceOptions options;
    options.lead_s = 0.0;
    options.snr_db = 20.0;
    const std::vector<VoiceNote> notes = {{0.0, 0.5, 57.0}, {0.55, 0.5, 62.0}};
    const auto voice = iclforge::pitch::synthesize_voice(notes, options);

    const TrackerConfig config;
    const auto whole = track(config, voice.samples);
    REQUIRE(whole.size() > 20);
    for (const std::size_t chunk :
         {std::size_t{1}, std::size_t{7}, std::size_t{333}, std::size_t{4096}}) {
        auto tracker = Tracker::make(config);
        REQUIRE(tracker.has_value());
        std::vector<Frame> frames;
        for (std::size_t at = 0; at < voice.samples.size(); at += chunk) {
            const std::size_t n = std::min(chunk, voice.samples.size() - at);
            tracker->push(std::span<const float>(voice.samples).subspan(at, n), frames);
        }
        INFO("chunk " << chunk);
        REQUIRE(frames.size() == whole.size());
        for (std::size_t i = 0; i < frames.size(); ++i) {
            CHECK(frames[i].centre_sample == whole[i].centre_sample);
            CHECK(frames[i].hz == whole[i].hz);
        }
    }
}

TEST_CASE("pitch: reset starts a new stream at sample 0", "[pitch][tracker]") {
    auto tracker = Tracker::make(TrackerConfig{});
    REQUIRE(tracker.has_value());
    const auto x = sine(16000, 262.0, 0.5, 0.3);
    std::vector<Frame> first;
    tracker->push(x, first);
    tracker->reset();
    std::vector<Frame> second;
    tracker->push(x, second);
    REQUIRE(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        CHECK(first[i].centre_sample == second[i].centre_sample);
        CHECK(first[i].hz == second[i].hz);
    }
}

TEST_CASE("pitch: 48 kHz and 44.1 kHz input are decimated and timed in input samples",
          "[pitch][tracker][decimate]") {
    {
        const TrackerConfig config = TrackerConfig::for_input_rate(48000);
        CHECK(config.analysis_rate == 16000);
        auto tracker = Tracker::make(config);
        REQUIRE(tracker.has_value());
        CHECK(tracker->decimation() == 3);
        std::vector<Frame> frames;
        tracker->push(sine(48000, 440.0, 1.0, 0.3), frames);
        REQUIRE(frames.size() > 50);
        // The window's centre is analysis sample 512, which is input sample 1,536 less the FIR's
        // 24-sample group delay; and a hop is 160 analysis samples, 480 input samples.
        CHECK(frames.front().centre_sample == 1536 - 24);
        CHECK(frames[1].centre_sample - frames[0].centre_sample == 480);
        CHECK(voiced_fraction(frames) > 0.95);
        std::vector<double> errors;
        for (const Frame& f : frames) {
            if (f.voiced()) {
                errors.push_back(std::fabs(cents_between(440.0, static_cast<double>(f.hz))));
            }
        }
        std::sort(errors.begin(), errors.end());
        CHECK(errors[errors.size() / 2] < 2.0);
    }
    {
        const TrackerConfig config = TrackerConfig::for_input_rate(44100);
        CHECK(config.analysis_rate == 14700);
        const auto frames = track(config, sine(44100, 523.25, 1.0, 0.3));
        REQUIRE(frames.size() > 50);
        CHECK(voiced_fraction(frames) > 0.95);
        std::vector<double> errors;
        for (const Frame& f : frames) {
            if (f.voiced()) {
                errors.push_back(std::fabs(cents_between(523.25, static_cast<double>(f.hz))));
            }
        }
        std::sort(errors.begin(), errors.end());
        CHECK(errors[errors.size() / 2] < 3.0);
    }
    {
        // 9 kHz is above the decimated rate's Nyquist; it must be filtered, not folded to 7 kHz.
        const auto frames =
            track(TrackerConfig::for_input_rate(48000), sine(48000, 9000.0, 1.0, 0.3));
        REQUIRE(!frames.empty());
        CHECK(voiced_fraction(frames) < 0.05);
    }
}

TEST_CASE("pitch: a configuration that cannot work is refused", "[pitch][tracker]") {
    TrackerConfig c;
    c.input_rate = 44100;  // not a whole multiple of 16 kHz
    CHECK(!Tracker::make(c).has_value());

    c = TrackerConfig{};
    c.min_hz = 500.0F;
    c.max_hz = 400.0F;
    CHECK(!Tracker::make(c).has_value());

    c = TrackerConfig{};
    c.max_hz = 7000.0F;  // too close to the Nyquist for a lag search
    CHECK(!Tracker::make(c).has_value());

    c = TrackerConfig{};
    c.analysis_rate = 0;
    CHECK(!Tracker::make(c).has_value());

    c = TrackerConfig{};
    c.frame_ms = 8;  // shorter than the lag range twice over
    CHECK(!Tracker::make(c).has_value());
}

TEST_CASE("pitch: synthetic voices are tracked, clean and in noise", "[pitch][tracker][voice]") {
    struct Range {
        const char* name;
        double low;
        double high;
    };
    const Range ranges[] = {
        {"bass", 40.0, 52.0}, {"tenor and alto", 48.0, 72.0}, {"soprano", 72.0, 84.0}};
    // Under the sanitizers (tests/support/sanitized.hpp) each range is tracked at one of its two
    // noise levels, the bass and the soprano clean and the tenor and alto in noise.
    std::size_t range_index = 0;
    for (const Range& range : ranges) {
        std::size_t snr_index = 0;
        for (const double snr : {1000.0, 10.0}) {
            if (iclforge::test::kSanitized && (range_index + snr_index++) % 2 != 0) {
                continue;
            }
            VoiceOptions options;
            options.lead_s = 0.0;
            options.seed = 7;
            if (snr < 100.0) {
                options.snr_db = snr;
            }
            const auto notes = iclforge::pitch::random_melody(range.low, range.high, 12);
            const auto voice = iclforge::pitch::synthesize_voice(notes, options);
            const auto frames = track(TrackerConfig{}, voice.samples);
            REQUIRE(frames.size() > 100);

            std::size_t truth_voiced = 0;
            std::size_t found = 0;
            std::size_t gross = 0;
            std::vector<double> errors;
            for (const Frame& f : frames) {
                const float truth = voice.f0[static_cast<std::size_t>(f.centre_sample)];
                if (truth <= 0.0F) {
                    continue;
                }
                ++truth_voiced;
                if (!f.voiced()) {
                    continue;
                }
                ++found;
                const double cents =
                    std::fabs(cents_between(static_cast<double>(truth), static_cast<double>(f.hz)));
                if (cents > 50.0) {
                    ++gross;
                } else {
                    errors.push_back(cents);
                }
            }
            INFO(range.name << ", SNR " << snr);
            REQUIRE(truth_voiced > 100);
            CHECK(static_cast<double>(found) / static_cast<double>(truth_voiced) > 0.88);
            CHECK(static_cast<double>(gross) /
                      static_cast<double>(std::max<std::size_t>(found, 1)) <
                  0.03);
            std::sort(errors.begin(), errors.end());
            REQUIRE(!errors.empty());
            CHECK(errors[errors.size() / 2] < 12.0);
            CHECK(errors[errors.size() * 95 / 100] < 40.0);
        }
        ++range_index;
    }
}

TEST_CASE("pitch: a stricter clarity floor drops the frames a noisy voice has least of",
          "[pitch][tracker][voice]") {
    VoiceOptions options;
    options.lead_s = 0.0;
    options.seed = 9;
    options.snr_db = 5.0;
    const auto notes = iclforge::pitch::random_melody(48.0, 72.0, 10);
    const auto voice = iclforge::pitch::synthesize_voice(notes, options);

    TrackerConfig loose;
    TrackerConfig strict;
    strict.clarity_min = 0.97F;
    const double loose_fraction = voiced_fraction(track(loose, voice.samples));
    const double strict_fraction = voiced_fraction(track(strict, voice.samples));
    CHECK(loose_fraction > 0.3);
    CHECK(strict_fraction < loose_fraction * 0.8);
}

TEST_CASE("pitch: the synthetic voice is the same for the same seed and not for another",
          "[pitch][voice]") {
    const std::vector<VoiceNote> notes = {{0.0, 0.3, 60.0}};
    VoiceOptions a;
    a.seed = 5;
    VoiceOptions b = a;
    b.seed = 6;
    const auto first = iclforge::pitch::synthesize_voice(notes, a);
    const auto second = iclforge::pitch::synthesize_voice(notes, a);
    const auto other = iclforge::pitch::synthesize_voice(notes, b);
    REQUIRE(first.samples.size() == second.samples.size());
    CHECK(first.samples == second.samples);
    CHECK(first.samples != other.samples);
}
