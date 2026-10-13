#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "iclforge/base/wav.hpp"
#include "iclforge/pitch/reference.hpp"
#include "iclforge/pitch/synth.hpp"
#include "microphone_input.hpp"
#include "microphone_session.hpp"

// apps/hearth/engine/src/microphone_input.*: a recording as the microphone (VoiceFileInput, for
// `hearth --sing`), and the reading of the file it is made from. The input is sung along to the
// item as the item is HEARD, so what is held here is where each sample of the recording lands, not
// that something is delivered.

namespace {

using iclforge::hearth::InputBlock;
using iclforge::hearth::MicrophoneHealth;
using iclforge::hearth::MicrophoneSession;
using iclforge::hearth::VoiceFileInput;
using iclforge::hearth::VoiceRecording;
using iclforge::pitch::Note;

constexpr std::uint32_t kRate = 48000;
constexpr std::int64_t kT0 = 7'000'000'000'000LL;
constexpr std::int64_t kMs = 1'000'000LL;

// Every sample says where it is: a value no other sample has, so that a block says which frames of
// the recording it holds.
VoiceRecording ramp(std::size_t frames) {
    VoiceRecording r;
    r.sample_rate = kRate;
    r.name = "voice.wav";
    r.samples.resize(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        r.samples[i] = 0.1F + static_cast<float>(i) / 1.0e6F;
    }
    return r;
}

// The clock is a cell of its own, so that the input's reading of it is of the same time wherever
// the rig is moved to.
struct Rig {
    std::shared_ptr<std::int64_t> cell = std::make_shared<std::int64_t>(kT0);
    std::int64_t& now = *cell;
    std::unique_ptr<VoiceFileInput> input;
};

Rig make(std::size_t frames = kRate * 10) {
    Rig r;
    r.input = std::make_unique<VoiceFileInput>(ramp(frames), [cell = r.cell] { return *cell; });
    return r;
}

bool all_zero(const InputBlock& block) {
    return std::all_of(block.samples.begin(), block.samples.end(),
                       [](float s) { return s == 0.0F; });
}

}  // namespace

TEST_CASE("voice file input: it is one input, opens nothing but itself, and is closed until started",
          "[microphone][voice-file]") {
    Rig r = make();
    const auto devices = r.input->devices();
    REQUIRE(devices.size() == 1);
    CHECK(devices[0].name == "voice.wav");
    CHECK(devices[0].is_default);
    CHECK(devices[0].sample_rate == kRate);
    CHECK(r.input->channels() == 1);
    CHECK(!r.input->running());
    InputBlock block;
    r.now += 100 * kMs;
    CHECK(!r.input->read(block));
    REQUIRE(r.input->start("anything").has_value());
    CHECK(r.input->running());
    r.input->stop();
    CHECK(!r.input->running());
}

TEST_CASE("voice file input: before the item is heard it is quiet and never stalls",
          "[microphone][voice-file]") {
    Rig r = make();
    REQUIRE(r.input->start("").has_value());
    InputBlock block;
    // Under a frame since the start: nothing to deliver yet.
    r.now += 10'000;
    CHECK(!r.input->read(block));
    r.now = kT0 + 20 * kMs;
    REQUIRE(r.input->read(block));
    CHECK(block.samples.size() == 960);
    CHECK(block.first_time_ns == kT0);
    CHECK(!block.time_estimated);
    CHECK(all_zero(block));
    // And the next block begins where this one ended, whatever the timer's period was.
    r.now = kT0 + 47 * kMs;
    REQUIRE(r.input->read(block));
    CHECK(block.first_time_ns == kT0 + 20 * kMs);
    CHECK(block.samples.size() == 1296);
    CHECK(!r.input->origin_ns().has_value());
}

TEST_CASE("voice file input: a sample of the recording sounds when the item reaches it",
          "[microphone][voice-file]") {
    Rig r = make();
    // The input is started a quarter of a second after the item began to be heard.
    r.now = kT0 + 250 * kMs;
    REQUIRE(r.input->start("").has_value());
    // Half a second on the engine says the item is at 500 ms, which puts its time 0 at kT0 + 0:
    // here and not at the time play() was called, whatever the output added.
    r.now = kT0 + 500 * kMs;
    r.input->heard_position(500, true, r.now);
    REQUIRE(r.input->origin_ns().has_value());
    CHECK(*r.input->origin_ns() == kT0);

    r.now = kT0 + 600 * kMs;
    InputBlock block;
    REQUIRE(r.input->read(block));
    // The block is the whole time since the start, 350 ms, and begins at the 250 ms of the item:
    // frame 12000 of the recording, the one that is the item's 250 ms.
    CHECK(block.first_time_ns == kT0 + 250 * kMs);
    REQUIRE(block.samples.size() == 16800);
    const VoiceRecording expected = ramp(kRate * 10);
    CHECK(block.samples.front() == expected.samples[12000]);
    CHECK(block.samples[100] == expected.samples[12100]);
    CHECK(block.samples.back() == expected.samples[12000 + 16799]);
}

TEST_CASE("voice file input: a held item is a quiet room", "[microphone][voice-file]") {
    Rig r = make();
    REQUIRE(r.input->start("").has_value());
    r.now = kT0 + 400 * kMs;
    r.input->heard_position(400, true, r.now);
    r.now = kT0 + 500 * kMs;
    InputBlock block;
    REQUIRE(r.input->read(block));
    CHECK(!all_zero(block));
    // Paused at 500 ms: what arrives is silence, and keeps arriving.
    r.input->heard_position(500, false, r.now);
    r.now = kT0 + 900 * kMs;
    REQUIRE(r.input->read(block));
    CHECK(block.samples.size() == 19200);
    CHECK(all_zero(block));
    // Played again, and heard at 600 ms at kT0 + 1.3 s: the time held has gone by, so the item's
    // time 0 is now at kT0 + 700 ms, and the recording follows the item to there.
    r.now = kT0 + 1'300 * kMs;
    r.input->heard_position(600, true, r.now);
    REQUIRE(r.input->origin_ns().has_value());
    CHECK(*r.input->origin_ns() == kT0 + 700 * kMs);
    r.now = kT0 + 1'400 * kMs;
    REQUIRE(r.input->read(block));
    // The block begins at kT0 + 900 ms, which is the item's 200 ms by that origin.
    CHECK(block.first_time_ns == kT0 + 900 * kMs);
    CHECK(block.samples.front() == ramp(kRate * 10).samples[9600]);
}

TEST_CASE("voice file input: an item that is moved is followed, one that is reported late is not",
          "[microphone][voice-file]") {
    Rig r = make();
    REQUIRE(r.input->start("").has_value());
    r.now = kT0 + 1'000 * kMs;
    r.input->heard_position(1000, true, r.now);
    REQUIRE(r.input->origin_ns().has_value());
    CHECK(*r.input->origin_ns() == kT0);
    // The position is read a poll before it is told: the same item reads 30 ms behind, which
    // would put its time 0 30 ms later than it was. Not taken.
    r.now = kT0 + 1'100 * kMs;
    r.input->heard_position(1070, true, r.now);
    CHECK(*r.input->origin_ns() == kT0);
    // A reading that is earlier is closer, and is.
    r.now = kT0 + 1'200 * kMs;
    r.input->heard_position(1210, true, r.now);
    CHECK(*r.input->origin_ns() == kT0 - 10 * kMs);
    // The item moved on by a seek: the voice goes to where it is, which is what a singer does.
    r.now = kT0 + 1'300 * kMs;
    r.input->heard_position(6000, true, r.now);
    CHECK(*r.input->origin_ns() == kT0 + 1'300 * kMs - 6'000 * kMs);
}

TEST_CASE("voice file input: an item that has not been heard for 100 ms gives no origin",
          "[microphone][voice-file]") {
    Rig r = make();
    REQUIRE(r.input->start("").has_value());
    // "Playing" at 0 while the output starts is the engine's reading before its clock moves, and
    // an origin taken from it would be as early as the output was slow.
    for (std::int64_t at : {0, 20, 99}) {
        r.now = kT0 + 300 * kMs;
        r.input->heard_position(at, true, r.now);
        CHECK(!r.input->origin_ns().has_value());
    }
    r.input->heard_position(100, true, r.now);
    REQUIRE(r.input->origin_ns().has_value());
    CHECK(*r.input->origin_ns() == kT0 + 200 * kMs);
}

TEST_CASE("voice file input: past the end of the recording it is silent", "[microphone][voice-file]") {
    // 100 ms of recording.
    Rig r = make(4800);
    REQUIRE(r.input->start("").has_value());
    r.now = kT0 + 200 * kMs;
    r.input->heard_position(200, true, r.now);
    CHECK(*r.input->origin_ns() == kT0);
    r.now = kT0 + 300 * kMs;
    InputBlock block;
    REQUIRE(r.input->read(block));
    REQUIRE(block.samples.size() == 14400);
    CHECK(block.samples[0] == ramp(4800).samples[0]);
    CHECK(block.samples[4799] == ramp(4800).samples[4799]);
    CHECK(block.samples[4800] == 0.0F);
    CHECK(block.samples.back() == 0.0F);
}

TEST_CASE("voice file input: the part of a frame that is left over goes to the next block",
          "[microphone][voice-file]") {
    Rig r = make();
    REQUIRE(r.input->start("").has_value());
    // 100 us is 4.8 frames at 48 kHz: four are delivered, and the 0.8 is not lost.
    r.now = kT0 + 100'000;
    InputBlock block;
    REQUIRE(r.input->read(block));
    CHECK(block.first_time_ns == kT0);
    CHECK(block.samples.size() == 4);
    // 200 us in all is 9.6: the next block begins where the four frames ended (83333 ns) and has
    // the five that make nine, not the four that "100 us since the last read" would give.
    r.now = kT0 + 200'000;
    REQUIRE(r.input->read(block));
    CHECK(block.first_time_ns == kT0 + 83'333);
    CHECK(block.samples.size() == 5);
}

TEST_CASE("voice file input: a read after a long stall is a second, not the whole stall",
          "[microphone][voice-file]") {
    Rig r = make();
    REQUIRE(r.input->start("").has_value());
    r.now = kT0 + 60'000 * kMs;
    InputBlock block;
    REQUIRE(r.input->read(block));
    CHECK(block.samples.size() == kRate);
    CHECK(block.first_time_ns == kT0 + 59'000 * kMs);
}

TEST_CASE("voice file input: a start after a stop begins at the time it is, not where it left off",
          "[microphone][voice-file]") {
    Rig r = make();
    REQUIRE(r.input->start("").has_value());
    r.now = kT0 + 100 * kMs;
    InputBlock block;
    REQUIRE(r.input->read(block));
    r.input->stop();
    r.now = kT0 + 5'000 * kMs;
    REQUIRE(r.input->start("").has_value());
    r.now = kT0 + 5'020 * kMs;
    REQUIRE(r.input->read(block));
    CHECK(block.first_time_ns == kT0 + 5'000 * kMs);
    CHECK(block.samples.size() == 960);
}

TEST_CASE("voice file input: a voice sung along to an item is placed where the item was when it sounded",
          "[microphone][voice-file]") {
    const std::vector<Note> melody = {{0, 700, 60.0}, {800, 1500, 64.0}, {1700, 2400, 67.0}};
    std::vector<iclforge::pitch::VoiceNote> notes;
    for (const Note& n : melody) {
        notes.push_back({static_cast<double>(n.start_ms) / 1000.0,
                         static_cast<double>(n.end_ms - n.start_ms) / 1000.0, n.midi});
    }
    iclforge::pitch::VoiceOptions options;
    options.sample_rate = kRate;
    options.lead_s = 0.0;
    options.tail_s = 0.3;
    options.vibrato_cents = 20.0;
    options.snr_db = 30.0;
    options.seed = 5;
    VoiceRecording voice;
    voice.sample_rate = kRate;
    voice.name = "voice.wav";
    voice.samples = iclforge::pitch::synthesize_voice(notes, options).samples;

    std::int64_t now = kT0;
    auto input = std::make_unique<VoiceFileInput>(std::move(voice), [&now] { return now; });
    MicrophoneSession session(std::move(input));
    session.set_reference(iclforge::pitch::contour_of_notes(melody, 10, 2600));
    REQUIRE(session.start("").has_value());
    session.set_headphones(true);

    // play() is called at kT0 and the output is heard to start 380 ms later: a recording begun
    // with play() would be 380 ms early. The engine's position holds at 0 until then.
    constexpr std::int64_t kStartsAt = 380 * kMs;
    MicrophoneHealth health = MicrophoneHealth::kOk;
    for (now = kT0; now <= kT0 + 3'400 * kMs; now += 20 * kMs) {
        const std::int64_t heard = std::max<std::int64_t>(0, now - kT0 - kStartsAt);
        // Held from 2.9 s on, as a capture holds the item at its position.
        const std::int64_t position = std::min<std::int64_t>(heard, 2'500 * kMs) / kMs;
        const bool playing = now - kT0 < 2'900 * kMs;
        session.anchor(position, playing && heard > 0, now);
        health = session.pump(now);
        REQUIRE(health == MicrophoneHealth::kOk);
    }
    const auto s = session.snapshot();
    CHECK(s.total.counted > 150);
    CHECK(s.total.fraction() > 0.85);
    REQUIRE(s.lag_ms.has_value());
    CHECK(std::abs(*s.lag_ms) <= 40);
    CHECK(!s.timing_estimated);

    // Held for another four seconds: nothing arrives but the quiet, and the session does not take
    // it for a stalled device.
    const std::uint32_t counted = s.total.counted;
    for (now += 20 * kMs; now <= kT0 + 7'400 * kMs; now += 20 * kMs) {
        session.anchor(2'500, false, now);
        health = session.pump(now);
        REQUIRE(health == MicrophoneHealth::kOk);
    }
    CHECK(session.snapshot().total.counted == counted);
}

TEST_CASE("read voice file: a WAV file is one channel, the channels averaged, at its own rate",
          "[microphone][voice-file]") {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "iclforge-hearth-voice-file-test";
    std::filesystem::create_directories(dir);
    const std::filesystem::path wav = dir / "two.wav";
    const std::vector<std::vector<float>> channels = {{0.5F, 0.25F, -0.5F}, {0.1F, 0.75F, 0.5F}};
    REQUIRE(iclforge::base::write_wav_f32(wav.string(), channels, 44100).has_value());

    const auto read = iclforge::hearth::read_voice_file(wav);
    REQUIRE(read.has_value());
    CHECK(read->sample_rate == 44100);
    CHECK(read->name == "two.wav");
    REQUIRE(read->samples.size() == 3);
    CHECK(read->samples[0] == Catch::Approx(0.3F));
    CHECK(read->samples[1] == Catch::Approx(0.5F));
    CHECK(read->samples[2] == Catch::Approx(0.0F).margin(1e-6));

    // A file that is not a WAV file, and one that is not there, say which file and why.
    const std::filesystem::path text = dir / "words.wav";
    {
        std::ofstream out(text, std::ios::binary);
        out << "these are words, not audio";
    }
    const auto not_wav = iclforge::hearth::read_voice_file(text);
    REQUIRE(!not_wav.has_value());
    CHECK(not_wav.error().find("words.wav") != std::string::npos);
    const auto missing = iclforge::hearth::read_voice_file(dir / "none.wav");
    REQUIRE(!missing.has_value());
    CHECK(missing.error().find("none.wav") != std::string::npos);

    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
}
