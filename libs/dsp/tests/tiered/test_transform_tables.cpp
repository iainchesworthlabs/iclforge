// The inverse transform's built-in tables (libs/dsp/src/tiered/transform_tables.hpp,
// planning/ac4.md, the decoder's memory): at the float and fixed-point tiers a transform reads its
// roots, twiddles and window from flash for every length Table 186 lists, and every value
// there must be the bits the decoder computes when it builds its own, on the compiler and C
// library at hand, or the decoder's output would move. At double nothing is built in.

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

#include "iclforge/dsp/tiered/real.hpp"
#include "tiered/fft.hpp"
#include "tiered/mdct.hpp"
#include "tiered/synthesis.hpp"
#include "tiered/transform_tables.hpp"

namespace dsp = iclforge::dsp::tiered;
using iclforge::dsp::tiered::Real;

namespace {

// A value's bits, so that -0.0 and +0.0 differ, as they can in what follows them.
template <typename R>
std::uint64_t bits(R value) {
    if constexpr (std::is_same_v<R, float>) {
        return std::bit_cast<std::uint32_t>(value);
    } else if constexpr (std::is_same_v<R, double>) {
        return std::bit_cast<std::uint64_t>(value);
    } else {
        return static_cast<std::uint32_t>(value.raw);
    }
}

// The index of the first value that differs, or the size when none does.
template <typename R>
std::size_t first_difference(std::span<const dsp::Complex<R>> built_in,
                             const std::vector<dsp::Complex<R>>& computed) {
    if (built_in.size() != computed.size()) {
        return 0;
    }
    for (std::size_t i = 0; i < computed.size(); ++i) {
        if (bits(built_in[i].re) != bits(computed[i].re) ||
            bits(built_in[i].im) != bits(computed[i].im)) {
            return i;
        }
    }
    return computed.size();
}

template <typename R>
std::size_t first_difference(std::span<const R> built_in, const std::vector<R>& computed) {
    if (built_in.size() != computed.size()) {
        return 0;
    }
    for (std::size_t i = 0; i < computed.size(); ++i) {
        if (bits(built_in[i]) != bits(computed[i])) {
            return i;
        }
    }
    return computed.size();
}

// Table 186 at 44.1 and 48 kHz: each of the decoder's frame lengths (2048, 1920, 1536, 1024, 960,
// 768, 512 and 384, and those over the high frame rate mode's 2 and 4) halved while the table has
// the length.
constexpr std::size_t kLengths[] = {2048, 1920, 1536, 1024, 960, 768, 512, 480,
                                    384,  256,  240,  192,  128, 120, 96};

}  // namespace

TEST_CASE("The built-in transform tables are the bits the decoder computes",
          "[ac4][core][dsp][transform_tables]") {
    for (const std::size_t n : kLengths) {
        CAPTURE(n);
        const dsp::TransformTable<Real>* const table = dsp::transform_table<Real>(n);
        if constexpr (std::is_same_v<Real, double>) {
            CHECK(table == nullptr);
            continue;
        }
        REQUIRE(table != nullptr);

        const auto roots = dsp::Fft<Real>::computed_roots(n / 2);
        CHECK(first_difference<Real>(table->fft_roots, roots) == roots.size());

        const auto pre = dsp::Imdct<Real>::computed_pre_twiddles(n);
        CHECK(first_difference<Real>(table->pre_twiddle, pre) == pre.size());

        const auto post = dsp::Imdct<Real>::computed_post_twiddles(n);
        CHECK(first_difference<Real>(table->post_twiddle, post) == post.size());
        CHECK(post.empty() == std::is_floating_point_v<Real>);

        const auto kbd = dsp::TransformSet<Real>::computed_kbd_left(static_cast<int>(n), 1);
        CHECK(first_difference<Real>(table->kbd_left, kbd) == kbd.size());
    }
}

TEST_CASE("A length with no built-in table has none", "[ac4][core][dsp][transform_tables]") {
    // Table 186 does not list these, so a transform of them has no window to take either.
    for (const std::size_t n : {std::size_t{4096}, std::size_t{1000}, std::size_t{64},
                                std::size_t{60}, std::size_t{48}, std::size_t{0}}) {
        CAPTURE(n);
        CHECK(dsp::transform_table<Real>(n) == nullptr);
    }
    CHECK(dsp::transform_table<double>(2048) == nullptr);
}
