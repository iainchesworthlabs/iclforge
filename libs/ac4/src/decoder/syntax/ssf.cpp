#include "decoder/syntax/ssf.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "core/tables/ssf_tables.hpp"

namespace iclforge::ac4::detail {
namespace {

namespace tab = tables;

// --- Pseudocode 40's fixed point operators -------------------------------------------------
//
// The text's SSF_I32_* macros work in a 32-bit register; here the values are held in 64 bits and
// every result is checked against the 32-bit range, which a stream within the tool's design never
// leaves. A result outside it marks the stream invalid (the macros would wrap).

[[nodiscard]] constexpr std::int64_t shr(std::int64_t a, int b) noexcept {
    return a >= 0 ? (a >> b) : -((-a) >> b);  // SSF_I32_SHIFT_RIGHT: sign preserving
}

[[nodiscard]] constexpr std::int64_t shl(std::int64_t a, int b) noexcept {
    return a >= 0 ? (a << b) : -((-a) << b);  // SSF_I32_SHIFT_LEFT
}

struct Fx {
    bool overflow = false;
    [[nodiscard]] std::int32_t n(std::int64_t value) noexcept {
        if (value < std::numeric_limits<std::int32_t>::min() ||
            value > std::numeric_limits<std::int32_t>::max()) {
            overflow = true;
            return 0;
        }
        return static_cast<std::int32_t>(value);
    }
};

// --- Pseudocodes 29 and 30: dB and linear ---------------------------------------------------

// Pseudocode 29. Input Qx.10, result Qx.10.
[[nodiscard]] bool map_db_to_lin(Fx& fx, std::int32_t input, std::int32_t& result) noexcept {
    const std::int32_t q4 = fx.n(shr(input, 6));  // Qx.4
    const std::int32_t index = fx.n(shr(q4, 6));
    if (index < 0) {
        return false;
    }
    if (index < 10) {
        const auto i = static_cast<std::size_t>(index);
        std::int32_t res = fx.n(std::int64_t{tab::kSsfSlopesDbToLin[i]} * q4);
        res = fx.n(shr(res, 4));
        res = fx.n(std::int64_t{res} + tab::kSsfOffsetsDbToLin[i]);
        result = fx.n(shl(res, 6));
    } else {
        result = fx.n(shl(100, 10));  // index out of range
    }
    return !fx.overflow;
}

// Pseudocode 30. Input Qx.10, result Q7.10.
[[nodiscard]] bool map_lin_to_db(Fx& fx, std::int32_t input, std::int32_t& result) noexcept {
    const std::int32_t in8 = fx.n(shr(input, 2));  // Qx.8
    const std::int32_t quant = fx.n(shr(in8, 1));
    const std::int32_t index = fx.n(shr(quant, 8));
    if (index < 0) {
        return false;
    }
    const std::int32_t integer = fx.n(shl(index, 8 + 1));
    const std::int32_t fract = fx.n(std::int64_t{in8} - integer);
    if (index < 50) {
        const auto i = static_cast<std::size_t>(index);
        const std::int32_t doubled = fx.n(shl(index, 1));
        const std::int32_t first = fx.n(std::int64_t{tab::kSsfSlopesLinToDb[i]} * doubled);
        std::int32_t second = fx.n(std::int64_t{tab::kSsfSlopesLinToDb[i]} * fract);
        second = fx.n(shr(second, 8));
        std::int32_t res = fx.n(std::int64_t{first} + second);
        res = fx.n(std::int64_t{res} + tab::kSsfOffsetsLinToDb[i]);
        result = fx.n(shl(res, 2));
    } else {
        result = fx.n(shl(40, 10));
    }
    return !fx.overflow;
}

// --- Pseudocode 28: HeuristicScaling() ------------------------------------------------------

// `env_in` is 3 * env_alloc in 1 dB steps (Qx.0), `i_rfu` f_rfu in Q.10, `widths` the bands'
// widths, `num_bins` the coded lines; the result is the weights in dB, Q.10, per band.
//
// Two readings (src/ac4/ERRATA.md): `band` is set to 0 before the reverse water-filling, which
// the text leaves at num_bands from the loop before it, and an input of Qx.10 is formed from
// f_rfu by rounding, in the caller.
[[nodiscard]] bool heuristic_scaling(Fx& fx, std::int32_t i_rfu,
                                     std::span<const std::int32_t> env_in,
                                     std::span<const std::uint8_t> widths, int num_bins,
                                     std::span<std::int32_t> weights_db) {
    const auto num_bands = env_in.size();
    constexpr std::int32_t kDynThreshold = 40 << 10;  // SSF_I32_SHIFT_LEFT(40, 10)
    constexpr std::int32_t kMaxWdB = 15 << 10;
    constexpr std::int32_t kInvThree = 341;
    const std::int32_t max_env = *std::ranges::max_element(env_in);
    const std::int32_t min_env = *std::ranges::min_element(env_in);
    const std::int32_t dyn_unscaled = fx.n(std::int64_t{max_env} - min_env);
    const std::int32_t dyn = fx.n(shl(dyn_unscaled, 10));
    std::array<std::int32_t, kSsfMaxBands> env_local{};
    if (dyn > kDynThreshold) {
        const std::int32_t cmp_fact = kDynThreshold / dyn_unscaled;  // Q.10
        for (std::size_t b = 0; b < num_bands; ++b) {
            const std::int32_t shifted = fx.n(std::int64_t{env_in[b]} - min_env);
            env_local[b] = fx.n(std::int64_t{shifted} * cmp_fact);
        }
    } else {
        for (std::size_t b = 0; b < num_bands; ++b) {
            const std::int32_t shifted = fx.n(std::int64_t{env_in[b]} - min_env);
            env_local[b] = fx.n(shl(shifted, 10));  // Q6.10
        }
    }
    // Sort in descending order; the order among equal entries does not matter below, which
    // groups them.
    std::array<std::size_t, kSsfMaxBands> indices{};
    for (std::size_t b = 0; b < num_bands; ++b) {
        indices[b] = b;
    }
    std::stable_sort(indices.begin(), indices.begin() + static_cast<std::ptrdiff_t>(num_bands),
                     [&](std::size_t a, std::size_t b) { return env_local[a] > env_local[b]; });
    std::array<std::int32_t, kSsfMaxBands> weights_lin{};
    std::int32_t mtr = 0;
    for (std::size_t b = 0; b < num_bands; ++b) {
        if (!map_db_to_lin(fx, env_local[indices[b]], weights_lin[b])) {
            return false;
        }
        const std::int32_t term = fx.n(std::int64_t{weights_lin[b]} * widths[indices[b]]);
        mtr = fx.n(std::int64_t{mtr} + term);
    }
    mtr = fx.n(shr(mtr, 10));
    mtr = fx.n(std::int64_t{mtr} * i_rfu);
    mtr = fx.n(shr(mtr, 7));
    mtr = fx.n(std::int64_t{mtr} * i_rfu);
    mtr = fx.n(shr(mtr, 3));
    // Reverse water-filling.
    std::size_t band = 0;
    std::int32_t mnt = 0;
    std::int32_t bsum = 0;
    const std::size_t last = num_bands - 1;
    while (mnt < mtr && band < last) {
        const std::int32_t cur_level = weights_lin[band];
        while (weights_lin[band] == cur_level && band < last) {
            bsum = fx.n(std::int64_t{bsum} + widths[indices[band]]);
            ++band;
        }
        std::int32_t step = fx.n(std::int64_t{cur_level} - weights_lin[band]);
        step = fx.n(std::int64_t{step} * bsum);
        mnt = fx.n(std::int64_t{mnt} + step);
    }
    if (mnt < mtr) {
        bsum = num_bins;
    }
    if (bsum == 0) {
        return false;  // nothing was filled: no level to divide the surplus over
    }
    std::int32_t surplus = fx.n(std::int64_t{mnt} - mtr);
    surplus = fx.n(shl(surplus, 4));
    std::int32_t share = surplus / bsum;
    share = fx.n(shr(share, 4));
    const std::int32_t level = fx.n(std::int64_t{weights_lin[band]} + share);
    std::int32_t level_db = 0;
    if (!map_lin_to_db(fx, level, level_db)) {
        return false;
    }
    for (std::size_t b = 0; b < num_bands; ++b) {
        std::int32_t value = fx.n(std::int64_t{env_local[b]} - level_db);
        value = fx.n(std::int64_t{value} * kInvThree);
        value = fx.n(shr(value, 10));
        value = value > 0 ? value : 0;
        weights_db[b] = value < kMaxWdB ? value : kMaxWdB;
    }
    return !fx.overflow;
}

// --- Pseudocodes 40 to 47: the arithmetic decoder -------------------------------------------

constexpr std::uint32_t kModelBits = 15;
constexpr std::uint32_t kModelUnit = 1U << kModelBits;
constexpr std::uint32_t kRangeBits = 30;
constexpr std::uint32_t kThresholdLarge = 1U << (kRangeBits - 1);
constexpr std::uint32_t kThresholdSmall = 1U << (kRangeBits - 2);
constexpr std::uint32_t kOffsetBits = 14;

// What the CDF of a coefficient (Pseudocode 51) is clamped to, Q.15: 10.
constexpr std::int32_t kMaxValue = 327680;

// Pseudocode 52.
[[nodiscard]] std::int32_t idx_to_reconstruction(Fx& fx, std::int32_t index, std::int32_t dither,
                                                 std::int32_t step) noexcept {
    std::int32_t tmp1 = fx.n(shl(index, 15));
    std::int32_t reconstruction = fx.n(std::int64_t{tmp1} - dither);
    const std::int32_t tmp2 = fx.n(shr(reconstruction, 15));
    tmp1 = fx.n(shl(tmp2, 15));
    tmp1 = fx.n(std::int64_t{reconstruction} - tmp1);
    tmp1 = fx.n(shr(tmp1, 3));
    reconstruction = fx.n(std::int64_t{tmp1} * step);
    reconstruction = fx.n(shr(reconstruction, 12));
    tmp1 = fx.n(std::int64_t{tmp2} * step);
    return fx.n(std::int64_t{reconstruction} + tmp1);
}

// Pseudocode 53.
[[nodiscard]] std::uint32_t cdf_est(Fx& fx, std::int32_t value) noexcept {
    const std::int32_t index = fx.n(shr(value, 10) + 352);
    // Within -352..352 for a value kept to +-kMaxValue, as Pseudocode 51 keeps it.
    return tab::kSsfCdfTable[static_cast<std::size_t>(index)];
}

class AcDecoder {
   public:
    // Pseudocode 43. The decoder reads ahead of what the data holds past its end as zeros.
    AcDecoder(const BitReader& reader, std::size_t start)
        : reader_(reader), start_(start), pos_(start) {
        range_ = kThresholdLarge;
        offset_ = read_bit();
        for (std::uint32_t i = 1; i < kRangeBits; ++i) {
            offset_ = (offset_ << 1U) + read_bit();
        }
        offset2_ = offset_;
    }

    // Pseudocode 45.
    [[nodiscard]] std::uint32_t decode_target() const noexcept {
        const std::uint32_t range = range_ >> kModelBits;
        const std::uint32_t shifts = range < (1U << kOffsetBits) ? kModelBits : kModelBits - 1;
        std::uint32_t num = offset_;
        const std::uint32_t den = range << shifts;
        std::uint32_t target = 0;
        for (std::uint32_t i = shifts; i > 0; --i) {
            if (num >= den) {
                num -= den;
                target += 1;
            }
            num <<= 1U;
            target <<= 1U;
        }
        if (num >= den) {
            num -= den;
            target += 1;
        }
        if (target >= kModelUnit) {
            target = kModelUnit - 1;
        }
        return target;
    }

    // Pseudocode 46.
    void decode(std::uint32_t cdf_low, std::uint32_t cdf_high) {
        const std::uint32_t range = range_ >> kModelBits;
        const std::uint32_t tmp1 = range * cdf_low;
        offset_ -= tmp1;
        if (cdf_high < kModelUnit) {
            range_ = range * (cdf_high - cdf_low);
        } else {
            range_ -= tmp1;
        }
        while (range_ <= kThresholdSmall) {
            const std::uint32_t bit = read_bit();
            range_ <<= 1U;
            offset_ = (offset_ << 1U) + bit;
            offset2_ <<= 1U;
            if ((offset_ & 1U) != 0) {
                ++offset2_;
            }
        }
    }

    // Pseudocode 44 for a CDF table of `count` symbols 0 to count - 1, whose symbol s has
    // [table[s], table[s + 1]). The text's call passes 32 as the largest symbol of tables of 33
    // entries, one more than they have; the last symbol's interval ends at 32 768, so a target
    // never gets past it. Returns -1 where no symbol holds the target.
    [[nodiscard]] int decode_symbol(std::span<const std::uint16_t> table, int count) {
        const std::uint32_t target = decode_target();
        for (int s = 0; s < count; ++s) {
            const std::uint32_t low = table[static_cast<std::size_t>(s)];
            const std::uint32_t high = table[static_cast<std::size_t>(s) + 1];
            if (target < high && target >= low) {
                decode(low, high);
                return s;
            }
        }
        return -1;
    }

    // Pseudocode 44 for a coefficient, with its CDF from Pseudocode 51. The symbols are signed:
    // -max_index to max_index, ascending, the first whose interval holds the target.
    // Returns false where none does (Fx overflow included).
    [[nodiscard]] bool decode_coefficient(Fx& fx, std::int32_t step, std::int32_t dither,
                                          int max_index, int& symbol) {
        const std::uint32_t target = decode_target();
        const std::int32_t half = fx.n(shr(step, 1));
        for (int s = -max_index; s <= max_index; ++s) {
            const std::int32_t mid = idx_to_reconstruction(fx, s, dither, step);
            std::int32_t left = fx.n(std::int64_t{mid} - half);
            std::int32_t right = fx.n(std::int64_t{left} + step);
            // Both ends are kept to +-kMaxValue: Pseudocode 51 clamps only the left end from below
            // and the right from above, which leaves CdfEst() a symbol wholly beyond +-10 to look
            // up outside its table (src/ac4/ERRATA.md). Such a symbol's interval is empty.
            left = std::clamp(left, -kMaxValue, kMaxValue);
            right = std::clamp(right, -kMaxValue, kMaxValue);
            const std::uint32_t low = cdf_est(fx, left);
            const std::uint32_t high = cdf_est(fx, right);
            if (fx.overflow) {
                return false;
            }
            if (target < high && target >= low) {
                decode(low, high);
                symbol = s;
                return true;
            }
        }
        return false;
    }

    // Pseudocode 47: the bits the arithmetic coded data took. Reading past the end of the data
    // gives zeros and is allowed here, but not by the caller, which checks the count against
    // what the data holds. False where no termination length fits, which a range of more than
    // 2^28 always has.
    [[nodiscard]] bool finish(std::size_t& bits) const noexcept {
        const std::size_t read = pos_ - start_;
        std::uint32_t low = (offset2_ & (kThresholdLarge - 1U));
        low += kThresholdLarge - offset_;
        std::uint32_t bit_index = 1;
        for (; bit_index <= kRangeBits; ++bit_index) {
            const std::uint32_t reverse = kRangeBits - bit_index;
            const std::uint32_t up_fact = (1U << reverse) - 1U;
            const std::uint32_t bits_value = (low + up_fact) >> reverse;
            const std::uint32_t val = bits_value << reverse;
            const std::uint32_t high = val + up_fact;
            const std::uint32_t limit = (range_ - 1U) + low;
            if (low <= val && high <= limit) {
                break;
            }
        }
        if (bit_index > kRangeBits) {
            return false;
        }
        bits = read - kRangeBits + bit_index;
        return true;
    }

   private:
    [[nodiscard]] std::uint32_t read_bit() noexcept { return reader_.bit(pos_++); }

    const BitReader& reader_;
    std::size_t start_;
    std::size_t pos_;
    std::uint32_t range_ = 0;
    std::uint32_t offset_ = 0;
    std::uint32_t offset2_ = 0;
};

// --- Pseudocode 33 --------------------------------------------------------------------------

[[nodiscard]] double mmse_laplace(double mid_point, double step_size) noexcept {
    const double upper = mid_point + step_size / 2.0;
    const double lower = mid_point - step_size / 2.0;
    const double root2 = std::sqrt(2.0);
    double pdf_lower = root2 / 2.0 * std::exp(-std::abs(lower) * root2);
    double pdf_upper = root2 / 2.0 * std::exp(-std::abs(upper) * root2);
    pdf_lower = pdf_lower < 0.0 ? 0.0 : pdf_lower;
    pdf_upper = pdf_upper < 0.0 ? 0.0 : pdf_upper;
    double mmse = pdf_lower * (root2 * lower - 1.0) + pdf_upper * (root2 * upper + 1.0);
    mmse /= root2 * (pdf_lower + pdf_upper) - 2.0;
    if (lower > 0) {
        mmse = pdf_upper * (root2 * upper + 1.0);
        mmse -= pdf_lower * (root2 * lower + 1.0);
        mmse /= root2 * (pdf_upper - pdf_lower);
    }
    if (upper < 0) {
        mmse = pdf_upper * (root2 * upper - 1.0);
        mmse -= pdf_lower * (root2 * lower - 1.0);
        mmse /= root2 * (pdf_upper - pdf_lower);
    }
    return mmse;
}

// --- Pseudocode 26: f_rfu, from the predictor gain ------------------------------------------

[[nodiscard]] double rfu_of(double gain) noexcept {
    if (gain < -1.0) {
        return 1.0;
    }
    if (gain < 0.0) {
        return -gain;
    }
    if (gain < 1.0) {
        return gain;
    }
    if (gain < 2.0) {
        return 2.0 - gain;
    }
    return 0.0;
}

// --- Annex C.6 and Pseudocode 39: C(nu, eta, k) ---------------------------------------------

// The quantized prediction coefficient of table `tab_idx` at (nu, eta, k), eta 0 to 32, in the
// layout ((nu + Rf) * 33 + eta) * Rt + k (src/ac4/ERRATA.md).
[[nodiscard]] double pred_coeff(int tab_idx, int nu, int eta, int k) noexcept {
    const int rf = tab::kSsfPredRfsTable[static_cast<std::size_t>(tab_idx)];
    const int rt = tab::kSsfPredRtsTable[static_cast<std::size_t>(tab_idx)];
    const auto index = static_cast<std::size_t>(((nu + rf) * 33 + eta) * rt + k);
    const int quantized = tab::kSsfPredCoeffQuantMat[static_cast<std::size_t>(tab_idx)][index];
    return 1.1787855 * static_cast<double>(quantized - 146) / 128.0;
}

// C(nu, eta, k) for eta -32 to 32: the rule of 5.2.8.1 for negative eta.
[[nodiscard]] double pred_coeff_signed(int tab_idx, int nu, int eta, int k) noexcept {
    if (eta >= 0) {
        return pred_coeff(tab_idx, nu, eta, k);
    }
    const double sign = (k % 2 == 0) ? -1.0 : 1.0;  // (-1)^(k + 1)
    return sign * pred_coeff(tab_idx, -nu, -eta, k);
}

// --- one granule -----------------------------------------------------------------------------

struct GranuleDecoder {
    BitReader& r;
    const SubstreamContext& ctx;
    SsfState& st;
    SsfData& out;
    int granule_index;
    bool b_iframe;

    [[nodiscard]] ParseResult run();
};

[[nodiscard]] int block_length_column(int n_mdct) noexcept {
    for (std::size_t i = 0; i < tab::kSsfBlockLengths.size(); ++i) {
        if (tab::kSsfBlockLengths[i] == n_mdct) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Clause 5.2.8.3 and Pseudocode 55: what an SSF-I-frame starts from.
void reset_for_iframe(SsfState& st) {
    st.dither_gen = RandGenState{};
    st.noise_gen = RandGenState{};
    st.prev_pred_lag_idx = 0;
    st.last_spec.clear();
    for (std::vector<double>& row : st.spec_buffer) {
        row.clear();
    }
    st.env_buffer = {};
    st.env_prev = {};
}

ParseResult GranuleDecoder::run() {
    const int frame_len_base = ctx.frame_len_base;
    const int granule_length = frame_len_base >= 1536 ? frame_len_base / 2 : frame_len_base;
    // Table 44.
    const int stride_flag = static_cast<int>(r.read(1, "stride_flag"));
    int num_bands = st.num_bands;
    if (b_iframe) {
        num_bands = static_cast<int>(r.read(3, "num_bands_minus12")) + 12;
    }
    if (auto ok = check(r); !ok) {
        return ok;
    }
    if (b_iframe) {
        reset_for_iframe(st);
        st.num_bands = num_bands;
        st.started = true;
    } else if (!st.started) {
        return fail(DecodeError::kMissingIFrame,
                    "an SSF granule that needs an SSF-I-frame no frame has sent");
    }
    // Table 111 and 112: one block, or four where the configuration allows a short stride (no
    // frame of 512 or 384 lines does).
    const bool short_stride = stride_flag == 1;
    if (short_stride && (frame_len_base == 512 || frame_len_base == 384)) {
        st.started = false;
        return fail(DecodeError::kInvalidStream,
                    "an SSF short stride in a frame length that has none");
    }
    const int num_blocks = short_stride ? 4 : 1;
    const int n_mdct = granule_length / num_blocks;
    const int column = block_length_column(n_mdct);
    if (column < 0 || num_bands < 12 || num_bands > kSsfMaxBands) {
        st.started = false;
        return fail(DecodeError::kInvalidStream, "an SSF block length Table C.1 has no column for");
    }
    const std::span<const std::uint8_t> widths(
        tab::kSsfBandWidths[static_cast<std::size_t>(column)]);
    // Pseudocode 7.
    std::array<int, kSsfMaxBands> start_bin{};
    std::array<int, kSsfMaxBands> end_bin{};
    int num_bins = 0;
    for (int b = 0; b < num_bands; ++b) {
        start_bin[static_cast<std::size_t>(b)] = num_bins;
        num_bins += widths[static_cast<std::size_t>(b)];
        end_bin[static_cast<std::size_t>(b)] = num_bins - 1;
    }

    int start_block = 0;
    int end_block = 0;
    if (!short_stride && !b_iframe) {
        end_block = 1;
    }
    if (short_stride) {
        end_block = 4;
        if (b_iframe) {
            start_block = 1;
        }
    }
    std::array<bool, kSsfMaxBlocks> presence{};
    std::array<bool, kSsfMaxBlocks> delta{};
    for (int block = start_block; block < end_block; ++block) {
        const auto bi = static_cast<std::size_t>(block);
        presence[bi] = r.read_flag("predictor_presence_flag");
        if (presence[bi]) {
            if (start_block == 1 && block == 1) {
                delta[bi] = false;
            } else {
                delta[bi] = r.read_flag("delta_flag");
            }
        }
    }
    // Table 45.
    const auto env_curr_band0 = static_cast<int>(r.read(5, "env_curr_band0_bits"));
    int env_startup_band0 = 0;
    if (b_iframe && short_stride) {
        env_startup_band0 = static_cast<int>(r.read(5, "env_startup_band0_bits"));
    }
    std::array<int, kSsfMaxBlocks> gain_idx{};
    if (short_stride) {
        for (int block = 0; block < 4; ++block) {
            gain_idx[static_cast<std::size_t>(block)] =
                static_cast<int>(r.read(4, "gain_bits")) - 8;  // clause 4.3.7.3.3
        }
    }
    std::array<int, kSsfMaxBlocks> lag_delta_bits{};
    std::array<int, kSsfMaxBlocks> lag_bits{};
    std::array<bool, kSsfMaxBlocks> variance_preserving{};
    std::array<int, kSsfMaxBlocks> alloc_offset_bits{};
    for (int block = 0; block < num_blocks; ++block) {
        const auto bi = static_cast<std::size_t>(block);
        if (block >= start_block && block < end_block && presence[bi]) {
            if (delta[bi]) {
                lag_delta_bits[bi] = static_cast<int>(r.read(4, "predictor_lag_delta_bits"));
            } else {
                lag_bits[bi] = static_cast<int>(r.read(9, "predictor_lag_bits"));
            }
        }
        variance_preserving[bi] = r.read_flag("variance_preserving_flag");
        alloc_offset_bits[bi] = static_cast<int>(r.read(5, "alloc_offset_bits"));
    }
    if (auto ok = check(r); !ok) {
        st.started = false;
        return ok;
    }

    const auto invalid = [this](std::string_view reason) -> ParseResult {
        st.started = false;
        return fail(DecodeError::kInvalidStream, reason);
    };

    // Table 46: the arithmetic coded data starts here.
    const std::size_t ac_start = r.bit_position();
    AcDecoder ac(r, ac_start);
    Fx fx;
    const auto bands = static_cast<std::size_t>(num_bands);

    // Pseudocode 48 and 4a: the envelope.
    constexpr int kEnvDeltaMin = -16;
    constexpr int kEnvBand0Min = -28;
    constexpr int kEnvMin = -64;
    constexpr int kEnvMax = 63;
    const auto decode_envelope = [&](int band0_bits, std::array<int, kSsfMaxBands>& env) -> bool {
        env[0] = band0_bits + kEnvBand0Min;
        for (std::size_t b = 1; b < bands; ++b) {
            const int symbol = ac.decode_symbol(tab::kSsfEnvelopeCdfLut, 32);
            if (symbol < 0) {
                return false;
            }
            env[b] = env[b - 1] + symbol + kEnvDeltaMin;
        }
        return true;
    };
    std::array<int, kSsfMaxBands> env{};
    if (!decode_envelope(env_curr_band0, env)) {
        return invalid("an SSF envelope index no symbol of the model holds");
    }
    std::array<int, kSsfMaxBands> env_prev{};
    if (b_iframe && short_stride) {
        if (!decode_envelope(env_startup_band0, env_prev)) {
            return invalid("an SSF startup envelope index no symbol of the model holds");
        }
    } else {
        env_prev = st.env_prev;
    }
    for (std::size_t b = 0; b < bands; ++b) {
        if (env[b] < kEnvMin || env[b] > kEnvMax ||
            ((b_iframe && short_stride) && (env_prev[b] < kEnvMin || env_prev[b] > kEnvMax))) {
            return invalid("an SSF envelope outside -64 to 63");
        }
    }

    // Pseudocode 4b: interpolation (SHORT_STRIDE), else none.
    std::array<std::array<int, kSsfMaxBands>, kSsfMaxBlocks> env_interp{};
    if (short_stride) {
        constexpr std::int64_t kUnit = 1024;
        constexpr std::int64_t kHalf = 512;
        constexpr std::int64_t kInvNumBlocks = 256;
        for (std::size_t b = 0; b < bands; ++b) {
            const std::int32_t left_delta = fx.n(std::int64_t{env[b] - env_prev[b]} * kUnit);
            std::int32_t left_slope = fx.n(std::int64_t{left_delta} * kInvNumBlocks);
            left_slope = fx.n(shr(left_slope, 10));
            for (int block = 0; block < 4; ++block) {
                std::int32_t interp = fx.n(std::int64_t{1 + block} * left_slope);
                const std::int32_t base = fx.n(std::int64_t{env_prev[b]} * kUnit);
                interp = fx.n(std::int64_t{interp} + base);
                interp =
                    fx.n(interp > 0 ? std::int64_t{interp} + kHalf : std::int64_t{interp} - kHalf);
                env_interp[static_cast<std::size_t>(block)][b] = fx.n(shr(interp, 10));
            }
        }
    } else {
        env_interp[0] = env;
    }
    if (fx.overflow) {
        return invalid("an SSF envelope that leaves the fixed point range");
    }

    constexpr int kHighFreqGainThreshold = 2;
    std::array<std::array<double, kSsfMaxBands>, kSsfMaxBlocks> f_env_signal{};
    std::array<std::array<int, kSsfMaxBands>, kSsfMaxBlocks> env_alloc{};
    for (int block = 0; block < num_blocks; ++block) {
        const auto bi = static_cast<std::size_t>(block);
        const double gain = std::pow(10.0, gain_idx[bi] * 0.1);  // Pseudocode 4c
        for (std::size_t b = 0; b < bands; ++b) {
            // Pseudocode 4d.
            f_env_signal[bi][b] = std::pow(2.0, 0.5 * env_interp[bi][b]);
            env_alloc[bi][b] = env_interp[bi][b];
            if (b >= static_cast<std::size_t>(kHighFreqGainThreshold)) {
                f_env_signal[bi][b] *= gain;
                env_alloc[bi][b] += static_cast<int>(std::round(2.0 * gain_idx[bi] / 3.0));
                env_alloc[bi][b] = std::clamp(env_alloc[bi][b], kEnvMin, kEnvMax);
            }
        }
    }

    // The frame's lines: this granule's blocks, n_mdct each, from the granule's own offset.
    const auto n = static_cast<std::size_t>(n_mdct);
    const std::size_t granule_offset =
        static_cast<std::size_t>(granule_index) * static_cast<std::size_t>(granule_length);
    const auto bins = static_cast<std::size_t>(num_bins);

    // Clause 5.2.8.3, Pseudocode 58: the dither of every block of the granule.
    std::vector<std::int32_t> dither_cur(static_cast<std::size_t>(num_blocks) * bins);
    for (std::int32_t& value : dither_cur) {
        value = tab::kSsfDitherTable[st.dither_gen.current_idx];  // Pseudocode 56
        advance(st.dither_gen);
    }

    // The spectra buffers hold lines of the length they were kept at: bring them to this
    // granule's (src/ac4/ERRATA.md).
    for (std::vector<double>& row : st.spec_buffer) {
        row.resize(bins, 0.0);
    }
    st.last_spec.resize(bins, 0.0);

    constexpr int kPredLagDeltaMin = -8;
    std::vector<double> spec_res(bins);
    std::vector<double> spec_invq(bins);
    std::vector<double> spec_extract(bins);
    std::vector<double> spec(bins);
    std::array<std::int32_t, kSsfMaxBands> alloc_table{};

    for (int block = 0; block < num_blocks; ++block) {
        const auto bi = static_cast<std::size_t>(block);
        // Pseudocode 4e: the predictor parameters.
        double f_pred_gain = 0.0;
        int pred_lag_idx = 0;
        if (block >= start_block && block < end_block && presence[bi]) {
            const int gain_symbol = ac.decode_symbol(tab::kSsfPredictorGainCdfLut, 32);
            if (gain_symbol < 0) {
                return invalid("an SSF predictor gain index no symbol of the model holds");
            }
            f_pred_gain = tab::kSsfPredGainQuantTab[static_cast<std::size_t>(gain_symbol)];
            if (delta[bi]) {
                pred_lag_idx = lag_delta_bits[bi] + st.prev_pred_lag_idx + kPredLagDeltaMin;
            } else {
                pred_lag_idx = lag_bits[bi];
            }
            if (pred_lag_idx < 0 || pred_lag_idx > 509) {
                return invalid("an SSF predictor lag index outside 0 to 509");
            }
        }
        st.prev_pred_lag_idx = pred_lag_idx;
        const double f_pred_lag = 640.0 * std::pow(2.0, (pred_lag_idx - 509) / 170.0);

        // Pseudocode 26: the helper variables.
        constexpr double kRfuThreshold = 0.75;
        constexpr int kAllocDitheringSmall = 3;
        constexpr int kAllocDitheringLarge = 5;
        const double f_rfu = rfu_of(f_pred_gain);
        int dithering_threshold =
            f_rfu > kRfuThreshold ? kAllocDitheringSmall : kAllocDitheringLarge;
        if (variance_preserving[bi]) {
            dithering_threshold = kAllocDitheringLarge;
        }
        const double adaptive_noise_gain_var_pres = std::sqrt(1.0 - (f_rfu * f_rfu));
        const double adaptive_noise_gain = 1.0 - f_rfu;

        // Pseudocode 27: heuristic scaling and the allocation envelope.
        std::array<int, kSsfMaxBands> env_alloc_mod{};
        std::array<double, kSsfMaxBands> f_gain_q{};
        f_gain_q.fill(1.0);
        for (std::size_t b = 0; b < bands; ++b) {
            env_alloc_mod[b] = env_alloc[bi][b];
        }
        if (f_rfu > 0.0 && !variance_preserving[bi]) {
            std::array<std::int32_t, kSsfMaxBands> env_in{};
            for (std::size_t b = 0; b < bands; ++b) {
                env_in[b] = 3 * env_alloc[bi][b];
            }
            // f_rfu in Q.10, which the text leaves unconverted: rounded.
            const auto i_rfu = static_cast<std::int32_t>(std::floor(f_rfu * 1024.0 + 0.5));
            std::array<std::int32_t, kSsfMaxBands> weights_db{};
            if (!heuristic_scaling(fx, i_rfu, std::span<const std::int32_t>(env_in).first(bands),
                                   widths, num_bins,
                                   std::span<std::int32_t>(weights_db).first(bands))) {
                return invalid("an SSF heuristic scaling the fixed point range cannot hold");
            }
            std::array<int, kSsfMaxBands> i_w_db{};
            for (std::size_t b = 0; b < bands; ++b) {
                i_w_db[b] = static_cast<int>(shr(weights_db[b] / 2, 10));
            }
            constexpr int kLfBoostThreshold = 3;
            i_w_db[0] = i_w_db[0] > kLfBoostThreshold ? i_w_db[0] - kLfBoostThreshold : 0;
            for (std::size_t b = 0; b < bands; ++b) {
                const double f_w_db = static_cast<double>(weights_db[b]) / 1024.0;
                f_gain_q[b] = std::pow(10.0, 1.5 / 20.0 * f_w_db);
                env_alloc_mod[b] = std::clamp(env_alloc[bi][b] - i_w_db[b], kEnvMin, kEnvMax);
            }
        }

        // Pseudocode 31: the allocation of each band.
        constexpr int kMinAllocOffset = -21;
        constexpr int kEnvMax2MinOffset = 20;
        const int alloc_offset = alloc_offset_bits[bi] + kMinAllocOffset;
        int i_max = *std::max_element(env_alloc_mod.begin(),
                                      env_alloc_mod.begin() + static_cast<std::ptrdiff_t>(bands));
        i_max -= kEnvMax2MinOffset;
        for (std::size_t b = 0; b < bands; ++b) {
            alloc_table[b] = std::clamp(env_alloc_mod[b] - i_max + alloc_offset, 0, 20);
        }

        // Pseudocodes 50 and 32: the coefficients, decoded and dequantised band by band.
        for (std::size_t b = 0; b < bands; ++b) {
            const auto start = static_cast<std::size_t>(start_bin[b]);
            const auto end = static_cast<std::size_t>(end_bin[b]);
            const std::int32_t i_alloc = alloc_table[b];
            const bool dithered = i_alloc != 0 && i_alloc < dithering_threshold;
            const bool var_pres = variance_preserving[bi] && b > 1;
            if (i_alloc == 0) {
                for (std::size_t bin = start; bin <= end; ++bin) {
                    spec_invq[bin] = static_cast<double>(get_random_noise_value(st.noise_gen));
                    spec_invq[bin] *= var_pres ? adaptive_noise_gain_var_pres : adaptive_noise_gain;
                }
                continue;
            }
            const std::int32_t step = tab::kSsfStepSizesQ4_15[static_cast<std::size_t>(i_alloc)];
            const int max_index = tab::kSsfAcCoeffMaxIndex[static_cast<std::size_t>(i_alloc)] + 1;
            for (std::size_t bin = start; bin <= end; ++bin) {
                const std::int32_t dither = dithered ? dither_cur[bi * bins + bin] : 0;
                int quant_idx = 0;
                if (!ac.decode_coefficient(fx, step, dither, max_index, quant_idx)) {
                    return invalid("an SSF coefficient no symbol of the model holds");
                }
                if (dithered) {
                    const std::int32_t mid_point =
                        idx_to_reconstruction(fx, quant_idx, dither, step);
                    const double f_mid_point = static_cast<double>(mid_point) / 32768.0;
                    double f_post_gain =
                        tab::kSsfPostGainLut[static_cast<std::size_t>(i_alloc - 1)];
                    if (var_pres) {
                        const double var_pres_gain =
                            std::sqrt(f_post_gain) * adaptive_noise_gain_var_pres;
                        if (var_pres_gain > f_post_gain) {
                            f_post_gain = var_pres_gain;
                        }
                    }
                    spec_invq[bin] = f_mid_point * f_post_gain;
                } else {
                    const std::int32_t mid_point = idx_to_reconstruction(fx, quant_idx, 0, step);
                    const double f_mid_point = static_cast<double>(mid_point) / 32768.0;
                    const double f_step_size = static_cast<double>(step) / 32768.0;
                    spec_invq[bin] = mmse_laplace(f_mid_point, f_step_size);
                }
            }
        }
        if (fx.overflow) {
            return invalid("an SSF block that leaves the fixed point range");
        }
        // Pseudocode 34: heuristic inverse scaling, unless variance preserving.
        for (std::size_t b = 0; b < bands; ++b) {
            const double f_gain_value = 1.0 / f_gain_q[b];
            for (auto bin = static_cast<std::size_t>(start_bin[b]);
                 bin <= static_cast<std::size_t>(end_bin[b]); ++bin) {
                spec_res[bin] =
                    variance_preserving[bi] ? spec_invq[bin] : spec_invq[bin] * f_gain_value;
            }
        }

        // Pseudocode 35: the buffers take the last block's lines and this block's envelope.
        for (int i = kSsfSpecBuffers - 1; i > 0; --i) {
            st.spec_buffer[static_cast<std::size_t>(i)] =
                st.spec_buffer[static_cast<std::size_t>(i - 1)];
        }
        st.spec_buffer[0] = st.last_spec;
        for (int i = kSsfEnvBuffers - 1; i > 0; --i) {
            st.env_buffer[static_cast<std::size_t>(i)] =
                st.env_buffer[static_cast<std::size_t>(i - 1)];
        }
        st.env_buffer[0] = f_env_signal[bi];

        // Pseudocodes 36 and 37: the subband predictor.
        std::fill(spec_extract.begin(), spec_extract.end(), 0.0);
        std::vector<double> spec_pred(bins, 0.0);
        if (f_pred_gain != 0.0) {
            double period = f_pred_lag / static_cast<double>(n_mdct);  // T_0
            int k_s = 0;
            if (period > 81.0 / 32.0) {
                k_s = 1;
                period -= 1.0;  // T
            }
            const int tab_idx =
                period <= 9.0 / 32.0 ? 0 : static_cast<int>(std::floor(16.0 * period + 0.5)) - 4;
            if (tab_idx < 0 || tab_idx > 36) {
                return invalid("an SSF predictor lag with no coefficient table");
            }
            const int rt = tab::kSsfPredRtsTable[static_cast<std::size_t>(tab_idx)];
            const int rf = tab::kSsfPredRfsTable[static_cast<std::size_t>(tab_idx)];
            // Z(n, k), extended by zeros above num_bins and by even reflection below 0.
            const auto z = [&](int nn, int k) -> double {
                if (nn < 0) {
                    nn = -1 - nn;
                }
                if (nn >= num_bins) {
                    return 0.0;
                }
                return st
                    .spec_buffer[static_cast<std::size_t>(k + k_s)][static_cast<std::size_t>(nn)];
            };
            const double min_2t = 2.0 * period < 1.0 ? 2.0 * period : 1.0;
            for (int p = 0; p < num_bins; ++p) {
                double sum = 0.0;
                for (int nu = -rf; nu <= rf; ++nu) {
                    const int mu = 2 * p + nu + 1;
                    double phi = (period / 4.0) * mu;
                    phi = std::floor(phi + 0.5) - phi;  // Phi(mu)
                    int eta = 0;
                    if (phi > period) {
                        eta = 32;
                    } else if (phi < -period) {
                        eta = -32;
                    } else {
                        eta = static_cast<int>(std::floor(64.0 * phi / min_2t + 0.5));
                    }
                    for (int k = 0; k < rt; ++k) {
                        const double sign = ((k + 1) * p) % 2 == 0 ? 1.0 : -1.0;
                        sum += sign * pred_coeff_signed(tab_idx, nu, eta, k) * z(p + nu, k);
                    }
                }
                spec_extract[static_cast<std::size_t>(p)] = sum;
            }
            // Pseudocode 37: the shaper.
            int integer_lag =
                static_cast<int>(std::floor(f_pred_lag / static_cast<double>(n_mdct) + 0.5));
            if (b_iframe && integer_lag > 0) {
                integer_lag = 0;
            }
            if (integer_lag >= kSsfEnvBuffers) {
                return invalid("an SSF predictor lag past the envelope buffer");
            }
            for (std::size_t b = 0; b < bands; ++b) {
                const double buffered = st.env_buffer[static_cast<std::size_t>(integer_lag)][b];
                if (buffered == 0.0) {
                    return invalid("an SSF predictor lag before the first block");
                }
                const double f_envelope = 1.0 / buffered;
                for (auto bin = static_cast<std::size_t>(start_bin[b]);
                     bin <= static_cast<std::size_t>(end_bin[b]); ++bin) {
                    spec_pred[bin] = spec_extract[bin] * f_envelope * f_pred_gain;
                }
            }
        }

        // Pseudocode 38: inverse flattening.
        for (std::size_t b = 0; b < bands; ++b) {
            for (auto bin = static_cast<std::size_t>(start_bin[b]);
                 bin <= static_cast<std::size_t>(end_bin[b]); ++bin) {
                spec[bin] = (spec_res[bin] + spec_pred[bin]) * f_env_signal[bi][b];
            }
        }
        st.last_spec = spec;
        const std::size_t offset = granule_offset + static_cast<std::size_t>(block) * n;
        std::copy(spec.begin(), spec.end(),
                  out.lines.begin() + static_cast<std::ptrdiff_t>(offset));
        std::fill(out.lines.begin() + static_cast<std::ptrdiff_t>(offset + bins),
                  out.lines.begin() + static_cast<std::ptrdiff_t>(offset + n), 0.0);
    }

    // Pseudocode 47: where the arithmetic coded data ended; the next granule starts there.
    std::size_t ac_bits = 0;
    if (!ac.finish(ac_bits)) {
        return invalid("an SSF arithmetic decoder that cannot terminate");
    }
    if (ac_start + ac_bits > r.size_bits()) {
        st.started = false;
        r.seek(r.size_bits() + 1);  // past the end: the reader's sticky overflow
        return check(r);
    }
    // One record for the whole of ssf_ac_data(), valued at its last 64 bits.
    std::uint64_t tail = 0;
    const std::size_t first = ac_bits > 64 ? ac_start + ac_bits - 64 : ac_start;
    for (std::size_t bit = first; bit < ac_start + ac_bits; ++bit) {
        tail = (tail << 1U) | r.bit(bit);
    }
    r.record(ac_start, static_cast<int>(ac_bits), tail, "ssf_ac_data");
    r.seek(ac_start + ac_bits);

    // What the next granule starts from.
    st.env_prev = env;

    out.granules[static_cast<std::size_t>(granule_index)] = SsfGranule{.stride_flag = stride_flag,
                                                                       .num_bands = num_bands,
                                                                       .n_mdct = n_mdct,
                                                                       .num_blocks = num_blocks,
                                                                       .num_bins = num_bins};
    return {};
}

}  // namespace

ParseResult parse_ssf_data(BitReader& r, const SubstreamContext& ctx, SsfState& state,
                           SsfData& out) {
    const int frame_len_base = ctx.frame_len_base;
    out.lines.assign(static_cast<std::size_t>(frame_len_base), 0.0);
    out.granule_count = frame_len_base >= 1536 ? 2 : 1;
    // Table 43.
    bool b_ssf_iframe = true;
    if (!ctx.b_iframe) {
        b_ssf_iframe = r.read_flag("b_ssf_iframe");
    }
    for (int g = 0; g < out.granule_count; ++g) {
        GranuleDecoder decoder{r, ctx, state, out, g, g == 0 ? b_ssf_iframe : false};
        if (auto ok = decoder.run(); !ok) {
            state.started = false;
            return ok;
        }
    }
    return check(r);
}

void ssf_window_lengths(const SsfData& data, std::vector<int>& lengths) {
    lengths.clear();
    for (int g = 0; g < data.granule_count; ++g) {
        const SsfGranule& granule = data.granules[static_cast<std::size_t>(g)];
        for (int b = 0; b < granule.num_blocks; ++b) {
            lengths.push_back(granule.n_mdct);
        }
    }
}

bool ssf_map_db_to_lin(std::int32_t input, std::int32_t& result) noexcept {
    Fx fx;
    return map_db_to_lin(fx, input, result);
}

bool ssf_map_lin_to_db(std::int32_t input, std::int32_t& result) noexcept {
    Fx fx;
    return map_lin_to_db(fx, input, result);
}

}  // namespace iclforge::ac4::detail
