#include "performance.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace iclforge::hearth {

namespace {

// More blocks than a tracker's window ever reaches back over, whatever size the caller feeds.
constexpr std::size_t kMaxBlocks = 256;

// The item is "moving" if a short time later it is somewhere else: a held position is a pause, and
// scoring a voice against it would pile every frame on one grid point.
constexpr std::int64_t kMovingProbeUs = 40'000;
constexpr std::int64_t kMovingMinimumMs = 10;

}  // namespace

std::optional<Performance> Performance::make(pitch::Contour reference,
                                             const PerformanceOptions& options) {
    if (reference.voiced_frames() == 0) {
        return std::nullopt;
    }
    pitch::TrackerConfig config = pitch::TrackerConfig::for_input_rate(options.input_rate);
    config.hop_ms = reference.hop_ms;
    config.clarity_min = options.clarity_min;
    config.level_floor_db = options.level_floor_db;
    auto tracker = pitch::Tracker::make(config);
    if (!tracker) {
        return std::nullopt;
    }
    return Performance(std::move(reference), options, std::move(*tracker));
}

Performance::Performance(pitch::Contour reference, const PerformanceOptions& options,
                         pitch::Tracker tracker)
    : options_(options),
      scorer_(std::move(reference), options.score),
      tracker_(std::move(tracker)) {
    frames_scratch_.reserve(64);
}

void Performance::anchor(std::int64_t item_ms, std::int64_t at_ns, double speed) {
    playhead_.anchor(item_ms, at_ns / 1000, speed);
}

void Performance::clear_singer() {
    scorer_.clear();
    tracker_.reset();
    samples_fed_ = 0;
    blocks_.clear();
    live_.reset();
    unplaced_ = 0;
    frames_ = 0;
}

void Performance::feed_interleaved(std::span<const float> samples, std::uint16_t channels,
                                   std::int64_t first_sample_time_ns) {
    if (channels <= 1) {
        feed(samples, first_sample_time_ns);
        return;
    }
    const std::size_t frames = samples.size() / channels;
    mono_scratch_.resize(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        float sum = 0.0F;
        for (std::uint16_t c = 0; c < channels; ++c) {
            sum += samples[i * channels + c];
        }
        mono_scratch_[i] = sum / static_cast<float>(channels);
    }
    feed(mono_scratch_, first_sample_time_ns);
}

void Performance::feed(std::span<const float> mono, std::int64_t first_sample_time_ns) {
    if (mono.empty()) {
        return;
    }
    blocks_.push_back({samples_fed_, first_sample_time_ns});
    if (blocks_.size() > kMaxBlocks) {
        blocks_.pop_front();
    }
    samples_fed_ += static_cast<std::int64_t>(mono.size());

    frames_scratch_.clear();
    tracker_.push(mono, frames_scratch_);
    for (const pitch::Frame& frame : frames_scratch_) {
        place(frame);
    }
}

void Performance::place(const pitch::Frame& frame) {
    ++frames_;
    // The time of the frame's centre sample: the block holding it, and the offset into that block.
    const auto it =
        std::upper_bound(blocks_.begin(), blocks_.end(), frame.centre_sample,
                         [](std::int64_t index, const Block& b) { return index < b.first_index; });
    const Block& block = it == blocks_.begin() ? blocks_.front() : *(it - 1);
    const double offset_ns = static_cast<double>(frame.centre_sample - block.first_index) * 1e9 /
                             static_cast<double>(options_.input_rate);
    const std::int64_t captured_ns = block.time_ns + static_cast<std::int64_t>(offset_ns);
    // The sound left the singer a little before the microphone's stamp.
    const std::int64_t sung_us = (captured_ns - options_.input_delay_ms * 1'000'000) / 1000;

    const auto position = playhead_.position_ms(sung_us);
    const auto later = playhead_.position_ms(sung_us + kMovingProbeUs);
    if (!position || !later || *later - *position < kMovingMinimumMs) {
        ++unplaced_;
        return;
    }
    const std::int64_t item_ms = *position + options_.offset_ms;
    scorer_.add(item_ms, frame.hz);

    PerformanceLive live;
    live.item_ms = item_ms;
    live.voiced = frame.voiced();
    live.hz = frame.hz;
    live.level_db = frame.level_db;
    const pitch::Contour& reference = scorer_.reference();
    if (item_ms >= 0 && reference.hop_ms > 0) {
        const std::size_t index =
            static_cast<std::size_t>((item_ms + static_cast<std::int64_t>(reference.hop_ms) / 2) /
                                     static_cast<std::int64_t>(reference.hop_ms));
        if (index < reference.hz.size()) {
            live.reference_hz = reference.hz[index];
        }
    }
    if (live.voiced && live.reference_hz > 0.0F) {
        double cents = pitch::cents_between(static_cast<double>(live.reference_hz),
                                            static_cast<double>(live.hz)) -
                       scorer_.options().key_shift_cents;
        if (scorer_.options().fold_octaves) {
            cents = pitch::fold_octaves(cents);
        }
        live.cents_from_reference = cents;
    }
    live_ = live;
}

}  // namespace iclforge::hearth
