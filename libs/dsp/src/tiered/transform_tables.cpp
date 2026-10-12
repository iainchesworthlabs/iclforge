#include "tiered/transform_tables.hpp"

#include <array>
#include <cstddef>
#include <type_traits>

#include "tiered/tables/transform_tables.hpp"

namespace iclforge::dsp::tiered {
namespace {

// What follows is used by the tiers that have fixed-point tables; a build whose scalar is double
// instantiates none of it, and the compilers that count an uninstantiated template as unused
// (-Wunused-template: Apple's and Homebrew's Clang, Emscripten's) stop on it under -Werror.

// {re, im} pairs narrowed as Fft and Imdct narrow theirs: each double cast to the scalar once.
template <typename R, std::size_t M>
[[maybe_unused]] constexpr std::array<Complex<R>, M / 2> complexes(
    const std::array<double, M>& values) {
    std::array<Complex<R>, M / 2> out{};
    for (std::size_t i = 0; i < M / 2; ++i) {
        out[i] = Complex<R>(static_cast<R>(values[2 * i]), static_cast<R>(values[2 * i + 1]));
    }
    return out;
}

// Imdct's post-twiddle at Fixed32: the pre-twiddle's doubles times 2^post_shift / N, with
// 2^(post_shift + 1) the least power of two not below N, then narrowed.
template <typename R, std::size_t N, std::size_t M>
[[maybe_unused]] constexpr std::array<Complex<R>, M / 2> post_twiddles(
    const std::array<double, M>& pre) {
    unsigned shift = 0;
    while ((std::size_t{1} << shift) < N) {
        ++shift;
    }
    const double factor =
        static_cast<double>(std::size_t{1} << (shift - 1)) / static_cast<double>(N);
    std::array<Complex<R>, M / 2> out{};
    for (std::size_t i = 0; i < M / 2; ++i) {
        out[i] = Complex<R>(R(pre[2 * i] * factor), R(pre[2 * i + 1] * factor));
    }
    return out;
}

template <typename R, std::size_t N>
[[maybe_unused]] constexpr std::array<R, N> reals(const std::array<double, N>& values) {
    std::array<R, N> out{};
    for (std::size_t i = 0; i < N; ++i) {
        out[i] = static_cast<R>(values[i]);
    }
    return out;
}

template <typename R, std::size_t N, std::size_t Roots, std::size_t Pre>
struct Built {
    static constexpr bool kFixed = !std::is_floating_point_v<R>;
    std::array<Complex<R>, Roots / 2> roots;
    std::array<Complex<R>, Pre / 2> pre;
    std::array<Complex<R>, kFixed ? Pre / 2 : 0> post;
    std::array<R, N> kbd;

    [[nodiscard]] constexpr TransformTable<R> view() const {
        return TransformTable<R>{roots, pre, post, kbd};
    }
};

template <typename R, std::size_t N, std::size_t Roots, std::size_t Pre>
[[maybe_unused]] constexpr Built<R, N, Roots, Pre> build(const std::array<double, Roots>& roots,
                                                         const std::array<double, Pre>& pre,
                                                         const std::array<double, N>& kbd) {
    Built<R, N, Roots, Pre> out{complexes<R>(roots), complexes<R>(pre), {}, reals<R>(kbd)};
    if constexpr (Built<R, N, Roots, Pre>::kFixed) {
        out.post = post_twiddles<R, N>(pre);
    }
    return out;
}

// Function-local constexpr, so a build whose scalar is double, which never calls this,
// neither evaluates nor links any of it. Constant-initialised: no guard.
template <typename R>
[[maybe_unused]] const TransformTable<R>* lookup(std::size_t length) noexcept {
    static constexpr auto k2048 = build<R>(iclforge::dsp::tiered::tables::kFftRoots2048,
                                           iclforge::dsp::tiered::tables::kPreTwiddle2048,
                                           iclforge::dsp::tiered::tables::kKbdLeft2048);
    static constexpr auto k1920 = build<R>(iclforge::dsp::tiered::tables::kFftRoots1920,
                                           iclforge::dsp::tiered::tables::kPreTwiddle1920,
                                           iclforge::dsp::tiered::tables::kKbdLeft1920);
    static constexpr auto k1536 = build<R>(iclforge::dsp::tiered::tables::kFftRoots1536,
                                           iclforge::dsp::tiered::tables::kPreTwiddle1536,
                                           iclforge::dsp::tiered::tables::kKbdLeft1536);
    static constexpr auto k1024 = build<R>(iclforge::dsp::tiered::tables::kFftRoots1024,
                                           iclforge::dsp::tiered::tables::kPreTwiddle1024,
                                           iclforge::dsp::tiered::tables::kKbdLeft1024);
    static constexpr auto k960 = build<R>(iclforge::dsp::tiered::tables::kFftRoots960,
                                          iclforge::dsp::tiered::tables::kPreTwiddle960,
                                          iclforge::dsp::tiered::tables::kKbdLeft960);
    static constexpr auto k768 = build<R>(iclforge::dsp::tiered::tables::kFftRoots768,
                                          iclforge::dsp::tiered::tables::kPreTwiddle768,
                                          iclforge::dsp::tiered::tables::kKbdLeft768);
    static constexpr auto k512 = build<R>(iclforge::dsp::tiered::tables::kFftRoots512,
                                          iclforge::dsp::tiered::tables::kPreTwiddle512,
                                          iclforge::dsp::tiered::tables::kKbdLeft512);
    static constexpr auto k480 = build<R>(iclforge::dsp::tiered::tables::kFftRoots480,
                                          iclforge::dsp::tiered::tables::kPreTwiddle480,
                                          iclforge::dsp::tiered::tables::kKbdLeft480);
    static constexpr auto k384 = build<R>(iclforge::dsp::tiered::tables::kFftRoots384,
                                          iclforge::dsp::tiered::tables::kPreTwiddle384,
                                          iclforge::dsp::tiered::tables::kKbdLeft384);
    static constexpr auto k256 = build<R>(iclforge::dsp::tiered::tables::kFftRoots256,
                                          iclforge::dsp::tiered::tables::kPreTwiddle256,
                                          iclforge::dsp::tiered::tables::kKbdLeft256);
    static constexpr auto k240 = build<R>(iclforge::dsp::tiered::tables::kFftRoots240,
                                          iclforge::dsp::tiered::tables::kPreTwiddle240,
                                          iclforge::dsp::tiered::tables::kKbdLeft240);
    static constexpr auto k192 = build<R>(iclforge::dsp::tiered::tables::kFftRoots192,
                                          iclforge::dsp::tiered::tables::kPreTwiddle192,
                                          iclforge::dsp::tiered::tables::kKbdLeft192);
    static constexpr auto k128 = build<R>(iclforge::dsp::tiered::tables::kFftRoots128,
                                          iclforge::dsp::tiered::tables::kPreTwiddle128,
                                          iclforge::dsp::tiered::tables::kKbdLeft128);
    static constexpr auto k120 = build<R>(iclforge::dsp::tiered::tables::kFftRoots120,
                                          iclforge::dsp::tiered::tables::kPreTwiddle120,
                                          iclforge::dsp::tiered::tables::kKbdLeft120);
    static constexpr auto k96 = build<R>(iclforge::dsp::tiered::tables::kFftRoots96,
                                         iclforge::dsp::tiered::tables::kPreTwiddle96,
                                         iclforge::dsp::tiered::tables::kKbdLeft96);
    static constexpr std::array<TransformTable<R>, 15> kTables{
        k2048.view(), k1920.view(), k1536.view(), k1024.view(), k960.view(),
        k768.view(),  k512.view(),  k480.view(),  k384.view(),  k256.view(),
        k240.view(),  k192.view(),  k128.view(),  k120.view(),  k96.view()};
    for (const TransformTable<R>& table : kTables) {
        if (table.kbd_left.size() == length) {
            return &table;
        }
    }
    return nullptr;
}

}  // namespace

const TransformTable<Real>* built_in_transform_table(std::size_t length) noexcept {
    return []<typename R>(std::size_t n) -> const TransformTable<R>* {
        if constexpr (std::is_same_v<R, double>) {
            (void)n;
            return nullptr;
        } else {
            return lookup<R>(n);
        }
    }.template operator()<Real>(length);
}

}  // namespace iclforge::dsp::tiered
