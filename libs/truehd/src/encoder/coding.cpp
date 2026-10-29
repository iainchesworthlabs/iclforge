#include "encoder/coding.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <numbers>

#include "core/huffman.hpp"

namespace iclforge::truehd::detail {

namespace {

constexpr int kMinSymbol = -7;  // TH spec App. C: every codebook starts at -7
constexpr std::array<int, 4> kMaxSymbol{0, 10, 8, 7};
constexpr int kMaxOffset = 16383;  // huff_offset is 15 bits, signed
constexpr int kMinOffset = -16384;

// Code length and code of symbol s (-7..10) in codebook `type`; length 0 where the codebook has no
// such symbol.
struct CodeTable {
    std::array<std::array<std::uint8_t, 18>, 4> length{};
    std::array<std::array<std::uint16_t, 18>, 4> code{};
};

template <typename Book>
constexpr void fill(CodeTable& table, int type, const Book& book) {
    for (const BookEntry& entry : book) {
        const auto slot = static_cast<std::size_t>(entry.symbol - kMinSymbol);
        table.length[static_cast<std::size_t>(type)][slot] = entry.bits;
        table.code[static_cast<std::size_t>(type)][slot] = entry.code;
    }
}

constexpr CodeTable make_code_table() {
    CodeTable table;
    fill(table, 1, kBook1);
    fill(table, 2, kBook2);
    fill(table, 3, kBook3);
    return table;
}

constexpr CodeTable kCodes = make_code_table();

[[nodiscard]] int ceil_log2(std::int64_t value) noexcept {
    int bits = 0;
    while ((std::int64_t{1} << bits) < value && bits < 40) {
        ++bits;
    }
    return bits;
}

// The term TH spec §11.3 subtracts so that symbol 0 sits at the middle of its codebook's useful
// range (ERRATA.md, entry 11): 1 << (lb + 2 - type) for a Huffman channel (nothing if that
// exponent is negative), 1 << (lb - 1) for raw bits.
[[nodiscard]] std::int64_t centre_term(int type, int lb) noexcept {
    if (type == 0) {
        return lb > 0 ? std::int64_t{1} << (lb - 1) : 0;
    }
    const int shift = lb + 2 - type;
    return shift >= 0 ? std::int64_t{1} << shift : 0;
}

}  // namespace

int coded_bits(std::span<const std::int32_t> residuals, int quant,
               const ChannelCoding& coding) noexcept {
    const int lb = static_cast<int>(coding.lsbs) - quant;
    if (lb < 0) {
        return kInfeasible;
    }
    const std::int64_t centre = centre_term(coding.type, lb) - coding.offset;
    std::int64_t total = 0;
    if (coding.type == 0) {
        const std::int64_t span = std::int64_t{1} << lb;
        for (const std::int32_t r : residuals) {
            const std::int64_t t = r + centre;
            if (t < 0 || t >= span) {
                return kInfeasible;
            }
        }
        return static_cast<int>(static_cast<std::int64_t>(residuals.size()) * lb);
    }
    const int high = kMaxSymbol[coding.type];
    const auto& lengths = kCodes.length[coding.type];
    for (const std::int32_t r : residuals) {
        const std::int64_t symbol = (r + centre) >> lb;
        if (symbol < kMinSymbol || symbol > high) {
            return kInfeasible;
        }
        const int length = lengths[static_cast<std::size_t>(symbol - kMinSymbol)];
        if (length + lb > kMaxSampleBits) {
            return kInfeasible;
        }
        total += length + lb;
    }
    return static_cast<int>(total);
}

int channel_header_bits(const ChannelCoding& before, const ChannelCoding& after,
                        int fir_bits) noexcept {
    if (fir_bits == 0 && before == after) {
        return 1;  // params_for_this_chan clear
    }
    // params_for_this_chan, the FIR flag (and the filter), the IIR flag, the offset flag (and the
    // offset), huff_type and huff_lsbs.
    int bits = 1 + 1 + fir_bits + 1 + 1 + 2 + 5;
    if (after.offset != before.offset) {
        bits += 15;
    }
    return bits;
}

CodingChoice choose_coding(std::span<const std::int32_t> residuals, int quant,
                           const ChannelCoding& before, int fir_bits) noexcept {
    CodingChoice best;
    if (residuals.empty()) {
        return best;
    }
    std::int64_t lowest = residuals[0];
    std::int64_t highest = residuals[0];
    std::int64_t sum = 0;
    for (const std::int32_t r : residuals) {
        lowest = std::min<std::int64_t>(lowest, r);
        highest = std::max<std::int64_t>(highest, r);
        sum += r;
    }
    const auto n = static_cast<std::int64_t>(residuals.size());
    const std::int64_t mean = (sum >= 0 ? sum + n / 2 : sum - n / 2) / n;
    const std::int64_t spread = highest - lowest + 1;
    const int lb_limit = 24 - quant;

    const auto clamp_offset = [](std::int64_t value) noexcept {
        return static_cast<std::int32_t>(std::clamp<std::int64_t>(value, kMinOffset, kMaxOffset));
    };
    const auto consider = [&](int type, int lb, std::int32_t offset) noexcept {
        if (lb < 0 || lb > lb_limit) {
            return;
        }
        ChannelCoding candidate;
        candidate.type = static_cast<std::uint8_t>(type);
        candidate.lsbs = static_cast<std::uint8_t>(lb + quant);
        candidate.offset = offset;
        const int data = coded_bits(residuals, quant, candidate);
        if (data >= kInfeasible) {
            return;
        }
        const int header = channel_header_bits(before, candidate, fir_bits);
        if (data + header < best.total()) {
            best.coding = candidate;
            best.data_bits = data;
            best.header_bits = header;
        }
    };

    for (int type = 0; type <= 3; ++type) {
        int first_lb = 0;
        int count = 5;
        if (type == 0) {
            first_lb = ceil_log2(spread);
            count = 2;
        } else {
            const int high = kMaxSymbol[static_cast<std::size_t>(type)];
            first_lb = std::max(0, ceil_log2((spread + 7 + high) / (8 + high)));
        }
        for (int step = 0; step < count; ++step) {
            const int lb = first_lb + step;
            const std::int64_t half = lb > 0 ? std::int64_t{1} << (lb - 1) : 0;
            consider(type, lb, before.offset);
            consider(type, lb, 0);
            consider(type, lb, clamp_offset(mean));
            if (type != 0 && half > 0) {
                consider(type, lb, clamp_offset(mean + half));
                consider(type, lb, clamp_offset(mean - half));
            }
            if (type == 0) {
                consider(type, lb, clamp_offset((lowest + highest) / 2));
            }
        }
        // The raw width the channel already has costs nothing to keep.
        if (before.type == type && before.lsbs >= quant) {
            consider(type, before.lsbs - quant, before.offset);
            consider(type, before.lsbs - quant, clamp_offset(mean));
        }
    }
    return best;
}

void put_residual(BitWriter& writer, std::int32_t residual, int quant,
                  const ChannelCoding& coding) noexcept {
    const int lb = static_cast<int>(coding.lsbs) - quant;
    const std::int64_t t = residual + centre_term(coding.type, lb) - coding.offset;
    if (coding.type == 0) {
        if (lb > 0) {
            writer.put(static_cast<std::uint64_t>(t), lb);
        }
        return;
    }
    const std::int64_t symbol = t >> lb;
    const auto slot = static_cast<std::size_t>(symbol - kMinSymbol);
    writer.put(kCodes.code[coding.type][slot], kCodes.length[coding.type][slot]);
    if (lb > 0) {
        writer.put(static_cast<std::uint64_t>(t - (symbol << lb)), lb);
    }
}

double estimated_bits(std::span<const std::int32_t> residuals) noexcept {
    if (residuals.empty()) {
        return 0.0;
    }
    double sum = 0.0;
    for (const std::int32_t r : residuals) {
        sum += std::fabs(static_cast<double>(r));
    }
    const double mean = sum / static_cast<double>(residuals.size());
    if (mean < 0.05) {
        return 0.0;
    }
    return static_cast<double>(residuals.size()) * (std::log2(mean + 1.0) + 1.6);
}

Predictors levinson(std::span<const double> r, int max_order) noexcept {
    Predictors out;
    max_order = std::min(max_order, Predictors::kMaxOrder);
    out.max_order = max_order;
    out.error[0] = r[0];
    std::array<double, Predictors::kMaxOrder + 1> a{};  // a[1..m]
    double error = r[0];
    for (int m = 1; m <= max_order; ++m) {
        double acc = r[static_cast<std::size_t>(m)];
        for (int k = 1; k < m; ++k) {
            acc -= a[static_cast<std::size_t>(k)] * r[static_cast<std::size_t>(m - k)];
        }
        const double reflection = error > 0.0 ? acc / error : 0.0;
        std::array<double, Predictors::kMaxOrder + 1> next = a;
        next[static_cast<std::size_t>(m)] = reflection;
        for (int k = 1; k < m; ++k) {
            next[static_cast<std::size_t>(k)] =
                a[static_cast<std::size_t>(k)] - reflection * a[static_cast<std::size_t>(m - k)];
        }
        a = next;
        error *= (1.0 - reflection * reflection);
        out.error[static_cast<std::size_t>(m)] = error;
        for (int k = 1; k <= m; ++k) {
            out.coeff[static_cast<std::size_t>(m)][static_cast<std::size_t>(k - 1)] =
                a[static_cast<std::size_t>(k)];
        }
    }
    return out;
}

std::vector<double> autocorrelation(std::span<const std::int32_t> samples, int max_lag) {
    const std::size_t n = samples.size();
    std::vector<double> windowed(n);
    // A Tukey window with 10 % tapers: the interval is analysed as a whole, so the ends are
    // softened only enough to keep the edge out of the spectrum.
    const double taper = std::max(1.0, 0.05 * static_cast<double>(n));
    for (std::size_t t = 0; t < n; ++t) {
        double w = 1.0;
        const auto position = static_cast<double>(t);
        if (position < taper) {
            w = 0.5 - 0.5 * std::cos(std::numbers::pi * position / taper);
        } else if (position > static_cast<double>(n) - taper) {
            w = 0.5 -
                0.5 * std::cos(std::numbers::pi * (static_cast<double>(n) - position) / taper);
        }
        windowed[t] = w * static_cast<double>(samples[t]);
    }
    std::vector<double> r(static_cast<std::size_t>(max_lag) + 1, 0.0);
    for (int lag = 0; lag <= max_lag; ++lag) {
        double acc = 0.0;
        for (std::size_t t = static_cast<std::size_t>(lag); t < n; ++t) {
            acc += windowed[t] * windowed[t - static_cast<std::size_t>(lag)];
        }
        r[static_cast<std::size_t>(lag)] = acc;
    }
    // White noise correction: keeps the recursion well conditioned on a pure tone.
    r[0] = r[0] * (1.0 + 1e-9) + 1e-3;
    return r;
}

FilterSpec quantise_fir(std::span<const double> coefficients) {
    FilterSpec filter;
    const int order = static_cast<int>(std::min<std::size_t>(coefficients.size(), kMaxFirOrder));
    if (order == 0) {
        return filter;
    }
    double largest = 0.0;
    for (int i = 0; i < order; ++i) {
        largest = std::max(largest, std::fabs(coefficients[static_cast<std::size_t>(i)]));
    }
    int shift = 15;  // TH spec §9.3: coeff_q is 8..15
    while (shift > 8 && largest * static_cast<double>(1 << shift) > 32767.0) {
        --shift;
    }
    std::array<std::int32_t, kMaxFirOrder> values{};
    for (int i = 0; i < order; ++i) {
        const double scaled =
            coefficients[static_cast<std::size_t>(i)] * static_cast<double>(1 << shift);
        const auto value = static_cast<std::int32_t>(
            std::clamp(std::lround(scaled), static_cast<long>(-32767), static_cast<long>(32767)));
        values[static_cast<std::size_t>(i)] = value;
    }
    // The coefficients are sent as coeff_bits wide integers shifted left by coeff_shift: take out
    // the zero bits every coefficient shares (TH spec §9.2).
    int common = 7;
    bool any = false;
    for (int i = 0; i < order; ++i) {
        const std::int32_t value = values[static_cast<std::size_t>(i)];
        if (value != 0) {
            any = true;
            common = std::min(common, std::countr_zero(static_cast<std::uint32_t>(value)));
        }
    }
    if (!any) {
        common = 0;
    }
    std::int32_t lowest = 0;
    std::int32_t highest = 0;
    for (int i = 0; i < order; ++i) {
        const std::int32_t stored = values[static_cast<std::size_t>(i)] / (1 << common);
        lowest = std::min(lowest, stored);
        highest = std::max(highest, stored);
    }
    int bits = 1;
    while (bits < 16 && (lowest < -(1 << (bits - 1)) || highest > (1 << (bits - 1)) - 1)) {
        ++bits;
    }
    filter.order = static_cast<std::uint8_t>(order);
    filter.shift = static_cast<std::uint8_t>(shift);
    filter.coeff_shift = static_cast<std::uint8_t>(common);
    filter.coeff_bits = static_cast<std::uint8_t>(bits);
    for (int i = 0; i < order; ++i) {
        filter.coeff[static_cast<std::size_t>(i)] = values[static_cast<std::size_t>(i)];
    }
    return filter;
}

int filter_header_bits(const FilterSpec& filter) noexcept {
    if (filter.order == 0) {
        return 0;
    }
    // order, shift, coeff_bits, coeff_shift, the coefficients, the state flag.
    return 4 + 4 + 5 + 3 + filter.order * filter.coeff_bits + 1;
}

std::int64_t fir_prediction(const FilterSpec& filter, std::span<const std::int32_t> history,
                            std::size_t t) noexcept {
    std::int64_t sum = 0;
    const std::size_t taps = std::min<std::size_t>(filter.order, t);
    for (std::size_t k = 0; k < taps; ++k) {
        sum += std::int64_t{history[t - 1 - k]} * filter.coeff[k];
    }
    return sum >> filter.shift;
}

}  // namespace iclforge::truehd::detail
