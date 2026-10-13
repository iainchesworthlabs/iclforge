// The fixed-point tier's inverse transform pair (libs/ac3/src/core/
// mdct_fixed.hpp) against the double one, on the inputs the header's own
// precondition admits: random dense blocks, sparse tonal ones, and the
// coherent worst case that takes the FFT to the top of the format.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numbers>
#include <random>
#include <span>
#include <utility>
#include <vector>

#include "iclforge/ac3/core/mdct.hpp"
#include "iclforge/base/arithmetic/fixed32.hpp"
#include "mdct_fixed.hpp"
#include "mdct_tables.hpp"

using iclforge::internal::Fixed32;

namespace {

constexpr double kUlp = 1.0 / 16777216.0;

struct Comparison {
    double snr_db = 0.0;
    double max_abs_ulp = 0.0;
    double peak = 0.0;
};

// The fixed inverse of a block against the double inverse of the SAME
// values (the block rounded to the format first, so the only difference is
// the arithmetic), as a signal-to-error ratio over the 512 outputs and the
// largest single-sample error in raw units.
Comparison compare(const std::array<double, 256>& wide_coeffs, bool short_block) {
    std::array<Fixed32, 256> fixed_coeffs{};
    std::array<double, 256> rounded{};
    for (std::size_t i = 0; i < 256; ++i) {
        fixed_coeffs[i] = Fixed32{wide_coeffs[i]};
        rounded[i] = static_cast<double>(fixed_coeffs[i]);
        REQUIRE(std::abs(fixed_coeffs[i].raw) < iclforge::ac3::internal::kFixedImdctInputLimit);
    }
    std::array<double, 512> wide_out{};
    std::array<Fixed32, 512> fixed_out{};
    if (short_block) {
        iclforge::ac3::imdct256_pair_windowed(rounded, wide_out, /*fast=*/true);
        iclforge::ac3::internal::imdct256_pair_windowed_fixed(fixed_coeffs, fixed_out);
    } else {
        iclforge::ac3::imdct512_windowed(rounded, wide_out, /*fast=*/true);
        iclforge::ac3::internal::imdct512_windowed_fixed(fixed_coeffs, fixed_out);
    }
    Comparison c;
    double signal = 0.0;
    double error = 0.0;
    for (std::size_t i = 0; i < 512; ++i) {
        const double d = static_cast<double>(fixed_out[i]) - wide_out[i];
        signal += wide_out[i] * wide_out[i];
        error += d * d;
        c.max_abs_ulp = std::max(c.max_abs_ulp, std::abs(d) / kUlp);
        c.peak = std::max(c.peak, std::abs(wide_out[i]));
    }
    c.snr_db = 10.0 * std::log10(signal / std::max(error, 1e-300));
    return c;
}

}  // namespace

TEST_CASE("the fixed inverse pair tracks the double one on dense random blocks", "[fixed32]") {
    std::mt19937 rng(0x1d3c);
    std::uniform_real_distribution<double> dist(-0.499, 0.499);
    for (const bool short_block : {false, true}) {
        double worst_snr = 1e9;
        double worst_ulp = 0.0;
        for (int trial = 0; trial < 40; ++trial) {
            std::array<double, 256> coeffs{};
            for (auto& v : coeffs) {
                v = dist(rng);
            }
            const auto c = compare(coeffs, short_block);
            worst_snr = std::min(worst_snr, c.snr_db);
            worst_ulp = std::max(worst_ulp, c.max_abs_ulp);
        }
        INFO((short_block ? "short" : "long") << " blocks: worst SNR " << worst_snr
             << " dB, worst sample error " << worst_ulp << " raw units");
        // A dense block's output is a few units in magnitude and its error a
        // few raw units: the header's estimate is 130 dB and the test asks
        // for less so a compiler's reordering of a sum cannot fail it.
        CHECK(worst_snr > 120.0);
        CHECK(worst_ulp < 64.0);
    }
}

TEST_CASE("the fixed inverse pair tracks the double one on sparse tonal blocks", "[fixed32]") {
    std::mt19937 rng(0x70a1);
    std::uniform_int_distribution<int> bin(0, 255);
    std::uniform_real_distribution<double> amp(-0.499, 0.499);
    for (const bool short_block : {false, true}) {
        double worst_snr = 1e9;
        for (int trial = 0; trial < 40; ++trial) {
            std::array<double, 256> coeffs{};
            for (int k = 0; k < 3; ++k) {
                coeffs[static_cast<std::size_t>(bin(rng))] = amp(rng);
            }
            const auto c = compare(coeffs, short_block);
            worst_snr = std::min(worst_snr, c.snr_db);
        }
        INFO((short_block ? "short" : "long") << " blocks: worst SNR " << worst_snr << " dB");
        // A sparse block's output is smaller, so the same raw-unit floor is
        // a larger fraction of it - the tier's precision is relative to the
        // block's peak coefficient, and a three-tone block's output peaks
        // well below the dense one's.
        CHECK(worst_snr > 110.0);
    }
}

TEST_CASE("the fixed inverse pair does not wrap on the coherent worst case", "[fixed32]") {
    // Every coefficient at the precondition's edge with one sign: the input
    // the header's seven-bit growth bound is stated for. The double output
    // reaches tens, the format holds 128, and the fixed output must be the
    // same values to within the arithmetic's own rounding rather than a
    // wrapped or saturated one.
    for (const bool short_block : {false, true}) {
        std::array<double, 256> coeffs{};
        coeffs.fill(0.499);
        const auto c = compare(coeffs, short_block);
        INFO((short_block ? "short" : "long") << " blocks: peak " << c.peak << ", SNR "
             << c.snr_db << " dB, worst sample error " << c.max_abs_ulp << " raw units");
        CHECK(c.peak < 128.0);
        CHECK(c.snr_db > 120.0);
        CHECK(c.max_abs_ulp < 256.0);
        // And the alternating-sign form, which drives the other half of the
        // spectrum.
        for (std::size_t i = 1; i < 256; i += 2) {
            coeffs[i] = -coeffs[i];
        }
        const auto d = compare(coeffs, short_block);
        CHECK(d.peak < 128.0);
        CHECK(d.snr_db > 120.0);
    }
}

TEST_CASE("the pair's product is Fixed32's wherever Fixed32's does not saturate", "[fixed32]") {
    // ImdctValue drops the saturate the pair's growth bound makes unreachable
    // (mdct_fixed.hpp); everywhere Fixed32's product fits, including the
    // rounding edges and a factor of exactly one or minus one, it has to be
    // the same raw value.
    using iclforge::ac3::internal::ImdctValue;
    std::mt19937_64 rng(0x7e11);
    int checked = 0;
    int mismatches = 0;
    const auto check = [&](std::int32_t a, std::int32_t w) {
        const std::int64_t rounded = ((static_cast<std::int64_t>(a) * w) + (1 << 23)) >> 24;
        if (rounded > std::numeric_limits<std::int32_t>::max() ||
            rounded < std::numeric_limits<std::int32_t>::min()) {
            return;
        }
        ++checked;
        const auto want = (Fixed32::from_raw(a) * Fixed32::from_raw(w)).raw;
        mismatches += (ImdctValue{a} * Fixed32::from_raw(w)).raw != want ? 1 : 0;
    };
    for (const std::int32_t a : {0, 1, -1, 127, -128, Fixed32::kOne / 2, -(Fixed32::kOne / 2),
                                 std::numeric_limits<std::int32_t>::max(),
                                 std::numeric_limits<std::int32_t>::min()}) {
        for (const std::int32_t w : {0, 1, -1, Fixed32::kOne, -Fixed32::kOne, Fixed32::kOne - 1,
                                     Fixed32::kOne / 2, -(Fixed32::kOne / 2)}) {
            check(a, w);
        }
    }
    for (int i = 0; i < 200000; ++i) {
        const auto a = static_cast<std::int32_t>(rng());
        check(a, static_cast<std::int32_t>(rng() % (2U * Fixed32::kOne + 1U)) - Fixed32::kOne);
        check(a, static_cast<std::int32_t>(rng() % 256) - 128);
    }
    INFO(checked << " products");
    CHECK(checked > 400000);
    CHECK(mismatches == 0);
}

TEST_CASE("the fixed inverse is linear in the block exponent", "[fixed32]") {
    // The decoders store a quiet block scaled up by its exponent and scale
    // the transform's output back down; that is only sound if the transform
    // of the scaled block is the scaled transform. Check the fixed inverse
    // of a block against the fixed inverse of the same block one bit down,
    // doubled: equal to within the two roundings.
    std::mt19937 rng(0x5ca1);
    std::uniform_real_distribution<double> dist(-0.499, 0.499);
    std::array<Fixed32, 256> full{};
    std::array<Fixed32, 256> half{};
    for (std::size_t i = 0; i < 256; ++i) {
        full[i] = Fixed32{dist(rng)};
        half[i] = full[i].scaled_by_pow2(-1);
    }
    std::array<Fixed32, 512> out_full{};
    std::array<Fixed32, 512> out_half{};
    iclforge::ac3::internal::imdct512_windowed_fixed(full, out_full);
    iclforge::ac3::internal::imdct512_windowed_fixed(half, out_half);
    double worst = 0.0;
    double signal = 0.0;
    double error = 0.0;
    for (std::size_t i = 0; i < 512; ++i) {
        const double doubled = static_cast<double>(out_half[i].scaled_by_pow2(1));
        const double d = doubled - static_cast<double>(out_full[i]);
        worst = std::max(worst, std::abs(d) / kUlp);
        signal += static_cast<double>(out_full[i]) * static_cast<double>(out_full[i]);
        error += d * d;
    }
    const double snr_db = 10.0 * std::log10(signal / std::max(error, 1e-300));
    INFO("worst difference " << worst << " raw units, " << snr_db << " dB");
    // Both transforms round, and the half block's roundings come back
    // doubled: a few tens of raw units on outputs of a few units, and no
    // saturation anywhere in either.
    CHECK(worst < 128.0);
    CHECK(snr_db > 115.0);
}

namespace {

// The tables as the C library makes them: what FixedImdctTables was before it was a constant,
// an expression for expression copy of its old constructor and of StockhamTables'.
template <std::size_t P>
void library_plan(std::array<iclforge::dsp::tiered::fft_kernels::Stage, 8>& stages,
                  std::vector<std::pair<std::int32_t, std::int32_t>>& factors) {
    std::size_t n = P;
    std::size_t stride = 1;
    std::size_t count = 0;
    std::size_t pass = 0;
    factors.clear();
    while (n > 1) {
        const std::size_t r = n % 4 == 0 ? 4 : 2;
        const std::size_t m = n / r;
        stages[pass++] = {static_cast<int>(r), n, stride, count};
        for (std::size_t p = 0; p < m; ++p) {
            for (std::size_t k = 0; k < r; ++k) {
                const double angle = -2.0 * std::numbers::pi * static_cast<double>((p * k) % n) /
                                     static_cast<double>(n);
                factors.emplace_back(Fixed32{std::cos(angle)}.raw, Fixed32{std::sin(angle)}.raw);
            }
        }
        count += m * r;
        n = m;
        stride *= r;
    }
}

template <std::size_t P, typename Plan>
void require_plan_is_the_library_one(const Plan& plan) {
    std::array<iclforge::dsp::tiered::fft_kernels::Stage, 8> stages{};
    std::vector<std::pair<std::int32_t, std::int32_t>> factors;
    library_plan<P>(stages, factors);
    REQUIRE(factors.size() == plan.twiddles.size());
    for (std::size_t i = 0; i < plan.stages.size(); ++i) {
        CHECK(plan.stages[i].radix == stages[i].radix);
        CHECK(plan.stages[i].n == stages[i].n);
        CHECK(plan.stages[i].stride == stages[i].stride);
        CHECK(plan.stages[i].twiddle == stages[i].twiddle);
    }
    std::size_t differing = 0;
    for (std::size_t i = 0; i < factors.size(); ++i) {
        differing += plan.twiddles[i].re.raw != factors[i].first ? 1U : 0U;
        differing += plan.twiddles[i].im.raw != factors[i].second ? 1U : 0U;
    }
    CHECK(differing == 0);
}

}  // namespace

TEST_CASE("the inverse pair's tables are the C library's, rounded to the format", "[fixed32]") {
    const auto& t = iclforge::ac3::internal::fixed_imdct_tables();
    constexpr double kPi = std::numbers::pi;
    constexpr std::size_t kN = iclforge::ac3::internal::FixedImdctTables::kN;
    std::size_t differing = 0;
    for (std::size_t k = 0; k < kN / 4; ++k) {
        const double angle =
            2.0 * kPi * (8.0 * static_cast<double>(k) + 1.0) / (8.0 * static_cast<double>(kN));
        differing += t.cos1[k].raw != Fixed32{-std::cos(angle)}.raw ? 1U : 0U;
        differing += t.sin1[k].raw != Fixed32{-std::sin(angle)}.raw ? 1U : 0U;
    }
    for (std::size_t k = 0; k < kN / 8; ++k) {
        const double angle =
            2.0 * kPi * (8.0 * static_cast<double>(k) + 1.0) / (4.0 * static_cast<double>(kN));
        differing += t.cos2[k].raw != Fixed32{-std::cos(angle)}.raw ? 1U : 0U;
        differing += t.sin2[k].raw != Fixed32{-std::sin(angle)}.raw ? 1U : 0U;
    }
    for (std::size_t i = 0; i < kN; ++i) {
        differing +=
            t.window[i].raw != Fixed32{iclforge::ac3::kAnalysisWindow[i]}.raw ? 1U : 0U;
    }
    CHECK(differing == 0);
    require_plan_is_the_library_one<kN / 4>(t.fft128);
    require_plan_is_the_library_one<kN / 8>(t.fft64);
}

namespace {

template <typename T, std::size_t N>
bool same_bits(const std::array<T, N>& a, const std::array<T, N>& b) {
    return std::memcmp(a.data(), b.data(), sizeof(T) * N) == 0;
}

template <std::size_t P, typename Plan, typename Reference>
void require_same_plan(const Plan& constant, const Reference& runtime) {
    REQUIRE(constant.twiddles.size() == runtime.twiddles.size());
    std::size_t differing = 0;
    for (std::size_t i = 0; i < constant.twiddles.size(); ++i) {
        differing += std::memcmp(&constant.twiddles[i], &runtime.twiddles[i],
                                 sizeof(constant.twiddles[i])) != 0
                         ? 1U
                         : 0U;
    }
    CHECK(differing == 0);
    for (std::size_t i = 0; i < constant.stages.size(); ++i) {
        CHECK(constant.stages[i].radix == runtime.stages[i].radix);
        CHECK(constant.stages[i].n == runtime.stages[i].n);
        CHECK(constant.stages[i].stride == runtime.stages[i].stride);
        CHECK(constant.stages[i].twiddle == runtime.stages[i].twiddle);
    }
}

}  // namespace

TEST_CASE("the float transform tables that are constants are the C library's, narrowed",
          "[fixed32]") {
    // JOC's float transforms in a build whose decoder is not float read constants (mdct_tables.hpp);
    // every bit of them is what the run-time form makes with the C library's cosine and sine.
    namespace internal = iclforge::ac3::internal;
    {
        const internal::Twiddles<float> runtime;
        const auto& constant = internal::kTwiddlesConstant<float>;
        CHECK(same_bits(constant.cos1, runtime.cos1));
        CHECK(same_bits(constant.sin1, runtime.sin1));
    }
    {
        const internal::Twiddles2<float> runtime;
        const auto& constant = internal::kTwiddles2Constant<float>;
        CHECK(same_bits(constant.cos2, runtime.cos2));
        CHECK(same_bits(constant.sin2, runtime.sin2));
    }
    {
        const internal::FastMdctTables<512, float> runtime;
        const auto& constant = internal::kFastMdctTablesConstant<512, float>;
        CHECK(same_bits(constant.pre_re, runtime.pre_re));
        CHECK(same_bits(constant.pre_im, runtime.pre_im));
        CHECK(same_bits(constant.post_re, runtime.post_re));
        CHECK(same_bits(constant.post_im, runtime.post_im));
        require_same_plan<128>(constant.fft, runtime.fft);
    }
    {
        const internal::FastMdctTables<256, float> runtime;
        const auto& constant = internal::kFastMdctTablesConstant<256, float>;
        CHECK(same_bits(constant.pre_re, runtime.pre_re));
        CHECK(same_bits(constant.pre_im, runtime.pre_im));
        CHECK(same_bits(constant.post_re, runtime.post_re));
        CHECK(same_bits(constant.post_im, runtime.post_im));
        require_same_plan<64>(constant.fft, runtime.fft);
    }
}
