#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <numbers>
#include <string>
#include <system_error>
#include <vector>

#include "iclforge/pitch/reference.hpp"
#include "iclforge/pitch/synth.hpp"
#include "iclforge/sendspin/codec.hpp"
#include "iclforge/sendspin/messages.hpp"
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

Rig rig(std::vector<float> samples, std::int64_t first_time_ns = kT0, std::uint16_t channels = 1) {
    auto input = std::make_unique<ScriptedMicrophoneInput>(
        std::move(samples), kRate, first_time_ns,
        std::vector<InputDevice>{{"mic-a", "Headset", false, kRate, 1},
                                 {"mic-b", "Desk microphone", true, kRate, 2}},
        channels);
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

namespace {

// A folder of this test's own under the system's temporary one.
struct TakeFolder {
    std::filesystem::path path;
    TakeFolder() {
        path = std::filesystem::temp_directory_path() /
               ("iclforge-session-take-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }
    ~TakeFolder() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

std::string read_text(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The samples of a take's FLAC file, 24-bit integers.
std::vector<std::int32_t> decode_take(const std::filesystem::path& p, std::uint32_t rate) {
    std::ifstream in(p, std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                          std::istreambuf_iterator<char>());
    if (bytes.size() <= 42) {
        return {};
    }
    iclforge::sendspin::messages::PlayerStream stream;
    stream.format.codec = iclforge::sendspin::messages::Codec::kFlac;
    stream.format.channels = 1;
    stream.format.sample_rate = static_cast<std::int32_t>(rate);
    stream.format.bit_depth = 24;
    stream.codec_header.assign(bytes.begin(), bytes.begin() + 42);
    auto decoder = iclforge::sendspin::codec::make_decoder(stream);
    if (!decoder) {
        return {};
    }
    const auto decoded =
        decoder->decode(std::vector<std::uint8_t>(bytes.begin() + 42, bytes.end()));
    return decoded ? *decoded : std::vector<std::int32_t>{};
}

}  // namespace

TEST_CASE("microphone session: a take needs the microphone on", "[microphone][take]") {
    Rig r = rig(sine(440.0, 1.0, 0.1));
    TakeFolder folder;
    const auto refused = r.session->begin_take(folder.path / "x.flac", {});
    REQUIRE(!refused.has_value());
    CHECK(refused.error().find("off") != std::string::npos);
    CHECK(!r.session->taking());
}

TEST_CASE("microphone session: a take is the dry voice, and says where in the item it begins",
          "[microphone][take]") {
    // The voice reaches the stamps 130 ms after it was sung, and 130 ms is declared.
    Rig r = rig(voice(), kT0 + 130'000'000);
    r.session->set_input_delay_ms(130);
    r.session->set_offset_ms(20);
    TakeFolder folder;
    REQUIRE(r.session->start("").has_value());
    r.session->set_headphones(true);
    iclforge::hearth::TakeMetadata partial;
    partial.item_title = "Song";
    partial.item_path = "x/song.ac3";
    partial.reference = "the melody guide (M)";
    // The take begins half a second in: the stream has delivered 370 ms of the voice by then (its
    // first sample was stamped 130 ms after the item began).
    run(r, kT0, kT0 + 500'000'000);
    REQUIRE(r.session->begin_take(folder.path / "song.flac", partial).has_value());
    CHECK(r.session->taking());
    run(r, kT0 + 520'000'000, kT0 + 2'500'000'000);
    const auto snapshot = r.session->snapshot();
    CHECK(snapshot.taking);
    CHECK(snapshot.take_seconds > 1.5);

    const auto info = r.session->end_take();
    REQUIRE(info.has_value());
    CHECK(!r.session->taking());
    CHECK(info->frames > static_cast<std::uint64_t>(kRate) * 3 / 2);
    CHECK(info->sample_rate == kRate);

    // The sidecar: the item was where the voice's first written sample was sung, and the 20 ms the
    // person moved it is in the figure.
    const std::string json = read_text(info->sidecar);
    CHECK(json.find("\"input_delay_ms\": 130") != std::string::npos);
    CHECK(json.find("\"offset_ms\": 20") != std::string::npos);
    CHECK(json.find("\"headphones\": true") != std::string::npos);
    CHECK(json.find("\"reference\": \"the melody guide (M)\"") != std::string::npos);
    CHECK(json.find("\"item_start_ms\": null") == std::string::npos);
    // A voice sung at item time t is captured at t + 130 ms and placed by the declared delay at t.
    // The take's first sample is the 370 ms one (500 ms of the clock less the 130 ms), so it was
    // sung at item time 370 ms, and 390 ms with the person's 20 ms, to within a poll.
    const auto at = json.find("\"item_start_ms\": ");
    REQUIRE(at != std::string::npos);
    const long start = std::stol(json.substr(at + 17));
    CHECK(start >= 370 + 20 - 5);
    CHECK(start <= 370 + 20 + 5);
}

TEST_CASE("microphone session: closing the microphone ends a take and keeps it",
          "[microphone][take]") {
    Rig r = rig(voice());
    TakeFolder folder;
    REQUIRE(r.session->start("").has_value());
    run(r, kT0, kT0 + 100'000'000);
    REQUIRE(r.session->begin_take(folder.path / "kept.flac", {}).has_value());
    run(r, kT0 + 120'000'000, kT0 + 1'000'000'000);
    r.session->stop();
    CHECK(!r.session->taking());
    CHECK(std::filesystem::exists(folder.path / "kept.flac"));
    CHECK(std::filesystem::exists(folder.path / "kept.take.json"));
    CHECK(std::filesystem::file_size(folder.path / "kept.flac") > 1000);
}

TEST_CASE("microphone session: a stereo microphone is kept as the mean of its channels",
          "[microphone][take]") {
    // The left channel carries a tone of 0.4 and the right is silent: one channel of 0.2, not 0.4.
    const std::vector<float> left = sine(440.0, 1.0, 0.4);
    std::vector<float> stereo(left.size() * 2, 0.0F);
    for (std::size_t i = 0; i < left.size(); ++i) {
        stereo[2 * i] = left[i];
    }
    Rig r = rig(std::move(stereo), kT0, 2);
    TakeFolder folder;
    REQUIRE(r.session->start("").has_value());
    REQUIRE(r.session->begin_take(folder.path / "stereo.flac", {}).has_value());
    run(r, kT0, kT0 + 1'200'000'000);
    const auto info = r.session->end_take();
    REQUIRE(info.has_value());
    CHECK(info->frames == left.size());

    const std::vector<std::int32_t> decoded = decode_take(info->audio, kRate);
    REQUIRE(decoded.size() == left.size());
    std::int32_t peak = 0;
    for (const std::int32_t v : decoded) {
        peak = std::max(peak, std::abs(v));
    }
    CHECK(peak == Catch::Approx(0.2 * 8388607.0).epsilon(0.01));
}

TEST_CASE("microphone session: a discarded take is gone", "[microphone][take]") {
    Rig r = rig(voice());
    TakeFolder folder;
    REQUIRE(r.session->start("").has_value());
    REQUIRE(r.session->begin_take(folder.path / "gone.flac", {}).has_value());
    run(r, kT0, kT0 + 500'000'000);
    r.session->discard_take();
    CHECK(!r.session->taking());
    CHECK(!std::filesystem::exists(folder.path / "gone.flac"));
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
