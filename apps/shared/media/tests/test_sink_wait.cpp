#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>
#include <vector>

#include "iclforge/audio/monitor.hpp"
#include "sink_wait.hpp"

// apps/shared/media/src/sink_wait.hpp: how forge's play, monitor, identify and
// live wait on a sink, and how the GUI's players wait for a clip to play out.
// None of those reaches its wait without a render device, and a device cannot
// be pulled on a CI runner, so these cases hold the waits against sinks that
// refuse and stop on cue. That a real sink stops itself when its device goes
// is the backends' part, and the hidden "[passthrough-unplug]" and
// "[monitor-unplug]" cases are where a person checks it.

using namespace std::chrono_literals;

namespace {

// A sink with a queue that is full for `full_offers` offers and then has
// room, and a device that goes away at offer `gone_at_offer` (0: never). Once
// gone it refuses everything, as a PassthroughSink that stopped itself does.
struct ScriptedSink {
    int full_offers = 0;
    int gone_at_offer = 0;
    int offers = 0;
    bool gone = false;
    std::vector<int> taken{};
    std::vector<std::size_t> frames_taken{};

    bool submit(int payload, std::size_t frames = 1) {
        ++offers;
        if (offers == gone_at_offer) {
            gone = true;
        }
        if (gone) {
            return false;
        }
        if (full_offers > 0) {
            --full_offers;
            return false;
        }
        taken.push_back(payload);
        frames_taken.push_back(frames);
        return true;
    }

    [[nodiscard]] bool running() const { return !gone; }
};

// A sink whose render thread stops it, as every backend's does when its
// device goes. Its queue never has room, which is what a caller waiting on a
// dead device sees until running() says why.
struct RenderThreadSink {
    std::atomic<bool> alive{true};

    bool submit(int /*payload*/) { return false; }

    [[nodiscard]] bool running() const { return alive.load(std::memory_order_acquire); }
};

// A sink that plays out what it was handed, one step per look at it: every call
// of stats() or position() moves its device on by `step` frames, so a case
// does not depend on how fast the machine runs. The queue drains into the
// device a step at a time, and the device plays what it holds a step behind.
// `silent` is a device that stops answering with the sink still running, and
// `gone_at_look` one that goes away (0: never).
struct PlayoutSink {
    std::uint64_t submitted = 0;
    std::uint32_t latency = 0;
    std::uint64_t step = 100;
    bool silent = false;
    bool answers_position = true;
    int gone_at_look = 0;

    mutable std::uint64_t rendered = 0;
    mutable std::uint64_t played = 0;
    mutable int looks = 0;
    mutable bool gone = false;

    void advance() const {
        if (++looks == gone_at_look) {
            gone = true;
        }
        if (gone || silent) {
            return;
        }
        played = std::min(rendered, played + step);
        rendered = std::min(submitted, rendered + step);
    }

    [[nodiscard]] iclforge::audio::MonitorStats stats() const {
        advance();
        return {.frames_submitted = submitted, .frames_rendered = rendered};
    }

    [[nodiscard]] std::optional<iclforge::audio::MonitorPosition> position() const {
        advance();
        if (gone || !answers_position) {
            return std::nullopt;
        }
        return iclforge::audio::MonitorPosition{
            .frames_played = played,
            .frames_queued = (submitted - rendered) + (rendered - played),
            .latency_frames = latency};
    }

    [[nodiscard]] bool running() const { return !gone; }
};

}  // namespace

TEST_CASE("sink wait: a full queue is waited out until the sink takes the payload",
          "[sink-wait][concurrency]") {
    ScriptedSink sink{.full_offers = 3};
    CHECK(iclforge::apps::submit_while_running(sink, 1ms, 7));
    CHECK(sink.offers == 4);
    CHECK(sink.taken == std::vector<int>{7});
}

TEST_CASE("sink wait: a sink that stops itself ends the wait with nothing queued",
          "[sink-wait][concurrency]") {
    // The queue stays full for longer than the test runs: only running()
    // going false can end this wait.
    ScriptedSink sink{.full_offers = 1'000'000, .gone_at_offer = 3};
    CHECK_FALSE(iclforge::apps::submit_while_running(sink, 1ms, 7));
    CHECK(sink.offers == 3);
    CHECK(sink.taken.empty());
}

TEST_CASE("sink wait: a sink already stopped is offered the payload once",
          "[sink-wait][concurrency]") {
    ScriptedSink sink{.gone = true};
    CHECK_FALSE(iclforge::apps::submit_while_running(sink, 1ms, 7));
    CHECK(sink.offers == 1);
}

TEST_CASE("sink wait: every argument reaches submit", "[sink-wait][concurrency]") {
    // PcmOutput::submit takes the rendered channels and a frame count.
    ScriptedSink sink{.full_offers = 1};
    CHECK(iclforge::apps::submit_while_running(sink, 1ms, 7, std::size_t{480}));
    CHECK(sink.taken == std::vector<int>{7});
    CHECK(sink.frames_taken == std::vector<std::size_t>{480});
}

TEST_CASE("sink wait: waiting for the queue to play out ends once it has",
          "[sink-wait][concurrency]") {
    const ScriptedSink sink;
    int looks = 0;
    CHECK(iclforge::apps::wait_while_running(sink, 1ms, [&looks] { return ++looks == 3; }));
    CHECK(looks == 3);
}

TEST_CASE("sink wait: waiting for the queue to play out ends when the sink stops itself",
          "[sink-wait][concurrency]") {
    ScriptedSink sink;
    int looks = 0;
    CHECK_FALSE(iclforge::apps::wait_while_running(sink, 1ms, [&] {
        // The device goes on the third look, with the queue still unplayed.
        if (++looks == 3) {
            sink.gone = true;
        }
        return false;
    }));
    CHECK(looks == 3);
}

TEST_CASE("sink wait: a queue that played out counts even if the sink has stopped since",
          "[sink-wait][concurrency]") {
    const ScriptedSink sink{.gone = true};
    CHECK(iclforge::apps::wait_while_running(sink, 1ms, [] { return true; }));
}

TEST_CASE("sink wait: a sink stopped by its own render thread ends both waits",
          "[sink-wait][concurrency]") {
    {
        RenderThreadSink sink;
        const std::jthread render([&sink] {
            std::this_thread::sleep_for(20ms);
            sink.alive.store(false, std::memory_order_release);
        });
        CHECK_FALSE(iclforge::apps::submit_while_running(sink, 1ms, 7));
    }
    {
        RenderThreadSink sink;
        const std::jthread render([&sink] {
            std::this_thread::sleep_for(20ms);
            sink.alive.store(false, std::memory_order_release);
        });
        CHECK_FALSE(iclforge::apps::wait_while_running(sink, 1ms, [] { return false; }));
    }
}

TEST_CASE("sink wait: a clip is played out when its queue has emptied and the device has played it",
          "[sink-wait][play-out]") {
    // 1000 frames at 100 a look: the queue empties on the tenth look, and the
    // device is a step behind it. Neither is the answer until both are.
    const PlayoutSink sink{.submitted = 1000};
    int polls = 0;
    CHECK(iclforge::apps::wait_until_played_out(sink, 48000, 1ms, [&polls] {
              ++polls;
              return true;
          }) == iclforge::apps::PlayOut::kPlayedOut);
    CHECK(sink.rendered == 1000);
    CHECK(sink.played == 1000);
    CHECK(polls > 0);
}

TEST_CASE("sink wait: a stop request ends the play-out at once, with the tail unplayed",
          "[sink-wait][play-out]") {
    const PlayoutSink sink{.submitted = 100'000};
    int polls = 0;
    CHECK(iclforge::apps::wait_until_played_out(sink, 48000, 1ms,
                                                [&polls] { return ++polls < 3; }) ==
          iclforge::apps::PlayOut::kStopped);
    CHECK(polls == 3);
    CHECK(sink.played < sink.submitted);
}

TEST_CASE("sink wait: a stop request is heard while the device plays out what it holds",
          "[sink-wait][play-out]") {
    // Everything is in the device and a long way from played: the second wait.
    PlayoutSink sink{.submitted = 100, .step = 100};
    sink.rendered = 100;
    sink.silent = true;
    int polls = 0;
    CHECK(iclforge::apps::wait_until_played_out(sink, 48000, 1ms,
                                                [&polls] { return ++polls < 4; }) ==
          iclforge::apps::PlayOut::kStopped);
    CHECK(polls == 4);
}

TEST_CASE("sink wait: a device lost with the queue still full ends the play-out",
          "[sink-wait][play-out]") {
    const PlayoutSink sink{.submitted = 100'000, .gone_at_look = 5};
    CHECK(iclforge::apps::wait_until_played_out(sink, 48000, 1ms, [] { return true; }) ==
          iclforge::apps::PlayOut::kSinkLost);
    CHECK(sink.played < sink.submitted);
}

TEST_CASE("sink wait: a device lost while it plays out what it holds ends the play-out",
          "[sink-wait][play-out]") {
    // The queue is empty; the device has 1000 frames and goes on the third look.
    PlayoutSink sink{.submitted = 1000, .step = 10, .gone_at_look = 3};
    sink.rendered = 1000;
    CHECK(iclforge::apps::wait_until_played_out(sink, 48000, 1ms, [] { return true; }) ==
          iclforge::apps::PlayOut::kSinkLost);
}

TEST_CASE("sink wait: a device that stops answering cannot hold the caller for ever",
          "[sink-wait][play-out]") {
    // Running, and nothing moves: the queue never empties. The bound is the
    // time what is queued should take plus the grace given: 1000 frames at
    // 48 kHz is 20 ms, and the grace 100 ms.
    {
        const PlayoutSink sink{.submitted = 1000, .silent = true};
        const auto begun = std::chrono::steady_clock::now();
        CHECK(iclforge::apps::wait_until_played_out(sink, 48000, 1ms, [] { return true; }, 100ms) ==
              iclforge::apps::PlayOut::kTimedOut);
        const auto waited = std::chrono::steady_clock::now() - begun;
        CHECK(waited >= 100ms);
        CHECK(waited < 5s);
    }
    // The queue emptied, and the device's clock stopped with its buffer unplayed.
    {
        PlayoutSink sink{.submitted = 1000, .silent = true};
        sink.rendered = 1000;
        CHECK(iclforge::apps::wait_until_played_out(sink, 48000, 1ms, [] { return true; }, 100ms) ==
              iclforge::apps::PlayOut::kTimedOut);
    }
}

TEST_CASE("sink wait: the output path's latency is waited for after the last frame played",
          "[sink-wait][play-out]") {
    // Everything played already; the platform says its path adds 100 ms
    // (4800 frames at 48 kHz), and a period's margin comes with it.
    PlayoutSink sink{.submitted = 1000, .latency = 4800};
    sink.rendered = 1000;
    sink.played = 1000;
    const auto begun = std::chrono::steady_clock::now();
    CHECK(iclforge::apps::wait_until_played_out(sink, 48000, 1ms, [] { return true; }) ==
          iclforge::apps::PlayOut::kPlayedOut);
    CHECK(std::chrono::steady_clock::now() - begun >= 100ms + iclforge::apps::kPlayOutPeriod);
}

TEST_CASE("sink wait: nothing submitted is played out after the margin alone",
          "[sink-wait][play-out]") {
    const PlayoutSink sink;
    CHECK(iclforge::apps::wait_until_played_out(sink, 48000, 1ms, [] { return true; }) ==
          iclforge::apps::PlayOut::kPlayedOut);
}

TEST_CASE("sink wait: a running sink that cannot say where it is is waited for by the margin",
          "[sink-wait][play-out]") {
    PlayoutSink sink{.submitted = 500, .answers_position = false};
    sink.rendered = 500;
    CHECK(iclforge::apps::wait_until_played_out(sink, 48000, 1ms, [] { return true; }) ==
          iclforge::apps::PlayOut::kPlayedOut);
}

TEST_CASE("sink wait: a sink already stopped has played out nothing", "[sink-wait][play-out]") {
    PlayoutSink sink{.submitted = 1000};
    sink.rendered = 1000;
    sink.gone = true;
    CHECK(iclforge::apps::wait_until_played_out(sink, 48000, 1ms, [] { return true; }) ==
          iclforge::apps::PlayOut::kSinkLost);
}
