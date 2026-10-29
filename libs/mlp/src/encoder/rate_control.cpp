#include "encoder/rate_control.hpp"

#include <algorithm>
#include <cmath>

namespace iclforge::mlp::detail {

Schedule::Schedule(std::uint64_t peak_bits_per_second, std::uint32_t sample_rate,
                   int samples_per_unit)
    : rate_(static_cast<double>(peak_bits_per_second)),
      fs_(static_cast<double>(sample_rate)),
      t_(static_cast<double>(samples_per_unit)),
      latency_(0.075 * static_cast<double>(sample_rate)) {}

std::expected<std::vector<std::uint16_t>, Error> Schedule::place(
    std::span<const std::size_t> bits) {
    const std::size_t count = bits.size();
    std::vector<double> duration(count);
    for (std::size_t k = 0; k < count; ++k) {
        duration[k] = static_cast<double>(bits[k]) * fs_ / rate_;
    }
    // Latest starts, back to front: each unit by its own deadline and before the next unit's start.
    std::vector<double> latest(count);
    double next_start = 1e300;
    for (std::size_t k = count; k-- > 0;) {
        const double n = static_cast<double>(next_ + static_cast<std::int64_t>(k));
        // The next unit starts no sooner than this one's end and a quarter unit after its start
        // (3.4), so this one starts at least that much before it.
        latest[k] = std::min(
            {(n - 1.0) * t_, n * t_ - duration[k], next_start - std::max(duration[k], t_ / 4.0)});
        next_start = latest[k];
    }
    std::vector<std::uint16_t> out(count);
    for (std::size_t k = 0; k < count; ++k) {
        const double n = static_cast<double>(next_ + static_cast<std::int64_t>(k));
        double start = latest[k];
        start = std::max({start, prev_end_, prev_start_ + t_ / 4.0});
        if (start + duration[k] > n * t_ + 1e-6 || n * t_ - start > latency_) {
            return std::unexpected(Error::kRateExceeded);
        }
        if (prev_start_ > -1e299) {
            const double gap = std::max(start - prev_start_, 1e-9);
            peak_ = std::max(peak_, static_cast<std::uint64_t>(prev_bits_ * fs_ / gap));
        }
        // Whole samples, rounded down so that the unit is never later than scheduled.
        const auto whole = static_cast<std::int64_t>(std::floor(start));
        out[k] = static_cast<std::uint16_t>(static_cast<std::uint64_t>(whole) & 0xFFFFU);
        prev_start_ = start;
        prev_end_ = start + duration[k];
        prev_bits_ = static_cast<double>(bits[k]);
    }
    next_ += static_cast<std::int64_t>(count);
    return out;
}

}  // namespace iclforge::mlp::detail
