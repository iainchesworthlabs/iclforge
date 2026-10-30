#include "coded_bits_avx2.hpp"

#include <immintrin.h>

#include <array>
#include <cstddef>

// See coded_bits_avx2.hpp. Eight residuals a step in 32-bit lanes; the tail of fewer than eight is
// the scalar body's own arithmetic in 64 bits.

namespace iclforge::truehd::internal::avx2 {

namespace {

constexpr std::int32_t kDomain = 1 << 30;

[[nodiscard]] bool any(__m256i mask) { return _mm256_movemask_epi8(mask) != 0; }

[[nodiscard]] std::int64_t horizontal_sum(__m256i v) {
    alignas(32) std::array<std::int32_t, 8> lanes{};
    _mm256_store_si256(reinterpret_cast<__m256i*>(lanes.data()), v);
    std::int64_t total = 0;
    for (const std::int32_t lane : lanes) {
        total += lane;
    }
    return total;
}

// True if a residual of the vector is outside +-2^30.
[[nodiscard]] __m256i outside_domain(__m256i x) {
    return _mm256_or_si256(_mm256_cmpgt_epi32(x, _mm256_set1_epi32(kDomain)),
                           _mm256_cmpgt_epi32(_mm256_set1_epi32(-kDomain), x));
}

}  // namespace

int huffman_bits(std::span<const std::int32_t> residuals, std::int32_t centre, int lb,
                 int min_symbol, int max_symbol, const std::uint8_t* lengths, int max_sample_bits,
                 int infeasible) {
    const int symbols = max_symbol - min_symbol + 1;
    alignas(32) std::array<std::int32_t, 32> table{};
    for (int i = 0; i < symbols; ++i) {
        table[static_cast<std::size_t>(i)] = lengths[i];
    }
    const __m256i v_centre = _mm256_set1_epi32(centre);
    const __m128i v_shift = _mm_cvtsi32_si128(lb);
    const __m256i v_min = _mm256_set1_epi32(min_symbol);
    const __m256i v_max = _mm256_set1_epi32(max_symbol);
    const __m256i v_last = _mm256_set1_epi32(symbols - 1);
    const __m256i v_zero = _mm256_setzero_si256();
    const __m256i v_lb = _mm256_set1_epi32(lb);
    const __m256i v_length_limit = _mm256_set1_epi32(max_sample_bits - lb);

    __m256i outside = v_zero;
    __m256i bad = v_zero;
    __m256i total = v_zero;
    const std::size_t n = residuals.size();
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        const __m256i x = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(residuals.data() + i));
        outside = _mm256_or_si256(outside, outside_domain(x));
        const __m256i symbol = _mm256_sra_epi32(_mm256_add_epi32(x, v_centre), v_shift);
        bad = _mm256_or_si256(bad, _mm256_or_si256(_mm256_cmpgt_epi32(v_min, symbol),
                                                   _mm256_cmpgt_epi32(symbol, v_max)));
        // A lane out of range reads a clamped slot, so the gather stays inside the table; the lane
        // has marked the block infeasible and its length is not used.
        const __m256i slot =
            _mm256_min_epi32(_mm256_max_epi32(_mm256_sub_epi32(symbol, v_min), v_zero), v_last);
        const __m256i length = _mm256_i32gather_epi32(table.data(), slot, 4);
        bad = _mm256_or_si256(bad, _mm256_cmpgt_epi32(length, v_length_limit));
        total = _mm256_add_epi32(total, _mm256_add_epi32(length, v_lb));
    }
    if (any(outside)) {
        return kUnhandled;
    }
    std::int64_t bits = horizontal_sum(total);
    bool infeasible_tail = false;
    for (; i < n; ++i) {
        const std::int64_t symbol = (std::int64_t{residuals[i]} + centre) >> lb;
        if (symbol < min_symbol || symbol > max_symbol) {
            infeasible_tail = true;
            break;
        }
        const int length = lengths[symbol - min_symbol];
        if (length + lb > max_sample_bits) {
            infeasible_tail = true;
            break;
        }
        bits += length + lb;
    }
    return (any(bad) || infeasible_tail) ? infeasible : static_cast<int>(bits);
}

int raw_bits(std::span<const std::int32_t> residuals, std::int32_t centre, int lb,
             int infeasible) {
    const __m256i v_centre = _mm256_set1_epi32(centre);
    const __m256i v_zero = _mm256_setzero_si256();
    const __m256i v_top = _mm256_set1_epi32((std::int32_t{1} << lb) - 1);
    __m256i outside = v_zero;
    __m256i bad = v_zero;
    const std::size_t n = residuals.size();
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        const __m256i x = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(residuals.data() + i));
        outside = _mm256_or_si256(outside, outside_domain(x));
        const __m256i t = _mm256_add_epi32(x, v_centre);
        bad = _mm256_or_si256(bad, _mm256_or_si256(_mm256_cmpgt_epi32(v_zero, t),
                                                   _mm256_cmpgt_epi32(t, v_top)));
    }
    if (any(outside)) {
        return kUnhandled;
    }
    bool infeasible_tail = false;
    const std::int64_t span = std::int64_t{1} << lb;
    for (; i < n; ++i) {
        const std::int64_t t = std::int64_t{residuals[i]} + centre;
        if (t < 0 || t >= span) {
            infeasible_tail = true;
            break;
        }
    }
    return (any(bad) || infeasible_tail)
               ? infeasible
               : static_cast<int>(static_cast<std::int64_t>(n) * lb);
}

}  // namespace iclforge::truehd::internal::avx2
