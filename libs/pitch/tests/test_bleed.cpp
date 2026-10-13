#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <vector>

#include "iclforge/pitch/bleed.hpp"
#include "iclforge/pitch/reference.hpp"
#include "iclforge/pitch/scorer.hpp"
#include "iclforge/pitch/synth.hpp"
#include "iclforge/pitch/tracker.hpp"

// libs/pitch/src/bleed.cpp: is the microphone mostly the speakers? (planning/karaoke-microphone.md,
// SC5; findings F6.) Every signal is synthetic: a played "backing", a singer, and a room that is a
// delay, a gain and a few reflections. The numbers hold for these signals and say nothing of a
// particular room.

namespace {

using iclforge::pitch::BleedFrame;
using iclforge::pitch::BleedMonitor;
using iclforge::pitch::BleedOptions;

constexpr std::uint32_t kRate = 16000;

// A played signal: a melody on one voice and another a fifth above it on a second, as a backing
// with a guide would be, for `seconds`.
std::vector<float> backing(double seconds, std::uint32_t seed, bool gapless = false) {
    iclforge::pitch::VoiceOptions options;
    options.sample_rate = kRate;
    options.lead_s = 0.0;
    options.tail_s = 0.1;
    options.amplitude = 0.4;
    options.seed = seed;
    options.vowel = 1;
    std::vector<iclforge::pitch::VoiceNote> notes;
    if (gapless) {
        // Note after note with no rest, so that the played signal is there in every frame.
        for (int i = 0; static_cast<double>(i) * 0.4 < seconds; ++i) {
            notes.push_back({0.4 * i, 0.4, 50.0 + static_cast<double>((i * 3) % 14)});
        }
    } else {
        notes = iclforge::pitch::random_melody(52.0, 72.0,
                                               static_cast<std::uint32_t>(seconds * 2.0), seed);
    }
    std::vector<float> a = iclforge::pitch::synthesize_voice(notes, options).samples;
    options.vowel = 2;
    options.seed = seed + 17;
    auto fifth = notes;
    for (auto& n : fifth) {
        n.midi += 7.0;
    }
    const std::vector<float> b = iclforge::pitch::synthesize_voice(fifth, options).samples;
    const auto n = static_cast<std::size_t>(seconds * kRate);
    std::vector<float> out(n, 0.0F);
    for (std::size_t i = 0; i < n; ++i) {
        const float x = i < a.size() ? a[i] : 0.0F;
        const float y = i < b.size() ? b[i] : 0.0F;
        out[i] = 0.7F * x + 0.5F * y;
    }
    return out;
}

// The room: the played signal `delay_ms` later, `gain` as loud, with reflections when `taps` is
// more than one.
std::vector<float> room(const std::vector<float>& played, double delay_ms, double gain,
                        std::size_t taps = 1) {
    static const double kTapDelayMs[] = {0.0, 2.7, 6.1, 11.3};
    static const double kTapGain[] = {1.0, 0.45, 0.25, 0.12};
    std::vector<float> out(played.size(), 0.0F);
    for (std::size_t t = 0; t < std::min<std::size_t>(taps, 4); ++t) {
        const auto d = static_cast<std::size_t>((delay_ms + kTapDelayMs[t]) * kRate / 1000.0);
        for (std::size_t i = d; i < played.size(); ++i) {
            out[i] += static_cast<float>(gain * kTapGain[t]) * played[i - d];
        }
    }
    return out;
}

double rms(std::span<const float> x) {
    double acc = 0.0;
    for (const float v : x) {
        acc += static_cast<double>(v) * static_cast<double>(v);
    }
    return std::sqrt(acc / static_cast<double>(std::max<std::size_t>(x.size(), 1)));
}

std::vector<float> sum(const std::vector<float>& a, const std::vector<float>& b,
                       float gain_b = 1.0F) {
    std::vector<float> out(std::max(a.size(), b.size()), 0.0F);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = (i < a.size() ? a[i] : 0.0F) + gain_b * (i < b.size() ? b[i] : 0.0F);
    }
    return out;
}

// A deterministic noise floor, to take the microphone off exact zeros.
std::vector<float> noise(std::size_t n, double rms_value, std::uint32_t seed) {
    std::vector<float> out(n);
    std::uint32_t state = seed * 2654435761U + 1U;
    for (auto& v : out) {
        state = state * 1664525U + 1013904223U;
        v = static_cast<float>((static_cast<double>(state >> 8) / 8388608.0 - 1.0) * rms_value *
                               1.7320508);
    }
    return out;
}

// Runs a monitor over both streams in chunks of 20 ms, played before microphone as a program does.
std::vector<BleedFrame> run(BleedMonitor& monitor, const std::vector<float>& played,
                            const std::vector<float>& mic) {
    std::vector<BleedFrame> frames;
    const std::size_t chunk = kRate / 50;
    const std::size_t n = std::min(played.size(), mic.size());
    for (std::size_t at = 0; at < n; at += chunk) {
        const std::size_t len = std::min(chunk, n - at);
        monitor.push_played(std::span<const float>(played).subspan(at, len));
        monitor.push_microphone(std::span<const float>(mic).subspan(at, len), frames);
    }
    return frames;
}

double share_bleed(const std::vector<BleedFrame>& frames, double from_s, double to_s) {
    std::size_t total = 0;
    std::size_t bleed = 0;
    for (const BleedFrame& f : frames) {
        const double t = static_cast<double>(f.centre_sample) / kRate;
        // A frame with nothing in it (the gap between notes of the played signal) has nothing to
        // attribute and is not counted either way.
        if (t < from_s || t >= to_s || f.level_db < -55.0F) {
            continue;
        }
        ++total;
        bleed += f.bleed ? 1U : 0U;
    }
    return total == 0 ? 0.0 : static_cast<double>(bleed) / static_cast<double>(total);
}

}  // namespace

TEST_CASE("bleed: a microphone that is the speakers is bleed, and the delay is found",
          "[pitch][bleed]") {
    const std::vector<float> played = backing(10.0, 3);
    const std::vector<float> mic = sum(room(played, 130.0, 0.3), noise(played.size(), 0.0005, 1));
    BleedMonitor monitor;
    const auto frames = run(monitor, played, mic);

    REQUIRE(monitor.delay_ms().has_value());
    CHECK(*monitor.delay_ms() == Catch::Approx(130.0).margin(1.0));
    // From the second the delay is known, every frame that has anything in it is bleed.
    CHECK(share_bleed(frames, 1.5, 10.0) > 0.95);
    CHECK(monitor.recent_explained() > 0.9F);
    // And its gain is the room's: 0.3 is -10.5 dB.
    float gain = -120.0F;
    for (const BleedFrame& f : frames) {
        if (f.bleed && static_cast<double>(f.centre_sample) / kRate > 5.0) {
            gain = f.gain_db;
            break;
        }
    }
    CHECK(gain == Catch::Approx(-10.5).margin(0.8));
}

TEST_CASE("bleed: a singer alone is not bleed", "[pitch][bleed]") {
    const std::vector<float> played = backing(10.0, 5);
    iclforge::pitch::VoiceOptions options;
    options.sample_rate = kRate;
    options.lead_s = 0.0;
    options.seed = 9;
    options.amplitude = 0.3;
    const auto notes = iclforge::pitch::random_melody(48.0, 68.0, 24, 11);
    const std::vector<float> voice = iclforge::pitch::synthesize_voice(notes, options).samples;
    const std::vector<float> mic = sum(voice, noise(played.size(), 0.0005, 2));
    BleedMonitor monitor;
    const auto frames = run(monitor, played, mic);
    CHECK(share_bleed(frames, 0.0, 10.0) < 0.02);
}

TEST_CASE("bleed: a voice above the speakers is not bleed, and below them is", "[pitch][bleed]") {
    const std::vector<float> played = backing(12.0, 7, true);
    const std::vector<float> echo = room(played, 110.0, 0.2);
    iclforge::pitch::VoiceOptions options;
    options.sample_rate = kRate;
    options.lead_s = 0.0;
    options.tail_s = 0.0;
    options.seed = 13;
    options.amplitude = 0.5;
    // Notes without gaps, so that the voice is there in every frame measured.
    std::vector<iclforge::pitch::VoiceNote> notes;
    for (int i = 0; i < 24; ++i) {
        notes.push_back({0.5 * i, 0.5, 57.0 + (i % 5)});
    }
    const std::vector<float> voice = iclforge::pitch::synthesize_voice(notes, options).samples;
    const double echo_rms = rms(std::span<const float>(echo).subspan(16000, 160000));
    const double voice_rms = rms(std::span<const float>(voice).subspan(16000, 160000));

    // The voice 10 dB above the echo.
    {
        const float gain = static_cast<float>(echo_rms / voice_rms * std::pow(10.0, 10.0 / 20.0));
        const std::vector<float> mic = sum(echo, voice, gain);
        BleedMonitor monitor;
        const auto frames = run(monitor, played, mic);
        CHECK(share_bleed(frames, 3.0, 11.0) < 0.1);
    }
    // The voice 10 dB below it.
    {
        const float gain = static_cast<float>(echo_rms / voice_rms * std::pow(10.0, -10.0 / 20.0));
        const std::vector<float> mic = sum(echo, voice, gain);
        BleedMonitor monitor;
        const auto frames = run(monitor, played, mic);
        CHECK(share_bleed(frames, 3.0, 11.0) > 0.9);
    }
}

TEST_CASE("bleed: a delay that changes is followed", "[pitch][bleed]") {
    const std::vector<float> played = backing(16.0, 21);
    // 130 ms for the first 8 s and 180 ms after.
    std::vector<float> first = room(played, 130.0, 0.3);
    std::vector<float> second = room(played, 180.0, 0.3);
    std::vector<float> mic(played.size());
    const std::size_t switch_at = 8 * kRate;
    for (std::size_t i = 0; i < mic.size(); ++i) {
        mic[i] = i < switch_at ? first[i] : second[i];
    }
    mic = sum(mic, noise(mic.size(), 0.0005, 3));
    BleedMonitor monitor;
    std::vector<BleedFrame> frames;
    const std::size_t chunk = kRate / 50;
    std::optional<double> before;
    for (std::size_t at = 0; at + chunk <= mic.size(); at += chunk) {
        monitor.push_played(std::span<const float>(played).subspan(at, chunk));
        monitor.push_microphone(std::span<const float>(mic).subspan(at, chunk), frames);
        if (at + chunk == switch_at) {
            before = monitor.delay_ms();
        }
    }
    REQUIRE(before.has_value());
    CHECK(*before == Catch::Approx(130.0).margin(1.5));
    REQUIRE(monitor.delay_ms().has_value());
    CHECK(*monitor.delay_ms() == Catch::Approx(180.0).margin(1.5));
    CHECK(share_bleed(frames, 12.0, 16.0) > 0.9);
}

TEST_CASE("bleed: silence is nothing to attribute", "[pitch][bleed]") {
    const std::vector<float> played = backing(6.0, 4);
    const std::vector<float> mic(played.size(), 0.0F);
    BleedMonitor monitor;
    const auto frames = run(monitor, played, mic);
    REQUIRE(!frames.empty());
    for (const BleedFrame& f : frames) {
        CHECK(!f.bleed);
    }
    CHECK(!monitor.delay_ms().has_value());

    // A played signal that is silent explains nothing, whatever the microphone hears.
    const std::vector<float> quiet(played.size(), 0.0F);
    const std::vector<float> hiss = noise(played.size(), 0.01, 5);
    BleedMonitor other;
    const auto hissy = run(other, quiet, hiss);
    for (const BleedFrame& f : hissy) {
        CHECK(!f.bleed);
    }
}

TEST_CASE("bleed: an echo too quiet to hear is silence, whatever it is a copy of",
          "[pitch][bleed]") {
    const std::vector<float> played = backing(8.0, 8, true);
    const std::vector<float> mic = room(played, 100.0, 0.0001);  // -80 dB: under the floor
    BleedMonitor monitor;
    const auto frames = run(monitor, played, mic);
    for (const BleedFrame& f : frames) {
        CHECK(!f.bleed);
    }
}

TEST_CASE("bleed: an inverted echo is still the speakers", "[pitch][bleed]") {
    const std::vector<float> played = backing(8.0, 9, true);
    const std::vector<float> mic = sum(room(played, 120.0, -0.3), noise(played.size(), 0.0005, 4));
    BleedMonitor monitor;
    const auto frames = run(monitor, played, mic);
    REQUIRE(monitor.delay_ms().has_value());
    CHECK(*monitor.delay_ms() == Catch::Approx(120.0).margin(1.0));
    CHECK(share_bleed(frames, 2.0, 8.0) > 0.95);
}

TEST_CASE("bleed: a microphone a little ahead of the played stamps is found", "[pitch][bleed]") {
    // Two clocks that disagree by a few milliseconds put the echo before the played sample it is
    // of.
    const std::vector<float> played = backing(8.0, 10, true);
    const std::size_t ahead = 8 * kRate / 1000;  // 8 ms
    std::vector<float> mic(played.size(), 0.0F);
    for (std::size_t i = 0; i + ahead < played.size(); ++i) {
        mic[i] = 0.3F * played[i + ahead];
    }
    mic = sum(mic, noise(mic.size(), 0.0005, 5));
    BleedMonitor monitor;
    const auto frames = run(monitor, played, mic);
    REQUIRE(monitor.delay_ms().has_value());
    CHECK(*monitor.delay_ms() == Catch::Approx(-8.0).margin(1.0));
    CHECK(share_bleed(frames, 2.0, 7.5) > 0.95);
}

TEST_CASE("bleed: a room with reflections is still the speakers", "[pitch][bleed]") {
    const std::vector<float> played = backing(10.0, 31);
    const std::vector<float> mic =
        sum(room(played, 95.0, 0.25, 4), noise(played.size(), 0.0005, 6));
    BleedMonitor monitor;
    const auto frames = run(monitor, played, mic);
    REQUIRE(monitor.delay_ms().has_value());
    CHECK(*monitor.delay_ms() == Catch::Approx(95.0).margin(3.0));
    CHECK(share_bleed(frames, 2.0, 10.0) > 0.9);
}

TEST_CASE("bleed: speakers that play the guide give a silent singer no credit",
          "[pitch][bleed][scorer]") {
    // The reference is the melody itself, and the speakers play it with a backing: a singer who
    // sings nothing, in front of speakers 7 dB down, is credited with the melody by the tracker
    // alone (findings F6). With the monitor's frames withheld, nothing is.
    iclforge::pitch::VoiceOptions options;
    options.sample_rate = kRate;
    options.lead_s = 0.0;
    options.tail_s = 0.2;
    options.amplitude = 0.4;
    options.seed = 41;
    std::vector<iclforge::pitch::VoiceNote> notes;
    for (int i = 0; i < 16; ++i) {
        notes.push_back({0.6 * i, 0.5, 60.0 + (i % 4) * 2});
    }
    const std::vector<float> guide = iclforge::pitch::synthesize_voice(notes, options).samples;
    const std::vector<float> accompaniment = backing(10.0, 43);
    const std::vector<float> played = sum(guide, accompaniment, 0.3F);
    const std::vector<float> mic =
        sum(room(played, 120.0, 0.45, 2), noise(played.size(), 0.0005, 8));

    iclforge::pitch::ContourOptions contour_options;
    const auto reference = iclforge::pitch::contour_of_signal(guide, kRate, contour_options);
    REQUIRE(!reference.empty());

    auto tracker =
        iclforge::pitch::Tracker::make(iclforge::pitch::TrackerConfig::for_input_rate(kRate));
    REQUIRE(tracker.has_value());
    std::vector<iclforge::pitch::Frame> pitch_frames;
    tracker->push(mic, pitch_frames);

    BleedMonitor monitor;
    const auto bleed_frames = run(monitor, played, mic);
    REQUIRE(monitor.delay_ms().has_value());

    const auto score = [&](bool withhold) {
        iclforge::pitch::Scorer scorer(reference);
        std::size_t k = 0;
        for (const auto& f : pitch_frames) {
            while (k + 1 < bleed_frames.size() &&
                   bleed_frames[k].centre_sample < f.centre_sample - 80) {
                ++k;
            }
            const bool bleed = k < bleed_frames.size() &&
                               std::abs(bleed_frames[k].centre_sample - f.centre_sample) <= 80 &&
                               bleed_frames[k].bleed;
            // The speakers' sound is heard at the delay found, so a singer in time with it is
            // that far behind the item.
            const double item_ms = 1000.0 * static_cast<double>(f.centre_sample) / kRate -
                                   monitor.delay_ms().value_or(0.0);
            const float hz = (withhold && bleed) ? 0.0F : (f.voiced() ? f.hz : 0.0F);
            scorer.add(static_cast<std::int64_t>(std::llround(item_ms)), hz);
        }
        return scorer.total();
    };
    const auto credited = score(false);
    const auto withheld = score(true);
    REQUIRE(credited.counted > 100);
    // The tracker alone gives most of the tune to a singer who sang nothing...
    CHECK(credited.fraction() > 0.6);
    // ...and with the bleed withheld, hardly any.
    CHECK(withheld.fraction() < 0.1);
}
