// The AC-4 encoder's spectral coder (libs/ac4/src/encoder/asf/coder.cpp) held to a plain
// transcription of what it did before it priced the codebooks of one shape together, kept
// its section search's running sums apart from the bands, and took the quantiser's
// 3/4 power from sqrt: the quantised lines (a std::pow of each), the cost of each band in
// each codebook it fits (one walk of the band per codebook) and the cheapest sections
// (three nested loops, cost summed as it goes). Pseudo-random bands, a fixed seed.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/tables/huffman_codes.hpp"
#include "core/tables/huffman_tables.hpp"
#include "encoder/asf/coder.hpp"
#include "encoder/asf/layout.hpp"

namespace {

using iclforge::ac4::detail::Codebook;
using iclforge::ac4::detail::CodedTrack;
using iclforge::ac4::detail::FrameLayout;
using iclforge::ac4::detail::Grouped;
using iclforge::ac4::detail::HuffCode;
using iclforge::ac4::detail::Section;
namespace tables = iclforge::ac4::detail::tables;

constexpr int kBooks = 12;
constexpr std::size_t kNever = std::numeric_limits<std::size_t>::max() / 4;
constexpr std::int32_t kMaxQuant = 8191;
constexpr int kEscape = 16;
constexpr double kRounding = 0.4054;

// ---- the reference: coder.cpp as it was, line for line where it matters ----

struct RefShape {
    int dim = 0;
    bool is_unsigned = false;
    int min_value = 0;
    int max_value = 0;
    int mod = 0, mod2 = 0, mod3 = 0, off = 0;
};

RefShape ref_shape(int cb) {
    const Codebook& book = *tables::kAsfSpectrumCodebooks[static_cast<std::size_t>(cb)];
    RefShape s;
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

bool ref_fits(const RefShape& s, int cb, std::int32_t max_abs) {
    if (cb == 11) {
        return true;
    }
    if (s.is_unsigned) {
        return max_abs <= s.max_value;
    }
    return max_abs <= s.max_value && -max_abs >= s.min_value;
}

std::size_t ref_index(const RefShape& s, int cb, std::span<const std::int32_t> values) {
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

unsigned ref_ext_bits(std::int32_t magnitude) {
    int n_ext = 0;
    while ((1 << (n_ext + 5)) <= magnitude) {
        ++n_ext;
    }
    return static_cast<unsigned>(n_ext) + 1 + static_cast<unsigned>(n_ext + 4);
}

std::size_t ref_band_cost(std::span<const std::int32_t> q, int cb) {
    const RefShape s = ref_shape(cb);
    const std::span<const HuffCode> codes = tables::kAsfSpectrumCodes[static_cast<std::size_t>(cb)];
    const auto dim = static_cast<std::size_t>(s.dim);
    std::size_t bits = 0;
    for (std::size_t k = 0; k + dim <= q.size(); k += dim) {
        const std::span<const std::int32_t> values = q.subspan(k, dim);
        bits += codes[ref_index(s, cb, values)].bits;
        if (s.is_unsigned) {
            for (const std::int32_t x : values) {
                bits += x != 0 ? 1U : 0U;
                if (cb == 11 && std::abs(x) >= kEscape) {
                    bits += ref_ext_bits(std::abs(x));
                }
            }
        }
    }
    return bits;
}

std::int32_t ref_quantize_line(double x, double inverse_gain) {
    const double scaled = std::pow(std::abs(x) * inverse_gain, 0.75) + kRounding;
    const auto magnitude = static_cast<std::int32_t>(std::min(scaled, static_cast<double>(kMaxQuant)));
    return x < 0.0 ? -magnitude : magnitude;
}

std::size_t ref_header_bits(int band_count, int width) {
    const int escape = (1 << width) - 1;
    const int increments = (band_count - 1) / escape + 1;
    return 4 + static_cast<std::size_t>(width * increments);
}

struct RefSections {
    std::vector<Section> sections;
    std::vector<int> cb;
    std::size_t section_bits = 0;
    std::size_t spectral_bits = 0;
};

// The cost table and the search over (end, codebook, start), as they were.
RefSections ref_sections(std::span<const std::int32_t> q, const std::vector<std::size_t>& offset,
                         const std::vector<std::int32_t>& max_abs, int width) {
    const int bands = static_cast<int>(max_abs.size());
    std::vector<std::array<std::size_t, kBooks>> cost(static_cast<std::size_t>(bands));
    for (int b = 0; b < bands; ++b) {
        const auto bi = static_cast<std::size_t>(b);
        const std::span<const std::int32_t> band = q.subspan(offset[bi], offset[bi + 1] - offset[bi]);
        for (int cb = 0; cb < kBooks; ++cb) {
            std::size_t c = kNever;
            if (cb == 0) {
                c = max_abs[bi] == 0 ? 0 : kNever;
            } else if (ref_fits(ref_shape(cb), cb, max_abs[bi])) {
                c = ref_band_cost(band, cb);
            }
            cost[bi][static_cast<std::size_t>(cb)] = c;
        }
    }
    std::vector<std::size_t> best(static_cast<std::size_t>(bands) + 1, kNever);
    std::vector<int> from(static_cast<std::size_t>(bands) + 1, 0);
    std::vector<int> book(static_cast<std::size_t>(bands) + 1, 0);
    best[0] = 0;
    for (int end = 1; end <= bands; ++end) {
        for (int cb = 0; cb < kBooks; ++cb) {
            std::size_t run = 0;
            for (int start = end - 1; start >= 0; --start) {
                const std::size_t c = cost[static_cast<std::size_t>(start)][static_cast<std::size_t>(cb)];
                if (c >= kNever) {
                    break;
                }
                run += c;
                const std::size_t total =
                    best[static_cast<std::size_t>(start)] + run + ref_header_bits(end - start, width);
                if (total < best[static_cast<std::size_t>(end)]) {
                    best[static_cast<std::size_t>(end)] = total;
                    from[static_cast<std::size_t>(end)] = start;
                    book[static_cast<std::size_t>(end)] = cb;
                }
            }
        }
    }
    RefSections out;
    for (int end = bands; end > 0; end = from[static_cast<std::size_t>(end)]) {
        out.sections.push_back(Section{book[static_cast<std::size_t>(end)], from[static_cast<std::size_t>(end)], end});
    }
    std::reverse(out.sections.begin(), out.sections.end());
    out.cb.assign(static_cast<std::size_t>(bands), 0);
    for (const Section& section : out.sections) {
        out.section_bits += ref_header_bits(section.end - section.start, width);
        for (int b = section.start; b < section.end; ++b) {
            out.cb[static_cast<std::size_t>(b)] = section.cb;
            out.spectral_bits +=
                section.cb == 0 ? 0 : cost[static_cast<std::size_t>(b)][static_cast<std::size_t>(section.cb)];
        }
    }
    return out;
}

// ---- the input: bands of every size of line, from silence to past the escape ----

class Random {
   public:
    explicit Random(std::uint64_t seed) : engine_(seed) {}
    double unit() { return static_cast<double>(engine_() >> 11U) * (1.0 / 9007199254740992.0); }
    double between(double low, double high) { return low + (high - low) * unit(); }
    int below(int n) { return static_cast<int>(engine_() % static_cast<std::uint64_t>(n)); }

   private:
    std::mt19937_64 engine_;
};

struct Coverage {
    std::array<int, 8> classes{};  // bands by max_abs: 0, 1, 2, 3-4, 5-7, 8-12, 13-15, 16 and up
    int escapes = 0;               // lines of 16 and up
    void note(std::int32_t max_abs) {
        const int cls = max_abs == 0 ? 0 : max_abs == 1 ? 1 : max_abs == 2 ? 2 : max_abs <= 4 ? 3
                        : max_abs <= 7 ? 4 : max_abs <= 12 ? 5 : max_abs <= 15 ? 6 : 7;
        ++classes[static_cast<std::size_t>(cls)];
    }
};

// One frame of lines in window order, `loudness` apart from band to band.
std::vector<double> random_spectrum(Random& rng, const FrameLayout& layout) {
    std::vector<double> lines;
    for (const int length : layout.window_length) {
        const std::span<const std::uint16_t> bands = iclforge::ac4::detail::band_offsets(length);
        const std::size_t start = lines.size();
        lines.resize(start + static_cast<std::size_t>(length), 0.0);
        for (std::size_t b = 0; b + 1 < bands.size(); ++b) {
            const int kind = rng.below(10);
            if (kind == 0) {
                continue;  // silence
            }
            const double loudness = std::pow(10.0, rng.between(-1.0, 5.0));
            const bool sparse = kind == 1;  // one loud line among zeros
            for (std::size_t l = bands[b]; l < bands[b + 1]; ++l) {
                double v = loudness * (2.0 * rng.unit() - 1.0);
                if (sparse && rng.below(8) != 0) {
                    v = 0.0;
                }
                lines[start + l] = v;
            }
        }
    }
    return lines;
}

void hold_to_reference(Random& rng, const FrameLayout& layout, Coverage& coverage, bool noise_fill) {
    const std::vector<double> spectrum = random_spectrum(rng, layout);
    const int max_sfb_a = 30 + rng.below(20);
    const Grouped grouped = iclforge::ac4::detail::regroup(spectrum, layout, {max_sfb_a, max_sfb_a});
    std::vector<std::vector<int>> sf(grouped.offset.size());
    for (std::size_t g = 0; g < sf.size(); ++g) {
        for (int b = 0; b < grouped.max_sfb[g]; ++b) {
            sf[g].push_back(40 + rng.below(100));
        }
    }
    const int offset = rng.below(9) - 4;
    const CodedTrack t = iclforge::ac4::detail::code_track(grouped, sf, offset, layout, noise_fill);

    std::size_t spectral = 0;
    std::size_t section_bits = 0;
    for (std::size_t g = 0; g < grouped.offset.size(); ++g) {
        const auto bands = static_cast<std::size_t>(grouped.max_sfb[g]);
        // The lines as std::pow quantises them, at the scale factors the coder settled on.
        std::vector<std::int32_t> max_abs(bands, 0);
        for (std::size_t b = 0; b < bands; ++b) {
            const double inverse_gain = std::pow(2.0, -0.25 * static_cast<double>(t.sf[g][b] - 100));
            for (std::size_t k = grouped.offset[g][b]; k < grouped.offset[g][b + 1]; ++k) {
                const std::int32_t expected = ref_quantize_line(grouped.lines[k], inverse_gain);
                if (t.q[k] != expected) {
                    CAPTURE(g, b, k, grouped.lines[k], t.sf[g][b]);
                    REQUIRE(t.q[k] == expected);
                }
                max_abs[b] = std::max(max_abs[b], std::abs(expected));
                coverage.escapes += std::abs(expected) >= kEscape ? 1 : 0;
            }
            coverage.note(max_abs[b]);
            CHECK(t.max_abs[g][b] == max_abs[b]);
        }
        const int index = iclforge::ac4::detail::group_transf_index(layout, g);
        const RefSections expected =
            ref_sections(t.q, grouped.offset[g], max_abs, index <= 2 ? 3 : 5);
        CAPTURE(g);
        REQUIRE(t.sections[g].size() == expected.sections.size());
        for (std::size_t i = 0; i < expected.sections.size(); ++i) {
            CAPTURE(i);
            CHECK(t.sections[g][i].cb == expected.sections[i].cb);
            CHECK(t.sections[g][i].start == expected.sections[i].start);
            CHECK(t.sections[g][i].end == expected.sections[i].end);
        }
        CHECK(t.cb[g] == expected.cb);
        spectral += expected.spectral_bits;
        section_bits += expected.section_bits;
    }
    CHECK(t.spectral_bits == spectral);
    CHECK(t.section_bits == section_bits);
}

}  // namespace

TEST_CASE("AC-4 spectral coder matches the per-codebook transcription on long frames", "[ac4][encoder][asf_coder]") {
    Random rng(0x41433443u);
    Coverage coverage;
    const FrameLayout layout = iclforge::ac4::detail::long_layout(1920);
    for (int frame = 0; frame < 160; ++frame) {
        hold_to_reference(rng, layout, coverage, frame % 3 == 0);
    }
    // The test means something only if it met every kind of band.
    for (std::size_t i = 0; i < coverage.classes.size(); ++i) {
        CAPTURE(i);
        CHECK(coverage.classes[i] >= 20);
    }
    CHECK(coverage.escapes > 100);
}

TEST_CASE("AC-4 spectral coder matches the per-codebook transcription on split frames", "[ac4][encoder][asf_coder]") {
    Random rng(0x53504c49u);
    Coverage coverage;
    const std::array<FrameLayout, 3> layouts{
        iclforge::ac4::detail::split_layout(1920, {2, 2}, {1, -1}),
        iclforge::ac4::detail::split_layout(1920, {1, 3}, {-1, -1}),
        iclforge::ac4::detail::split_layout(1920, {0, 2}, {3, 1}),
    };
    for (int frame = 0; frame < 150; ++frame) {
        hold_to_reference(rng, layouts[static_cast<std::size_t>(frame % 3)], coverage, false);
    }
    for (std::size_t i = 0; i < coverage.classes.size(); ++i) {
        CAPTURE(i);
        CHECK(coverage.classes[i] >= 20);
    }
}
