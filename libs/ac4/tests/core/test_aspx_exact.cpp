// Pseudocode 85's pre-flattening gains of the AC-4 SBR high-frequency generator, held to the code
// they replaced (planning/ac4.md, D14e): the generator keeps the cubic fit's vectors between calls
// and the reference makes them afresh each frame, and the two give the same BITS at the decoder's
// scalar and at double.
//
// This was the last test of libs/dsp/tests to include a header of libs/ac4/src: dsp is the lower
// library, and a comparison with the generator's own header is the generator's. The reference
// below is the code as it was, verbatim.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <random>
#include <span>
#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/aspx/hf_generator.hpp"
#include "iclforge/base/arithmetic/scalar_math.hpp"
#include "iclforge/dsp/detail/complex.hpp"
#include "iclforge/dsp/tiered/real.hpp"

namespace {

namespace dsp = iclforge::dsp::tiered;
using iclforge::dsp::tiered::Real;

template <typename Scalar>
bool same_bits(std::span<const Scalar> a, std::span<const Scalar> b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(Scalar)) == 0;
}

// Pseudocode 85's pre-flattening gains as they were before the cubic fit's vectors were kept
// between calls: libs/ac4/src/core/aspx/hf_generator.cpp's fit_cubic and preflattening_gains, with
// its two dB conversions, verbatim.
namespace reference_aspx {

constexpr double kTenOverLog2Of10 = 3.010299956639812;
constexpr double kLog2Of10Over20 = 0.16609640474436812;

template <typename Scalar>
Scalar power_db(Scalar x) {
    if constexpr (std::is_same_v<Scalar, double>) {
        return Scalar{10} * std::log10(x);
    } else {
        return static_cast<Scalar>(kTenOverLog2Of10) * iclforge::internal::scalar_log2(x);
    }
}

template <typename Scalar>
Scalar from_power_db(Scalar db) {
    if constexpr (std::is_same_v<Scalar, double>) {
        return std::pow(Scalar{10}, db / Scalar{20});
    } else {
        return iclforge::internal::scalar_exp2(static_cast<Scalar>(kLog2Of10Over20) * db);
    }
}

template <typename Scalar>
void fit_cubic(std::span<const Scalar> y, std::span<Scalar> fitted) {
    const std::size_t n = y.size();
    std::array<std::vector<double>, 4> basis;
    for (std::size_t k = 0; k < basis.size(); ++k) {
        basis[k].resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            const double t = n > 1 ? (2.0 * static_cast<double>(i) - static_cast<double>(n - 1)) /
                                         static_cast<double>(n - 1)
                                   : 0.0;
            basis[k][i] = std::pow(t, static_cast<double>(k));
        }
    }
    std::ranges::fill(fitted, Scalar{});
    for (std::size_t k = 0; k < basis.size(); ++k) {
        for (std::size_t m = 0; m < k; ++m) {
            double dot = 0.0;
            for (std::size_t i = 0; i < n; ++i) {
                dot += basis[k][i] * basis[m][i];
            }
            for (std::size_t i = 0; i < n; ++i) {
                basis[k][i] -= dot * basis[m][i];
            }
        }
        double norm = 0.0;
        for (const double v : basis[k]) {
            norm += v * v;
        }
        if (norm < 1e-18) {
            std::ranges::fill(basis[k], 0.0);
            continue;
        }
        norm = std::sqrt(norm);
        double projection = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            basis[k][i] /= norm;
            projection += basis[k][i] * static_cast<double>(y[i]);
        }
        for (std::size_t i = 0; i < n; ++i) {
            fitted[i] += static_cast<Scalar>(projection * basis[k][i]);
        }
    }
}

template <typename Scalar>
void preflattening_gains(std::span<const dsp::Complex<Scalar>> q_low, int sbx, int ts_begin,
                         int ts_end, std::span<Scalar> gain_vec) {
    const auto n = static_cast<std::size_t>(sbx);
    if (ts_end <= ts_begin || n == 0) {
        std::ranges::fill(gain_vec.first(n), Scalar{1});
        return;
    }
    std::vector<Scalar> pow_env(n);
    Scalar mean_energy{};
    for (std::size_t sb = 0; sb < n; ++sb) {
        Scalar energy{};
        for (int ts = ts_begin; ts < ts_end; ++ts) {
            energy += norm(q_low[static_cast<std::size_t>(ts) * 64 + sb]);
        }
        energy /= static_cast<Scalar>(ts_end - ts_begin);
        pow_env[sb] = power_db(energy + Scalar{1});
        mean_energy += pow_env[sb];
    }
    mean_energy /= static_cast<Scalar>(n);
    std::vector<Scalar> slope(n);
    fit_cubic<Scalar>(pow_env, slope);
    for (std::size_t sb = 0; sb < n; ++sb) {
        gain_vec[sb] = from_power_db(mean_energy - slope[sb]);
    }
}

}  // namespace reference_aspx

template <typename Scalar>
void preflattening_matches_reference() {
    namespace aspx = iclforge::ac4::detail::aspx;
    std::mt19937 rng(7);
    std::normal_distribution<double> normal;
    // One channel's state across frames whose crossover moves: the cache is for the last count and
    // is made again for a new one.
    aspx::CubicBasis cubic;
    std::vector<int> sbx_sequence;
    for (int sbx = 1; sbx <= 63; ++sbx) {
        sbx_sequence.push_back(sbx);
        sbx_sequence.push_back(sbx);  // the same count again: the vectors kept
    }
    for (int sbx = 63; sbx >= 1; sbx -= 7) {
        sbx_sequence.push_back(sbx);
    }
    for (const int sbx : sbx_sequence) {
        CAPTURE(sbx);
        const int slots = 8 + static_cast<int>(rng() % 24);
        std::vector<dsp::Complex<Scalar>> q_low(static_cast<std::size_t>(slots) * 64);
        for (auto& v : q_low) {
            v = dsp::Complex<Scalar>{static_cast<Scalar>(normal(rng) * 100.0),
                                     static_cast<Scalar>(normal(rng) * 100.0)};
        }
        std::array<Scalar, 64> kept{};
        std::array<Scalar, 64> fresh{};
        std::array<Scalar, 64> before{};
        aspx::preflattening_gains<Scalar>(q_low, sbx, 0, slots, kept, cubic);
        aspx::preflattening_gains<Scalar>(q_low, sbx, 0, slots, fresh);
        reference_aspx::preflattening_gains<Scalar>(q_low, sbx, 0, slots, before);
        CHECK(same_bits<Scalar>(
            std::span<const Scalar>(kept).first(static_cast<std::size_t>(sbx)),
            std::span<const Scalar>(before).first(static_cast<std::size_t>(sbx))));
        CHECK(same_bits<Scalar>(
            std::span<const Scalar>(fresh).first(static_cast<std::size_t>(sbx)),
            std::span<const Scalar>(before).first(static_cast<std::size_t>(sbx))));
    }
}
}  // namespace

TEST_CASE(
    "pre-flattening's gains keep the bits of the fit made afresh each frame at the decoder's "
    "scalar",
    "[ac4][core][aspx][exact]") {
    // At Fixed32 the gains are MantExp values, which this floating reference does not form.
    []<typename R>() {
        if constexpr (std::is_floating_point_v<R>) {
            preflattening_matches_reference<R>();
        }
    }.template operator()<Real>();
}

TEST_CASE("pre-flattening's gains keep the bits of the fit made afresh each frame at double",
          "[ac4][core][aspx][exact]") {
    preflattening_matches_reference<double>();
}
