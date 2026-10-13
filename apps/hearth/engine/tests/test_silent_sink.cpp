#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "iclforge/render/layout.hpp"
#include "pcm_sink.hpp"
#include "silent_sink.hpp"

// apps/hearth/engine/src/silent_sink.*: a sink with a device's behaviour and no device, for a run
// nobody listens to (`hearth --sing`). The test says what time it is.

namespace {

using iclforge::hearth::OutputMode;
using iclforge::hearth::PcmSink;

constexpr std::int64_t kT0 = 3'000'000'000'000LL;
constexpr std::int64_t kMs = 1'000'000LL;

struct Rig {
    std::int64_t now = kT0;
    std::unique_ptr<PcmSink> sink;
    std::vector<float> left = std::vector<float>(48000, 0.0F);
    std::vector<float> right = std::vector<float>(48000, 0.0F);

    Rig() {
        std::int64_t* clock = &now;
        sink = iclforge::hearth::make_silent_sink("Speakers", [clock] { return *clock; });
    }

    [[nodiscard]] PcmSink::Format format(std::uint32_t rate = 48000) const {
        PcmSink::Format f;
        f.sample_rate = rate;
        f.layout = *iclforge::render::OutputLayout::parse("2.0");
        return f;
    }

    bool submit(std::size_t frames) {
        const std::span<const float> slots[] = {std::span<const float>(left),
                                                std::span<const float>(right)};
        return sink->submit(slots, frames);
    }
};

}  // namespace

TEST_CASE("silent sink: it is a device with a name and no device", "[silent-sink]") {
    Rig r;
    CHECK(!r.sink->is_open());
    CHECK(!r.sink->position().has_value());
    CHECK(!r.submit(480));
    const auto opened = r.sink->open(r.format());
    REQUIRE(opened.has_value());
    CHECK(opened->sample_rate == 48000);
    CHECK(opened->channels == 2);
    CHECK(opened->mode == OutputMode::kLocalPcm);
    CHECK(r.sink->is_open());
    CHECK(r.sink->device_name() == "Speakers");
    CHECK(r.sink->speaker_mask() != 0);
    r.sink->close();
    CHECK(!r.sink->is_open());
    CHECK(!r.sink->position().has_value());
}

TEST_CASE("silent sink: the clock begins with the first block and runs at the rate", "[silent-sink]") {
    Rig r;
    REQUIRE(r.sink->open(r.format()).has_value());
    // Opened and not fed: nothing has played, however long it has been open, so the position of
    // an item is never ahead of its first sample by the time the engine took to decode it.
    r.now += 500 * kMs;
    auto position = r.sink->position();
    REQUIRE(position.has_value());
    CHECK(position->frames_played == 0);
    CHECK(position->frames_queued == 0);

    REQUIRE(r.submit(4800));
    position = r.sink->position();
    CHECK(position->frames_played == 0);
    CHECK(position->frames_queued == 4800);
    // 40 ms on: 1920 frames have played, the rest are held.
    r.now += 40 * kMs;
    position = r.sink->position();
    CHECK(position->frames_played == 1920);
    CHECK(position->frames_queued == 4800 - 1920);
    CHECK(position->latency_frames == 0);
    // And a second block follows the first.
    REQUIRE(r.submit(4800));
    r.now += 100 * kMs;
    position = r.sink->position();
    CHECK(position->frames_played == 1920 + 4800);
    CHECK(position->frames_queued == 9600 - (1920 + 4800));
}

TEST_CASE("silent sink: it holds a quarter of a second and refuses more until it has played some",
          "[silent-sink]") {
    Rig r;
    REQUIRE(r.sink->open(r.format()).has_value());
    REQUIRE(r.submit(6000));
    REQUIRE(r.submit(6000));
    // 12000 frames is a quarter of a second: full.
    CHECK(!r.submit(1));
    r.now += 10 * kMs;  // 480 frames played
    CHECK(!r.submit(481));
    CHECK(r.submit(480));
    // A refused block took nothing.
    const auto position = r.sink->position();
    REQUIRE(position.has_value());
    CHECK(position->frames_played + position->frames_queued == 12480);
}

TEST_CASE("silent sink: a device that is not fed plays on through the silence, as a device does",
          "[silent-sink]") {
    Rig r;
    REQUIRE(r.sink->open(r.format()).has_value());
    REQUIRE(r.submit(2400));
    r.now += 500 * kMs;
    const auto position = r.sink->position();
    REQUIRE(position.has_value());
    // The clock has run 24000 frames, 2400 of them the block's: the engine's drain test reads
    // exactly this ("the clock also runs through the silence an underrun inserts").
    CHECK(position->frames_played == 24000);
    CHECK(position->frames_queued == 0);
}

TEST_CASE("silent sink: a flush counts from zero again, and a pause does not play", "[silent-sink]") {
    Rig r;
    REQUIRE(r.sink->open(r.format()).has_value());
    REQUIRE(r.submit(4800));
    r.now += 50 * kMs;
    CHECK(r.sink->position()->frames_played == 2400);

    REQUIRE(r.sink->pause());
    r.now += 1'000 * kMs;
    CHECK(r.sink->position()->frames_played == 2400);
    REQUIRE(r.sink->resume());
    // The second held is not a second played.
    r.now += 20 * kMs;
    CHECK(r.sink->position()->frames_played == 2400 + 960);

    r.sink->flush();
    auto position = r.sink->position();
    REQUIRE(position.has_value());
    CHECK(position->frames_played == 0);
    CHECK(position->frames_queued == 0);
    // Flushed, it waits for the next block as an opened one does.
    r.now += 500 * kMs;
    CHECK(r.sink->position()->frames_played == 0);
    REQUIRE(r.submit(480));
    r.now += 5 * kMs;
    CHECK(r.sink->position()->frames_played == 240);
}

TEST_CASE("silent sink: the rate it was opened at is the rate of its clock", "[silent-sink]") {
    Rig r;
    const auto opened = r.sink->open(r.format(44100));
    REQUIRE(opened.has_value());
    CHECK(opened->sample_rate == 44100);
    REQUIRE(r.submit(4410));
    r.now += 100 * kMs;
    CHECK(r.sink->position()->frames_played == 4410);
}
