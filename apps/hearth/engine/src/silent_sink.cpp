#include "silent_sink.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <utility>

#include "iclforge/audio/speakers.hpp"

namespace iclforge::hearth {

namespace {

// What the device holds at most, in seconds of audio: a real output holds a few tens of
// milliseconds and the engine fills it a little ahead.
constexpr double kCapacitySeconds = 0.25;

[[nodiscard]] std::int64_t steady_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

class SilentSink final : public PcmSink {
public:
    SilentSink(std::string name, std::function<std::int64_t()> clock)
        : name_(std::move(name)),
          clock_(clock ? std::move(clock) : std::function<std::int64_t()>(steady_ns)) {}

    std::expected<OpenOutputFormat, std::string> open(const Format& format) override {
        const std::scoped_lock lock(mutex_);
        open_ = true;
        paused_ = false;
        rate_ = std::max<std::uint32_t>(format.sample_rate, 1);
        channels_ = static_cast<std::uint16_t>(format.layout.slots());
        capacity_ = static_cast<std::uint64_t>(kCapacitySeconds * static_cast<double>(rate_));
        restart_locked();
        return OpenOutputFormat{
            .sample_rate = rate_, .channels = channels_, .mode = OutputMode::kLocalPcm};
    }

    void close() override {
        const std::scoped_lock lock(mutex_);
        open_ = false;
    }

    [[nodiscard]] bool is_open() const override {
        const std::scoped_lock lock(mutex_);
        return open_;
    }

    bool submit(std::span<const std::span<const float>> slots, std::size_t frames) override {
        static_cast<void>(slots);  // played to nothing
        const std::scoped_lock lock(mutex_);
        if (!open_) {
            return false;
        }
        advance_locked();
        if (submitted_ - heard_ + frames > capacity_) {
            return false;
        }
        if (!running_) {
            running_ = true;
            last_ns_ = clock_();
        }
        submitted_ += frames;
        return true;
    }

    [[nodiscard]] std::optional<audio::MonitorPosition> position() const override {
        const std::scoped_lock lock(mutex_);
        if (!open_) {
            return std::nullopt;
        }
        advance_locked();
        return audio::MonitorPosition{
            .frames_played = clock_frames_, .frames_queued = submitted_ - heard_, .latency_frames = 0};
    }

    void flush() override {
        const std::scoped_lock lock(mutex_);
        restart_locked();
    }

    bool pause() override {
        const std::scoped_lock lock(mutex_);
        advance_locked();
        paused_ = true;
        return open_;
    }

    bool resume() override {
        const std::scoped_lock lock(mutex_);
        if (paused_ && running_) {
            last_ns_ = clock_();  // the time held is not time played
        }
        paused_ = false;
        return open_;
    }

    [[nodiscard]] std::string device_name() const override { return name_; }
    [[nodiscard]] std::string device_id() const override { return "silent"; }
    [[nodiscard]] std::uint32_t speaker_mask() const override {
        const std::scoped_lock lock(mutex_);
        return audio::default_speakers(channels_);
    }

private:
    // Counts from zero again, with the clock not yet started.
    void restart_locked() const {
        submitted_ = 0;
        heard_ = 0;
        clock_frames_ = 0;
        running_ = false;
        last_ns_ = 0;
    }

    // The device's clock, brought up to the time it is: whole frames, and the part of a frame
    // that is left stays for the next call.
    void advance_locked() const {
        if (!open_ || paused_ || !running_) {
            return;
        }
        const std::int64_t now = clock_();
        const std::int64_t whole =
            (now - last_ns_) * static_cast<std::int64_t>(rate_) / 1'000'000'000LL;
        if (whole <= 0) {
            return;
        }
        last_ns_ += whole * 1'000'000'000LL / static_cast<std::int64_t>(rate_);
        const auto frames = static_cast<std::uint64_t>(whole);
        clock_frames_ += frames;
        heard_ += std::min(submitted_ - heard_, frames);
    }

    const std::string name_;
    const std::function<std::int64_t()> clock_;
    mutable std::mutex mutex_;
    bool open_ = false;
    bool paused_ = false;
    std::uint32_t rate_ = 48000;
    std::uint16_t channels_ = 2;
    std::uint64_t capacity_ = 0;
    // What the engine and the clock have done since the last open or flush.
    mutable bool running_ = false;
    mutable std::int64_t last_ns_ = 0;
    mutable std::uint64_t submitted_ = 0;
    mutable std::uint64_t heard_ = 0;
    mutable std::uint64_t clock_frames_ = 0;
};

}  // namespace

std::unique_ptr<PcmSink> make_silent_sink(std::string device_name,
                                          std::function<std::int64_t()> clock) {
    return std::make_unique<SilentSink>(std::move(device_name), std::move(clock));
}

}  // namespace iclforge::hearth
