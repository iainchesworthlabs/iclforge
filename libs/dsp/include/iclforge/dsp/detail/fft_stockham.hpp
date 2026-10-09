#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <type_traits>
#include <utility>

#include "iclforge/dsp/detail/complex.hpp"

// The one FFT of the family (planning/consolidation.md decision 20): the passes of a Stockham
// autosort transform, decimation in frequency, one function for each radix and direction, with
// nothing a pass does decided at run time. AC-4's plan for every length 2^a 3^b 5^c
// (libs/dsp/src/tiered/fft.hpp) runs them, and so does every transform of AC-3 and dsp at a
// length known when it is compiled (StockhamTables and stockham_forward below): the MDCT's 64 and
// 128 points, enhanced coupling's 512.
//
// A pass takes a[i] = x[q + s (p + i m)] for i < r, forms the radix-r butterfly b[k] =
// sum_i a[i] e^(-/+2 pi i ik/r) and writes b[k] w^(pk) to y[q + s (r p + k)], for p < m
// and q < s, where w^(pk) is the factor the plan holds for the pass (twiddles[p r + k]),
// conjugated in the inverse direction. The radix and the direction are template arguments, so
// the butterfly is straight-line code on the registers, which on a core with a single-precision
// FPU and no out-of-order issue is a factor of three or more (planning/ac4.md, D14e).
//
// The value and the factor have types of their own, V and W. AC-4's plan has both its scalar;
// AC-3's MDCT runs double, float, four transforms side by side in an AVX2 register (V f64x4, W
// double) and its fixed-point tier's ImdctValue with Fixed32 factors (libs/ac3/src/core/
// mdct_fixed.hpp). Every expression below is a sum, a difference or a product of a V and a W, so
// each instantiation performs the same operations in the same order.
//
// Every statement performs the operations of the loop this replaced in AC-4's plan, in the same
// order, so its outputs are the same bits at every scalar (libs/dsp/tests/tiered/test_dsp_exact.cpp
// holds each to a verbatim copy of that loop). Two rewrites are not a copy of the old text, and
// each is an identity of IEEE 754 arithmetic rather than a reassociation:
//
//   x - (-y) is x + y, and (-x) + y is y - x, since a subtraction is the addition of
//   the negated operand and an addition commutes. The old code multiplied by the
//   conjugate of a factor, c = (w.re, -w.im), where the products b.re c.im and b.im c.im
//   are exactly the negations of b.re w.im and b.im w.im, so b c is
//   (b.re w.re + b.im w.im, b.im w.re - b.re w.im) with the same two products rounded
//   the same way and the same sum of them. A factor of 1 is not skipped: a product with
//   it can change the sign of a zero, and so a bit of the output.

namespace iclforge::dsp::tiered::fft_kernels {

// One pass's description, as the plan lays it out (dsp/fft.hpp).
struct Stage {
    int radix = 0;
    std::size_t n = 0;        // the sub-transform length this pass splits
    std::size_t stride = 0;   // how many sub-transforms run side by side
    std::size_t twiddle = 0;  // offset of this pass's factors in the plan's table
};

// b * w, or b * conj(w) in the inverse direction.
template <bool Inverse, typename V, typename W>
[[nodiscard]] inline Complex<V> turn(Complex<V> b, Complex<W> w) noexcept {
    if constexpr (Inverse) {
        return {b.re * w.re + b.im * w.im, b.im * w.re - b.re * w.im};
    } else {
        return {b.re * w.re - b.im * w.im, b.re * w.im + b.im * w.re};
    }
}

template <typename V, bool Inverse>
inline void butterfly2(const Complex<V>* a, Complex<V>* b) noexcept {
    b[0] = a[0] + a[1];
    b[1] = a[0] - a[1];
}

// Forward: d13 = (a1 - a3) times -i; inverse: times +i.
template <typename V, bool Inverse>
inline void butterfly4(const Complex<V>* a, Complex<V>* b) noexcept {
    const Complex<V> s02 = a[0] + a[2];
    const Complex<V> d02 = a[0] - a[2];
    const Complex<V> s13 = a[1] + a[3];
    const Complex<V> t = a[1] - a[3];
    b[0] = s02 + s13;
    b[2] = s02 - s13;
    if constexpr (Inverse) {
        b[1] = Complex<V>{d02.re - t.im, d02.im + t.re};
        b[3] = Complex<V>{d02.re + t.im, d02.im - t.re};
    } else {
        b[1] = Complex<V>{d02.re + t.im, d02.im - t.re};
        b[3] = Complex<V>{d02.re - t.im, d02.im + t.re};
    }
}

// f(std::integral_constant<int, 0>), ... f(std::integral_constant<int, Count - 1>).
template <int Count, typename F>
inline void static_for(F&& f) {
    [&]<int... I>(std::integer_sequence<int, I...>) {
        (f(std::integral_constant<int, I>{}), ...);
    }(std::make_integer_sequence<int, Count>{});
}

// The radix-3 and radix-5 butterflies: b[k] = a[0] + sum over i >= 1 of a[i] roots[ik mod r],
// each product turned by the root, the terms added in the order of i.
template <typename V, int Radix, bool Inverse, typename W>
inline void butterfly_odd(const Complex<V>* a, Complex<V>* b, const Complex<W>* roots) noexcept {
    static_for<Radix>([&](auto kc) {
        constexpr int k = decltype(kc)::value;
        Complex<V> sum = a[0];
        static_for<Radix - 1>([&](auto ic) {
            constexpr int i = decltype(ic)::value + 1;
            sum += turn<Inverse>(a[i], roots[(i * k) % Radix]);
        });
        b[k] = sum;
    });
}

template <typename V, int Radix, bool Inverse, typename W>
inline void butterfly(const Complex<V>* a, Complex<V>* b, const Complex<W>* roots3,
                      const Complex<W>* roots5) noexcept {
    if constexpr (Radix == 2) {
        butterfly2<V, Inverse>(a, b);
    } else if constexpr (Radix == 4) {
        butterfly4<V, Inverse>(a, b);
    } else if constexpr (Radix == 3) {
        butterfly_odd<V, 3, Inverse>(a, b, roots3);
    } else {
        butterfly_odd<V, 5, Inverse>(a, b, roots5);
    }
}

// One pass over inputs `in(index)` (a callable returning the Complex at that index of the pass's
// input), each output handed to `store(index, value)`. SkipUnit leaves out the products with the
// factors that are 1 - those of k = 0, and all of p = 0, which is every factor of a pass that
// splits n into sub-transforms of one - for a transform whose outputs are not held to AC-4's
// plan's bits (see above: the product can change the sign of a zero).
//
// m and s are a std::size_t, or a std::integral_constant where the plan is known when the program
// is compiled (stockham_forward below), which makes every loop bound and index step a constant.
template <typename V, int Radix, bool Inverse, bool SkipUnit = false, typename In, typename Store,
          typename M, typename S, typename W>
inline void pass_to(const In& in, const Store& store, M m, S s, const Complex<W>* tw,
                    const Complex<W>* roots3, const Complex<W>* roots5) noexcept {
    constexpr auto r = static_cast<std::size_t>(Radix);
    const std::size_t step = s * m;
    for (std::size_t p = 0; p < m; ++p) {
        const Complex<W>* w = tw + p * r;
        const std::size_t out = s * r * p;
        for (std::size_t q = 0; q < s; ++q) {
            Complex<V> a[r];
            Complex<V> b[r];
            const std::size_t first = q + s * p;
            static_for<Radix>([&](auto ic) {
                constexpr int i = decltype(ic)::value;
                a[i] = in(first + static_cast<std::size_t>(i) * step);
            });
            butterfly<V, Radix, Inverse>(a, b, roots3, roots5);
            if constexpr (SkipUnit) {
                // w^(p 0) is 1 for every p, and w^(0 k) for every k.
                store(out + q, b[0]);
                if (p == 0) {
                    static_for<Radix - 1>([&](auto kc) {
                        constexpr int k = decltype(kc)::value + 1;
                        store(out + q + static_cast<std::size_t>(k) * s, b[k]);
                    });
                } else {
                    static_for<Radix - 1>([&](auto kc) {
                        constexpr int k = decltype(kc)::value + 1;
                        store(out + q + static_cast<std::size_t>(k) * s, turn<Inverse>(b[k], w[k]));
                    });
                }
            } else {
                static_for<Radix>([&](auto kc) {
                    constexpr int k = decltype(kc)::value;
                    store(out + q + static_cast<std::size_t>(k) * s, turn<Inverse>(b[k], w[k]));
                });
            }
        }
    }
}

// The same pass into the array y.
template <typename V, int Radix, bool Inverse, typename In, typename W>
inline void pass(const In& in, Complex<V>* y, std::size_t m, std::size_t s,
                 const Complex<W>* tw, const Complex<W>* roots3,
                 const Complex<W>* roots5) noexcept {
    pass_to<V, Radix, Inverse>(
        in, [y](std::size_t index, Complex<V> value) noexcept { y[index] = value; }, m, s, tw,
        roots3, roots5);
}

// The pass `stage` over the plain array x into y.
template <typename V, bool Inverse, typename W>
inline void pass_from_array(const Stage& stage, const Complex<V>* x, Complex<V>* y,
                            const Complex<W>* twiddles, const Complex<W>* roots3,
                            const Complex<W>* roots5) noexcept {
    const std::size_t m = stage.n / static_cast<std::size_t>(stage.radix);
    const Complex<W>* tw = twiddles + stage.twiddle;
    const auto in = [x](std::size_t index) noexcept { return x[index]; };
    switch (stage.radix) {
        case 2:
            pass<V, 2, Inverse>(in, y, m, stage.stride, tw, roots3, roots5);
            break;
        case 3:
            pass<V, 3, Inverse>(in, y, m, stage.stride, tw, roots3, roots5);
            break;
        case 4:
            pass<V, 4, Inverse>(in, y, m, stage.stride, tw, roots3, roots5);
            break;
        default:
            pass<V, 5, Inverse>(in, y, m, stage.stride, tw, roots3, roots5);
            break;
    }
}

// All passes of a plan, the first reading `first(index)` and the others the two buffers
// in turn: the first writes b, the second a, and so on. Returns the buffer the last pass
// wrote. The first pass is radix 4 in every plan this library builds for a transform
// length that is a multiple of 4, and then it takes the fused input; a plan whose first
// pass is another radix gets the same input read from `first` by that radix's pass.
template <typename V, bool Inverse, typename W, typename First>
[[nodiscard]] inline Complex<V>* run_stages(const Stage* stages, std::size_t count,
                                            const Complex<W>* twiddles,
                                            const Complex<W>* roots3,
                                            const Complex<W>* roots5, const First& first,
                                            Complex<V>* a, Complex<V>* b) noexcept {
    Complex<V>* in = nullptr;
    Complex<V>* out = b;
    for (std::size_t i = 0; i < count; ++i) {
        const Stage& stage = stages[i];
        if (i == 0) {
            const std::size_t m = stage.n / static_cast<std::size_t>(stage.radix);
            const Complex<W>* tw = twiddles + stage.twiddle;
            switch (stage.radix) {
                case 2:
                    pass<V, 2, Inverse>(first, out, m, stage.stride, tw, roots3, roots5);
                    break;
                case 3:
                    pass<V, 3, Inverse>(first, out, m, stage.stride, tw, roots3, roots5);
                    break;
                case 4:
                    pass<V, 4, Inverse>(first, out, m, stage.stride, tw, roots3, roots5);
                    break;
                default:
                    pass<V, 5, Inverse>(first, out, m, stage.stride, tw, roots3, roots5);
                    break;
            }
        } else {
            pass_from_array<V, Inverse>(stage, in, out, twiddles, roots3, roots5);
        }
        in = out;
        out = (out == b) ? a : b;
    }
    return in;
}

}  // namespace iclforge::dsp::tiered::fft_kernels

namespace iclforge::dsp::fft {

using tiered::Complex;
using tiered::fft_kernels::Stage;

// The plan of a P-point transform, P a power of two known when it is compiled: radix 4 passes,
// and one radix-2 pass last when log2 P is odd, as AC-4's plan factors it (tiered/fft.cpp), with
// each pass's factors w^(pk) = e^(-2 pi i (pk mod n) / n) computed in double, the angle reduced
// first, and narrowed to W once.
template <std::size_t P, typename W = double>
struct StockhamTables {
    static_assert((P & (P - 1)) == 0 && P >= 4, "a power of two, at least 4");

    static constexpr std::size_t passes() {
        std::size_t count = 0;
        for (std::size_t n = P; n > 1; n /= (n % 4 == 0 ? 4 : 2)) {
            ++count;
        }
        return count;
    }
    static constexpr std::size_t kPasses = passes();
    // A pass splitting n holds n factors.
    static constexpr std::size_t factors() {
        std::size_t total = 0;
        for (std::size_t n = P; n > 1; n /= (n % 4 == 0 ? 4 : 2)) {
            total += n;
        }
        return total;
    }

    std::array<Stage, kPasses> stages{};
    std::array<Complex<W>, factors()> twiddles{};

    StockhamTables() {
        std::size_t n = P;
        std::size_t stride = 1;
        std::size_t count = 0;
        for (std::size_t i = 0; i < kPasses; ++i) {
            const std::size_t r = n % 4 == 0 ? 4 : 2;
            const std::size_t m = n / r;
            stages[i] = Stage{static_cast<int>(r), n, stride, count};
            for (std::size_t p = 0; p < m; ++p) {
                for (std::size_t k = 0; k < r; ++k) {
                    const double angle = -2.0 * std::numbers::pi *
                                         static_cast<double>((p * k) % n) / static_cast<double>(n);
                    twiddles[count + p * r + k] = Complex<W>{static_cast<W>(std::cos(angle)),
                                                             static_cast<W>(std::sin(angle))};
                }
            }
            count += m * r;
            n = m;
            stride *= r;
        }
    }
};

namespace detail {

// Pass I of a P-point plan, with its length, stride and factors' offset as constants, so that
// every loop bound and index step in it is one: AC-3's transforms run where a loop over a table of
// passes costs as much as the arithmetic, a fixed-point decoder on a microcontroller.
template <std::size_t P, std::size_t I>
struct PassShape {
    using Before = PassShape<P, I - 1>;
    static constexpr std::size_t n = Before::n / Before::r;
    static constexpr std::size_t r = n % 4 == 0 ? 4 : 2;
    static constexpr std::size_t m = n / r;
    static constexpr std::size_t s = Before::s * Before::r;
    static constexpr std::size_t offset = Before::offset + Before::n;
};

template <std::size_t P>
struct PassShape<P, 0> {
    static constexpr std::size_t n = P;
    static constexpr std::size_t r = n % 4 == 0 ? 4 : 2;
    static constexpr std::size_t m = n / r;
    static constexpr std::size_t s = 1;
    static constexpr std::size_t offset = 0;
};

// Storage for N values that is not initialised: every pass writes the whole of its output before
// the next reads it, and a value type with a default member initialiser (AC-3's fixed-point
// ImdctValue) would otherwise be zeroed on every transform, a pass's worth of stores on a
// microcontroller.
template <typename T, std::size_t N>
union Scratch {
    static_assert(std::is_trivially_destructible_v<T>);
    std::array<T, N> values;
    Scratch() noexcept {}
};

}  // namespace detail

// The forward transform, unscaled, sum_n x[n] e^(-2 pi i k n / P), over separate real and
// imaginary parts in natural order, in place, with the unit factors' products left out (SkipUnit
// above): these transforms run on parts with no FPU, where each is a software multiply.
// `between(re, im)` sees the values after each pass, in whichever buffer holds them, and may
// change them: a fixed-point caller sheds bits there (libs/ac3/src/core/eac3_tools.cpp's
// dft512_fixed).
template <std::size_t P, typename V, typename W, typename Between>
void stockham_forward(const StockhamTables<P, W>& t, std::span<V, P> re, std::span<V, P> im,
                      const Between& between) {
    using tiered::fft_kernels::pass_to;
    using tiered::fft_kernels::static_for;
    detail::Scratch<V, P> re2;
    detail::Scratch<V, P> im2;
    V* const buffers_re[2] = {re.data(), re2.values.data()};
    V* const buffers_im[2] = {im.data(), im2.values.data()};
    constexpr auto kPasses = static_cast<int>(StockhamTables<P, W>::kPasses);
    static_for<kPasses>([&](auto ic) {
        constexpr auto i = static_cast<std::size_t>(decltype(ic)::value);
        using Shape = detail::PassShape<P, i>;
        const V* const src_re = buffers_re[i % 2];
        const V* const src_im = buffers_im[i % 2];
        V* const dst_re = buffers_re[(i + 1) % 2];
        V* const dst_im = buffers_im[(i + 1) % 2];
        const Complex<W>* const tw = t.twiddles.data() + Shape::offset;
        const auto in = [src_re, src_im](std::size_t index) noexcept {
            return Complex<V>{src_re[index], src_im[index]};
        };
        const auto store = [dst_re, dst_im](std::size_t index, Complex<V> value) noexcept {
            dst_re[index] = value.re;
            dst_im[index] = value.im;
        };
        pass_to<V, static_cast<int>(Shape::r), false, true>(
            in, store, std::integral_constant<std::size_t, Shape::m>{},
            std::integral_constant<std::size_t, Shape::s>{}, tw, tw, tw);
        between(std::span<V, P>(dst_re, P), std::span<V, P>(dst_im, P));
    });
    if constexpr (kPasses % 2 != 0) {
        for (std::size_t k = 0; k < P; ++k) {
            re[k] = re2.values[k];
            im[k] = im2.values[k];
        }
    }
}

template <std::size_t P, typename V, typename W>
void stockham_forward(const StockhamTables<P, W>& t, std::span<V, P> re, std::span<V, P> im) {
    stockham_forward(t, re, im, [](std::span<V, P>, std::span<V, P>) noexcept {});
}

}  // namespace iclforge::dsp::fft
