#include "iclforge/base/levels.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <vector>

namespace iclforge::base {

namespace {

constexpr double kDegToRad = std::numbers::pi / 180.0;

}  // namespace

double to_dbfs(double linear) {
    const double magnitude = std::abs(linear);
    if (magnitude <= 0.0) {
        return kFloorDb;
    }
    return std::max(kFloorDb, 20.0 * std::log10(magnitude));
}

double ChannelSummary::rms() const {
    return samples == 0 ? 0.0 : std::sqrt(sum_squares / static_cast<double>(samples));
}

double ChannelSummary::peak_db() const { return to_dbfs(peak); }

double ChannelSummary::rms_db() const { return to_dbfs(rms()); }

// Every private data member, following the same pimpl pattern as
// iclforge::base::WavStreamReader/Writer.
struct LevelMeter::Impl {
    std::uint32_t sample_rate_;
    MeterBallistics ballistics_;
    std::vector<ChannelLevel> levels_;
    std::vector<ChannelSummary> summary_;
    std::vector<double> mean_square_;   // one-pole RMS state, linear power
    std::vector<double> hold_elapsed_;  // seconds the hold marker has been parked
};

LevelMeter::~LevelMeter() = default;
LevelMeter::LevelMeter(LevelMeter&&) noexcept = default;
LevelMeter& LevelMeter::operator=(LevelMeter&&) noexcept = default;

std::span<const ChannelLevel> LevelMeter::levels() const { return impl_->levels_; }
std::span<const ChannelSummary> LevelMeter::summary() const { return impl_->summary_; }
int LevelMeter::channel_count() const { return static_cast<int>(impl_->levels_.size()); }
std::uint32_t LevelMeter::sample_rate() const { return impl_->sample_rate_; }

LevelMeter::LevelMeter(int channels, std::uint32_t sample_rate, const MeterBallistics& ballistics)
    : impl_(std::make_unique<Impl>(Impl{
          .sample_rate_ = sample_rate == 0 ? 48000u : sample_rate,
          .ballistics_ = ballistics,
          .levels_ = std::vector<ChannelLevel>(static_cast<std::size_t>(std::max(channels, 0))),
          .summary_ = {},
          .mean_square_ = {},
          .hold_elapsed_ = {},
      })) {
    impl_->summary_.resize(impl_->levels_.size());
    impl_->mean_square_.assign(impl_->levels_.size(), 0.0);
    impl_->hold_elapsed_.assign(impl_->levels_.size(), 0.0);
}

LevelMeter::LevelMeter(std::span<const Speaker> speakers, std::uint32_t sample_rate,
                       const MeterBallistics& ballistics)
    : LevelMeter(static_cast<int>(speakers.size()), sample_rate, ballistics) {}

void LevelMeter::reset() {
    std::ranges::fill(impl_->levels_, ChannelLevel{});
    std::ranges::fill(impl_->summary_, ChannelSummary{});
    std::ranges::fill(impl_->mean_square_, 0.0);
    std::ranges::fill(impl_->hold_elapsed_, 0.0);
}

void LevelMeter::advance(std::size_t channel, double block_peak, double mean_square,
                         double seconds) {
    auto& level = impl_->levels_[channel];

    // Peak: instantaneous attack, constant-rate fallback. Starting from the
    // floor the decay term stays at the floor, so silence in never lifts the
    // needle.
    const double block_peak_db = to_dbfs(block_peak);
    const double decayed = level.peak_db - impl_->ballistics_.peak_decay_db_per_s * seconds;
    level.peak_db = std::max({block_peak_db, decayed, kFloorDb});

    // Hold: parks on the maximum, then descends at the peak's rate but never
    // below it, so the marker rejoins the bar instead of vanishing.
    if (block_peak_db >= level.hold_db) {
        level.hold_db = block_peak_db;
        impl_->hold_elapsed_[channel] = 0.0;
    } else {
        impl_->hold_elapsed_[channel] += seconds;
        const double over = impl_->hold_elapsed_[channel] - impl_->ballistics_.peak_hold_ms / 1000.0;
        if (over > 0.0) {
            level.hold_db = std::max(
                level.peak_db, level.hold_db - impl_->ballistics_.peak_decay_db_per_s * seconds);
        }
    }

    // RMS: one-pole average of the block's mean square. Over a block longer
    // than the integration time alpha saturates at 1, which is the right
    // answer — the block already contains more history than the filter holds.
    const double tau = impl_->ballistics_.rms_integration_ms / 1000.0;
    const double alpha = tau > 0.0 ? -std::expm1(-seconds / tau) : 1.0;
    impl_->mean_square_[channel] += alpha * (mean_square - impl_->mean_square_[channel]);
    level.rms_db = to_dbfs(std::sqrt(impl_->mean_square_[channel]));
}

void LevelMeter::process(std::span<const std::span<const float>> channels) {
    std::size_t length = 0;
    for (std::size_t ch = 0; ch < impl_->levels_.size() && ch < channels.size(); ++ch) {
        length = ch == 0 ? channels[ch].size() : std::min(length, channels[ch].size());
    }
    if (length == 0) {
        return;
    }
    const double seconds = static_cast<double>(length) / impl_->sample_rate_;

    for (std::size_t ch = 0; ch < impl_->levels_.size(); ++ch) {
        double block_peak = 0.0;
        double sum_squares = 0.0;
        std::uint64_t clipped = 0;
        if (ch < channels.size()) {
            for (const float sample : channels[ch].first(length)) {
                const double value = std::abs(static_cast<double>(sample));
                block_peak = std::max(block_peak, value);
                sum_squares += value * value;
                clipped += std::abs(sample) >= kFullScale ? 1u : 0u;
            }
        }
        auto& total = impl_->summary_[ch];
        total.peak = std::max(total.peak, block_peak);
        total.sum_squares += sum_squares;
        total.samples += length;
        total.clipped_samples += clipped;
        impl_->levels_[ch].clipped = impl_->levels_[ch].clipped || clipped > 0;
        advance(ch, block_peak, sum_squares / static_cast<double>(length), seconds);
    }
}

void LevelMeter::process_interleaved(std::span<const float> samples, std::size_t stride) {
    if (stride == 0) {
        return;
    }
    const std::size_t length = samples.size() / stride;
    if (length == 0) {
        return;
    }
    const double seconds = static_cast<double>(length) / impl_->sample_rate_;

    for (std::size_t ch = 0; ch < impl_->levels_.size(); ++ch) {
        double block_peak = 0.0;
        double sum_squares = 0.0;
        std::uint64_t clipped = 0;
        if (ch < stride) {
            for (std::size_t i = 0; i < length; ++i) {
                const float sample = samples[i * stride + ch];
                const double value = std::abs(static_cast<double>(sample));
                block_peak = std::max(block_peak, value);
                sum_squares += value * value;
                clipped += std::abs(sample) >= kFullScale ? 1u : 0u;
            }
        }
        auto& total = impl_->summary_[ch];
        total.peak = std::max(total.peak, block_peak);
        total.sum_squares += sum_squares;
        total.samples += length;
        total.clipped_samples += clipped;
        impl_->levels_[ch].clipped = impl_->levels_[ch].clipped || clipped > 0;
        advance(ch, block_peak, sum_squares / static_cast<double>(length), seconds);
    }
}

SoundfieldVector energy_vector(std::span<const ChannelLevel> levels,
                               std::span<const std::optional<double>> azimuths_deg) {
    SoundfieldVector result;

    double x = 0.0;
    double y = 0.0;
    double total = 0.0;
    for (std::size_t ch = 0; ch < azimuths_deg.size() && ch < levels.size(); ++ch) {
        const auto& azimuth = azimuths_deg[ch];
        // A channel resting on the floor contributes nothing: counting it
        // would let the floor's own symmetry invent an image, and a silent
        // bed would report a phantom centre.
        if (!azimuth.has_value() || levels[ch].rms_db <= kFloorDb) {
            continue;
        }
        // rms_db is a level, so 10^(dB/10) recovers the power the vector sum
        // must weight by.
        const double energy = std::pow(10.0, levels[ch].rms_db / 10.0);
        x += energy * std::cos(*azimuth * kDegToRad);
        y += energy * std::sin(*azimuth * kDegToRad);
        total += energy;
    }
    if (total <= 0.0) {
        return result;
    }

    const double length = std::hypot(x, y);
    result.magnitude = std::min(1.0, length / total);
    result.level_db = std::max(kFloorDb, 10.0 * std::log10(total));
    // Below the numerical noise of the sum the direction is meaningless;
    // reporting front-centre would claim a phantom image that is not there.
    if (length > 0.0) {
        result.azimuth_deg = std::atan2(y, x) / kDegToRad;
    }
    return result;
}

}  // namespace iclforge::base
