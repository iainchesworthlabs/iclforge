#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "iclforge/audio/capture.hpp"

// Where in time each frame of a capture was captured (planning/karaoke-microphone.md, MI6).
//
// A capture hands out stamps, one per packet: "frame N of the stream was captured at time T". A
// consumer wants the time of a frame in the middle of a packet, or of a frame whose packet is not
// the last one, and the stamps of a real device carry a little jitter (the driver reads its clock
// when it gets round to it) and the device's clock runs a little off nominal. So a line is fitted
// through the most recent stamps, by least squares, and the time of a frame is read off it: the
// mean of 64 jittery stamps is much better than the nearest one.
//
// Standard library only and device-free, so that every platform tests the arithmetic the same way
// whether or not its backend gives stamps.

namespace iclforge::audio {

class CaptureTimeline {
   public:
    // How many of the most recent stamps the line is fitted through.
    static constexpr std::size_t kCapacity = 64;
    // Fewer stamps than this and the line is not fitted: the nominal rate is used from the last
    // one.
    static constexpr std::size_t kMinimumForFit = 8;

    // `sample_rate` is the nominal frames a second of the stream: the slope until a fit exists.
    explicit CaptureTimeline(std::uint32_t sample_rate) : nominal_rate_(sample_rate) {}

    void add(const CaptureStamp& stamp) {
        stamps_[(first_ + size_) % kCapacity] = stamp;
        if (size_ < kCapacity) {
            ++size_;
        } else {
            first_ = (first_ + 1) % kCapacity;
        }
    }

    void reset() {
        first_ = 0;
        size_ = 0;
    }

    [[nodiscard]] std::size_t size() const { return size_; }

    // The capture time of `frame`, in nanoseconds on the stamps' clock, or nothing before the first
    // stamp. A frame outside the stamps is extrapolated along the line.
    [[nodiscard]] std::optional<std::int64_t> time_ns_of_frame(std::uint64_t frame) const {
        if (size_ == 0) {
            return std::nullopt;
        }
        const Line line = fit();
        const double df = static_cast<double>(frame) - line.frame0;
        return line.time0 +
               static_cast<std::int64_t>(df * line.ns_per_frame + (df >= 0 ? 0.5 : -0.5));
    }

    // The frame, fractional, that was captured at `time_ns`: the inverse of the above.
    [[nodiscard]] std::optional<double> frame_at_time_ns(std::int64_t time_ns) const {
        if (size_ == 0 || nominal_rate_ == 0) {
            return std::nullopt;
        }
        const Line line = fit();
        return line.frame0 + static_cast<double>(time_ns - line.time0) / line.ns_per_frame;
    }

    // The rate the stamps say the device runs at, frames a second, once there are enough to fit.
    [[nodiscard]] std::optional<double> measured_rate() const {
        if (size_ < kMinimumForFit) {
            return std::nullopt;
        }
        return 1e9 / fit().ns_per_frame;
    }

   private:
    struct Line {
        // The line passes through (frame0, time0) with this slope.
        double frame0 = 0.0;
        std::int64_t time0 = 0;
        double ns_per_frame = 0.0;
    };

    [[nodiscard]] const CaptureStamp& at(std::size_t i) const {
        return stamps_[(first_ + i) % kCapacity];
    }

    [[nodiscard]] Line fit() const {
        const double nominal_ns =
            nominal_rate_ > 0 ? 1e9 / static_cast<double>(nominal_rate_) : 0.0;
        const CaptureStamp& last = at(size_ - 1);
        if (size_ < kMinimumForFit) {
            return {static_cast<double>(last.frame), last.time_ns, nominal_ns};
        }
        // Relative to the first stored stamp, so that nothing is lost to the size of a nanosecond
        // count on a clock that has run for days.
        const CaptureStamp& base = at(0);
        double sum_f = 0.0;
        double sum_t = 0.0;
        for (std::size_t i = 0; i < size_; ++i) {
            sum_f += static_cast<double>(at(i).frame - base.frame);
            sum_t += static_cast<double>(at(i).time_ns - base.time_ns);
        }
        const double n = static_cast<double>(size_);
        const double mean_f = sum_f / n;
        const double mean_t = sum_t / n;
        double sxx = 0.0;
        double sxy = 0.0;
        for (std::size_t i = 0; i < size_; ++i) {
            const double df = static_cast<double>(at(i).frame - base.frame) - mean_f;
            const double dt = static_cast<double>(at(i).time_ns - base.time_ns) - mean_t;
            sxx += df * df;
            sxy += df * dt;
        }
        if (sxx <= 0.0) {
            return {static_cast<double>(last.frame), last.time_ns, nominal_ns};
        }
        double slope = sxy / sxx;
        // A fit that says the device runs more than 5 % off nominal is a broken stamp, not a clock.
        if (nominal_ns > 0.0 && (slope < nominal_ns * 0.95 || slope > nominal_ns * 1.05)) {
            slope = nominal_ns;
        }
        const double frame0 = static_cast<double>(base.frame) + mean_f;
        const std::int64_t time0 =
            base.time_ns + static_cast<std::int64_t>(mean_t + (mean_t >= 0 ? 0.5 : -0.5));
        return {frame0, time0, slope};
    }

    std::uint32_t nominal_rate_;
    std::array<CaptureStamp, kCapacity> stamps_{};
    std::size_t first_ = 0;
    std::size_t size_ = 0;
};

}  // namespace iclforge::audio
