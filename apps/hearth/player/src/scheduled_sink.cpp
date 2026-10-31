#include "scheduled_sink.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace iclforge::hearth::player {

namespace {

// Frames handed to the device at a time.
constexpr std::size_t kChunkFrames = 1024;
// Bursts the error is smoothed over, and the smoothed error (in frames, about a millisecond) from
// which the clock is followed.
constexpr double kSmoothing = 16.0;
constexpr double kTrimThresholdFrames = 48.0;

}  // namespace

ScheduledPcmSink::ScheduledPcmSink(std::unique_ptr<FrameDevice> device, Clock now_us, Options options)
    : device_(std::move(device)), now_us_(std::move(now_us)), options_(options) {
    if (options_.thread) {
        thread_ = std::thread([this] {
            while (!stopping_.load()) {
                (void)pump();
                std::this_thread::sleep_for(std::chrono::microseconds(options_.pump_interval_us));
            }
        });
    }
}

ScheduledPcmSink::~ScheduledPcmSink() {
    stopping_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
    device_->close();
}

bool ScheduledPcmSink::open(std::uint32_t sample_rate, std::uint16_t channels) {
    const std::lock_guard lock(mutex_);
    if (sample_rate == 0 || channels == 0) {
        return false;
    }
    if (device_open_) {
        if (!finalized_) {
            total_underruns_ += counted_underruns();
        }
        device_->close();
        device_open_ = false;
    }
    ignored_underruns_ = 0;
    idle_ = false;
    ended_ = false;
    finalized_ = false;
    stream_bursts_ = 0;
    smoothed_error_us_ = 0.0;
    rate_ = sample_rate;
    channels_ = channels;
    pending_.clear();
    pending_offset_ = 0;
    next_frame_ = 0;
    written_frames_ = 0;
    anchors_.clear();
    aligned_ = false;
    return true;
}

void ScheduledPcmSink::burst(std::int64_t local_time_us, std::uint64_t first_frame, std::uint64_t /*frames*/) {
    const std::lock_guard lock(mutex_);
    anchors_.push_back({first_frame, local_time_us});
}

void ScheduledPcmSink::write(std::span<const float> interleaved) {
    const std::lock_guard lock(mutex_);
    if (channels_ == 0) {
        return;
    }
    const std::size_t from = pending_.size();
    pending_.insert(pending_.end(), interleaved.begin(), interleaved.end());
    if (options_.gain != 1.0F) {
        std::for_each(pending_.begin() + static_cast<std::ptrdiff_t>(from), pending_.end(),
                      [gain = options_.gain](float& sample) { sample *= gain; });
    }
    written_frames_ += interleaved.size() / channels_;
}

void ScheduledPcmSink::clear(std::uint64_t /*frames*/) {
    const std::lock_guard lock(mutex_);
    // What is queued is dropped; frames still to be written carry on from the stream's count, and
    // the next burst's anchor places them.
    next_frame_ = written_frames_;
    pending_.clear();
    pending_offset_ = 0;
    anchors_.clear();
    aligned_ = false;
    stream_bursts_ = 0;
    smoothed_error_us_ = 0.0;
    if (device_open_) {
        device_->flush();
    }
}

// The stream ended: what is waiting is still played, then the device is let go.
void ScheduledPcmSink::close() {
    const std::lock_guard lock(mutex_);
    ended_ = true;
}

PlayStats ScheduledPcmSink::stats() const {
    const std::lock_guard lock(mutex_);
    PlayStats stats = stats_;
    stats.underruns = total_underruns_ + (device_open_ && !finalized_ ? counted_underruns() : 0);
    stats.pending_frames = channels_ == 0 ? 0 : (pending_.size() - pending_offset_) / channels_;
    return stats;
}

bool ScheduledPcmSink::idle() const {
    const std::lock_guard lock(mutex_);
    return pending_.size() == pending_offset_;
}

bool ScheduledPcmSink::pump() {
    const std::lock_guard lock(mutex_);
    return pump_locked();
}

bool ScheduledPcmSink::submit_locked(std::span<const float> frames) {
    if (!device_open_) {
        if (!device_->open(rate_, channels_)) {
            return false;
        }
        device_open_ = true;
    }
    return device_->submit(frames);
}

std::uint64_t ScheduledPcmSink::counted_underruns() const {
    const std::uint64_t all = device_->underruns();
    return all - ignored_underruns_ - (idle_ ? all - idle_mark_ : 0);
}

bool ScheduledPcmSink::pump_locked() {
    bool worked = false;
    if (device_open_ && idle_ && pending_.size() > pending_offset_) {
        ignored_underruns_ += device_->underruns() - idle_mark_;
        idle_ = false;
    }
    const std::size_t channels = channels_;
    while (channels != 0 && pending_.size() > pending_offset_) {
        const std::int64_t now = now_us_();
        const std::size_t available = (pending_.size() - pending_offset_) / channels;
        if (!anchors_.empty() && anchors_.front().first_frame <= next_frame_) {
            const Anchor anchor = anchors_.front();
            // Not before it is within the horizon: the device would be holding it.
            if (anchor.due_us - now > options_.horizon_us) {
                break;
            }
            // Opened now, so that the delay it adds is in the measure; the silence or the frames
            // that follow are queued at once.
            if (!device_open_ && !device_->open(rate_, channels_)) {
                break;
            }
            device_open_ = true;
            const std::uint64_t held = device_->pending_frames();
            const std::int64_t heard_at = now + static_cast<std::int64_t>(held * 1'000'000 / rate_);
            const std::int64_t error = heard_at - anchor.due_us;
            // The first burst of a stream has the device's start-up in it, and the next few are
            // the device settling: they place the stream, and the worst error is of the rest.
            const bool counted = stream_bursts_ >= options_.settle_bursts;
            // The stream's first burst is placed exactly; later ones within the tolerance are left.
            const std::int64_t tolerance = aligned_ ? options_.tolerance_us : 1'000;
            if (aligned_) {
                stats_.sync_error_us = error;
                if (counted && std::llabs(error) > std::llabs(stats_.worst_sync_error_us)) {
                    stats_.worst_sync_error_us = error;
                }
            }
            ++stream_bursts_;
            aligned_ = true;
            ++stats_.measured;
            if (std::llabs(error) > tolerance) {
                // A burst off its time once the stream has settled is a fault.
                stats_.corrected += counted ? 1 : 0;
                if (error > 0) {
                    // Late: skip what has already gone by.
                    const auto late = std::min<std::size_t>(
                        available, static_cast<std::size_t>(error * static_cast<std::int64_t>(rate_) / 1'000'000));
                    pending_offset_ += late * channels;
                    next_frame_ += late;
                    stats_.dropped_frames += late;
                } else {
                    const auto early = static_cast<std::size_t>(
                        std::min(-error, options_.max_silence_us) * static_cast<std::int64_t>(rate_) / 1'000'000);
                    const std::vector<float> silence(early * channels, 0.0F);
                    if (!submit_locked(silence)) {
                        break;
                    }
                    stats_.silence_frames += early;
                    worked = true;
                }
            }
            if (!counted || std::llabs(error) > tolerance) {
                smoothed_error_us_ = 0.0;
            } else {
                // Within the tolerance: follow the device's clock a frame or two at a time, from
                // the error smoothed over some bursts, so that its period does not move it.
                smoothed_error_us_ += (static_cast<double>(error) - smoothed_error_us_) / kSmoothing;
                const double off_frames = smoothed_error_us_ * rate_ / 1e6;
                if (std::fabs(off_frames) >= kTrimThresholdFrames) {
                    const auto step = static_cast<std::size_t>(std::min(4.0, std::ceil(std::fabs(off_frames) / 24.0)));
                    const double step_us = static_cast<double>(step) * 1e6 / rate_;
                    if (off_frames > 0) {
                        const std::size_t drop = std::min(step, available);
                        pending_offset_ += drop * channels;
                        next_frame_ += drop;
                        stats_.trimmed_frames += drop;
                        smoothed_error_us_ -= step_us;
                    } else {
                        const std::vector<float> silence(step * channels, 0.0F);
                        if (submit_locked(silence)) {
                            stats_.trimmed_frames += step;
                            smoothed_error_us_ += step_us;
                            worked = true;
                        }
                    }
                }
            }
            anchors_.pop_front();
            continue;
        }
        // Up to the next anchor, or a chunk.
        std::size_t frames = std::min(available, kChunkFrames);
        if (!anchors_.empty()) {
            frames = std::min<std::size_t>(frames, anchors_.front().first_frame - next_frame_);
        }
        const std::span<const float> chunk(pending_.data() + pending_offset_, frames * channels);
        if (!submit_locked(chunk)) {
            break;
        }
        pending_offset_ += frames * channels;
        next_frame_ += frames;
        worked = true;
    }
    if (device_open_ && !idle_ && worked && pending_.size() == pending_offset_) {
        idle_ = true;
        idle_mark_ = device_->underruns();
    }
    if (ended_ && device_open_ && pending_.size() == pending_offset_) {
        if (!finalized_) {
            total_underruns_ += counted_underruns();
            finalized_ = true;
        }
        if (device_->pending_frames() == 0) {
            device_->close();
            device_open_ = false;
        }
    }
    if (pending_offset_ > 0 && pending_offset_ == pending_.size()) {
        pending_.clear();
        pending_offset_ = 0;
    } else if (pending_offset_ > (1U << 20)) {
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(pending_offset_));
        pending_offset_ = 0;
    }
    return worked;
}

}  // namespace iclforge::hearth::player
