#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <type_traits>

#include "iclforge/ac3/core/window.hpp"
#include "iclforge/dsp/detail/fft_stockham.hpp"
// The cosine and sine a constant evaluator can run (the QMF's constants and the sample rate
// converter's design are made with them): dsp's own, which this reads where it is.
#include "../../../dsp/src/tiered/portable_math.hpp"

// The transforms' angle-dependent tables (mdct.cpp's fast N/4-FFT structure and the §7.9.4.1
// pre/post twiddles), which were function-local statics of mdct.cpp and are here so that a test
// can hold the compile-time form of them to the run-time one.
//
// Two ways to have one. Built on first use, with the C library's cosine and sine (the
// constructors a table has with no argument): a guard to test at every call, a table in .bss, and
// the cost of its first transform. Or a constant (`kTwiddlesConstant` and the others below): data
// made by the compiler with dsp's portable cosine and sine (portable_math.hpp: within three units
// in the last place of a double, which a float table rounds away except for one entry in 2^29,
// and libs/ac3/tests/core/test_mdct_fixed.cpp holds every entry to the C library's on whatever
// machine runs it). Which a build uses for which scalar is mdct.cpp's rule, with the measurement
// behind it.
namespace iclforge::ac3::internal {

inline constexpr int kMdctN = kTransformLength;  // 512
inline constexpr double kMdctPi = std::numbers::pi;

// The C library's functions, for a table made when the program runs.
struct LibmCosSin {
    [[nodiscard]] static double cos(double angle) { return std::cos(angle); }
    [[nodiscard]] static double sin(double angle) { return std::sin(angle); }
};

// dsp's, for a table made by a constant evaluator.
struct PortableCosSin {
    [[nodiscard]] static constexpr double cos(double angle) {
        return iclforge::dsp::tiered::portable::cos(angle);
    }
    [[nodiscard]] static constexpr double sin(double angle) {
        // The C library's sine of a zero is that zero, with its sign, and the series' is +0: a
        // table's zero of the other sign is a different bit in whatever a product with it gives
        // (fft_stockham.hpp's header says why a factor's zero is not skipped).
        if (angle == 0.0) {
            return angle;
        }
        return iclforge::dsp::tiered::portable::sin(angle);
    }
};

// §7.9.4.1 step 2: xcos1[k] = -cos(2pi(8k+1)/8N), xsin1[k] = -sin(2pi(8k+1)/8N).
//
// Scalar (float32 for the minimum-footprint profile): the type the twiddles are STORED in.
// Computed in double and narrowed once on the way in, for the same reason
// the transform's own factors are (iclforge/dsp/detail/fft_stockham.hpp) - the angle here is small
// and exact and deserves the library call at full precision whatever the table holds. Default
// double, so every existing use is the one it always was.
template <typename Scalar = double>
struct Twiddles {
    std::array<Scalar, kMdctN / 4> cos1{};
    std::array<Scalar, kMdctN / 4> sin1{};

    Twiddles() : Twiddles(LibmCosSin{}) {}

    template <typename CosSin>
        requires requires(double angle) { CosSin::cos(angle); }
    constexpr explicit Twiddles(CosSin) {
        for (int k = 0; k < kMdctN / 4; ++k) {
            const double angle = 2.0 * kMdctPi * (8.0 * k + 1.0) / (8.0 * kMdctN);
            cos1[static_cast<std::size_t>(k)] = static_cast<Scalar>(-CosSin::cos(angle));
            sin1[static_cast<std::size_t>(k)] = static_cast<Scalar>(-CosSin::sin(angle));
        }
    }
};

// §7.9.4.2 step 2: xcos2[k] = -cos(2pi(8k+1)/4N), xsin2[k] = -sin(2pi(8k+1)/4N)
// (N = 512 throughout this section, per the spec's own note - these are NOT
// the 256-sample transform's own N).
template <typename Scalar = double>
struct Twiddles2 {
    std::array<Scalar, kMdctN / 8> cos2{};
    std::array<Scalar, kMdctN / 8> sin2{};

    Twiddles2() : Twiddles2(LibmCosSin{}) {}

    template <typename CosSin>
        requires requires(double angle) { CosSin::cos(angle); }
    constexpr explicit Twiddles2(CosSin) {
        for (int k = 0; k < kMdctN / 8; ++k) {
            const double angle = 2.0 * kMdctPi * (8.0 * k + 1.0) / (4.0 * kMdctN);
            cos2[static_cast<std::size_t>(k)] = static_cast<Scalar>(-CosSin::cos(angle));
            sin2[static_cast<std::size_t>(k)] = static_cast<Scalar>(-CosSin::sin(angle));
        }
    }
};

// Everything angle-dependent in the fold below, computed once per NLen -
// the same treatment Twiddles and the direct-form matrices give every
// other transform in this file, applied to the fast path itself (phase-5
// target 1 of the performance programme: this kernel runs 36x per frame in
// every encode path, plus 6x per object per frame inside band_energy, and
// its per-call cost was dominated by the 512 std::cos/std::sin libm calls
// below being made fresh on every transform). Two exactness classes:
//
// - pre/post twiddles: the EXACT expressions the fold used to evaluate per
//   call (std::cos/std::sin of -pi*m/M and -pi*(4k+1)/(4M)), stored instead
//   of re-evaluated - bit-identical values, the direct-form tables' own
//   reasoning (src/core/transform/reference/reference_transform.cpp).
// - FFT stage twiddles + bit-reversal permutation: the previous in-place
//   FFT generated each butterfly group's j-th twiddle by ITERATED complex
//   multiply (w *= wlen), so it carried j-1 accumulated rounding steps.
//   The table stores std::cos/std::sin of each exact angle -2*pi*j/len
//   instead - a (tiny) numerical change in the direction of MORE precision,
//   re-verified against the direct form's ground truth by
//   libs/ac3/tests/core/test_mdct_fast.cpp's unchanged 1e-10 bound.
template <int NLen, typename Scalar = double>
struct FastMdctTables {
    static constexpr std::size_t kM = static_cast<std::size_t>(NLen) / 2;
    static constexpr std::size_t kP = kM / 2;
    // z[m] pre-twiddle exp(-i*pi*m/M), split re/im.
    std::array<Scalar, kP> pre_re{};
    std::array<Scalar, kP> pre_im{};
    // w[k] post-twiddle exp(-i*pi*(4k+1)/(4M)), split re/im.
    std::array<Scalar, kP> post_re{};
    std::array<Scalar, kP> post_im{};
    // The P-point transform's plan - the family's one FFT, which dft512
    // runs at P = 512 too; see iclforge/dsp/detail/fft_stockham.hpp.
    iclforge::dsp::fft::StockhamTables<kP, Scalar> fft{};

    FastMdctTables() : FastMdctTables(LibmCosSin{}, 0) {}

    template <typename CosSin>
        requires requires(double angle) { CosSin::cos(angle); }
    constexpr explicit FastMdctTables(CosSin, int /*tag: a table made with this pair*/)
        : fft(
              [](double angle) { return CosSin::cos(angle); },
              [](double angle) { return CosSin::sin(angle); }) {
        for (std::size_t m = 0; m < kP; ++m) {
            const double ang = -kMdctPi * static_cast<double>(m) / static_cast<double>(kM);
            pre_re[m] = static_cast<Scalar>(CosSin::cos(ang));
            pre_im[m] = static_cast<Scalar>(CosSin::sin(ang));
            const double ang2 =
                -kMdctPi * (4.0 * static_cast<double>(m) + 1.0) / (4.0 * static_cast<double>(kM));
            post_re[m] = static_cast<Scalar>(CosSin::cos(ang2));
            post_im[m] = static_cast<Scalar>(CosSin::sin(ang2));
        }
    }
};

// The constants themselves, whichever scalar asks (instantiated only where used): what the rule
// above hands out, and what a test holds to the run-time form.
template <typename Scalar>
inline constexpr Twiddles<Scalar> kTwiddlesConstant{PortableCosSin{}};

template <typename Scalar>
inline constexpr Twiddles2<Scalar> kTwiddles2Constant{PortableCosSin{}};

template <int NLen, typename Scalar>
inline constexpr FastMdctTables<NLen, Scalar> kFastMdctTablesConstant{PortableCosSin{}, 0};

}  // namespace iclforge::ac3::internal
