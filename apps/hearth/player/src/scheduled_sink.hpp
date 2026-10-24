#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include "frame_device.hpp"
#include "iclforge/sink/decoding_output.hpp"

// The PcmSink that plays (docs/hearth/player.md): decoded frames, each burst with the time on the
// sink's steady clock it is due, to a FrameDevice, so that every frame is heard when the server
// said. Nothing is handed to the device more than `horizon_us` before it is due, which keeps
// stream/clear quick. When a burst is due, the device's own queue says when it would be heard; the
// difference is the sync error, and an error beyond `tolerance_us` is corrected by dropping frames
// (it is late) or playing silence first (it is early). The device is opened at the first frame of
// a stream, so what it counts as an underrun is a gap in a stream and not the wait for one.
//
// pump() does the work; a thread calls it every few milliseconds, or a test calls it with a clock
// of its own.

namespace iclforge::hearth::player {

struct PlayStats {
    // The newest and the largest difference between when a burst would be heard and when it was
    // due, in microseconds: positive is late. The first burst of a stream sets the stream's
    // alignment and is not counted in the worst.
    std::int64_t sync_error_us = 0;
    std::int64_t worst_sync_error_us = 0;
    // Bursts measured, and the ones off by more than the tolerance and corrected.
    std::uint64_t measured = 0;
    std::uint64_t corrected = 0;
    // Frames dropped for being late, and frames of silence played for being early.
    std::uint64_t dropped_frames = 0;
    std::uint64_t silence_frames = 0;
    // Frames dropped or silence played a few at a time to follow the device's clock, which is
    // not the sink's: the drift, taken up before it is an error.
    std::uint64_t trimmed_frames = 0;
    std::uint64_t underruns = 0;
    // Frames waiting for their time.
    std::uint64_t pending_frames = 0;
};

class ScheduledPcmSink final : public sink::PcmSink {
   public:
    struct Options {
        std::int64_t horizon_us = 150'000;
        // The platform's own position is only as fine as its period (10 ms in WASAPI's shared mode),
        // so less than a period and a half is not a fault.
        std::int64_t tolerance_us = 25'000;
        // Bursts at the start of a stream during which a correction places the stream and is not
        // counted: the device is still settling.
        std::uint64_t settle_bursts = 16;
        // The most silence played to wait for a burst that is due further off than this.
        std::int64_t max_silence_us = 2'000'000;
        // Runs a thread that calls pump() every `pump_interval_us`; off for a test that does.
        bool thread = true;
        // Linear gain on every sample played (--gain-db).
        float gain = 1.0F;
        std::int64_t pump_interval_us = 4'000;
    };

    using Clock = std::function<std::int64_t()>;

    // `now_us` reads the sink's steady clock, in microseconds.
    ScheduledPcmSink(std::unique_ptr<FrameDevice> device, Clock now_us, Options options);
    ~ScheduledPcmSink() override;

    bool open(std::uint32_t sample_rate, std::uint16_t channels) override;
    [[nodiscard]] bool taking() const override { return true; }
    void burst(std::int64_t local_time_us, std::uint64_t first_frame, std::uint64_t frames) override;
    void write(std::span<const float> interleaved) override;
    void clear(std::uint64_t frames) override;
    void close() override;
    [[nodiscard]] const std::filesystem::path& file() const override { return file_; }

    // Hands the frames that are due to the device; returns whether it did anything.
    bool pump();
    [[nodiscard]] PlayStats stats() const;
    // Every decoded frame has reached the device.
    [[nodiscard]] bool idle() const;

   private:
    struct Anchor {
        std::uint64_t first_frame;
        std::int64_t due_us;
    };

    bool pump_locked();
    [[nodiscard]] std::uint64_t counted_underruns() const;
    bool submit_locked(std::span<const float> frames);

    std::unique_ptr<FrameDevice> device_;
    Clock now_us_;
    Options options_;
    std::filesystem::path file_;

    mutable std::mutex mutex_;
    std::uint32_t rate_ = 0;
    std::uint16_t channels_ = 0;
    // The decoded samples not yet handed to the device, from pending_offset_; the stream's frame
    // number of the first of them; the frames written so far; and the bursts' anchors not yet
    // reached.
    std::vector<float> pending_;
    std::size_t pending_offset_ = 0;
    std::uint64_t next_frame_ = 0;
    std::uint64_t written_frames_ = 0;
    std::deque<Anchor> anchors_;
    bool device_open_ = false;
    // The stream has had its first burst, which sets its alignment.
    bool aligned_ = false;
    std::uint64_t stream_bursts_ = 0;
    // The sync error, smoothed over some bursts so that the device's period does not move it, in
    // microseconds; what the small steps follow.
    double smoothed_error_us_ = 0.0;
    PlayStats stats_;
    // Underruns of the streams that are over, and whether this one's have been taken: they are
    // counted until its last frame is queued, and the silence after it is not a gap.
    std::uint64_t total_underruns_ = 0;
    bool ended_ = false;
    bool finalized_ = false;
    // The device's underruns while nothing was waiting to be played - the silence after a stream's
    // last frame, before its end is announced - are not a fault and are not counted: those so far,
    // and where the current idle stretch began.
    std::uint64_t ignored_underruns_ = 0;
    std::uint64_t idle_mark_ = 0;
    bool idle_ = false;

    std::atomic<bool> stopping_{false};
    std::thread thread_;
};

}  // namespace iclforge::hearth::player
