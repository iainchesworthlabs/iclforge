#include "ratectl_shape.hpp"

#include <algorithm>
#include <bit>

namespace iclforge::truehd::detail {

namespace {

constexpr std::int64_t kHighest = (std::int64_t{1} << 23) - 1;
constexpr std::int64_t kLowest = -(std::int64_t{1} << 23);
constexpr int kSilent = 24;  // every bit of a 24 bit sample is clear in silence

}  // namespace

int common_zero_bits(std::span<const std::vector<std::int32_t>> planes) noexcept {
    std::uint32_t seen = 0;
    for (const std::vector<std::int32_t>& plane : planes) {
        for (const std::int32_t v : plane) {
            seen |= static_cast<std::uint32_t>(v);
        }
    }
    return seen == 0 ? kSilent : std::countr_zero(seen);
}

int shape_planes(std::span<std::vector<std::int32_t>> planes, int drop_bits, int limit_zero_bits) {
    std::uint32_t seen = 0;
    for (const std::vector<std::int32_t>& plane : planes) {
        for (const std::int32_t v : plane) {
            seen |= static_cast<std::uint32_t>(v);
        }
    }
    if (seen == 0) {
        return kSilent;
    }
    const int clear = std::countr_zero(seen);
    const int step = std::min(clear + std::max(drop_bits, 0), limit_zero_bits);
    if (step <= clear) {
        return clear;
    }
    const std::int64_t half = std::int64_t{1} << (step - 1);
    const std::int64_t top = (kHighest >> step) << step;
    const std::int64_t bottom = (kLowest >> step) << step;
    for (std::vector<std::int32_t>& plane : planes) {
        for (std::int32_t& v : plane) {
            const std::int64_t rounded = ((std::int64_t{v} + half) >> step) << step;
            v = static_cast<std::int32_t>(std::clamp(rounded, bottom, top));
        }
    }
    return step;
}

}  // namespace iclforge::truehd::detail
