#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <numbers>
#include <thread>
#include <vector>

#include "iclforge/audio/capture.hpp"
#include "iclforge/audio/capture_timeline.hpp"
#include "iclforge/audio/monitor.hpp"

// Capture's stamps and timing against a real device (libs/audio/src/backend/*/capture.cpp;
// planning/karaoke-microphone.md, MI6).
//
// Hidden: the tag starts with a dot, so `iclforge-tests` does not run this. It needs a sound card
// and plays about a second of quiet tone through the default output. It captures that output in
// LOOPBACK, not a microphone: it opens no input device and records nothing but a count and times.
//
// Run it deliberately:  iclforge-audio-tests "[capture-live]"

namespace {

constexpr std::uint32_t kRate = 48'000;
constexpr std::uint16_t kChannels = 2;

}  // namespace

TEST_CASE(
    "capture live: stamps arrive in order, on the steady clock, and the timeline finds the rate",
    "[.][capture-live]") {
    using iclforge::audio::Capture;
    using iclforge::audio::CaptureStamp;
    using iclforge::audio::DeviceKind;

    iclforge::audio::MonitorSink sink;
    const auto sink_started = sink.start(/*device_id=*/"", kRate, kChannels);
    if (!sink_started) {
        WARN("no default output device: " << iclforge::audio::describe(sink_started.error()));
        return;
    }

    Capture capture;
    iclforge::audio::CaptureOptions options;
    options.period_frames = 0;
    const auto started = capture.start("", DeviceKind::kLoopback, options);
    if (!started) {
        WARN("no loopback capture: " << iclforge::audio::describe(started.error()));
        return;
    }
    if (capture.stamps() == nullptr) {
        WARN("this platform's capture gives no stamps");
        capture.stop();
        return;
    }

    // A quiet tone, so that something plays and the loopback has packets to stamp.
    double phase = 0.0;
    const double step = 2.0 * std::numbers::pi * 440.0 / kRate;
    std::vector<float> chunk(480 * kChannels);
    iclforge::audio::CaptureTimeline timeline(capture.sample_rate());
    std::vector<CaptureStamp> seen;
    const auto begin = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(1500)) {
        for (std::size_t frame = 0; frame < 480; ++frame) {
            const auto s = static_cast<float>(0.03 * std::sin(phase));
            phase += step;
            for (std::uint16_t c = 0; c < kChannels; ++c) {
                chunk[frame * kChannels + c] = s;
            }
        }
        for (int attempt = 0; attempt < 100 && !sink.submit(chunk); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CaptureStamp stamp;
        while (capture.stamps()->read(std::span<CaptureStamp>(&stamp, 1)) == 1) {
            seen.push_back(stamp);
            timeline.add(stamp);
        }
    }
    const auto end_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    capture.stop();
    sink.stop();

    REQUIRE(seen.size() > 20);
    for (std::size_t i = 1; i < seen.size(); ++i) {
        CHECK(seen[i].frame > seen[i - 1].frame);
        CHECK(seen[i].time_ns >= seen[i - 1].time_ns);
    }
    // On the steady clock: within a couple of seconds of now (a loopback stamp is a play-out time,
    // which is a little ahead of the packet's arrival).
    CHECK(std::llabs(seen.back().time_ns - end_ns) < 2'000'000'000LL);
    const auto rate = timeline.measured_rate();
    REQUIRE(rate.has_value());
    CHECK(std::fabs(*rate - static_cast<double>(capture.sample_rate())) <
          0.01 * capture.sample_rate());

    const auto timing = capture.timing();
    INFO("period " << timing.period_frames << " frames, buffer " << timing.buffer_frames
                   << " frames");
    CHECK(timing.buffer_frames > 0);
}

// This one opens the default INPUT (a microphone) for about a second, reads only the stamps and the
// period the platform gave, and stores no audio. It is its own tag, so that opening a microphone is
// never a side effect of `[capture-live]`.
//
// Run it deliberately:  iclforge-audio-tests "[capture-live-input]"
TEST_CASE("capture live input: a requested period is given or falls back, and stamps arrive",
          "[.][capture-live-input]") {
    using iclforge::audio::Capture;
    using iclforge::audio::CaptureStamp;
    using iclforge::audio::DeviceKind;

    for (const std::uint32_t request : {0U, 480U, 128U}) {
        Capture capture;
        iclforge::audio::CaptureOptions options;
        options.period_frames = request;
        const auto started = capture.start("", DeviceKind::kInput, options);
        if (!started) {
            WARN("no default input: " << iclforge::audio::describe(started.error()));
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        std::size_t stamps = 0;
        CaptureStamp stamp;
        if (capture.stamps() != nullptr) {
            while (capture.stamps()->read(std::span<CaptureStamp>(&stamp, 1)) == 1) {
                ++stamps;
            }
        }
        const auto timing = capture.timing();
        INFO("requested " << request << " frames: period " << timing.period_frames << ", buffer "
                          << timing.buffer_frames << ", " << stamps << " stamps in 0.7 s");
        capture.stop();
        CHECK(timing.buffer_frames > 0);
        CHECK(timing.period_frames > 0);
        CHECK(stamps > 20);
        UNSCOPED_INFO("requested " << request << ": period " << timing.period_frames
                                   << " frames, buffer " << timing.buffer_frames << " frames, "
                                   << stamps << " stamps");
    }
}
