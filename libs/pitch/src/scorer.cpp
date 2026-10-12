#include "iclforge/pitch/scorer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace iclforge::pitch {

Scorer::Scorer(Contour reference, const ScoreOptions& options)
    : reference_(std::move(reference)), options_(options), sung_(reference_.hz.size(), kUnset) {}

void Scorer::clear() {
    std::fill(sung_.begin(), sung_.end(), kUnset);
}

void Scorer::add(std::int64_t item_ms, float hz) {
    if (item_ms < 0 || reference_.hop_ms == 0 || !(hz >= 0.0F)) {
        return;
    }
    const std::int64_t hop = static_cast<std::int64_t>(reference_.hop_ms);
    const std::int64_t index = (item_ms + hop / 2) / hop;
    if (static_cast<std::size_t>(index) >= sung_.size()) {
        return;
    }
    sung_[static_cast<std::size_t>(index)] = hz;
}

double Scorer::deviation(float reference_hz, float sung_hz) const {
    double cents = cents_between(static_cast<double>(reference_hz), static_cast<double>(sung_hz)) -
                   options_.key_shift_cents;
    if (options_.fold_octaves) {
        cents = fold_octaves(cents);
    }
    return cents;
}

Tally Scorer::tally(std::int64_t start_ms, std::int64_t end_ms) const {
    Tally tally;
    if (reference_.hop_ms == 0 || end_ms <= start_ms) {
        return tally;
    }
    const std::int64_t hop = static_cast<std::int64_t>(reference_.hop_ms);
    // Grid point i is at i * hop; a point belongs to [start, end) when its time does.
    const std::int64_t first = (std::max<std::int64_t>(start_ms, 0) + hop - 1) / hop;
    const std::int64_t last = (end_ms - 1) / hop;
    double sum = 0.0;
    for (std::int64_t i = first; i <= last && static_cast<std::size_t>(i) < reference_.hz.size();
         ++i) {
        const std::size_t at = static_cast<std::size_t>(i);
        const float ref = reference_.hz[at];
        if (!(ref > 0.0F)) {
            continue;
        }
        ++tally.counted;
        const float sung = sung_[at];
        if (!(sung > 0.0F)) {
            continue;
        }
        ++tally.sung;
        const double d = deviation(ref, sung);
        sum += d;
        if (std::fabs(d) <= options_.tolerance_cents) {
            ++tally.hit;
        }
    }
    tally.mean_cents = tally.sung == 0 ? 0.0 : sum / static_cast<double>(tally.sung);
    return tally;
}

Tally Scorer::total() const {
    return tally(0, reference_.duration_ms() + static_cast<std::int64_t>(reference_.hop_ms));
}

std::optional<std::int32_t> Scorer::best_lag_ms(std::int32_t max_lag_ms) const {
    if (reference_.hop_ms == 0 || max_lag_ms < 0) {
        return std::nullopt;
    }
    const std::int64_t hop = static_cast<std::int64_t>(reference_.hop_ms);
    const std::int64_t max_steps = max_lag_ms / hop;
    const std::int64_t n = static_cast<std::int64_t>(reference_.hz.size());

    std::uint32_t counted = 0;
    for (const float ref : reference_.hz) {
        counted += ref > 0.0F ? 1U : 0U;
    }
    if (counted < 20) {
        return std::nullopt;
    }
    std::uint32_t best_hits = 0;
    std::int64_t best_step = 0;
    bool any = false;
    // Singer late by L: the singer's point at i + L matches the reference's at i.
    for (std::int64_t step = -max_steps; step <= max_steps; ++step) {
        std::uint32_t hits = 0;
        for (std::int64_t i = 0; i < n; ++i) {
            const std::int64_t j = i + step;
            if (j < 0 || j >= n) {
                continue;
            }
            const float ref = reference_.hz[static_cast<std::size_t>(i)];
            const float sung = sung_[static_cast<std::size_t>(j)];
            if (ref > 0.0F && sung > 0.0F &&
                std::fabs(deviation(ref, sung)) <= options_.tolerance_cents) {
                ++hits;
            }
        }
        if (!any || hits > best_hits ||
            (hits == best_hits && std::llabs(step) < std::llabs(best_step))) {
            best_hits = hits;
            best_step = step;
            any = true;
        }
    }
    if (!any || best_hits < 10) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(best_step * hop);
}

std::optional<double> Scorer::key_offset_cents() const {
    std::vector<double> deviations;
    const std::size_t n = std::min(reference_.hz.size(), sung_.size());
    for (std::size_t i = 0; i < n; ++i) {
        const float ref = reference_.hz[i];
        const float sung = sung_[i];
        if (ref > 0.0F && sung > 0.0F) {
            deviations.push_back(
                fold_octaves(cents_between(static_cast<double>(ref), static_cast<double>(sung))));
        }
    }
    if (deviations.size() < 20) {
        return std::nullopt;
    }
    std::sort(deviations.begin(), deviations.end());
    const std::size_t mid = deviations.size() / 2;
    return deviations.size() % 2 == 1 ? deviations[mid]
                                      : 0.5 * (deviations[mid - 1U] + deviations[mid]);
}

}  // namespace iclforge::pitch
