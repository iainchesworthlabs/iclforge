#include "encoder/asf/coder.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "core/tables/huffman_codes.hpp"
#include "core/tables/huffman_tables.hpp"
#include "core/tables/sfb_tables.hpp"

namespace iclforge::ac4::detail {
namespace {

constexpr int kCodebooks = 12;          // 0, and Tables A.2 to A.12's 1 to 11
constexpr std::int32_t kMaxQuant = 8191; // what ext_code (Pseudocode 20) reaches
constexpr int kEscape = 16;              // codebook 11's escape value
constexpr int kMaxDelta = 60;            // Table A.1's deltas run -60 to 60
// Noise fill (Part 1 5.1.4.2): 1.44269504 is the text's own rounding of 1/ln 2,
// which the decoder measures its levels with; dpcm_snf is the delta plus 17,
// 0 (delta -17) escapes, and Table A.13 has 22 indices.
constexpr double kLog2E = 1.44269504;
constexpr int kNoiseOffset = 17;
constexpr int kNoiseDeltaMin = -16;
constexpr int kNoiseDeltaMax = 4;
// The quantiser's rounding offset: q = floor(|x/g|^(3/4) + kRounding). 0.5
// would round in the |q| domain; less than that lowers the error in the line
// domain, where the reconstruction's 4/3 power stretches the upper half of
// each interval.
constexpr double kRounding = 0.4054;

// Table 39: section lengths in 3-bit or 5-bit increments.
[[nodiscard]] int section_bits_width(int index) noexcept {
    return index <= 2 ? 3 : 5;
}

[[nodiscard]] std::size_t section_header_bits(int band_count, int width) {
    const int escape = (1 << width) - 1;
    const int increments = (band_count - 1) / escape + 1;
    return 4 + static_cast<std::size_t>(width * increments);
}

// The largest magnitude a codebook codes directly, and its values' range.
struct CodebookShape {
    int dim = 0;
    bool is_unsigned = false;
    int min_value = 0;
    int max_value = 0;  // codebook 11: 16, the escape
    int mod = 0, mod2 = 0, mod3 = 0, off = 0;
};

[[nodiscard]] CodebookShape shape(int cb) {
    const Codebook& book = *tables::kAsfSpectrumCodebooks[static_cast<std::size_t>(cb)];
    CodebookShape s;
    s.dim = tables::kCbDim[static_cast<std::size_t>(cb)];
    s.is_unsigned = tables::kUnsignedCb[static_cast<std::size_t>(cb)];
    s.mod = book.cb_mod;
    s.mod2 = book.cb_mod2;
    s.mod3 = book.cb_mod3;
    s.off = book.cb_off;
    if (s.is_unsigned) {
        s.min_value = 0;
        s.max_value = book.cb_mod - 1;
    } else {
        s.min_value = -book.cb_off;
        s.max_value = book.cb_mod - 1 - book.cb_off;
    }
    return s;
}

const std::array<CodebookShape, kCodebooks>& shapes() {
    static const std::array<CodebookShape, kCodebooks> kShapes = [] {
        std::array<CodebookShape, kCodebooks> all{};
        for (int cb = 1; cb < kCodebooks; ++cb) {
            all[static_cast<std::size_t>(cb)] = shape(cb);
        }
        return all;
    }();
    return kShapes;
}

// Pseudocode 19 in reverse: the index of the codeword for `dim` values.
[[nodiscard]] std::size_t codeword_index(const CodebookShape& s, int cb, std::span<const std::int32_t> values) {
    std::array<int, 4> v{};
    for (std::size_t i = 0; i < values.size(); ++i) {
        int x = values[i];
        if (s.is_unsigned) {
            x = std::abs(x);
            if (cb == 11) {
                x = std::min(x, kEscape);
            }
        }
        v[i] = x + s.off;
    }
    if (s.dim == 4) {
        return static_cast<std::size_t>(v[0] * s.mod3 + v[1] * s.mod2 + v[2] * s.mod + v[3]);
    }
    return static_cast<std::size_t>(v[0] * s.mod + v[1]);
}

// Pseudocode 20 in reverse: N_ext leading ones and a zero, then N_ext + 4 bits
// of the magnitude less 2^(N_ext + 4).
struct ExtCode {
    unsigned bits = 0;
    std::uint64_t raw = 0;
};

[[nodiscard]] ExtCode ext_code(std::int32_t magnitude) {
    int n_ext = 0;
    while ((1 << (n_ext + 5)) <= magnitude) {
        ++n_ext;
    }
    ExtCode code;
    const auto ones = static_cast<unsigned>(n_ext);
    const std::uint64_t prefix = ((std::uint64_t{1} << ones) - 1) << 1U;  // n_ext ones, then a zero
    const auto payload = static_cast<unsigned>(n_ext + 4);
    const auto value = static_cast<std::uint64_t>(magnitude - (1 << (n_ext + 4)));
    code.bits = ones + 1 + payload;
    code.raw = (prefix << payload) | value;
    return code;
}

constexpr std::size_t kNever = std::numeric_limits<std::size_t>::max() / 4;

// Codebooks that share a shape (dimension, signedness and range: Tables A.2 to
// A.12 pair them up) index their codewords alike and differ only in the
// codeword lengths, so a band is walked once for both. `bits` holds the two
// books' lengths side by side, by index.
struct PriceGroup {
    int limit = 0;  // the largest magnitude a book of the group codes directly
    int mod = 0, mod2 = 0, mod3 = 0;
    int base = 0;   // what cb_off adds to the index
    std::array<std::array<std::uint8_t, 2>, 289> bits{};
};

// Signed quads (books 1, 2), unsigned quads (3, 4), signed pairs (5, 6),
// unsigned pairs (7, 8) and (9, 10), then book 11 on its own.
struct Pricing {
    std::array<PriceGroup, 6> group{};
};

[[nodiscard]] Pricing build_pricing() {
    Pricing p;
    const std::array<std::array<int, 2>, 6> books{{{1, 2}, {3, 4}, {5, 6}, {7, 8}, {9, 10}, {11, 11}}};
    for (std::size_t i = 0; i < books.size(); ++i) {
        PriceGroup& g = p.group[i];
        const CodebookShape& s = shapes()[static_cast<std::size_t>(books[i][0])];
        g.limit = books[i][0] == 11 ? std::numeric_limits<int>::max() : s.max_value;
        g.mod = s.mod;
        g.mod2 = s.mod2;
        g.mod3 = s.mod3;
        g.base = s.dim == 4 ? s.off * (s.mod3 + s.mod2 + s.mod + 1) : s.off * (s.mod + 1);
        for (std::size_t j = 0; j < 2; ++j) {
            const std::span<const HuffCode> codes =
                tables::kAsfSpectrumCodes[static_cast<std::size_t>(books[i][j])];
            for (std::size_t k = 0; k < codes.size() && k < g.bits.size(); ++k) {
                g.bits[k][j] = codes[k].bits;
            }
        }
    }
    return p;
}

[[nodiscard]] const Pricing& pricing() {
    static const Pricing kPricing = build_pricing();
    return kPricing;
}

// The two quad groups' lengths over `quads` tuples, and how many lines are not
// zero (a sign bit each, in the unsigned books).
template <bool WithSigned>
void price_quads(const std::int32_t* d, std::size_t quads, const Pricing& p, std::array<std::uint32_t, 4>& acc,
                 std::uint32_t& nonzero) {
    const PriceGroup& sg = p.group[0];
    const PriceGroup& ug = p.group[1];
    for (std::size_t i = 0; i < quads; ++i, d += 4) {
        const int a0 = std::abs(d[0]);
        const int a1 = std::abs(d[1]);
        const int a2 = std::abs(d[2]);
        const int a3 = std::abs(d[3]);
        const auto ui = static_cast<std::size_t>(a0 * ug.mod3 + a1 * ug.mod2 + a2 * ug.mod + a3 + ug.base);
        acc[2] += ug.bits[ui][0];
        acc[3] += ug.bits[ui][1];
        nonzero += static_cast<std::uint32_t>((a0 != 0) + (a1 != 0) + (a2 != 0) + (a3 != 0));
        if constexpr (WithSigned) {
            const auto si = static_cast<std::size_t>(d[0] * sg.mod3 + d[1] * sg.mod2 + d[2] * sg.mod + d[3] + sg.base);
            acc[0] += sg.bits[si][0];
            acc[1] += sg.bits[si][1];
        }
    }
}

// The pair groups' lengths: acc[0], [1] books 5 and 6, [2], [3] books 7 and 8,
// [4], [5] books 9 and 10, [6] book 11 (which codes magnitudes past 16 as 16
// and an escape). Only the groups the band fits are read.
template <bool Signed, bool Group78, bool Group910>
void price_pairs(const std::int32_t* d, std::size_t pairs, const Pricing& p, std::array<std::uint32_t, 7>& acc,
                 std::uint32_t& nonzero) {
    const PriceGroup& sg = p.group[2];
    const PriceGroup& g78 = p.group[3];
    const PriceGroup& g910 = p.group[4];
    const PriceGroup& g11 = p.group[5];
    for (std::size_t i = 0; i < pairs; ++i, d += 2) {
        const int a0 = std::abs(d[0]);
        const int a1 = std::abs(d[1]);
        nonzero += static_cast<std::uint32_t>((a0 != 0) + (a1 != 0));
        acc[6] += g11.bits[static_cast<std::size_t>(std::min(a0, kEscape) * g11.mod + std::min(a1, kEscape))][0];
        if constexpr (Group910) {
            const auto index = static_cast<std::size_t>(a0 * g910.mod + a1);
            acc[4] += g910.bits[index][0];
            acc[5] += g910.bits[index][1];
        }
        if constexpr (Group78) {
            const auto index = static_cast<std::size_t>(a0 * g78.mod + a1);
            acc[2] += g78.bits[index][0];
            acc[3] += g78.bits[index][1];
        }
        if constexpr (Signed) {
            const auto index = static_cast<std::size_t>(d[0] * sg.mod + d[1] + sg.base);
            acc[0] += sg.bits[index][0];
            acc[1] += sg.bits[index][1];
        }
    }
}

// The bits of one band's lines in every codebook the band fits (kNever in the
// others; codebook 0 for a band of zeros): codewords, sign bits and escapes.
void price_band(std::span<const std::int32_t> q, std::int32_t max_abs, const Pricing& p,
                std::array<std::size_t, kCodebooks>& cost) {
    cost.fill(kNever);
    if (max_abs == 0) {
        cost[0] = 0;
    }
    const std::int32_t* d = q.data();
    if (max_abs <= p.group[1].limit) {
        std::array<std::uint32_t, 4> acc{};
        std::uint32_t nonzero = 0;
        if (max_abs <= p.group[0].limit) {
            price_quads<true>(d, q.size() / 4, p, acc, nonzero);
            cost[1] = acc[0];
            cost[2] = acc[1];
        } else {
            price_quads<false>(d, q.size() / 4, p, acc, nonzero);
        }
        cost[3] = acc[2] + nonzero;
        cost[4] = acc[3] + nonzero;
    }
    std::array<std::uint32_t, 7> acc{};
    std::uint32_t nonzero = 0;
    const std::size_t pairs = q.size() / 2;
    if (max_abs <= p.group[2].limit) {
        price_pairs<true, true, true>(d, pairs, p, acc, nonzero);
        cost[5] = acc[0];
        cost[6] = acc[1];
    } else if (max_abs <= p.group[3].limit) {
        price_pairs<false, true, true>(d, pairs, p, acc, nonzero);
    } else if (max_abs <= p.group[4].limit) {
        price_pairs<false, false, true>(d, pairs, p, acc, nonzero);
    } else {
        price_pairs<false, false, false>(d, pairs, p, acc, nonzero);
    }
    if (max_abs <= p.group[3].limit) {
        cost[7] = acc[2] + nonzero;
        cost[8] = acc[3] + nonzero;
    }
    if (max_abs <= p.group[4].limit) {
        cost[9] = acc[4] + nonzero;
        cost[10] = acc[5] + nonzero;
    }
    std::size_t bits = acc[6] + nonzero;
    if (max_abs >= kEscape) {
        for (std::size_t k = 0; k < pairs * 2; ++k) {
            if (std::abs(d[k]) >= kEscape) {
                bits += ext_code(std::abs(d[k])).bits;
            }
        }
    }
    cost[11] = bits;
}

// How far the fast power may sit from an integer boundary before the exact one
// decides. sqrt(y sqrt y) is within a few ulp (some 1e-12 at the top of the
// range) of std::pow(y, 0.75), so a result more than this from a boundary
// floors to the same integer either way.
constexpr double kBoundaryMargin = 1e-9;
constexpr double kFastLimit = 8000.0;

[[nodiscard]] std::int32_t quantize_line(double x, double inverse_gain) {
    const double y = std::abs(x) * inverse_gain;
    const double fast = std::sqrt(y * std::sqrt(y)) + kRounding;
    std::int32_t magnitude = -1;
    if (fast < kFastLimit) {
        const auto whole = static_cast<std::int32_t>(fast);
        const double fraction = fast - static_cast<double>(whole);
        if (fraction > kBoundaryMargin && fraction < 1.0 - kBoundaryMargin) {
            magnitude = whole;
        }
    }
    if (magnitude < 0) {  // too close to call, past the range, or not a number
        const double scaled = std::pow(y, 0.75) + kRounding;
        magnitude = static_cast<std::int32_t>(std::min(scaled, static_cast<double>(kMaxQuant)));
    }
    return x < 0.0 ? -magnitude : magnitude;
}

[[nodiscard]] std::int32_t quantize_band(std::span<const double> lines, int sf, std::span<std::int32_t> out) {
    const double inverse_gain = std::pow(2.0, -0.25 * static_cast<double>(sf - 100));
    std::int32_t max_abs = 0;
    for (std::size_t k = 0; k < lines.size(); ++k) {
        out[k] = quantize_line(lines[k], inverse_gain);
        max_abs = std::max(max_abs, std::abs(out[k]));
    }
    return max_abs;
}

// Buffers code_track reuses from one call to the next on a thread.
static_assert(std::tuple_size_v<decltype(CoderScratch::cost)::value_type> == kCodebooks);

[[nodiscard]] CoderScratch& scratch_buffers() {
    thread_local CoderScratch scratch;
    return scratch;
}

// A band's mean square as the decoder reconstructs it: sign(q) |q|^(4/3) at
// the band's scale factor's gain (clause 5.1.3), the measure Pseudocodes 22 and
// 23 take of scaled_spec.
[[nodiscard]] double reconstructed_mean_square(std::span<const std::int32_t> q, int sf) {
    const double gain = std::pow(2.0, 0.25 * static_cast<double>(sf - 100));
    double sum = 0.0;
    for (const std::int32_t x : q) {
        const double line = std::pow(static_cast<double>(std::abs(x)), 4.0 / 3.0) * gain;
        sum += line * line;
    }
    return sum / static_cast<double>(q.size());
}

}  // namespace

std::span<const std::uint16_t> band_offsets(int transform_length) {
    return tables::sfb_offsets_48(transform_length);
}

Grouped regroup(std::span<const double> spectrum, const FrameLayout& layout, std::array<int, 2> max_sfb) {
    Grouped out;
    regroup_into(out, spectrum, layout, max_sfb);
    return out;
}

void regroup_into(Grouped& out, std::span<const double> spectrum, const FrameLayout& layout,
                  std::array<int, 2> max_sfb) {
    out.lines.clear();
    out.max_sfb.clear();
    out.offset.resize(layout.group_windows.size());
    std::size_t total = 0;
    for (const int length : layout.window_length) {
        total += static_cast<std::size_t>(length);
    }
    out.lines.reserve(total);
    std::size_t base = 0;
    std::size_t window = 0;
    for (std::size_t g = 0; g < layout.group_windows.size(); ++g) {
        const auto windows = static_cast<std::size_t>(layout.group_windows[g]);
        const std::span<const std::uint16_t> bands = band_offsets(layout.group_length[g]);
        const int available = std::max(0, static_cast<int>(bands.size()) - 1);
        const int limit = std::clamp(max_sfb[static_cast<std::size_t>(layout.group_half[g])], 0, available);
        out.max_sfb.push_back(limit);
        std::vector<std::size_t>& offsets = out.offset[g];
        offsets.resize(static_cast<std::size_t>(limit) + 1);
        for (std::size_t b = 0; b < offsets.size(); ++b) {
            offsets[b] = base + static_cast<std::size_t>(bands[b]) * windows;
        }
        // Where each of the group's windows starts in the spectrum.
        std::size_t first_start = 0;
        for (std::size_t w = 0; w < window; ++w) {
            first_start += static_cast<std::size_t>(layout.window_length[w]);
        }
        for (int b = 0; b < limit; ++b) {
            const auto bi = static_cast<std::size_t>(b);
            std::size_t first = first_start;
            for (std::size_t w = 0; w < windows; ++w) {
                for (std::size_t l = bands[bi]; l < bands[bi + 1]; ++l) {
                    out.lines.push_back(spectrum[first + l]);
                }
                first += static_cast<std::size_t>(layout.window_length[window + w]);
            }
        }
        base = offsets[static_cast<std::size_t>(limit)];
        window += windows;
    }
}

void TrackCoder::analyse(CoderScratch& search, std::vector<std::int32_t>* q_out, const Grouped& grouped,
                         const std::vector<std::vector<int>>& sf, int offset, const FrameLayout& layout,
                         bool noise_fill) {
    const std::size_t groups = grouped.offset.size();
    // Only the track a caller keeps needs every quantised line at once; to
    // count bits, one band's are priced as soon as they are quantised.
    const bool full = q_out != nullptr;
    if (full) {
        // Room for the most lines a frame has, as regroup_into() left it in
        // `grouped`, so that the track's storage does not grow with the
        // frames that come to need more of it.
        q_out->reserve(grouped.lines.capacity());
        q_out->assign(grouped.lines.size(), 0);
    }
    band_base_.assign(groups + 1, 0);
    for (std::size_t g = 0; g < groups; ++g) {
        band_base_[g + 1] = band_base_[g] + static_cast<std::size_t>(grouped.max_sfb[g]);
    }
    const std::size_t total_bands = band_base_[groups];
    sf_.assign(total_bands, 0);
    peak_.assign(total_bands, 0);
    cb_.assign(total_bands, 0);
    if (noise_fill) {
        level_.assign(total_bands, 0.0);
    }
    sections_.clear();
    section_base_.assign(groups + 1, 0);
    section_bits_ = 0;
    spectral_bits_ = 0;
    scalefac_bits_ = 0;
    noise_bits_ = 0;
    noise_fill_ = false;

    const Pricing& prices = pricing();
    CoderScratch& scratch = search;
    // Scale factors: each band's, offset and clamped, then held to the deltas
    // Table A.1 can send from one transmitted band to the next.
    bool first = true;
    int previous = 0;
    for (std::size_t g = 0; g < groups; ++g) {
        const int bands = grouped.max_sfb[g];
        const int width = section_bits_width(group_transf_index(layout, g));
        const auto count = static_cast<std::size_t>(bands);
        const std::size_t stride = count + 1;
        std::vector<std::array<std::size_t, kCodebooks>>& cost = scratch.cost;
        cost.resize(count);
        for (std::size_t b = 0; b < count; ++b) {
            const std::size_t begin = grouped.offset[g][b];
            const std::size_t end = grouped.offset[g][b + 1];
            const std::span<const double> lines = std::span<const double>(grouped.lines).subspan(begin, end - begin);
            if (!full && band_q_.size() < end - begin) {
                band_q_.resize(end - begin);
            }
            const std::span<std::int32_t> q = full ? std::span<std::int32_t>(*q_out).subspan(begin, end - begin)
                                                   : std::span<std::int32_t>(band_q_).first(end - begin);
            int s = std::clamp(sf[g][b] + offset, 0, 255);
            if (!first) {
                s = std::clamp(s, std::max(0, previous - kMaxDelta), std::min(255, previous + kMaxDelta));
            }
            std::int32_t peak = quantize_band(lines, s, q);
            // A line past what ext_code reaches clips; coarser steps avoid it
            // where the delta from the band before allows.
            while (peak >= kMaxQuant && s < 255 && (first || s < previous + kMaxDelta)) {
                ++s;
                peak = quantize_band(lines, s, q);
            }
            sf_[band_base_[g] + b] = s;
            peak_[band_base_[g] + b] = peak;
            if (peak > 0) {
                first = false;
                previous = s;
                if (noise_fill) {
                    // The level the decoder tracks from this band (it has energy).
                    level_[band_base_[g] + b] = kLog2E * std::log(reconstructed_mean_square(q, s));
                }
            }
            price_band(q, peak, prices, cost[b]);
        }
        // Codebooks and sections, by exact cost: each band is priced in every
        // codebook its values fit (above), then the cheapest division into
        // sections found, counting each section's header.
        // Per codebook, over the bands in order: prefix[cb][b] the cost of
        // bands [0, b) (the unpriced ones count nothing), and lowest[cb][e]
        // the first band from which a section of cb can run up to e, -1 where
        // band e - 1 is not priced.
        std::vector<std::size_t>& prefix = scratch.prefix;
        std::vector<int>& lowest = scratch.lowest;
        std::vector<std::size_t>& ahead = scratch.ahead;
        std::vector<std::size_t>& header = scratch.header;
        prefix.resize(kCodebooks * stride);
        lowest.resize(kCodebooks * stride);
        ahead.resize(kCodebooks * stride);
        header.resize(stride);
        for (std::size_t len = 1; len < stride; ++len) {
            header[len] = section_header_bits(static_cast<int>(len), width);
        }
        for (std::size_t cb = 0; cb < kCodebooks; ++cb) {
            std::size_t* p = prefix.data() + cb * stride;
            int* low = lowest.data() + cb * stride;
            p[0] = 0;
            low[0] = -1;
            for (std::size_t b = 0; b < count; ++b) {
                const std::size_t c = cost[b][cb];
                const bool priced = c < kNever;
                p[b + 1] = p[b] + (priced ? c : 0);
                low[b + 1] = !priced ? -1 : (low[b] >= 0 ? low[b] : static_cast<int>(b));
            }
        }
        // best[b]: the cheapest coding of bands [0, b); from[b] and book[b]
        // say where its last section started and which codebook it took.
        // A section [start, end) in cb costs best[start] + prefix[end] -
        // prefix[start] + header, so ahead[cb][start] holds the part that
        // does not depend on end. The candidates are met in the same order as
        // ever (end, then cb, then start downwards, a later one winning only
        // by being cheaper) so the same ones win ties.
        std::vector<std::size_t>& best = scratch.best;
        std::vector<int>& from = scratch.from;
        std::vector<int>& book = scratch.book;
        best.assign(stride, kNever);
        from.assign(stride, 0);
        book.assign(stride, 0);
        best[0] = 0;
        for (std::size_t end = 1; end <= count; ++end) {
            for (std::size_t cb = 0; cb < kCodebooks; ++cb) {
                ahead[cb * stride + end - 1] = best[end - 1] - prefix[cb * stride + end - 1];
            }
            std::size_t cheapest = kNever;
            int cheapest_start = 0;
            int cheapest_book = 0;
            for (std::size_t cb = 0; cb < kCodebooks; ++cb) {
                const int earliest = lowest[cb * stride + end];
                if (earliest < 0) {
                    continue;
                }
                const std::size_t at_end = prefix[cb * stride + end];
                const std::size_t* before = ahead.data() + cb * stride;
                for (std::size_t start = end; start-- > static_cast<std::size_t>(earliest);) {
                    const std::size_t total = before[start] + at_end + header[end - start];
                    if (total < cheapest) {
                        cheapest = total;
                        cheapest_start = static_cast<int>(start);
                        cheapest_book = static_cast<int>(cb);
                    }
                }
            }
            best[end] = cheapest;
            from[end] = cheapest_start;
            book[end] = cheapest_book;
        }
        // The sections from the last back, which is the order they are found in.
        const std::size_t first_section = sections_.size();
        for (int end = bands; end > 0; end = from[static_cast<std::size_t>(end)]) {
            const int start = from[static_cast<std::size_t>(end)];
            const int chosen = book[static_cast<std::size_t>(end)];
            section_bits_ += section_header_bits(end - start, width);
            for (int b = start; b < end; ++b) {
                cb_[band_base_[g] + static_cast<std::size_t>(b)] = chosen;
                spectral_bits_ += chosen == 0 ? 0 : cost[static_cast<std::size_t>(b)][static_cast<std::size_t>(chosen)];
            }
            if (full) {
                sections_.push_back(Section{chosen, start, end});
            }
        }
        std::reverse(sections_.begin() + static_cast<std::ptrdiff_t>(first_section), sections_.end());
        section_base_[g + 1] = sections_.size();
    }

    // asf_scalefac_data(): the reference, then a delta for each later band
    // that sends a scale factor.
    scalefac_bits_ = 8;
    bool found = false;
    int last = 0;
    for (std::size_t bi = 0; bi < total_bands; ++bi) {
        if (cb_[bi] == 0 || peak_[bi] == 0) {
            continue;
        }
        if (found) {
            const int index = sf_[bi] - last + kMaxDelta;
            scalefac_bits_ += tables::kAsfHcbScalefacCodes[static_cast<std::size_t>(index)].bits;
        }
        found = true;
        last = sf_[bi];
    }
    if (noise_fill) {
        plan_noise_fill(grouped);
    }
}

// Pseudocodes 22 and 23 run forwards over a coded track: the codeword each
// noise band sends, so that the decoder's noise comes out at the band's own
// level to the nearest 3 dB step. The level the decoder tracks (previous_rms)
// starts at that of the first band that has energy, moves to that of each band
// that has some, and by the delta of each noise band that is not the escape.
void TrackCoder::plan_noise_fill(const Grouped& grouped) {
    const std::size_t groups = grouped.offset.size();
    const std::size_t total_bands = band_base_[groups];
    const auto has_energy = [&](std::size_t bi) { return cb_[bi] != 0 && peak_[bi] > 0; };
    // The level of a band that has energy, measured as it was quantised.
    const auto level_of = [&](std::size_t g, std::size_t b) { return level_[band_base_[g] + b]; };

    double previous = 0.0;
    bool found = false;
    for (std::size_t g = 0; g < groups && !found; ++g) {
        for (std::size_t b = 0; b < static_cast<std::size_t>(grouped.max_sfb[g]); ++b) {
            if (has_energy(band_base_[g] + b)) {
                previous = level_of(g, b);
                found = true;
                break;
            }
        }
    }
    noise_fill_ = false;
    noise_bits_ = 0;
    if (!found) {
        return;  // no level to be relative to: every band would send the escape
    }
    dpcm_.assign(total_bands, -1);
    std::size_t bits = 0;
    bool any = false;
    for (std::size_t g = 0; g < groups; ++g) {
        for (std::size_t b = 0; b < static_cast<std::size_t>(grouped.max_sfb[g]); ++b) {
            if (has_energy(band_base_[g] + b)) {
                previous = level_of(g, b);
                continue;
            }
            int index = 0;  // the escape
            const std::size_t begin = grouped.offset[g][b];
            const std::size_t end = grouped.offset[g][b + 1];
            double sum = 0.0;
            for (std::size_t k = begin; k < end; ++k) {
                sum += grouped.lines[k] * grouped.lines[k];
            }
            const double mean_square = sum / static_cast<double>(end - begin);
            if (mean_square > 0.0) {
                const double wanted = kLog2E * std::log(mean_square);
                const long delta = std::lround(wanted - previous);
                if (delta >= kNoiseDeltaMin) {
                    const int step = static_cast<int>(std::min<long>(delta, kNoiseDeltaMax));
                    index = step + kNoiseOffset;
                    previous += static_cast<double>(step);
                    any = true;
                }
            }
            dpcm_[band_base_[g] + b] = index;
            bits += tables::kAsfHcbSnfCodes[static_cast<std::size_t>(index)].bits;
        }
    }
    if (any) {
        noise_fill_ = true;
        noise_bits_ = bits;
    }
}

void TrackCoder::fill(CodedTrack& out, const Grouped& grouped, bool noise_fill) const {
    const std::size_t groups = grouped.offset.size();
    out.offset = grouped.offset;
    out.cb.resize(groups);
    out.sf.resize(groups);
    out.max_abs.resize(groups);
    out.sections.resize(groups);
    for (std::size_t g = 0; g < groups; ++g) {
        const auto from = static_cast<std::ptrdiff_t>(band_base_[g]);
        const auto to = static_cast<std::ptrdiff_t>(band_base_[g + 1]);
        out.cb[g].assign(cb_.begin() + from, cb_.begin() + to);
        out.sf[g].assign(sf_.begin() + from, sf_.begin() + to);
        out.max_abs[g].assign(peak_.begin() + from, peak_.begin() + to);
        out.sections[g].assign(sections_.begin() + static_cast<std::ptrdiff_t>(section_base_[g]),
                               sections_.begin() + static_cast<std::ptrdiff_t>(section_base_[g + 1]));
    }
    out.noise_fill = noise_fill_;
    out.section_bits = section_bits_;
    out.spectral_bits = spectral_bits_;
    out.scalefac_bits = scalefac_bits_;
    out.noise_bits = noise_bits_;
    // A track that asked for noise fill has one entry per group, and per band
    // where it sends any; one that did not has none.
    if (!noise_fill) {
        out.dpcm_snf.clear();
        return;
    }
    out.dpcm_snf.resize(groups);
    for (std::size_t g = 0; g < groups; ++g) {
        if (noise_fill_) {
            out.dpcm_snf[g].assign(dpcm_.begin() + static_cast<std::ptrdiff_t>(band_base_[g]),
                                   dpcm_.begin() + static_cast<std::ptrdiff_t>(band_base_[g + 1]));
        } else {
            out.dpcm_snf[g].clear();
        }
    }
}

std::size_t TrackCoder::bits(const Grouped& grouped, const std::vector<std::vector<int>>& sf, int offset,
                             const FrameLayout& layout, bool noise_fill) {
    analyse(search_, nullptr, grouped, sf, offset, layout, noise_fill);
    return section_bits_ + spectral_bits_ + scalefac_bits_ + noise_bits_ + 1;
}

void TrackCoder::code(CodedTrack& out, const Grouped& grouped, const std::vector<std::vector<int>>& sf, int offset,
                      const FrameLayout& layout, bool noise_fill) {
    analyse(search_, &out.q, grouped, sf, offset, layout, noise_fill);
    fill(out, grouped, noise_fill);
}

CodedTrack code_track(const Grouped& grouped, const std::vector<std::vector<int>>& sf, int offset,
                      const FrameLayout& layout, bool noise_fill) {
    TrackCoder coder;
    CodedTrack t;
    coder.analyse(scratch_buffers(), &t.q, grouped, sf, offset, layout, noise_fill);
    coder.fill(t, grouped, noise_fill);
    return t;
}

std::size_t sf_info_bits(const FrameLayout& layout, std::array<int, 2> max_sfb) {
    BitWriter w;
    write_sf_info(w, layout, max_sfb);
    return w.bit_count();
}

namespace {

void write_sf_info(BitWriter& w, const FrameLayout& layout, std::array<int, 2> max_sfb,
                   const std::array<int, 2>* max_sfb_side) {
    // Table 37: from frame_len_base 1 536, b_long_frame and a transform length
    // per half; below it, one for the frame.
    if (layout.single) {
        w.write(2, static_cast<std::uint64_t>(layout.transf_length[0]), "transf_length");
    } else {
        w.write(1, layout.long_frame ? 1U : 0U, "b_long_frame");
        if (!layout.long_frame) {
            w.write(2, static_cast<std::uint64_t>(layout.transf_length[0]), "transf_length");
            w.write(2, static_cast<std::uint64_t>(layout.transf_length[1]), "transf_length");
        }
    }
    const int length0 = layout.window_length.front();
    // Table 38, without b_side_limited.
    const auto bits0 = static_cast<unsigned>(max_sfb_bits(length0));
    w.write(bits0, static_cast<std::uint64_t>(max_sfb[0]), "max_sfb");
    if (max_sfb_side != nullptr) {
        w.write(bits0, static_cast<std::uint64_t>((*max_sfb_side)[0]), "max_sfb_side");
    }
    if (layout.different_framing) {
        const auto bits1 = static_cast<unsigned>(max_sfb_bits(layout.window_length.back()));
        w.write(bits1, static_cast<std::uint64_t>(max_sfb[1]), "max_sfb");
        if (max_sfb_side != nullptr) {
            w.write(bits1, static_cast<std::uint64_t>((*max_sfb_side)[1]), "max_sfb_side");
        }
    }
    for (const std::uint8_t bit : layout.grouping_bits) {
        w.write(1, bit, "scale_factor_grouping_bit");
    }
}

}  // namespace

void write_sf_info(BitWriter& w, const FrameLayout& layout, std::array<int, 2> max_sfb) {
    write_sf_info(w, layout, max_sfb, nullptr);
}

void write_sf_info_dual(BitWriter& w, const FrameLayout& layout, std::array<int, 2> max_sfb,
                        std::array<int, 2> max_sfb_side) {
    write_sf_info(w, layout, max_sfb, &max_sfb_side);
}

void write_sf_data(BitWriter& w, const CodedTrack& track, const FrameLayout& layout) {
    const std::size_t groups = track.sections.size();
    // Table 39.
    for (std::size_t g = 0; g < groups; ++g) {
        const int width = section_bits_width(group_transf_index(layout, g));
        const int escape = (1 << width) - 1;
        for (const Section& section : track.sections[g]) {
            w.write(4, static_cast<std::uint64_t>(section.cb), "sect_cb");
            int remaining = section.end - section.start - 1;
            while (remaining >= escape) {
                w.write(static_cast<unsigned>(width), static_cast<std::uint64_t>(escape), "sect_len_incr");
                remaining -= escape;
            }
            w.write(static_cast<unsigned>(width), static_cast<std::uint64_t>(remaining), "sect_len_incr");
        }
    }
    // Table 40.
    for (std::size_t g = 0; g < groups; ++g) {
        for (const Section& section : track.sections[g]) {
            if (section.cb == 0) {
                continue;
            }
            const CodebookShape& s = shapes()[static_cast<std::size_t>(section.cb)];
            const std::span<const HuffCode> codes = tables::kAsfSpectrumCodes[static_cast<std::size_t>(section.cb)];
            const auto dim = static_cast<std::size_t>(s.dim);
            const std::size_t begin = track.offset[g][static_cast<std::size_t>(section.start)];
            const std::size_t end = track.offset[g][static_cast<std::size_t>(section.end)];
            for (std::size_t k = begin; k < end; k += dim) {
                const std::span<const std::int32_t> values = std::span<const std::int32_t>(track.q).subspan(k, dim);
                write_codeword(w, codes, codeword_index(s, section.cb, values), "asf_qspec_hcw");
                if (!s.is_unsigned) {
                    continue;
                }
                unsigned count = 0;
                std::uint64_t signs = 0;
                for (const std::int32_t x : values) {
                    if (x != 0) {
                        signs = (signs << 1U) | (x < 0 ? 1U : 0U);
                        ++count;
                    }
                }
                w.write(count, signs, dim == 4 ? "quad_sign_bits" : "pair_sign_bits");
                if (section.cb == 11) {
                    for (const std::int32_t x : values) {
                        if (std::abs(x) >= kEscape) {
                            const ExtCode code = ext_code(std::abs(x));
                            w.write_as(code.bits, code.raw, static_cast<std::uint64_t>(std::abs(x)), "ext_code");
                        }
                    }
                }
            }
        }
    }
    // Table 41: the first band that sends a scale factor sends it whole, as
    // reference_scale_factor, and each later one sends its delta.
    const auto transmitted = [&](std::size_t g, std::size_t b) {
        return track.cb[g][b] != 0 && track.max_abs[g][b] > 0;
    };
    int reference = 0;
    bool found = false;
    for (std::size_t g = 0; g < groups && !found; ++g) {
        for (std::size_t b = 0; b < track.cb[g].size(); ++b) {
            if (transmitted(g, b)) {
                reference = track.sf[g][b];
                found = true;
                break;
            }
        }
    }
    w.write(8, static_cast<std::uint64_t>(reference), "reference_scale_factor");
    int last = reference;
    bool first = true;
    for (std::size_t g = 0; g < groups; ++g) {
        for (std::size_t b = 0; b < track.cb[g].size(); ++b) {
            if (!transmitted(g, b)) {
                continue;
            }
            if (!first) {
                const auto index = static_cast<std::size_t>(track.sf[g][b] - last + kMaxDelta);
                write_codeword(w, tables::kAsfHcbScalefacCodes, index, "asf_sf_hcw");
            }
            first = false;
            last = track.sf[g][b];
        }
    }
    // Table 42: b_snf_data_exists, then a codeword for each band that has no
    // energy to send (codebook 0 or max_quant_idx 0), in the same order.
    w.write(1, track.noise_fill ? 1U : 0U, "b_snf_data_exists");
    if (!track.noise_fill) {
        return;
    }
    for (std::size_t g = 0; g < groups; ++g) {
        for (std::size_t b = 0; b < track.cb[g].size(); ++b) {
            if (track.cb[g][b] == 0 || track.max_abs[g][b] == 0) {
                write_codeword(w, tables::kAsfHcbSnfCodes, static_cast<std::size_t>(track.dpcm_snf[g][b]),
                               "asf_snf_hcw");
            }
        }
    }
}

}  // namespace iclforge::ac4::detail
