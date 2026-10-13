#include "iclforge/pitch/bleed.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace iclforge::pitch {

namespace {

// The coarse search runs at a quarter of the rate: a delay and a correlation do not need more, and
// the search is the expensive part.
constexpr std::size_t kDecimation = 4;
// A correlation under this finds no delay: what the two streams share is not worth a name.
constexpr double kMinDelayCorrelation = 0.12;

[[nodiscard]] float level_db_of(double energy, std::size_t count) {
    if (count == 0) {
        return -120.0F;
    }
    const double rms = std::sqrt(energy / static_cast<double>(count));
    return static_cast<float>(std::max(-120.0, 20.0 * std::log10(rms + 1e-12)));
}

}  // namespace

void BleedMonitor::Ring::init(std::size_t capacity) {
    data.assign(std::max<std::size_t>(capacity, 1), 0.0F);
    seen = 0;
}

void BleedMonitor::Ring::push(float v) {
    data[static_cast<std::size_t>(seen) % data.size()] = v;
    ++seen;
}

float BleedMonitor::Ring::at(std::int64_t index) const {
    const auto capacity = static_cast<std::int64_t>(data.size());
    if (index < 0 || index >= seen || index < seen - capacity) {
        return 0.0F;
    }
    return data[static_cast<std::size_t>(index % capacity)];
}

void BleedMonitor::Ring::clear() {
    std::fill(data.begin(), data.end(), 0.0F);
    seen = 0;
}

BleedMonitor::BleedMonitor(const BleedOptions& options) : options_(options) {
    const auto samples = [&](std::uint32_t ms) {
        return static_cast<std::size_t>(static_cast<std::uint64_t>(options_.sample_rate) * ms /
                                        1000U);
    };
    frame_ = std::max<std::size_t>(samples(options_.frame_ms), 16);
    hop_ = std::max<std::size_t>(samples(options_.hop_ms), 1);
    max_delay_ = samples(options_.max_delay_ms);
    lead_ = samples(options_.lead_ms);
    window_ = std::max<std::size_t>(samples(options_.delay_window_ms), 4 * kDecimation * 16);
    update_ = std::max<std::size_t>(samples(options_.delay_update_ms), 1);
    // 2.5 ms either side of the delay found, which is more than a device's jitter and less than a
    // pitch period of the lowest voice.
    refine_ = std::max<std::size_t>(samples(3), 1);
    const std::size_t played_capacity = window_ + max_delay_ + lead_ + 2 * frame_ + 4096;
    played_.init(played_capacity);
    mic_.init(window_ + 2 * frame_ + 4096);
    reset();
}

void BleedMonitor::reset() {
    played_.clear();
    mic_.clear();
    next_end_ = static_cast<std::int64_t>(frame_);
    // The first search waits for a second of microphone (or the whole window if that is shorter).
    next_update_ = static_cast<std::int64_t>(std::min<std::size_t>(options_.sample_rate, window_));
    delay_.reset();
    recent_explained_ = 0.0F;
}

void BleedMonitor::push_played(std::span<const float> samples) {
    for (const float s : samples) {
        played_.push(s);
    }
}

std::optional<double> BleedMonitor::delay_ms() const {
    if (!delay_) {
        return std::nullopt;
    }
    return 1000.0 * static_cast<double>(*delay_) / static_cast<double>(options_.sample_rate);
}

void BleedMonitor::push_microphone(std::span<const float> samples, std::vector<BleedFrame>& out) {
    for (const float s : samples) {
        mic_.push(s);
        if (mic_.seen >= next_update_) {
            estimate_delay();
            next_update_ = mic_.seen + static_cast<std::int64_t>(update_);
        }
        while (mic_.seen >= next_end_) {
            // A frame is analysed as soon as it is complete; the loop only repeats if a hop has
            // been outrun, which a sample-by-sample push cannot do.
            out.push_back(analyse(next_end_));
            next_end_ += static_cast<std::int64_t>(hop_);
        }
    }
}

void BleedMonitor::estimate_delay() {
    const std::int64_t end = mic_.seen;
    const std::size_t width =
        static_cast<std::size_t>(std::min<std::int64_t>(static_cast<std::int64_t>(window_), end));
    if (width < static_cast<std::size_t>(options_.sample_rate) / 2) {
        return;
    }
    const std::int64_t start = end - static_cast<std::int64_t>(width);

    // The coarse streams: means of four samples.
    const std::size_t n = width / kDecimation;
    coarse_mic_.assign(n, 0.0F);
    double mic_energy = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        float sum = 0.0F;
        for (std::size_t k = 0; k < kDecimation; ++k) {
            sum += mic_.at(start + static_cast<std::int64_t>(i * kDecimation + k));
        }
        coarse_mic_[i] = sum / static_cast<float>(kDecimation);
        const auto m = static_cast<double>(coarse_mic_[i]);
        mic_energy += m * m;
    }
    if (mic_energy <= 0.0) {
        return;
    }
    // The played stream over [start - max_delay, end + lead): the microphone at i heard the played
    // signal at i - delay.
    const std::int64_t base = start - static_cast<std::int64_t>(max_delay_);
    const std::size_t span_len = (width + max_delay_ + lead_) / kDecimation + 1;
    coarse_played_.assign(span_len, 0.0F);
    for (std::size_t i = 0; i < span_len; ++i) {
        float sum = 0.0F;
        for (std::size_t k = 0; k < kDecimation; ++k) {
            sum += played_.at(base + static_cast<std::int64_t>(i * kDecimation + k));
        }
        coarse_played_[i] = sum / static_cast<float>(kDecimation);
    }
    // Prefix sums of the played energy, for the energy under each lag.
    coarse_energy_.assign(span_len + 1, 0.0);
    for (std::size_t i = 0; i < span_len; ++i) {
        coarse_energy_[i + 1] = coarse_energy_[i] + static_cast<double>(coarse_played_[i]) *
                                                        static_cast<double>(coarse_played_[i]);
    }

    // Lag l (coarse samples): the microphone at i is compared with the played at i - l, in the
    // played array at index (max_delay / D) + i - l.
    const std::int64_t origin = static_cast<std::int64_t>(max_delay_ / kDecimation);
    const std::int64_t lag_min =
        -static_cast<std::int64_t>((lead_ + kDecimation - 1) / kDecimation);
    const std::int64_t lag_max = static_cast<std::int64_t>(max_delay_ / kDecimation);
    double best = 0.0;
    std::int64_t best_lag = 0;
    for (std::int64_t lag = lag_min; lag <= lag_max; ++lag) {
        const std::int64_t at = origin - lag;  // played index aligned with microphone index 0
        if (at < 0 || at + static_cast<std::int64_t>(n) > static_cast<std::int64_t>(span_len)) {
            continue;
        }
        double dot = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            dot += static_cast<double>(coarse_mic_[i]) *
                   static_cast<double>(coarse_played_[static_cast<std::size_t>(at) + i]);
        }
        const double played_energy = coarse_energy_[static_cast<std::size_t>(at) + n] -
                                     coarse_energy_[static_cast<std::size_t>(at)];
        if (played_energy <= 1e-12) {
            continue;
        }
        const double ncc = std::fabs(dot) / std::sqrt(mic_energy * played_energy);
        if (ncc > best) {
            best = ncc;
            best_lag = lag;
        }
    }
    if (best < kMinDelayCorrelation) {
        return;
    }

    // Refine at the full rate around four times the coarse lag.
    const std::int64_t centre = best_lag * static_cast<std::int64_t>(kDecimation);
    double full_mic_energy = 0.0;
    for (std::size_t i = 0; i < width; ++i) {
        const double m = static_cast<double>(mic_.at(start + static_cast<std::int64_t>(i)));
        full_mic_energy += m * m;
    }
    if (full_mic_energy <= 0.0) {
        return;
    }
    double best_full = 0.0;
    std::int64_t best_full_lag = centre;
    const std::int64_t reach = static_cast<std::int64_t>(2 * kDecimation);
    for (std::int64_t lag = centre - reach; lag <= centre + reach; ++lag) {
        double dot = 0.0;
        double played_energy = 0.0;
        for (std::size_t i = 0; i < width; ++i) {
            const std::int64_t m_index = start + static_cast<std::int64_t>(i);
            const double m = static_cast<double>(mic_.at(m_index));
            const double p = static_cast<double>(played_.at(m_index - lag));
            dot += m * p;
            played_energy += p * p;
        }
        if (played_energy <= 1e-12) {
            continue;
        }
        const double ncc = std::fabs(dot) / std::sqrt(full_mic_energy * played_energy);
        if (ncc > best_full) {
            best_full = ncc;
            best_full_lag = lag;
        }
    }
    if (best_full < kMinDelayCorrelation) {
        return;
    }
    delay_ = best_full_lag;
    recent_explained_ = static_cast<float>(best_full * best_full);
}

BleedFrame BleedMonitor::analyse(std::int64_t end) {
    BleedFrame frame;
    const std::int64_t start = end - static_cast<std::int64_t>(frame_);
    frame.centre_sample = start + static_cast<std::int64_t>(frame_ / 2);

    double mic_energy = 0.0;
    for (std::size_t i = 0; i < frame_; ++i) {
        const double m = static_cast<double>(mic_.at(start + static_cast<std::int64_t>(i)));
        mic_energy += m * m;
    }
    frame.level_db = level_db_of(mic_energy, frame_);
    if (!delay_) {
        return frame;
    }
    frame.known = true;
    if (mic_energy <= 0.0) {
        return frame;
    }
    // The delay found, searched again within a few milliseconds for this frame alone.
    double best_ncc = 0.0;
    double best_gain = 0.0;
    const std::int64_t reach = static_cast<std::int64_t>(refine_);
    for (std::int64_t lag = *delay_ - reach; lag <= *delay_ + reach; ++lag) {
        double dot = 0.0;
        double played_energy = 0.0;
        for (std::size_t i = 0; i < frame_; ++i) {
            const std::int64_t m_index = start + static_cast<std::int64_t>(i);
            const double m = static_cast<double>(mic_.at(m_index));
            const double p = static_cast<double>(played_.at(m_index - lag));
            dot += m * p;
            played_energy += p * p;
        }
        if (played_energy <= 1e-12) {
            continue;
        }
        const double ncc = std::fabs(dot) / std::sqrt(mic_energy * played_energy);
        if (ncc > best_ncc) {
            best_ncc = ncc;
            best_gain = std::fabs(dot) / played_energy;
        }
    }
    frame.explained = static_cast<float>(best_ncc * best_ncc);
    if (best_gain > 0.0) {
        frame.gain_db = static_cast<float>(std::max(-120.0, 20.0 * std::log10(best_gain)));
    }
    frame.bleed =
        frame.explained > options_.explained_min && frame.level_db > options_.level_floor_db;
    return frame;
}

}  // namespace iclforge::pitch
