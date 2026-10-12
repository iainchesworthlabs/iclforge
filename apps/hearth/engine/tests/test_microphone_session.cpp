#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <vector>

#include "iclforge/pitch/reference.hpp"
#include "iclforge/pitch/synth.hpp"
#include "microphone_input.hpp"
#include "microphone_session.hpp"

// apps/hearth/engine/src/microphone_session.*: a singer's microphone from the person turning it on
// to what it heard (planning/karaoke-microphone.md, M4). A scripted input stands in for the device
// and the test says what time it is.

namespace {

using iclforge::hearth::InputDevice;
using iclforge::hearth::MicrophoneHealth;
using iclforge::hearth::MicrophoneSession;
using iclforge::hearth::ScriptedMicrophoneInput;
using iclforge::pitch::Note;

constexpr std::uint32_t kRate = 48000;
constexpr std::int64_t kT0 = 9'000'000'000'000LL;

std::vector<float> sine(double hz, double seconds, double amplitude) {
    std::vector<float> x(static_cast<std::size_t>(seconds * kRate));
    for (std::size_t i = 0; i < x.size(); ++i) {
        x[i] = static_cast<float>(
            amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / kRate));
    }
    return x;
}

std::vector<Note> melody() {
    return {{0, 700, 60.0}, {800, 1500, 64.0}, {1700, 2400, 67.0}};
}

std::vector<float> voice() {
    std::vector<iclforge::pitch::VoiceNote> notes;
    for (const Note& n : melody()) {
        notes.push_back({static_cast<double>(n.start_ms) / 1000.0,
                         static_cast<double>(n.end_ms - n.start_ms) / 1000.0, n.midi});
    }
    iclforge::pitch::VoiceOptions options;
    options.sample_rate = kRate;
    options.lead_s = 0.0;
    options.vibrato_cents = 20.0;
    options.snr_db = 30.0;
    options.seed = 5;
    return iclforge::pitch::synthesize_voice(notes, options).samples;
}

struct Rig {
    ScriptedMicrophoneInput* input = nullptr;  // owned by the session
    std::unique_ptr<MicrophoneSession> session;
};

Rig rig(std::vector<float> samples, std::int64_t first_time_ns = kT0) {
    auto input = std::make_unique<ScriptedMicrophoneInput>(
        std::move(samples), kRate, first_time_ns,
        std::vector<InputDevice>{{"mic-a", "Headset", false, kRate, 1},
                                 {"mic-b", "Desk microphone", true, kRate, 2}});
    Rig r;
    r.input = input.get();
    r.session = std::make_unique<MicrophoneSession>(std::move(input));
    return r;
}

// Runs the clock from `from_ns` to `to_ns` in 20 ms steps: the input has captured up to then, the
// engine's position is where the item would be had it started at kT0, and the session is pumped.
MicrophoneHealth run(Rig& r, std::int64_t from_ns, std::int64_t to_ns, bool anchor = true) {
    MicrophoneHealth health = MicrophoneHealth::kOk;
    for (std::int64_t now = from_ns; now <= to_ns; now += 20'000'000) {
        r.input->set_now_ns(now);
        if (anchor) {
            r.session->anchor((now - kT0) / 1'000'000, true, now);
        }
        health = r.session->pump(now);
    }
    return health;
}

}  // namespace

TEST_CASE("microphone session: nothing is opened until it is asked", "[microphone]") {
    Rig r = rig(sine(440.0, 1.0, 0.1));
    CHECK(!r.session->open());
    CHECK(r.input->starts() == 0);
    CHECK(r.session->pump(kT0) == MicrophoneHealth::kNotOpen);
    CHECK(!r.session->snapshot().open);
    // Listing what there is opens nothing either.
    const auto devices = r.session->devices();
    REQUIRE(devices.size() == 2);
    CHECK(devices[1].is_default);
    CHECK(r.input->starts() == 0);
}

TEST_CASE("microphone session: start opens the chosen input and stop closes it", "[microphone]") {
    Rig r = rig(sine(440.0, 1.0, 0.1));
    REQUIRE(r.session->start("mic-a").has_value());
    CHECK(r.session->open());
    CHECK(r.input->last_device_id() == "mic-a");
    CHECK(r.input->running());
    CHECK(r.session->snapshot().sample_rate == kRate);
    r.session->stop();
    CHECK(!r.session->open());
    CHECK(!r.input->running());
    CHECK(r.session->pump(kT0) == MicrophoneHealth::kNotOpen);
}

TEST_CASE("microphone session: a device that will not open says why and stays closed",
          "[microphone]") {
    Rig r = rig(sine(440.0, 1.0, 0.1));
    r.input->refuse("The microphone is in use by another application.");
    const auto started = r.session->start("");
    REQUIRE(!started.has_value());
    CHECK(started.error() == "The microphone is in use by another application.");
    CHECK(!r.session->open());
}

TEST_CASE("microphone session: the meter reads the level and lets go", "[microphone]") {
    // One second of a sine of amplitude 0.1 (-23 dBFS rms), then silence.
    std::vector<float> samples = sine(440.0, 1.0, 0.1);
    samples.resize(kRate * 3, 0.0F);
    Rig r = rig(samples);
    REQUIRE(r.session->start("").has_value());
    run(r, kT0, kT0 + 900'000'000, false);
    CHECK(r.session->snapshot().level_db == Catch::Approx(-23.0).margin(1.5));
    CHECK(!r.session->snapshot().clipping);
    // Two seconds on, in silence: the meter has fallen by tens of dB, not stuck.
    run(r, kT0 + 920'000'000, kT0 + 2'900'000'000, false);
    CHECK(r.session->snapshot().level_db < -60.0F);
    CHECK(r.session->snapshot().silent_seconds > 1.0);
}

TEST_CASE("microphone session: a sample at full scale is clipping for a second", "[microphone]") {
    std::vector<float> samples(kRate * 3, 0.0F);
    samples[kRate / 2] = 1.0F;
    Rig r = rig(samples);
    REQUIRE(r.session->start("").has_value());
    run(r, kT0, kT0 + 700'000'000, false);
    CHECK(r.session->snapshot().clipping);
    run(r, kT0 + 720'000'000, kT0 + 2'900'000'000, false);
    CHECK(!r.session->snapshot().clipping);
}

TEST_CASE("microphone session: a voice on the melody is scored once there is a reference",
          "[microphone]") {
    Rig r = rig(voice());
    const auto notes = melody();
    r.session->set_reference(iclforge::pitch::contour_of_notes(notes, 10, 2600));
    REQUIRE(r.session->start("").has_value());
    r.session->set_headphones(true);
    run(r, kT0, kT0 + 2'700'000'000);

    const auto s = r.session->snapshot();
    CHECK(s.open);
    CHECK(!s.score_withheld);
    CHECK(s.total.counted > 140);
    CHECK(s.total.fraction() > 0.85);
    CHECK(s.live.has_value());
    CHECK(!s.timing_estimated);
    REQUIRE(s.lag_ms.has_value());
    CHECK(std::abs(*s.lag_ms) <= 40);
}

TEST_CASE(
    "microphone session: without the person saying they wear headphones the score is withheld",
    "[microphone]") {
    Rig r = rig(voice());
    r.session->set_reference(iclforge::pitch::contour_of_notes(melody(), 10, 2600));
    REQUIRE(r.session->start("").has_value());
    run(r, kT0, kT0 + 2'700'000'000);
    const auto s = r.session->snapshot();
    CHECK(s.score_withheld);
    CHECK(!s.headphones);
    // The number is there to be shown once it is trusted; the page decides, and says why.
    CHECK(s.total.fraction() > 0.85);
}

TEST_CASE("microphone session: a reference that arrives after the start scores from then on",
          "[microphone]") {
    Rig r = rig(voice());
    REQUIRE(r.session->start("").has_value());
    run(r, kT0, kT0 + 1'000'000'000);
    CHECK(r.session->snapshot().total.counted == 0);
    CHECK(!r.session->has_reference());
    r.session->set_reference(iclforge::pitch::contour_of_notes(melody(), 10, 2600));
    CHECK(r.session->has_reference());
    run(r, kT0 + 1'020'000'000, kT0 + 2'700'000'000);
    const auto s = r.session->snapshot();
    CHECK(s.total.counted > 140);
    // Only what was sung after it arrived is held against it: the second note and the third.
    CHECK(s.total.sung > 40);
    CHECK(s.total.sung < s.total.counted);
}

TEST_CASE("microphone session: the person's alignment reaches the score", "[microphone]") {
    // The voice reaches the stamps 130 ms late.
    Rig r = rig(voice(), kT0 + 130'000'000);
    r.session->set_reference(iclforge::pitch::contour_of_notes(melody(), 10, 2600));
    REQUIRE(r.session->start("").has_value());
    r.session->set_input_delay_ms(130);
    run(r, kT0, kT0 + 2'900'000'000);
    const auto s = r.session->snapshot();
    CHECK(s.input_delay_ms == 130);
    CHECK(s.total.fraction() > 0.85);
}

TEST_CASE("microphone session: an alignment set before the start is the one it starts with", "[microphone]") {
    // The delay and the person's offset are preferences, read before anything is opened.
    Rig r = rig(voice(), kT0 + 130'000'000);
    r.session->set_input_delay_ms(130);
    r.session->set_reference(iclforge::pitch::contour_of_notes(melody(), 10, 2600));
    REQUIRE(r.session->start("").has_value());
    run(r, kT0, kT0 + 2'900'000'000);
    const auto s = r.session->snapshot();
    CHECK(s.input_delay_ms == 130);
    CHECK(s.total.fraction() > 0.85);
}

TEST_CASE("microphone session: a device that goes away or stalls is reported", "[microphone]") {
    SECTION("unplugged") {
        Rig r = rig(sine(440.0, 5.0, 0.1));
        REQUIRE(r.session->start("").has_value());
        CHECK(run(r, kT0, kT0 + 500'000'000, false) == MicrophoneHealth::kOk);
        r.input->unplug();
        CHECK(r.session->pump(kT0 + 600'000'000) == MicrophoneHealth::kGone);
    }
    SECTION("stalled") {
        Rig r = rig(sine(440.0, 10.0, 0.1));
        REQUIRE(r.session->start("").has_value());
        CHECK(run(r, kT0, kT0 + 500'000'000, false) == MicrophoneHealth::kOk);
        r.input->stall(true);
        CHECK(r.session->pump(kT0 + 2'000'000'000) == MicrophoneHealth::kOk);
        CHECK(r.session->pump(kT0 + 4'000'000'000) == MicrophoneHealth::kStalled);
    }
}
