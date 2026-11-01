#include "encoder/cluster_elements.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace iclforge::truehd::detail {

namespace {

constexpr double kHeightWeight = 0.5;  // z runs -1 to 1 where x and y run 0 to 1
constexpr int kKmeansPasses = 3;
constexpr double kTinyWeight = 1e-9;
constexpr std::size_t kMaxObjects = 128;
constexpr auto kBedRowCount = static_cast<std::size_t>(kRenderBedRows);

[[nodiscard]] double distance(const ElementPlacement& a, const ElementPlacement& b) noexcept {
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    const double dz = (a.z - b.z) * kHeightWeight;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

[[nodiscard]] ClusterError invalid(ClusterErrorKind kind, std::string detail) {
    return ClusterError{kind, std::move(detail)};
}

// The key track of an object at one sample: a straight line between the keys around it, held
// before the first and after the last.
[[nodiscard]] ClusterKey key_at(const std::vector<ClusterKey>& keys, std::int64_t sample) noexcept {
    const auto after = std::ranges::upper_bound(keys, sample, {}, &ClusterKey::sample);
    if (after == keys.begin()) {
        return keys.front();
    }
    if (after == keys.end()) {
        return keys.back();
    }
    const ClusterKey& from = *(after - 1);
    const ClusterKey& to = *after;
    const double f = static_cast<double>(sample - from.sample) /
                     static_cast<double>(to.sample - from.sample);
    ClusterKey out;
    out.sample = sample;
    out.x = from.x + (to.x - from.x) * f;
    out.y = from.y + (to.y - from.y) * f;
    out.z = from.z + (to.z - from.z) * f;
    out.gain = from.gain + (to.gain - from.gain) * f;
    return out;
}

[[nodiscard]] ElementPlacement place_of(const ClusterKey& key) noexcept {
    return ElementPlacement{key.x, key.y, key.z, 1.0};
}

// The gain of an object at consecutive samples, without a search for each one.
class GainCursor {
   public:
    explicit GainCursor(const std::vector<ClusterKey>& keys) noexcept : keys_(&keys) {}

    [[nodiscard]] double at(std::int64_t sample) noexcept {
        const std::vector<ClusterKey>& keys = *keys_;
        while (index_ + 1 < keys.size() && keys[index_ + 1].sample <= sample) {
            ++index_;
        }
        const ClusterKey& from = keys[index_];
        if (index_ + 1 >= keys.size() || sample <= from.sample) {
            return from.gain;
        }
        const ClusterKey& to = keys[index_ + 1];
        const double f = static_cast<double>(sample - from.sample) /
                         static_cast<double>(to.sample - from.sample);
        return from.gain + (to.gain - from.gain) * f;
    }

   private:
    const std::vector<ClusterKey>* keys_;
    std::size_t index_ = 0;
};

[[nodiscard]] std::expected<void, ClusterError> check(std::span<const ClusterObject> objects,
                                                      const ClusterConfig& config) {
    if (config.elements != 12 && config.elements != 14 && config.elements != 16) {
        return std::unexpected(invalid(ClusterErrorKind::kInvalidConfig,
                                       "the number of elements is 12, 14 or 16"));
    }
    if (config.section_samples <= 0 || config.handover_samples < 0 ||
        config.handover_samples % 2 != 0 || config.handover_samples > config.section_samples) {
        return std::unexpected(invalid(
            ClusterErrorKind::kInvalidConfig,
            "the section is at least one sample and the handover is even and within it"));
    }
    if (config.snap_radius < 0.0 || config.hysteresis < 0.0 || config.bits < 2 ||
        config.bits > 32) {
        return std::unexpected(
            invalid(ClusterErrorKind::kInvalidConfig,
                    "snap radius and hysteresis are not negative; bits 2 to 32"));
    }
    if (objects.size() > kMaxObjects) {
        return std::unexpected(invalid(ClusterErrorKind::kInvalidObject, "too many objects"));
    }
    for (const ClusterObject& object : objects) {
        if (object.pcm.size() != objects.front().pcm.size()) {
            return std::unexpected(
                invalid(ClusterErrorKind::kInvalidObject, "the objects differ in length"));
        }
        if (object.bed_row < -1 || object.bed_row >= kRenderBedRows) {
            return std::unexpected(
                invalid(ClusterErrorKind::kInvalidObject, "a bed row is 0 to 7, or -1"));
        }
        if (object.keys.empty()) {
            return std::unexpected(
                invalid(ClusterErrorKind::kInvalidObject, "an object has no key"));
        }
        for (std::size_t k = 1; k < object.keys.size(); ++k) {
            if (object.keys[k].sample <= object.keys[k - 1].sample) {
                return std::unexpected(invalid(ClusterErrorKind::kInvalidObject,
                                               "the keys of an object increase in sample"));
            }
        }
    }
    return {};
}

// What the assignment of one section needs to know about an object.
struct Candidate {
    std::size_t index = 0;
    ElementPlacement place;
    double weight = 0.0;
};

// Assigns the dynamic objects that are not at a speaker to the free elements, element numbers
// `first` to `first + slots`. Returns each candidate's element, in candidate order.
[[nodiscard]] std::vector<int> assign_free(const std::vector<Candidate>& candidates,
                                           const std::vector<int>& previous, int first, int slots,
                                           double hysteresis) {
    const auto count = candidates.size();
    const auto slot_count = static_cast<std::size_t>(slots);
    std::vector<int> result(count, first);
    const auto previous_slot = [&](std::size_t c) {
        const int element = previous[candidates[c].index];
        return element >= first && element < first + slots ? element - first : -1;
    };

    if (count <= slot_count) {
        // One element each: an object keeps the one it had, the others take the lowest left.
        std::vector<bool> taken(slot_count, false);
        std::vector<int> chosen(count, -1);
        for (std::size_t c = 0; c < count; ++c) {
            const int slot = previous_slot(c);
            if (slot >= 0 && !taken[static_cast<std::size_t>(slot)]) {
                taken[static_cast<std::size_t>(slot)] = true;
                chosen[c] = slot;
            }
        }
        std::size_t lowest = 0;
        for (std::size_t c = 0; c < count; ++c) {
            if (chosen[c] < 0) {
                while (taken[lowest]) {
                    ++lowest;
                }
                taken[lowest] = true;
                chosen[c] = static_cast<int>(lowest);
            }
        }
        for (std::size_t c = 0; c < count; ++c) {
            result[c] = first + chosen[c];
        }
        return result;
    }

    // More objects than elements: the loudest ones seed an element each, the others join the
    // nearest, and the elements move to the centre of what they have, a few passes.
    std::vector<std::size_t> by_weight(count);
    std::iota(by_weight.begin(), by_weight.end(), std::size_t{0});
    std::ranges::stable_sort(by_weight, [&](std::size_t a, std::size_t b) {
        return candidates[a].weight > candidates[b].weight;
    });
    std::vector<ElementPlacement> centre(slot_count);
    std::vector<bool> taken(slot_count, false);
    std::vector<std::size_t> seeds(by_weight.begin(),
                                   by_weight.begin() + static_cast<std::ptrdiff_t>(slot_count));
    std::vector<int> seed_slot(slot_count, -1);
    for (std::size_t s = 0; s < slot_count; ++s) {
        const int slot = previous_slot(seeds[s]);
        if (slot >= 0 && !taken[static_cast<std::size_t>(slot)]) {
            taken[static_cast<std::size_t>(slot)] = true;
            seed_slot[s] = slot;
        }
    }
    std::size_t lowest = 0;
    for (std::size_t s = 0; s < slot_count; ++s) {
        if (seed_slot[s] < 0) {
            while (taken[lowest]) {
                ++lowest;
            }
            taken[lowest] = true;
            seed_slot[s] = static_cast<int>(lowest);
        }
        centre[static_cast<std::size_t>(seed_slot[s])] = candidates[seeds[s]].place;
    }
    std::vector<int> member(count, 0);
    for (int pass = 0; pass < kKmeansPasses; ++pass) {
        for (std::size_t c = 0; c < count; ++c) {
            int best = 0;
            double best_cost = std::numeric_limits<double>::max();
            for (std::size_t s = 0; s < slot_count; ++s) {
                double cost = distance(candidates[c].place, centre[s]);
                if (previous_slot(c) == static_cast<int>(s)) {
                    cost -= hysteresis;
                }
                if (cost < best_cost) {
                    best_cost = cost;
                    best = static_cast<int>(s);
                }
            }
            member[c] = best;
        }
        for (std::size_t s = 0; s < slot_count; ++s) {
            double total = 0.0;
            double x = 0.0;
            double y = 0.0;
            double z = 0.0;
            for (std::size_t c = 0; c < count; ++c) {
                if (member[c] == static_cast<int>(s)) {
                    const double w = candidates[c].weight + kTinyWeight;
                    total += w;
                    x += w * candidates[c].place.x;
                    y += w * candidates[c].place.y;
                    z += w * candidates[c].place.z;
                }
            }
            if (total > 0.0) {
                centre[s] = ElementPlacement{x / total, y / total, z / total, 1.0};
            }
        }
    }
    for (std::size_t c = 0; c < count; ++c) {
        result[c] = first + member[c];
    }
    return result;
}

}  // namespace

std::expected<ClusterResult, ClusterError> cluster_objects(std::span<const ClusterObject> objects,
                                                           const ClusterConfig& config) {
    if (auto ok = check(objects, config); !ok) {
        return std::unexpected(ok.error());
    }
    const std::size_t count = objects.size();
    const std::int64_t total =
        objects.empty() ? 0 : static_cast<std::int64_t>(objects.front().pcm.size());
    const std::int64_t length = config.section_samples;
    const std::int64_t sections = (total + length - 1) / length;
    const auto section_count = static_cast<std::size_t>(sections);
    const auto elements = static_cast<std::size_t>(config.elements);

    ClusterResult result;
    result.elements = config.elements;
    result.section_samples = length;
    result.total_samples = total;
    result.signals.assign(elements, std::vector<std::int32_t>(static_cast<std::size_t>(total), 0));
    if (total == 0 || count == 0) {
        result.placements.assign(section_count + 1, std::vector<ElementPlacement>(elements));
        for (auto& boundary : result.placements) {
            for (std::size_t e = 0; e < kBedRowCount; ++e) {
                boundary[e] = bed_speaker_placement(static_cast<int>(e));
            }
        }
        result.assignment.assign(section_count, std::vector<int>(count, 0));
        return result;
    }

    // The energy of each object (its gain included) in each section.
    std::vector<std::vector<double>> energy(count, std::vector<double>(section_count, 0.0));
    for (std::size_t i = 0; i < count; ++i) {
        GainCursor gain(objects[i].keys);
        for (std::int64_t t = 0; t < total; ++t) {
            const auto at = static_cast<std::size_t>(t);
            const double v = gain.at(t) * static_cast<double>(objects[i].pcm[at]);
            energy[i][static_cast<std::size_t>(t / length)] += v * v;
        }
    }

    // The assignment of each section.
    std::array<ElementPlacement, kRenderBedRows> speaker;
    for (int row = 0; row < kRenderBedRows; ++row) {
        speaker[static_cast<std::size_t>(row)] = bed_speaker_placement(row);
    }
    const int first_free = kRenderBedRows;
    const int free_slots = config.elements - kRenderBedRows;
    std::vector<int> previous(count, -1);
    result.assignment.reserve(section_count);
    for (std::size_t s = 0; s < section_count; ++s) {
        const std::int64_t start = static_cast<std::int64_t>(s) * length;
        const std::int64_t centre = start + std::min(length, total - start) / 2;
        std::vector<int> now(count, -1);
        std::vector<Candidate> loose;
        for (std::size_t i = 0; i < count; ++i) {
            if (objects[i].bed_row >= 0) {
                now[i] = objects[i].bed_row;
                continue;
            }
            const ElementPlacement place = place_of(key_at(objects[i].keys, centre));
            int nearest = -1;
            double nearest_cost = std::numeric_limits<double>::max();
            for (int row = 0; row < kRenderBedRows; ++row) {
                if (row == kBedLfeRow) {
                    continue;  // nothing is panned to the LFE
                }
                double cost =
                    distance(place, speaker[static_cast<std::size_t>(row)]) - config.snap_radius;
                if (previous[i] == row) {
                    cost -= config.hysteresis;
                }
                if (cost < nearest_cost) {
                    nearest_cost = cost;
                    nearest = row;
                }
            }
            if (nearest >= 0 && nearest_cost <= 0.0) {
                now[i] = nearest;
            } else {
                loose.push_back(Candidate{i, place, energy[i][s]});
            }
        }
        const std::vector<int> chosen =
            assign_free(loose, previous, first_free, free_slots, config.hysteresis);
        for (std::size_t c = 0; c < loose.size(); ++c) {
            now[loose[c].index] = chosen[c];
        }
        previous = now;
        result.assignment.push_back(std::move(now));
    }

    // Where each element is at each boundary: the energy-weighted centre of the objects that are
    // in it on either side of the boundary, at the boundary's instant.
    result.placements.assign(section_count + 1, std::vector<ElementPlacement>(elements));
    std::vector<ElementPlacement> held(elements);
    for (std::size_t e = 0; e < elements; ++e) {
        held[e] = e < kBedRowCount ? speaker[e] : ElementPlacement{0.5, 0.0, 0.0, 1.0};
    }
    for (std::size_t b = 0; b <= section_count; ++b) {
        const std::int64_t at = std::min(static_cast<std::int64_t>(b) * length, total);
        std::vector<double> weight(elements, 0.0);
        std::vector<std::array<double, 3>> sum(elements, std::array<double, 3>{0.0, 0.0, 0.0});
        std::vector<std::size_t> members(elements, 0);
        std::vector<std::array<double, 3>> plain(elements, std::array<double, 3>{0.0, 0.0, 0.0});
        for (std::size_t i = 0; i < count; ++i) {
            const ElementPlacement place = place_of(key_at(objects[i].keys, at));
            for (int side = 0; side < 2; ++side) {
                if ((side == 0 && b == 0) || (side == 1 && b == section_count)) {
                    continue;
                }
                const std::size_t section = side == 0 ? b - 1 : b;
                const auto e = static_cast<std::size_t>(result.assignment[section][i]);
                const double w = energy[i][section];
                weight[e] += w;
                sum[e][0] += w * place.x;
                sum[e][1] += w * place.y;
                sum[e][2] += w * place.z;
                ++members[e];
                plain[e][0] += place.x;
                plain[e][1] += place.y;
                plain[e][2] += place.z;
            }
        }
        for (std::size_t e = 0; e < elements; ++e) {
            if (e >= kBedRowCount && members[e] > 0) {
                if (weight[e] > 0.0) {
                    held[e] = ElementPlacement{sum[e][0] / weight[e], sum[e][1] / weight[e],
                                               sum[e][2] / weight[e], 1.0};
                } else {
                    const auto n = static_cast<double>(members[e]);
                    held[e] = ElementPlacement{plain[e][0] / n, plain[e][1] / n, plain[e][2] / n,
                                               1.0};
                }
            }
            result.placements[b][e] = held[e];
        }
    }

    // The signals: each object into its element with its gain, handed over from one element to
    // the next over `handover_samples` centred on a boundary where its element changes.
    const std::int64_t half = config.handover_samples / 2;
    const auto limit_high = static_cast<double>((std::int64_t{1} << (config.bits - 1)) - 1);
    const auto limit_low = -static_cast<double>(std::int64_t{1} << (config.bits - 1));
    std::vector<GainCursor> gains;
    gains.reserve(count);
    for (const ClusterObject& object : objects) {
        gains.emplace_back(object.keys);
    }
    std::vector<double> accumulator(elements);
    for (std::int64_t t = 0; t < total; ++t) {
        std::ranges::fill(accumulator, 0.0);
        const auto at = static_cast<std::size_t>(t);
        const auto section = static_cast<std::size_t>(t / length);
        const std::int64_t section_start = static_cast<std::int64_t>(section) * length;
        // The boundary whose handover this sample is in, if any, and how far through it is.
        std::size_t other = section;
        double through = 1.0;  // the share of the section's own element
        if (half > 0 && section > 0 && t < section_start + half) {
            other = section - 1;
            through = static_cast<double>(t - (section_start - half) + 1) /
                      static_cast<double>(2 * half + 1);
        } else if (half > 0 && section + 1 < section_count &&
                   t >= section_start + length - half) {
            other = section + 1;
            through = 1.0 - static_cast<double>(t - (section_start + length - half) + 1) /
                                static_cast<double>(2 * half + 1);
        }
        for (std::size_t i = 0; i < count; ++i) {
            const double v = gains[i].at(t) * static_cast<double>(objects[i].pcm[at]);
            if (v == 0.0) {
                continue;
            }
            accumulator[static_cast<std::size_t>(result.assignment[section][i])] += v * through;
            if (other != section) {
                const auto other_element = static_cast<std::size_t>(result.assignment[other][i]);
                accumulator[other_element] += v * (1.0 - through);
            }
        }
        for (std::size_t e = 0; e < elements; ++e) {
            const double clipped = std::clamp(accumulator[e], limit_low, limit_high);
            if (clipped != accumulator[e] && std::abs(accumulator[e] - clipped) >= 0.5) {
                ++result.clipped_samples;
            }
            result.signals[e][at] = static_cast<std::int32_t>(std::llround(clipped));
        }
    }
    return result;
}

RenderPlan make_render_plan(const ClusterResult& clustering) {
    std::vector<std::vector<ElementPlacement>> free_places;
    free_places.reserve(clustering.placements.size());
    for (const std::vector<ElementPlacement>& boundary : clustering.placements) {
        free_places.emplace_back(boundary.begin() + kRenderBedRows, boundary.end());
    }
    return build_render_plan(clustering.elements, clustering.section_samples,
                             clustering.total_samples, free_places);
}

}  // namespace iclforge::truehd::detail
