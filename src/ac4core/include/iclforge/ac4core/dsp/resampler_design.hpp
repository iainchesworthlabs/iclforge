#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <numbers>

#include "iclforge/ac4core/dsp/kbd.hpp"
#include "iclforge/ac4core/dsp/portable_math.hpp"
#include "iclforge/ac4core/dsp/resampler.hpp"

// The design of the sample rate converter's filter (dsp/resampler.hpp), and the float tables built
// from it at compile time.
//
// The design is a function of the ratio, written once over the mathematical functions it calls:
// `Math` supplies ceil, sqrt, sin and the Kaiser window's I0. The converter at double designs its
// table at run time with the C library's (the `LibmMath` of dsp/resampler.cpp), as it always has,
// so that its output is the bytes it was. The converter at float keeps the table of the three
// ratios the decoder uses in read-only memory, built here by the compiler's constant evaluator with
// `PortableMath` (dsp/portable_math.hpp: no library, the same bits on every compiler and every
// target), so that no target designs anything at run time, which on a part with a single-precision
// FPU takes seconds at 1001/960 (planning/ac4.md, D14a5), and so that the table is the same float
// everywhere. A ratio that is not one of the three, which only a test asks for, is designed at run
// time with the same functions and gives the same coefficients.

namespace iclforge::ac4::detail::dsp {

// What the design fixes for a ratio up / down (in lowest terms, not 1): the filter's length and
// the constants its coefficients are computed from.
struct ResamplerDesign {
    int up = 1;
    int down = 1;
    int taps = 1;
    double passband = 0.5;  // edges in cycles per input sample
    double stopband = 0.5;
    double cutoff = 0.5;
    double beta = 0.0;  // the Kaiser window's, from Kaiser's estimate at the attenuation
    double norm = 1.0;  // I0(beta)
};

// The design's mathematical functions without a library, for the compile-time table.
struct PortableMath {
    [[nodiscard]] static constexpr double ceil(double x) noexcept { return portable::ceil(x); }
    [[nodiscard]] static constexpr double sqrt(double x) noexcept { return portable::sqrt(x); }
    [[nodiscard]] static constexpr double sin(double x) noexcept { return portable::sin(x); }
    [[nodiscard]] static constexpr double bessel_i0(double x) noexcept { return dsp::bessel_i0(x); }
};

// The passband runs to 0.86 of the lower rate's Nyquist frequency and the stopband starts at it;
// the cutoff is between the two; Kaiser's estimates give the window's beta and the length at the
// attenuation. up and down are positive, in lowest terms, and not equal.
template <typename Math>
[[nodiscard]] constexpr ResamplerDesign design_resampler(int up, int down) noexcept {
    ResamplerDesign d;
    d.up = up;
    d.down = down;
    // In cycles per input sample: the lower rate's Nyquist frequency is the stopband edge, and the
    // passband runs to 0.86 of it.
    const double nyquist = 0.5 * std::min(1.0, static_cast<double>(up) / static_cast<double>(down));
    d.passband = 0.86 * nyquist;
    d.stopband = nyquist;
    d.cutoff = 0.5 * (d.passband + d.stopband);
    // Kaiser's estimates for the window's beta and length at the attenuation.
    d.beta = 0.1102 * (kResamplerAttenuationDb - 8.7);
    const double length = (kResamplerAttenuationDb - 7.95) /
                              (2.285 * 2.0 * std::numbers::pi * (d.stopband - d.passband)) +
                          1.0;
    d.taps = 2 * static_cast<int>(Math::ceil(length / 2.0));
    d.norm = Math::bessel_i0(d.beta);
    return d;
}

// Phase p of the design, 0 <= p < up: `taps` coefficients, normalised to sum to 1 in double, in
// `row`. Coefficient k is the window and the sinc at t = (k - taps / 2 + 1) - p / up, the distance
// in input samples from the output to the input sample the coefficient weights.
//
// Phase up - p is phase p read from its last coefficient to its first, for 0 < p < up: the window
// and the sinc are even, and the t of coefficient k in the one is minus the t of coefficient
// `taps - 1 - k` in the other. That is why a table can keep half the phases.
template <typename Math>
constexpr void design_phase(const ResamplerDesign& d, int p, double* row) noexcept {
    const double half_width = static_cast<double>(d.taps) / 2.0;
    double sum = 0.0;
    for (int k = 0; k < d.taps; ++k) {
        // The distance from the output's position to the tap's input sample.
        const double t = static_cast<double>(k - d.taps / 2 + 1) -
                         static_cast<double>(p) / static_cast<double>(d.up);
        const double x = t / half_width;
        const double window =
            Math::bessel_i0(d.beta * Math::sqrt(std::max(0.0, 1.0 - x * x))) / d.norm;
        const double arg = std::numbers::pi * 2.0 * d.cutoff * t;
        const double sinc = t == 0.0 ? 1.0 : Math::sin(arg) / arg;
        row[k] = 2.0 * d.cutoff * sinc * window;
        sum += row[k];
    }
    for (int k = 0; k < d.taps; ++k) {
        row[k] = row[k] / sum;
    }
}

// How a coefficient of the double design is stored: rounded to float once, for the converter at
// float, or as Q1.30, for the fixed-point tier (planning/ac4.md, D14d), each coefficient times 2^30
// rounded half away from zero.
struct FloatCoefficient {
    using Type = float;
    [[nodiscard]] static constexpr float from_design(double value) noexcept {
        return static_cast<float>(value);
    }
};

struct Q30Coefficient {
    using Type = std::int32_t;
    [[nodiscard]] static constexpr std::int32_t from_design(double value) noexcept {
        const double scaled = value * 1073741824.0;
        return static_cast<std::int32_t>(scaled < 0.0 ? scaled - 0.5 : scaled + 0.5);
    }
};

// Phases 0 to up / 2 of the design, each coefficient stored as `Coefficient` says, in phase order,
// into `out`: the half of the table that a converter keeps. `row` is `taps` doubles of scratch.
template <typename Math, typename Coefficient>
constexpr void design_half_phases_as(const ResamplerDesign& d, typename Coefficient::Type* out,
                                     double* row) noexcept {
    const int phases = d.up / 2 + 1;
    for (int p = 0; p < phases; ++p) {
        design_phase<Math>(d, p, row);
        auto* phase_out = out + static_cast<std::size_t>(p) * static_cast<std::size_t>(d.taps);
        for (int k = 0; k < d.taps; ++k) {
            phase_out[k] = Coefficient::from_design(row[k]);
        }
    }
}

// The half a converter at float keeps, and the half the fixed-point tier keeps in Q1.30.
template <typename Math>
constexpr void design_half_phases(const ResamplerDesign& d, float* out, double* row) noexcept {
    design_half_phases_as<Math, FloatCoefficient>(d, out, row);
}

template <typename Math>
constexpr void design_half_phases_q30(const ResamplerDesign& d, std::int32_t* out, double* row) noexcept {
    design_half_phases_as<Math, Q30Coefficient>(d, out, row);
}

// The half table of the ratio Up / Down, for the compiler's constant evaluator.
template <typename Coefficient, int Up, int Down>
struct BasicHalfTable {
    static constexpr ResamplerDesign kDesign = design_resampler<PortableMath>(Up, Down);
    static constexpr int kTaps = kDesign.taps;
    static constexpr int kPhases = Up / 2 + 1;
    std::array<typename Coefficient::Type,
               static_cast<std::size_t>(kPhases) * static_cast<std::size_t>(kTaps)>
        coefficients{};
};

template <int Up, int Down>
using HalfTable = BasicHalfTable<FloatCoefficient, Up, Down>;
template <int Up, int Down>
using HalfTableQ30 = BasicHalfTable<Q30Coefficient, Up, Down>;

// consteval: the table is built by the compiler and at no other time, so no call that reaches run
// time can be written. The functions it calls stay constexpr, since a ratio outside the three is
// designed at run time with them (dsp/resampler.cpp).
template <typename Coefficient, int Up, int Down>
[[nodiscard]] consteval BasicHalfTable<Coefficient, Up, Down> design_half_table_as() {
    BasicHalfTable<Coefficient, Up, Down> table;
    std::array<double, static_cast<std::size_t>(BasicHalfTable<Coefficient, Up, Down>::kTaps)> row{};
    design_half_phases_as<PortableMath, Coefficient>(BasicHalfTable<Coefficient, Up, Down>::kDesign,
                                                     table.coefficients.data(), row.data());
    return table;
}

template <int Up, int Down>
[[nodiscard]] consteval HalfTable<Up, Down> design_half_table() {
    return design_half_table_as<FloatCoefficient, Up, Down>();
}

template <int Up, int Down>
[[nodiscard]] consteval HalfTableQ30<Up, Down> design_half_table_q30() {
    return design_half_table_as<Q30Coefficient, Up, Down>();
}

// The tables, evaluated by the compiler: variable templates, so that only a build that uses a
// ratio's table pays for evaluating it (the decoder's at float: dsp/resampler.cpp; at Fixed32, the
// Q1.30 one, so neither build evaluates or links the other's), and constexpr, so that the table is
// data in the program's read-only memory (flash, on a part that executes from it) and nothing the
// program computes.
template <int Up, int Down>
inline constexpr HalfTable<Up, Down> kHalfTable = design_half_table<Up, Down>();

template <int Up, int Down>
inline constexpr HalfTableQ30<Up, Down> kHalfTableQ30 = design_half_table_q30<Up, Down>();

}  // namespace iclforge::ac4::detail::dsp
