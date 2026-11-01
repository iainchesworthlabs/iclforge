#include "encoder/render_bed.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace iclforge::truehd::detail {

namespace {

constexpr double kDegToRad = std::numbers::pi / 180.0;
constexpr int kMixFracBits = 12;
constexpr double kCentreEpsilon = 1e-12;

struct RingSpeaker {
    int row;
    double azimuth_deg;  // counter-clockwise from the front, in [0, 360)
};

// The 7.1 ring in ascending azimuth. The widest arc between neighbours is 60 degrees, so a pair
// always encloses a direction and the two-speaker solution below never goes singular.
constexpr std::array<RingSpeaker, 7> kRing{{
    {2, 0.0},     // C
    {0, 30.0},    // L
    {4, 90.0},    // Ls
    {6, 150.0},   // Lrs
    {7, 210.0},   // Rrs
    {5, 270.0},   // Rs
    {1, 330.0},   // R
}};

// The azimuth of a bed row (the sign is the left side's), for the speaker placements.
[[nodiscard]] double azimuth_of_row(int row) noexcept {
    switch (row) {
        case 0: return 30.0;
        case 1: return -30.0;
        case 4: return 90.0;
        case 5: return -90.0;
        case 6: return 150.0;
        case 7: return -150.0;
        default: return 0.0;  // C, and the LFE, which has no direction
    }
}

[[nodiscard]] double wrap360(double degrees) noexcept {
    double value = std::fmod(degrees, 360.0);
    if (value < 0.0) {
        value += 360.0;
    }
    return value;
}

}  // namespace

ElementPlacement bed_speaker_placement(int row) noexcept {
    const double azimuth = azimuth_of_row(row) * kDegToRad;
    const double left = std::sin(azimuth);
    const double forward = std::cos(azimuth);
    // Out to the wall along that direction: the larger of the two components reaches 0.5.
    const double scale = 0.5 / std::max(std::abs(left), std::abs(forward));
    ElementPlacement placement;
    placement.x = 0.5 - left * scale;
    placement.y = 0.5 - forward * scale;
    placement.z = 0.0;
    return placement;
}

std::array<double, kRenderBedRows> pan_to_bed(double x, double y, double z) noexcept {
    static_cast<void>(z);  // the legacy bed has no height: see the header
    std::array<double, kRenderBedRows> gains{};
    const double left = 0.5 - x;
    const double forward = 0.5 - y;
    const double azimuth = (std::abs(left) < kCentreEpsilon && std::abs(forward) < kCentreEpsilon)
                               ? 0.0
                               : wrap360(std::atan2(left, forward) / kDegToRad);
    std::size_t a = kRing.size() - 1;
    for (std::size_t i = 0; i + 1 < kRing.size(); ++i) {
        if (azimuth >= kRing[i].azimuth_deg && azimuth < kRing[i + 1].azimuth_deg) {
            a = i;
            break;
        }
    }
    const std::size_t b = (a + 1) % kRing.size();
    const double a_deg = kRing[a].azimuth_deg;
    const double b_deg = b > a ? kRing[b].azimuth_deg : kRing[b].azimuth_deg + 360.0;
    const double u_deg = azimuth >= a_deg ? azimuth : azimuth + 360.0;
    // 2D VBAP: solve [pa pb] g = u for the unit vectors, clamp at 0 and normalise.
    const double ax = std::cos(a_deg * kDegToRad);
    const double ay = std::sin(a_deg * kDegToRad);
    const double bx = std::cos(b_deg * kDegToRad);
    const double by = std::sin(b_deg * kDegToRad);
    const double ux = std::cos(u_deg * kDegToRad);
    const double uy = std::sin(u_deg * kDegToRad);
    const double det = ax * by - ay * bx;
    const double ga = std::max((ux * by - uy * bx) / det, 0.0);
    const double gb = std::max((ax * uy - ay * ux) / det, 0.0);
    const double norm = std::sqrt(ga * ga + gb * gb);
    if (norm > 0.0) {
        gains[static_cast<std::size_t>(kRing[a].row)] = ga / norm;
        gains[static_cast<std::size_t>(kRing[b].row)] = gb / norm;
    }
    return gains;
}

BedMixGains mix_gains(std::span<const ElementPlacement> free_elements) noexcept {
    BedMixGains gains{};
    const std::size_t count = std::min(free_elements.size(), gains[0].size());
    for (std::size_t k = 0; k < count; ++k) {
        const ElementPlacement& place = free_elements[k];
        const auto pan = pan_to_bed(place.x, place.y, place.z);
        for (std::size_t row = 0; row < gains.size(); ++row) {
            gains[row][k] = pan[row] * place.gain;
        }
    }
    return gains;
}

RenderPlan build_render_plan(int elements, std::int64_t section_samples,
                             std::int64_t total_samples,
                             std::span<const std::vector<ElementPlacement>> placements) {
    RenderPlan plan;
    plan.elements = elements;
    if (section_samples <= 0 || total_samples <= 0) {
        return plan;
    }
    const auto free_count = static_cast<std::size_t>(std::max(elements - kRenderBedRows, 0));
    const auto gains_of = [&](std::size_t boundary) {
        if (placements.empty()) {
            return BedMixGains{};
        }
        const std::vector<ElementPlacement>& at =
            placements[std::min(boundary, placements.size() - 1)];
        return mix_gains(
            std::span<const ElementPlacement>(at).first(std::min(at.size(), free_count)));
    };
    const std::int64_t sections = (total_samples + section_samples - 1) / section_samples;
    plan.sections.reserve(static_cast<std::size_t>(sections));
    for (std::int64_t s = 0; s < sections; ++s) {
        RenderSection section;
        section.start = s * section_samples;
        section.samples = std::min(section_samples, total_samples - section.start);
        section.begin = gains_of(static_cast<std::size_t>(s));
        section.end = gains_of(static_cast<std::size_t>(s) + 1);
        plan.sections.push_back(section);
    }
    return plan;
}

BedMixGains gains_at(const RenderPlan& plan, std::int64_t sample) noexcept {
    if (plan.sections.empty()) {
        return BedMixGains{};
    }
    const auto after = std::ranges::upper_bound(plan.sections, sample, {}, &RenderSection::start);
    const RenderSection& section =
        after == plan.sections.begin() ? plan.sections.front() : *(after - 1);
    const double fraction = std::clamp(
        static_cast<double>(sample - section.start) / static_cast<double>(section.samples), 0.0,
        1.0);
    BedMixGains gains{};
    for (std::size_t row = 0; row < gains.size(); ++row) {
        for (std::size_t k = 0; k < gains[row].size(); ++k) {
            const double from = section.begin[row][k];
            gains[row][k] = from + (section.end[row][k] - from) * fraction;
        }
    }
    return gains;
}

std::int32_t quantise_mix_gain(double gain, int frac_bits) noexcept {
    const double raw = std::nearbyint(gain * static_cast<double>(std::int64_t{1} << frac_bits));
    const double limit = static_cast<double>(std::int64_t{1} << (frac_bits + 1));
    return static_cast<std::int32_t>(std::clamp(raw, -limit, limit - 1.0));
}

std::vector<MatrixSpec> bed_mix_matrices(const BedMixGains& gains, int elements, int noise_shift) {
    std::vector<MatrixSpec> matrices;
    const auto free_count =
        static_cast<std::size_t>(std::clamp(elements - kRenderBedRows, 0, kRenderMaxFree));
    for (std::size_t row = 0; row < gains.size(); ++row) {
        MatrixSpec matrix;
        matrix.out_ch = static_cast<std::uint8_t>(row);
        matrix.frac_bits = kMixFracBits;
        matrix.field_m4 = static_cast<std::uint8_t>(noise_shift);
        matrix.present[row] = true;
        matrix.coeff[row] = 1 << kMixFracBits;
        bool mixes = false;
        for (std::size_t k = 0; k < free_count; ++k) {
            const std::int32_t raw = quantise_mix_gain(gains[row][k], kMixFracBits);
            if (raw != 0) {
                const auto column = static_cast<std::size_t>(kRenderBedRows) + k;
                matrix.present[column] = true;
                matrix.coeff[column] = -raw;
                mixes = true;
            }
        }
        if (mixes) {
            matrices.push_back(matrix);
        }
    }
    return matrices;
}

}  // namespace iclforge::truehd::detail
