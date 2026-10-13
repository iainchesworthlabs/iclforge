#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>

// Waiting on an iclforge::audio sink without waiting for ever.
//
// A sink's submit() refuses for two reasons. Its queue is full, which time
// cures: the caller is ahead of real time and should wait. Or it is not
// running, which nothing cures: it stopped itself because its device went
// away (PassthroughSink::running(), MonitorSink::running()). A loop that
// retries on a refusal has to tell the two apart, and so does one waiting for
// the queue to play out, since a stopped sink's counts never move again.
// forge's play, monitor, identify and live all wait both ways.
//
// Templates over the sink, since PassthroughSink, MonitorSink and PcmOutput
// share the shape but no base class. Compiled straight into forge and
// iclforge-app-media-tests, as stream_playback.hpp beside it is, so a test can hold both
// helpers against a sink that stops on cue: no command reaches its wait
// without a render device, and no device can be pulled on a CI runner.

namespace iclforge::apps {

// Offers `payload` to sink.submit() until the sink takes it, sleeping `pause`
// between offers. False, having queued nothing, once the sink is not running.
template <typename Sink, typename... Payload>
[[nodiscard]] bool submit_while_running(Sink& sink, std::chrono::milliseconds pause,
                                        const Payload&... payload) {
    while (!sink.submit(payload...)) {
        if (!sink.running()) {
            return false;
        }
        std::this_thread::sleep_for(pause);
    }
    return true;
}

// Sleeps `pause` at a time until `done()` is true: the queue has played out,
// say. False once the sink is not running and done() is still false, since
// what is left will never play.
template <typename Sink, typename Done>
[[nodiscard]] bool wait_while_running(const Sink& sink, std::chrono::milliseconds pause,
                                      Done done) {
    while (!done()) {
        if (!sink.running()) {
            return false;
        }
        std::this_thread::sleep_for(pause);
    }
    return true;
}

// How wait_until_played_out ended.
enum class PlayOut : std::uint8_t {
    // Everything the sink took has been played, and the output path's own
    // delay with it: stop() now drops nothing that was meant to be heard.
    kPlayedOut,
    // keep_waiting() said no first - the listener stopped, paused or went
    // elsewhere. What was still queued is theirs to drop, and stop() does.
    kStopped,
    // The sink stopped itself, as its device went away, with some of what it
    // was handed unplayed.
    kSinkLost,
    // The sink is running and has stopped answering: its counts have not
    // moved for as long as what was left should have taken, and a second
    // more. The caller stops it; the tail is lost to the device, not to this.
    kTimedOut,
};

// How often a play-out looks, which is how soon a stop or a pause is heard
// while the tail of a clip is playing: small beside a render period, large
// enough not to spin.
inline constexpr std::chrono::milliseconds kPlayOutPoll{10};

// How far past the time what is left should take a play-out may run before
// the sink is taken to have stopped answering: a render thread descheduled
// for a moment, a device that stalls and comes back.
inline constexpr std::chrono::milliseconds kPlayOutGrace{1000};

// What a position is allowed to be behind the device. A backend reports where
// the device has got to once a render period, and a period is 10 ms on
// WASAPI's default and of that order on ALSA, PipeWire and Core Audio, so the
// last frame can have left the device up to that long before it says so.
inline constexpr std::chrono::milliseconds kPlayOutPeriod{20};

// Waits until a sink has played everything it was handed, so that stop() after
// it drops nothing. A caller that submits a finite clip is done with its
// submit() calls long before the clip is: the sink's queue holds more than a
// second (bit_ceil of channels * rate samples: 1.4 s for one channel at
// 48 kHz, 1.8 s for six) and the device's own buffer sits beyond it, and
// stop() discards both. A player that stops on its last submit() cuts off up
// to that much of what it was asked to play - all of a clip that fits the
// queue, which is taken whole within milliseconds - and says it has finished
// while the listener is still hearing it, or never did.
//
// The wait is three: the sink's queue empties into the device
// (stats().frames_rendered reaches frames_submitted), then the device plays
// what it holds (position().frames_played reaches what position() counted as
// handed over when the queue emptied), then the platform's own output delay
// (latency_frames) and a period's margin pass. A device's own buffer never
// reaches zero while it runs - a drained queue is padded with silence - so
// the second is waited for by the device's clock, not for a count.
//
// `keep_waiting` is called at every poll, about every `pause`, and ends the
// wait with kStopped when it returns false: the stop request path, which a
// press of stop or pause needs to be as quick as the poll. A caller that
// shows progress while the tail plays can do it from there, too. Every wait
// is bounded, by `grace` past the time it should take, so a device that stops
// answering cannot hold the caller for ever.
//
// Templates over the sink, as the two above are; needs stats(), position(),
// running() and MonitorStats/MonitorPosition's fields (MonitorSink, PcmOutput).
template <typename Sink, typename KeepWaiting>
[[nodiscard]] PlayOut wait_until_played_out(const Sink& sink, std::uint32_t sample_rate,
                                            std::chrono::milliseconds pause,
                                            KeepWaiting keep_waiting,
                                            std::chrono::milliseconds grace = kPlayOutGrace) {
    using Clock = std::chrono::steady_clock;
    const auto span_of = [rate = std::max<std::uint32_t>(sample_rate, 1)](std::uint64_t frames) {
        return std::chrono::milliseconds(static_cast<std::int64_t>(frames * 1000U / rate));
    };
    // What every wait below asks before it sleeps once more.
    const auto reason_to_stop = [&](Clock::time_point give_up) -> std::optional<PlayOut> {
        if (!keep_waiting()) {
            return PlayOut::kStopped;
        }
        if (!sink.running()) {
            return PlayOut::kSinkLost;
        }
        if (Clock::now() >= give_up) {
            return PlayOut::kTimedOut;
        }
        return std::nullopt;
    };

    // The queue empties into the device.
    auto counts = sink.stats();
    auto give_up =
        Clock::now() +
        span_of(counts.frames_submitted > counts.frames_rendered
                    ? counts.frames_submitted - counts.frames_rendered
                    : 0) +
        grace;
    while (counts.frames_rendered < counts.frames_submitted) {
        if (const auto why = reason_to_stop(give_up)) {
            return *why;
        }
        std::this_thread::sleep_for(pause);
        counts = sink.stats();
    }

    // The device plays what it holds. Everything handed over by now is
    // frames_played + frames_queued, and the sink's own queue is empty, so
    // that is the figure frames_played has to reach.
    std::uint64_t handed_over = 0;
    std::chrono::milliseconds delay{0};
    auto position = sink.position();
    if (position) {
        handed_over = position->frames_played + position->frames_queued;
        delay = span_of(position->latency_frames);
    } else if (!sink.running()) {
        return PlayOut::kSinkLost;
    }
    // (A running sink that cannot say where it is leaves only the margin.)
    give_up = Clock::now() + (position ? span_of(position->frames_queued) : pause) + delay + grace;
    while (position && position->frames_played < handed_over) {
        if (const auto why = reason_to_stop(give_up)) {
            return *why;
        }
        std::this_thread::sleep_for(pause);
        position = sink.position();
        if (!position) {
            return PlayOut::kSinkLost;
        }
    }

    // The frames have left the device; the output path past it, and a
    // position that may be a period behind, are waited out by the clock.
    const auto settled = Clock::now() + delay + kPlayOutPeriod;
    for (auto now = Clock::now(); now < settled; now = Clock::now()) {
        if (!keep_waiting()) {
            return PlayOut::kStopped;
        }
        std::this_thread::sleep_for(std::min<std::chrono::milliseconds>(
            pause, std::chrono::duration_cast<std::chrono::milliseconds>(settled - now) +
                       std::chrono::milliseconds(1)));
    }
    return PlayOut::kPlayedOut;
}

}  // namespace iclforge::apps
