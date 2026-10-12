#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "iclforge/pitch/reference.hpp"
#include "iclforge/pitch/synth.hpp"
#include "performance.hpp"

// apps/hearth/engine/src/performance.*: a singer's performance placed in an item and scored against
// a reference melody (planning/karaoke-microphone.md, M4). No device: the voice is synthetic, and
// the position and the capture times are made here.

namespace {

using iclforge::hearth::Performance;
using iclforge::hearth::PerformanceOptions;
using iclforge::pitch::Contour;
using iclforge::pitch::Note;
using iclforge::pitch::VoiceNote;

constexpr std::uint32_t kRate = 48000;
constexpr std::int64_t kT0 =
    7'000'000'000'000LL;  // a steady clock that has run for two hours, in ns

// Four notes, with a gap, over about 3.6 s.
std::vector<Note> melody() {
    return {{0, 700, 60.0}, {800, 1500, 64.0}, {1700, 2400, 67.0}, {2600, 3500, 72.0}};
}

Contour reference() {
    const auto notes = melody();
    return iclforge::pitch::contour_of_notes(notes, 10, 3700);
}

std::vector<float> voice_of(double shift_semitones = 0.0, double snr_db = 30.0) {
    std::vector<VoiceNote> notes;
    for (const Note& n : melody()) {
        notes.push_back({static_cast<double>(n.start_ms) / 1000.0,
                         static_cast<double>(n.end_ms - n.start_ms) / 1000.0,
                         n.midi + shift_semitones});
    }
    iclforge::pitch::VoiceOptions options;
    options.sample_rate = kRate;
    options.lead_s = 0.0;
    options.tail_s = 0.2;
    options.vibrato_cents = 20.0;
    options.snr_db = snr_db;
    options.seed = 21;
    return iclforge::pitch::synthesize_voice(notes, options).samples;
}

// What a microphone would deliver: blocks of `block` samples, the first of the whole stream stamped
// `first_time_ns`. Returns when the stream ends.
void feed_all(Performance& performance, const std::vector<float>& samples,
              std::int64_t first_time_ns, std::size_t block = 480) {
    for (std::size_t at = 0; at < samples.size(); at += block) {
        const std::size_t n = std::min(block, samples.size() - at);
        const std::int64_t time =
            first_time_ns + static_cast<std::int64_t>(at) * 1'000'000'000LL / kRate;
        performance.feed(std::span<const float>(samples).subspan(at, n), time);
    }
}

Performance make(const PerformanceOptions& options = {}) {
    PerformanceOptions o = options;
    o.input_rate = kRate;
    auto performance = Performance::make(reference(), o);
    REQUIRE(performance.has_value());
    return std::move(*performance);
}

}  // namespace

TEST_CASE("performance: a singer in time with the item scores well", "[performance]") {
    Performance performance = make();
    performance.anchor(0, kT0, 1.0);  // the item starts at kT0 and plays
    feed_all(performance, voice_of(), kT0);

    const auto total = performance.total();
    CHECK(total.counted == 300);
    CHECK(total.fraction() > 0.85);
    CHECK(performance.unplaced_frames() == 0);
    // The tracker's window is 64 ms, so a voiced stretch begins about 10 ms before its note and
    // runs about 30 ms past its end, and the first 30 ms of an item cannot be seen at all: the lag
    // is known to within a few tens of milliseconds, which is what it is shown to.
    REQUIRE(performance.lag_ms().has_value());
    CHECK(std::abs(*performance.lag_ms()) <= 40);
}

TEST_CASE("performance: a microphone that stamps late looks late until its delay is declared",
          "[performance]") {
    // The voice reaches the stamps 130 ms after it was sung (the measured webcam path's order).
    constexpr std::int64_t kDelayNs = 130'000'000;
    double undeclared = 0.0;
    {
        Performance performance = make();  // delay not declared
        performance.anchor(0, kT0, 1.0);
        feed_all(performance, voice_of(), kT0 + kDelayNs);
        undeclared = performance.total().fraction();
        // 130 ms behind a melody of notes 700 ms long: it loses the head of every note.
        CHECK(undeclared < 0.9);
        REQUIRE(performance.lag_ms().has_value());
        CHECK(*performance.lag_ms() >= 110);
        CHECK(*performance.lag_ms() <= 170);
    }
    {
        PerformanceOptions options;
        options.input_delay_ms = 130;
        Performance performance = make(options);
        performance.anchor(0, kT0, 1.0);
        feed_all(performance, voice_of(), kT0 + kDelayNs);
        // Declaring the delay gives back what it cost.
        CHECK(performance.total().fraction() > 0.85);
        CHECK(performance.total().fraction() > undeclared + 0.08);
        REQUIRE(performance.lag_ms().has_value());
        CHECK(std::abs(*performance.lag_ms()) <= 40);
    }
}

TEST_CASE("performance: the person's own offset moves the alignment", "[performance]") {
    // The singer is 200 ms behind the words, and says so.
    constexpr std::int64_t kBehindNs = 200'000'000;
    PerformanceOptions options;
    options.offset_ms = -200;
    Performance performance = make(options);
    performance.anchor(0, kT0, 1.0);
    feed_all(performance, voice_of(), kT0 + kBehindNs);
    CHECK(performance.total().fraction() > 0.85);
}

TEST_CASE("performance: the same voice in any block size scores the same", "[performance]") {
    const auto voice = voice_of();
    Performance a = make();
    a.anchor(0, kT0, 1.0);
    feed_all(a, voice, kT0, 480);
    Performance b = make();
    b.anchor(0, kT0, 1.0);
    feed_all(b, voice, kT0, 4096);
    Performance c = make();
    c.anchor(0, kT0, 1.0);
    feed_all(c, voice, kT0, 97);
    CHECK(a.total().hit == b.total().hit);
    CHECK(a.total().hit == c.total().hit);
    CHECK(a.total().sung == b.total().sung);
}

TEST_CASE("performance: nothing is scored before the first anchor or while the item is held",
          "[performance]") {
    SECTION("no anchor yet") {
        Performance performance = make();
        feed_all(performance, voice_of(), kT0);
        CHECK(performance.total().sung == 0);
        CHECK(performance.unplaced_frames() > 200);
        CHECK(!performance.live().has_value());
    }
    SECTION("paused for a second in the middle") {
        Performance performance = make();
        performance.anchor(0, kT0, 1.0);
        // Held at 1,000 ms from kT0 + 1 s to kT0 + 2 s, then on.
        performance.anchor(1000, kT0 + 1'000'000'000LL, 0.0);
        performance.anchor(1000, kT0 + 2'000'000'000LL, 1.0);
        feed_all(performance, voice_of(), kT0);
        // The second of voice sung while it was held was not placed anywhere.
        CHECK(performance.unplaced_frames() > 80);
        CHECK(performance.unplaced_frames() < 130);
        // The first second is still scored against the first second of the melody.
        const auto first = performance.tally(0, 1000);
        CHECK(first.counted > 60);
        CHECK(first.fraction() > 0.8);
    }
}

TEST_CASE("performance: a seek places the voice at the new position", "[performance]") {
    Performance performance = make();
    performance.anchor(0, kT0, 1.0);
    performance.anchor(2000, kT0 + 1'000'000'000LL,
                       1.0);  // at one second in, the item jumped to 2 s
    const auto voice = voice_of();
    // Feed up to 1.5 s of the stream.
    const std::vector<float> head(voice.begin(),
                                  voice.begin() + static_cast<std::ptrdiff_t>(kRate * 3 / 2));
    feed_all(performance, head, kT0);
    const auto live = performance.live();
    REQUIRE(live.has_value());
    // The latest frame's centre is 32 ms before the end of the fed audio, 1.5 s - 32 ms in: at the
    // new anchor that is 2,000 + 468 ms.
    CHECK(live->item_ms == Catch::Approx(2468).margin(15));
}

TEST_CASE("performance: a gap in the capture times places what follows by its own time",
          "[performance]") {
    Performance performance = make();
    performance.anchor(0, kT0, 1.0);
    const auto voice = voice_of();
    // The first second as it came; then the device stalled for 300 ms and the next block arrived
    // stamped 300 ms later than its place in the stream says.
    const std::size_t split = kRate;
    feed_all(performance,
             std::vector<float>(voice.begin(), voice.begin() + static_cast<std::ptrdiff_t>(split)),
             kT0);
    const std::int64_t resumed = kT0 + 1'000'000'000LL + 300'000'000LL;
    // Ten blocks, 100 ms: enough that a window is centred inside the part that followed the stall.
    for (std::size_t k = 0; k < 10; ++k) {
        const std::size_t at = split + k * 480;
        performance.feed(std::span<const float>(voice).subspan(at, 480),
                         resumed + static_cast<std::int64_t>(k * 480) * 1'000'000'000LL / kRate);
    }
    const auto live = performance.live();
    REQUIRE(live.has_value());
    // The resumed part starts at 1,300 ms in the item; the latest window ends 100 ms into it, and
    // its centre is 32 ms before that (less the up to 10 ms the hop leaves).
    CHECK(live->item_ms >= 1300 + 100 - 32 - 10);
    CHECK(live->item_ms <= 1300 + 100 - 32);
}

TEST_CASE("performance: the live view says how far from the reference the singer is",
          "[performance]") {
    SECTION("on the note") {
        Performance performance = make();
        performance.anchor(0, kT0, 1.0);
        // Feed the first 400 ms: inside the first note.
        const auto voice = voice_of();
        const std::vector<float> head(voice.begin(),
                                      voice.begin() + static_cast<std::ptrdiff_t>(kRate * 4 / 10));
        feed_all(performance, head, kT0);
        const auto live = performance.live();
        REQUIRE(live.has_value());
        CHECK(live->voiced);
        CHECK(live->reference_hz == Catch::Approx(261.63F).margin(0.5));
        REQUIRE(live->cents_from_reference.has_value());
        CHECK(std::fabs(*live->cents_from_reference) < 40.0);
    }
    SECTION("sharp by a tone and a half") {
        Performance performance = make();
        performance.anchor(0, kT0, 1.0);
        const auto voice = voice_of(1.5);
        const std::vector<float> head(voice.begin(),
                                      voice.begin() + static_cast<std::ptrdiff_t>(kRate * 4 / 10));
        feed_all(performance, head, kT0);
        const auto live = performance.live();
        REQUIRE(live.has_value());
        REQUIRE(live->cents_from_reference.has_value());
        CHECK(*live->cents_from_reference == Catch::Approx(150.0).margin(40.0));
    }
    SECTION("silence") {
        Performance performance = make();
        performance.anchor(0, kT0, 1.0);
        feed_all(performance, std::vector<float>(kRate, 0.0F), kT0);
        const auto live = performance.live();
        REQUIRE(live.has_value());
        CHECK(!live->voiced);
        CHECK(!live->cents_from_reference.has_value());
        CHECK(performance.total().sung == 0);
    }
}

TEST_CASE("performance: interleaved channels are mixed to one", "[performance]") {
    Performance performance = make();
    performance.anchor(0, kT0, 1.0);
    const auto mono = voice_of();
    std::vector<float> stereo(mono.size() * 2);
    for (std::size_t i = 0; i < mono.size(); ++i) {
        stereo[2 * i] = mono[i];
        stereo[2 * i + 1] = mono[i];
    }
    for (std::size_t at = 0; at < mono.size(); at += 480) {
        const std::size_t n = std::min<std::size_t>(480, mono.size() - at);
        performance.feed_interleaved(std::span<const float>(stereo).subspan(at * 2, n * 2), 2,
                                     kT0 + static_cast<std::int64_t>(at) * 1'000'000'000LL / kRate);
    }
    CHECK(performance.total().fraction() > 0.85);
}

TEST_CASE("performance: clearing the singer keeps the reference and starts over", "[performance]") {
    Performance performance = make();
    performance.anchor(0, kT0, 1.0);
    feed_all(performance, voice_of(), kT0);
    REQUIRE(performance.total().hit > 0);
    performance.clear_singer();
    CHECK(performance.total().sung == 0);
    CHECK(performance.total().counted == 300);
    CHECK(!performance.live().has_value());
    // And it scores again from a new take: the playhead is the engine's, so the same anchor holds.
    feed_all(performance, voice_of(), kT0);
    CHECK(performance.total().fraction() > 0.85);
}

TEST_CASE(
    "performance: a reference with no note in it, or a rate that cannot be tracked, is refused",
    "[performance]") {
    Contour empty;
    CHECK(!Performance::make(empty).has_value());

    Contour silent;
    silent.hz.assign(100, 0.0F);
    CHECK(!Performance::make(silent).has_value());

    PerformanceOptions bad;
    bad.input_rate = 0;
    CHECK(!Performance::make(reference(), bad).has_value());
}
