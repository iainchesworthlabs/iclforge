#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "iclforge/audio/capture_timeline.hpp"

// iclforge::audio::CaptureTimeline (capture_timeline.hpp): the time of any frame of a capture from
// the stamps its packets carried (planning/karaoke-microphone.md, MI6). Device-free: the stamps are
// made here, so every platform checks the same arithmetic whether or not its backend gives any.

namespace {

using iclforge::audio::CaptureStamp;
using iclforge::audio::CaptureTimeline;

constexpr std::int64_t kBase =
    5'000'000'000'000LL;  // a clock that has run for about an hour and a half

// A small repeatable jitter, in nanoseconds, of +-`span_ns`.
std::int64_t jitter(std::uint32_t& state, std::int64_t span_ns) {
    state = state * 1664525U + 1013904223U;
    const double unit = static_cast<double>(state >> 8) / 8388608.0 - 1.0;  // -1 .. 1
    return static_cast<std::int64_t>(unit * static_cast<double>(span_ns));
}

}  // namespace

TEST_CASE("capture timeline: nothing is known before the first stamp", "[capture-timeline]") {
    const CaptureTimeline timeline(48000);
    CHECK(!timeline.time_ns_of_frame(0).has_value());
    CHECK(!timeline.frame_at_time_ns(kBase).has_value());
    CHECK(!timeline.measured_rate().has_value());
    CHECK(timeline.size() == 0);
}

TEST_CASE("capture timeline: one stamp and the nominal rate give a frame's time",
          "[capture-timeline]") {
    CaptureTimeline timeline(48000);
    timeline.add({4800, kBase});
    REQUIRE(timeline.time_ns_of_frame(4800).has_value());
    CHECK(*timeline.time_ns_of_frame(4800) == kBase);
    // 480 frames on is 10 ms on.
    CHECK(*timeline.time_ns_of_frame(5280) == kBase + 10'000'000);
    // And a frame before the stamp extrapolates back.
    CHECK(*timeline.time_ns_of_frame(4320) == kBase - 10'000'000);
    CHECK(!timeline.measured_rate().has_value());
}

TEST_CASE("capture timeline: exact stamps are read back exactly", "[capture-timeline]") {
    CaptureTimeline timeline(48000);
    for (std::uint64_t i = 0; i < 100; ++i) {
        timeline.add({i * 480, kBase + static_cast<std::int64_t>(i) * 10'000'000});
    }
    CHECK(timeline.size() == CaptureTimeline::kCapacity);
    CHECK(*timeline.time_ns_of_frame(99 * 480) == kBase + 990'000'000);
    // Mid-packet, which no stamp names.
    CHECK(*timeline.time_ns_of_frame(50 * 480 + 240) == kBase + 505'000'000);
    CHECK(timeline.measured_rate().value() == Catch::Approx(48000.0).epsilon(1e-6));
    const auto frame = timeline.frame_at_time_ns(kBase + 505'000'000);
    REQUIRE(frame.has_value());
    CHECK(*frame == Catch::Approx(50.0 * 480 + 240).margin(0.01));
}

TEST_CASE("capture timeline: jitter is averaged out and the device's own rate is found",
          "[capture-timeline]") {
    // The device really runs at 48,060 Hz (0.125 % fast), its packets are 480 frames, and each
    // stamp is read up to 400 us early or late.
    constexpr double kActual = 48'060.0;
    CaptureTimeline timeline(48000);
    std::uint32_t state = 99;
    for (std::uint64_t i = 0; i < 200; ++i) {
        const double ideal_ns = static_cast<double>(i * 480) / kActual * 1e9;
        timeline.add(
            {i * 480, kBase + static_cast<std::int64_t>(ideal_ns) + jitter(state, 400'000)});
    }
    // The fit is over the last 64 stamps, frames 136*480 to 199*480.
    const std::uint64_t probe = 170 * 480 + 123;
    const double truth_ns = static_cast<double>(probe) / kActual * 1e9;
    const auto estimate = timeline.time_ns_of_frame(probe);
    REQUIRE(estimate.has_value());
    // Better than the jitter of one stamp (400 us), by a good margin.
    CHECK(std::llabs(*estimate - (kBase + static_cast<std::int64_t>(truth_ns))) < 150'000);
    CHECK(std::fabs(timeline.measured_rate().value() - kActual) < 40.0);
}

TEST_CASE("capture timeline: a stamp that says the clock is wildly off is not believed",
          "[capture-timeline]") {
    CaptureTimeline timeline(48000);
    // Every packet stamped with the same instant: a slope of zero, which is not a device.
    for (std::uint64_t i = 0; i < 20; ++i) {
        timeline.add({i * 480, kBase});
    }
    const auto t = timeline.time_ns_of_frame(10 * 480);
    REQUIRE(t.has_value());
    // The nominal slope through the mean of the stamps: 10 ms a packet, so frame 4800 is the
    // stamps' own mean frame (9.5 packets) plus half a packet.
    CHECK(std::llabs(*t - (kBase + 5'000'000)) < 100);
    CHECK(timeline.measured_rate().value() == Catch::Approx(48000.0));
}

TEST_CASE("capture timeline: the oldest stamps are forgotten and a step in the clock is followed",
          "[capture-timeline]") {
    CaptureTimeline timeline(48000);
    for (std::uint64_t i = 0; i < 64; ++i) {
        timeline.add({i * 480, kBase + static_cast<std::int64_t>(i) * 10'000'000});
    }
    // The clock jumps forward by a second between packets (a suspended device resumed).
    constexpr std::int64_t kStep = 1'000'000'000;
    for (std::uint64_t i = 64; i < 140; ++i) {
        timeline.add({i * 480, kBase + kStep + static_cast<std::int64_t>(i) * 10'000'000});
    }
    CHECK(timeline.size() == CaptureTimeline::kCapacity);
    CHECK(std::llabs(*timeline.time_ns_of_frame(139 * 480) - (kBase + kStep + 1'390'000'000)) <
          1000);
}

TEST_CASE("capture timeline: reset forgets everything", "[capture-timeline]") {
    CaptureTimeline timeline(44100);
    timeline.add({0, kBase});
    timeline.reset();
    CHECK(timeline.size() == 0);
    CHECK(!timeline.time_ns_of_frame(0).has_value());
}
