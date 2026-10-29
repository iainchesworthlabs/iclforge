#include "encoder/coder.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <vector>

#include "core/huffman.hpp"
#include "core/reconstruct.hpp"

namespace iclforge::mlp::detail {

namespace {

constexpr int kMaxRawLsbs = 24;      // huff_lsbs, 8.3
constexpr int kMaxHuffmanLsbs = 20;  // 11.4: at most 29 bits a sample, 9 of them the codeword
constexpr int kMinOffset = -16384;
constexpr int kMaxOffset = 16383;

// floor(a / 2^n) for any sign.
constexpr std::int64_t floor_shift(std::int64_t a, int n) noexcept {
    return a >> n;
}

}  // namespace

std::int64_t coding_cost(std::span<const std::int32_t> residuals, const CodingParams& p) noexcept {
    const int n = p.lsbs;
    std::int64_t bits = 0;
    if (p.huff_type == 0) {
        const std::int64_t half = n > 0 ? std::int64_t{1} << (n - 1) : 0;
        const std::int64_t span = std::int64_t{1} << n;
        for (const std::int32_t r : residuals) {
            const std::int64_t raw = std::int64_t{r} - p.offset + half;
            if (n == 0 ? raw != 0 : (raw < 0 || raw >= span)) {
                return kInfiniteCost;
            }
        }
        return static_cast<std::int64_t>(residuals.size()) * n;
    }
    const auto book = static_cast<std::size_t>(p.huff_type - 1);
    const std::int64_t bias = huffman_bias(p.huff_type, n);
    for (const std::int32_t r : residuals) {
        const std::int64_t msb = floor_shift(std::int64_t{r} - p.offset + bias, n);
        if (msb < kMinSymbol[book] || msb > kMaxSymbol[book]) {
            return kInfiniteCost;
        }
        bits += codeword(p.huff_type - 1, static_cast<int>(msb)).length + n;
    }
    return bits;
}

int header_bits(const CodingParams& next, const CodingParams& current,
                bool filter_changed) noexcept {
    const bool offset_changed = next.offset != current.offset;
    const bool params_changed =
        next.huff_type != current.huff_type || next.lsbs != current.lsbs || offset_changed;
    int bits = 0;
    if (params_changed || filter_changed) {
        bits += 2 + 5;  // huff_type, huff_lsbs
        bits += 1;      // new_huff_offset
        if (offset_changed) {
            bits += 15;
        }
    }
    return bits;
}

CodingChoice choose_coding(std::span<const std::int32_t> residuals, const CodingParams& current,
                           int max_lsbs) {
    const int max_raw = std::min(kMaxRawLsbs, max_lsbs);
    const int max_huffman = std::min(kMaxHuffmanLsbs, max_lsbs);
    CodingChoice best;
    if (residuals.empty()) {
        best.params = current;
        best.bits = 0;
        return best;
    }
    std::int64_t lo = residuals[0];
    std::int64_t hi = residuals[0];
    double sum = 0.0;
    for (const std::int32_t r : residuals) {
        lo = std::min<std::int64_t>(lo, r);
        hi = std::max<std::int64_t>(hi, r);
        sum += r;
    }
    const double mean = sum / static_cast<double>(residuals.size());
    double spread = 0.0;
    for (const std::int32_t r : residuals) {
        spread += std::abs(static_cast<double>(r) - mean);
    }
    spread /= static_cast<double>(residuals.size());

    auto consider = [&](const CodingParams& p) {
        if (p.offset < kMinOffset || p.offset > kMaxOffset) {
            return;
        }
        const std::int64_t bits = coding_cost(residuals, p);
        if (bits >= kInfiniteCost) {
            return;
        }
        const std::int64_t total = bits + header_bits(p, current, false);
        if (total < best.bits) {
            best.bits = total;
            best.params = p;
        }
    };

    // The decoder's current parameters cost no header bits: always a candidate.
    consider(current);

    // huff_type 0: the narrowest raw width that holds the range, its offset centring it.
    {
        const std::uint64_t range = static_cast<std::uint64_t>(hi - lo);
        int n = range == 0 ? 0 : std::bit_width(range);
        for (; n <= max_raw; ++n) {
            CodingParams p{0, n, 0};
            const std::int64_t centre = n == 0 ? lo : lo + (std::int64_t{1} << (n - 1));
            p.offset = static_cast<int>(std::clamp<std::int64_t>(centre, kMinOffset, kMaxOffset));
            const std::int64_t before = best.bits;
            consider(p);
            if (best.bits < before || coding_cost(residuals, p) < kInfiniteCost) {
                break;
            }
        }
    }

    // The codebooks: widths around the spread, offsets centring the MSBs.
    const int estimate = spread < 1.0 ? 0 : static_cast<int>(std::floor(std::log2(spread)));
    for (int type = 1; type <= 3; ++type) {
        const auto book = static_cast<std::size_t>(type - 1);
        for (int n = std::max(0, estimate - 2); n <= std::min(max_huffman, estimate + 3); ++n) {
            const std::int64_t bias = huffman_bias(type, n);
            const std::int64_t step = std::int64_t{1} << n;
            // Offsets that keep every MSB inside the codebook.
            const std::int64_t o_min = hi + bias - (kMaxSymbol[book] + 1) * step + 1;
            const std::int64_t o_max = lo + bias - kMinSymbol[book] * step;
            if (o_min > o_max) {
                continue;
            }
            std::array<std::int64_t, 5> candidates{
                static_cast<std::int64_t>(std::llround(mean)),
                static_cast<std::int64_t>(std::llround(mean)) - step / 2,
                static_cast<std::int64_t>(std::llround(mean)) + step / 2,
                o_min,
                o_max,
            };
            for (const std::int64_t o : candidates) {
                CodingParams p{type, n, static_cast<int>(std::clamp(o, o_min, o_max))};
                consider(p);
            }
            if (current.offset >= o_min && current.offset <= o_max) {
                consider(CodingParams{type, n, current.offset});
            }
        }
    }

    // The widest raw coding is always possible for a 24-bit residual: the fallback.
    if (best.bits >= kInfiniteCost) {
        for (int n = 0; n <= max_raw && best.bits >= kInfiniteCost; ++n) {
            for (const std::int64_t centre : {std::int64_t{0}, (lo + hi) / 2}) {
                consider(CodingParams{
                    0, n,
                    static_cast<int>(std::clamp<std::int64_t>(centre, kMinOffset, kMaxOffset))});
            }
        }
    }
    return best;
}

void write_sample(BitWriter& out, std::int32_t residual, const CodingParams& p) {
    const int n = p.lsbs;
    if (p.huff_type == 0) {
        if (n > 0) {
            const std::int64_t raw =
                std::int64_t{residual} - p.offset + (std::int64_t{1} << (n - 1));
            out.put(static_cast<std::uint64_t>(raw), n);
        }
        return;
    }
    const std::int64_t u = std::int64_t{residual} - p.offset + huffman_bias(p.huff_type, n);
    const std::int64_t msb = u >> n;
    const Codeword w = codeword(p.huff_type - 1, static_cast<int>(msb));
    out.put(w.code, w.length);
    if (n > 0) {
        out.put(static_cast<std::uint64_t>(u - (msb << n)), n);
    }
}

}  // namespace iclforge::mlp::detail
