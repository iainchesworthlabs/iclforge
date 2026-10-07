#include "meta/drc.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>

#include "tiered/tables/qmf_tables.hpp"

namespace iclforge::ac4::detail {
namespace {

// |H(f)|^2 of a biquad at 48 kHz.
[[nodiscard]] double biquad_power(const std::array<double, 3>& b, const std::array<double, 3>& a,
                                  double hz) {
    const double w = 2.0 * std::numbers::pi * hz / 48000.0;
    const std::complex<double> z1 = std::polar(1.0, -w);
    const std::complex<double> z2 = z1 * z1;
    const std::complex<double> h = (b[0] + b[1] * z1 + b[2] * z2) / (a[0] + a[1] * z1 + a[2] * z2);
    return std::norm(h);
}

}  // namespace

double DrcCurve::gain(double level) const noexcept {
    const auto between = [](double from_gain, double to_gain, double t) {
        return from_gain + (to_gain - from_gain) * t;
    };
    if (level < max_boost_level) {
        return max_boost_gain;
    }
    if (level <= section_boost_level) {
        const double span = section_boost_level - max_boost_level;
        return span > 0.0 ? between(section_boost_gain, max_boost_gain,
                                    (section_boost_level - level) / span)
                          : section_boost_gain;
    }
    if (level <= null_low) {
        const double span = null_low - section_boost_level;
        return span > 0.0 ? section_boost_gain * (null_low - level) / span : 0.0;
    }
    if (level <= null_high) {
        return 0.0;
    }
    if (level <= section_cut_level) {
        const double span = section_cut_level - null_high;
        return span > 0.0 ? section_cut_gain * (level - null_high) / span : 0.0;
    }
    if (level <= max_cut_level) {
        const double span = max_cut_level - section_cut_level;
        return span > 0.0
                   ? between(section_cut_gain, max_cut_gain, (level - section_cut_level) / span)
                   : section_cut_gain;
    }
    return max_cut_gain;
}

double k_weight(double hz) {
    constexpr std::array<double, 3> kShelfB = {1.53512485958697, -2.69169618940638,
                                               1.19839281085285};
    constexpr std::array<double, 3> kShelfA = {1.0, -1.69065929318241, 0.73248077421585};
    constexpr std::array<double, 3> kHighPassB = {1.0, -2.0, 1.0};
    constexpr std::array<double, 3> kHighPassA = {1.0, -1.99004745483398, 0.99007225036621};
    const double f = std::min(hz, 23500.0);
    return biquad_power(kShelfB, kShelfA, f) * biquad_power(kHighPassB, kHighPassA, f);
}

std::array<double, 64> k_weights(double rate_hz) {
    constexpr int kSubbands = 64;
    std::array<double, kSubbands> weights{};
    for (int k = 0; k < kSubbands; ++k) {
        weights[static_cast<std::size_t>(k)] =
            k_weight((static_cast<double>(k) + 0.5) * rate_hz / (2.0 * kSubbands));
    }
    return weights;
}

double qmf_energy_gain() noexcept {
    double gain = 0.0;
    for (const float w : tables::kQwin) {
        gain += static_cast<double>(w) * static_cast<double>(w);
    }
    return gain;
}

double DrcSmoothing::step(const DrcCurve& curve, double power, double dialnorm,
                          double slot_ms) noexcept {
    const double level_lkfs = 10.0 * std::log10(power) + kLkfsOffset - dialnorm;
    const double target = std::exp2(curve.gain(level_lkfs) / 6.0);
    if (!primed) {
        level = power;
        gain = target;
        primed = true;
        return gain;
    }
    double tau = level < power ? curve.attack_ms : curve.release_ms;
    if (curve.adaptive) {
        const double change = 10.0 * std::log10(power / std::max(level, kDrcPowerFloor));
        if (change > curve.attack_threshold) {
            tau = curve.attack_fast_ms;
        } else if (change > 0.0) {
            tau = curve.attack_ms;
        } else if (-change <= curve.release_threshold) {
            tau = curve.release_ms;
        } else {
            tau = curve.release_fast_ms;
        }
    }
    const double alpha = tau > 0.0 ? std::exp2(-slot_ms / tau) : 0.0;
    level = alpha * level + (1.0 - alpha) * power;
    gain = alpha * gain + (1.0 - alpha) * target;
    return gain;
}

}  // namespace iclforge::ac4::detail
