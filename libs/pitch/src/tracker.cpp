#include "iclforge/pitch/tracker.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

// The McLeod Pitch Method over a window of samples (planning/karaoke-microphone.md, D7). See
// tracker.hpp for what it answers; this file is the arithmetic.

namespace iclforge::pitch {

namespace {

constexpr double kPi = std::numbers::pi;

// The most key maxima one window can have: a lobe is at least two lags wide.
constexpr std::size_t kNoMaximum = static_cast<std::size_t>(-1);

// A Blackman-windowed sinc low-pass for decimating by `factor`: the cut-off is a quarter of the
// input rate per unit of the ratio (4 kHz for 48 kHz to 16 kHz), which is all a voice's pitch needs
// and leaves the stopband under -70 dB from 6.9 kHz, so that nothing above the new Nyquist folds
// into the range a pitch is looked for in (a 9 kHz tone folded to a voiced 1 kHz with a wider
// transition). The length is 16 taps per input sample of the ratio plus one (odd, so the delay is a
// whole number of input samples), and the gain at DC is 1.
std::vector<float> decimation_taps(std::uint32_t factor) {
    if (factor <= 1) {
        return {1.0F};
    }
    const std::size_t length = static_cast<std::size_t>(factor) * 16 + 1;
    const double cutoff = 0.25 / static_cast<double>(factor);  // cycles per input sample
    const double centre = static_cast<double>(length - 1) / 2.0;
    std::vector<double> h(length);
    double sum = 0.0;
    for (std::size_t i = 0; i < length; ++i) {
        const double x = static_cast<double>(i) - centre;
        const double sinc = x == 0.0 ? 2.0 * cutoff : std::sin(2.0 * kPi * cutoff * x) / (kPi * x);
        const double w =
            0.42 -
            0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(length - 1)) +
            0.08 * std::cos(4.0 * kPi * static_cast<double>(i) / static_cast<double>(length - 1));
        h[i] = sinc * w;
        sum += h[i];
    }
    std::vector<float> taps(length);
    for (std::size_t i = 0; i < length; ++i) {
        taps[i] = static_cast<float>(h[i] / sum);
    }
    return taps;
}

std::uint32_t rounded(double value) {
    const double r = std::floor(value + 0.5);
    return r < 1.0 ? 1U : static_cast<std::uint32_t>(r);
}

}  // namespace

TrackerConfig TrackerConfig::for_input_rate(std::uint32_t input_rate) {
    TrackerConfig config;
    config.input_rate = input_rate;
    config.analysis_rate = input_rate;
    if (input_rate == 0) {
        return config;
    }
    // The smallest ratio that brings the rate to 16 kHz or under and divides it exactly.
    for (std::uint32_t factor = 1; factor <= input_rate; ++factor) {
        if (input_rate % factor == 0 && input_rate / factor <= 16000U) {
            config.analysis_rate = input_rate / factor;
            break;
        }
    }
    return config;
}

std::optional<Tracker> Tracker::make(const TrackerConfig& config) {
    if (config.input_rate == 0 || config.analysis_rate == 0 || config.frame_ms == 0 ||
        config.hop_ms == 0 || config.input_rate % config.analysis_rate != 0) {
        return std::nullopt;
    }
    if (!(config.min_hz > 0.0F) || !(config.max_hz > config.min_hz) ||
        !(config.max_hz * 4.0F <= static_cast<float>(config.analysis_rate))) {
        return std::nullopt;
    }
    if (!(config.peak_ratio > 0.0F && config.peak_ratio <= 1.0F) || !(config.clarity_min >= 0.0F)) {
        return std::nullopt;
    }
    const std::uint32_t factor = config.input_rate / config.analysis_rate;
    const std::uint32_t frame = rounded(static_cast<double>(config.analysis_rate) *
                                        static_cast<double>(config.frame_ms) / 1000.0);
    const std::uint32_t hop = rounded(static_cast<double>(config.analysis_rate) *
                                      static_cast<double>(config.hop_ms) / 1000.0);
    const std::uint32_t tau_min = std::max<std::uint32_t>(
        2U, static_cast<std::uint32_t>(std::floor(static_cast<double>(config.analysis_rate) /
                                                  static_cast<double>(config.max_hz))));
    const std::uint32_t tau_max =
        static_cast<std::uint32_t>(std::ceil(static_cast<double>(config.analysis_rate) /
                                             static_cast<double>(config.min_hz))) +
        1U;
    // The window must hold the lag range twice over: the correlation is taken over `frame -
    // tau_max`.
    if (tau_max <= tau_min + 2U || frame < 2U * tau_max) {
        return std::nullopt;
    }
    return Tracker(config, factor, frame, hop, tau_min, tau_max, decimation_taps(factor));
}

Tracker::Tracker(const TrackerConfig& config, std::uint32_t factor, std::uint32_t frame,
                 std::uint32_t hop, std::uint32_t tau_min, std::uint32_t tau_max,
                 std::vector<float> taps)
    : config_(config),
      factor_(factor),
      frame_(frame),
      hop_(hop),
      tau_min_(tau_min),
      tau_max_(tau_max),
      taps_(std::move(taps)),
      history_(taps_.size(), 0.0F),
      ring_(frame, 0.0F),
      window_(frame, 0.0F),
      nsdf_(static_cast<std::size_t>(tau_max) + 1U, 0.0),
      energy_(static_cast<std::size_t>(frame) + 1U, 0.0) {}

void Tracker::reset() {
    std::fill(history_.begin(), history_.end(), 0.0F);
    std::fill(ring_.begin(), ring_.end(), 0.0F);
    history_at_ = 0;
    phase_ = 0;
    input_seen_ = 0;
    ring_at_ = 0;
    analysis_seen_ = 0;
}

void Tracker::push(std::span<const float> samples, std::vector<Frame>& out) {
    const std::size_t length = taps_.size();
    for (const float sample : samples) {
        history_[history_at_] = sample;
        history_at_ = (history_at_ + 1U) % length;
        ++input_seen_;
        if (phase_ == 0) {
            // y[k] = sum h[j] x[kF - j]: the newest sample is the one just written.
            double acc = 0.0;
            std::size_t at = history_at_;
            for (std::size_t j = 0; j < length; ++j) {
                at = at == 0 ? length - 1U : at - 1U;
                acc += static_cast<double>(taps_[j]) * static_cast<double>(history_[at]);
            }
            push_analysis(static_cast<float>(acc), out);
        }
        phase_ = (phase_ + 1U) % factor_;
    }
}

void Tracker::push_analysis(float sample, std::vector<Frame>& out) {
    ring_[ring_at_] = sample;
    ring_at_ = (ring_at_ + 1U) % ring_.size();
    ++analysis_seen_;
    if (analysis_seen_ < static_cast<std::int64_t>(frame_)) {
        return;
    }
    if ((analysis_seen_ - static_cast<std::int64_t>(frame_)) % static_cast<std::int64_t>(hop_) !=
        0) {
        return;
    }
    // The window's first analysis sample is index analysis_seen_ - frame_; its centre is half a
    // window on.
    const std::int64_t centre_analysis =
        analysis_seen_ - static_cast<std::int64_t>(frame_) + static_cast<std::int64_t>(frame_ / 2U);
    // y[k] sits at input sample kF less the FIR's group delay.
    const std::int64_t delay = static_cast<std::int64_t>((taps_.size() - 1U) / 2U);
    out.push_back(analyse(centre_analysis * static_cast<std::int64_t>(factor_) - delay));
}

Frame Tracker::analyse(std::int64_t centre_sample) {
    const std::size_t n = frame_;
    // The ring's oldest sample is the one about to be overwritten.
    double mean = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const float v = ring_[(ring_at_ + i) % n];
        window_[i] = v;
        mean += static_cast<double>(v);
    }
    mean /= static_cast<double>(n);
    energy_[0] = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double v = static_cast<double>(window_[i]) - mean;
        window_[i] = static_cast<float>(v);
        energy_[i + 1U] = energy_[i] + v * v;
    }

    Frame frame;
    frame.centre_sample = centre_sample;
    const double rms = std::sqrt(energy_[n] / static_cast<double>(n));
    frame.level_db = static_cast<float>(std::max(-120.0, 20.0 * std::log10(rms + 1e-9)));
    if (frame.level_db < config_.level_floor_db) {
        return frame;
    }

    // The normalised square difference function: 2 r(tau) / m(tau), over the first w samples.
    const std::size_t tau_max = tau_max_;
    const std::size_t w = n - tau_max;
    const double e0 = energy_[w] - energy_[0];
    nsdf_[0] = 1.0;
    for (std::size_t tau = 1; tau <= tau_max; ++tau) {
        double r = 0.0;
        for (std::size_t j = 0; j < w; ++j) {
            r += static_cast<double>(window_[j]) * static_cast<double>(window_[j + tau]);
        }
        const double m = e0 + (energy_[tau + w] - energy_[tau]);
        nsdf_[tau] = m > 1e-12 ? 2.0 * r / m : 0.0;
    }

    // Key maxima: the highest point of each positive lobe after the first, from the lag floor on.
    // A window of a voiced frame holds at most a few dozen; two passes avoid a container.
    auto lobe_peaks = [&](auto&& visit) {
        std::size_t t = 1;
        while (t <= tau_max && nsdf_[t] > 0.0) {
            ++t;  // the lobe around lag 0 is not a period
        }
        while (t <= tau_max) {
            while (t <= tau_max && nsdf_[t] <= 0.0) {
                ++t;
            }
            std::size_t best = kNoMaximum;
            double value = 0.0;
            while (t <= tau_max && nsdf_[t] > 0.0) {
                if (nsdf_[t] > value) {
                    value = nsdf_[t];
                    best = t;
                }
                ++t;
            }
            // Parabolic interpolation needs a neighbour on both sides inside the computed range.
            if (best != kNoMaximum && best >= tau_min_ && best + 1U <= tau_max) {
                visit(best, value);
            }
        }
    };

    double highest = 0.0;
    lobe_peaks([&](std::size_t, double value) { highest = std::max(highest, value); });
    if (highest < static_cast<double>(config_.clarity_min)) {
        frame.clarity = static_cast<float>(std::clamp(highest, 0.0, 1.0));
        return frame;
    }
    const double threshold = static_cast<double>(config_.peak_ratio) * highest;
    std::size_t chosen = kNoMaximum;
    lobe_peaks([&](std::size_t tau, double value) {
        if (chosen == kNoMaximum && value >= threshold) {
            chosen = tau;
        }
    });
    if (chosen == kNoMaximum) {
        return frame;
    }

    const double a = nsdf_[chosen - 1U];
    const double b = nsdf_[chosen];
    const double c = nsdf_[chosen + 1U];
    const double denominator = a - 2.0 * b + c;
    double shift = denominator != 0.0 ? 0.5 * (a - c) / denominator : 0.0;
    shift = std::clamp(shift, -0.5, 0.5);
    const double peak = b - 0.25 * (a - c) * shift;
    const double lag = static_cast<double>(chosen) + shift;
    const double hz = static_cast<double>(config_.analysis_rate) / lag;
    frame.clarity = static_cast<float>(std::clamp(peak, 0.0, 1.0));
    if (peak < static_cast<double>(config_.clarity_min) ||
        hz < static_cast<double>(config_.min_hz) || hz > static_cast<double>(config_.max_hz)) {
        frame.clarity = 0.0F;
        return frame;
    }
    frame.hz = static_cast<float>(hz);
    return frame;
}

}  // namespace iclforge::pitch
