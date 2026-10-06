#include "ac4dec_hsf.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <span>
#include <stdexcept>

#include "ac4/core/ac4_toc_writer.hpp"
#include "ac4dec_bits.hpp"
#include "ac4dec_printed_matrices.hpp"
#include "core/dsp/kbd.hpp"
#include "core/dsp/mdct.hpp"
#include "core/tables/huffman_tables.hpp"
#include "core/tables/sfb_tables.hpp"
#include "iclforge/ac4/encoder/encoder.hpp"

namespace ac4dec_test {
namespace {

namespace tables = iclforge::ac4::detail::tables;
namespace dsp = iclforge::ac4::detail::dsp;
using iclforge::ac4::detail::Codebook;

// Table 83's frame_len_base by frame_rate_index.
constexpr std::array<int, 14> kBaseLength = {1920, 1920, 2048, 1536, 1536, 960, 960,
                                             1024, 768,  768,  512,  384,  384, 2048};

// The quantised value the largest line of a band is brought near: well inside codebook 11's
// escapes (16 and up, to 8 191), so that they are used, and fine enough to hold a tone's level.
constexpr double kTargetQuantised = 100.0;
// Lines under this fraction of the frame's largest are not coded.
constexpr double kFloor = 1e-4;

// Table 109: the scale_factor_grouping bits of two half-frames' transf_length indices.
constexpr std::array<std::array<int, 4>, 4> kGroupingBits = {{
    {15, 10, 8, 7},
    {10, 7, 4, 3},
    {8, 4, 3, 1},
    {7, 3, 1, 1},
}};

// One window group of a frame: the windows that share its section data and scale factors, which
// are of one transform length, and the bands it codes.
struct Group {
    std::vector<int> windows;  // frame-local, ascending
    int length = 0;            // the windows' lines, at the stream's rate
    int half = 0;              // which max_sfb and max_sfb_ext_hsf it takes
    int transf_index = 4;      // the index asf_transform_info() gives its windows (4: long)
    int n48 = 0;  // num_sfb_48 of the transform at the base rate: where the extension's bands start
    int max_sfb = 0;      // the core's max_sfb: every band for a track, fewer for an LFE
    int bands = 0;        // the bands coded in all: max_sfb and the extension's
    int total_bands = 0;  // the transform's, at the stream's rate
    std::span<const std::uint16_t>
        offsets;  // the stream's rate's, which hold the base rate's first
};

struct FramePlan {
    int base = 0;
    int multiplier = 2;
    bool long_frame = true;
    int transf_length0 = -1;
    int transf_length1 = -1;
    bool different = false;
    std::vector<int> block_lengths;  // each window's, in order, at the stream's rate
    std::vector<Group> groups;
    std::vector<bool> grouping_bits;
    std::array<int, 2> core_bands{};  // max_sfb[0] and [1], the core's every band
    bool lfe = false;                 // the plan of an LFE's track
};

int n_sect_bits(const Group& g) {
    return g.transf_index <= 2 ? 3 : 5;
}

// n_msfb_bits (Table 106) of a transform of `length` lines at the base rate; the lengths here are
// all 96 lines and up.
int n_msfb_bits(int length) {
    if (length >= 384) {
        return 6;
    }
    if (length >= 192) {
        return 5;
    }
    return 4;
}

// ext_decode() (Pseudocode 20): N_ext ones, a zero, N_ext + 4 bits of the value past 2^(N_ext + 4).
void put_ext(BitWriter& w, int magnitude) {
    const int n_ext = std::bit_width(static_cast<unsigned>(magnitude)) - 1 - 4;
    for (int i = 0; i < n_ext; ++i) {
        w.flag(true);
    }
    w.flag(false);
    w.put(static_cast<std::uint64_t>(magnitude - (1 << (n_ext + 4))), n_ext + 4);
}

// One pair of quantised lines in codebook 11 (Table 40): the codeword of their magnitudes clipped
// to 16, the sign bits of the nonzero ones, then the escapes.
void put_pair(BitWriter& w, int q0, int q1) {
    const Codebook& cb = *tables::kAsfSpectrumCodebooks[11];
    const int m0 = std::abs(q0);
    const int m1 = std::abs(q1);
    w.code(cb, (std::min(m0, 16) + cb.cb_off) * cb.cb_mod + (std::min(m1, 16) + cb.cb_off));
    for (const int q : {q0, q1}) {
        if (q != 0) {
            w.flag(q < 0);
        }
    }
    for (const int m : {m0, m1}) {
        if (m >= 16) {
            put_ext(w, m);
        }
    }
}

struct Band {
    int cb = 0;  // 0 or 11
    int sf = 0;
};

// A group's lines in the order the syntax holds them: by band, then window, then line.
struct GroupLines {
    std::vector<double> lines;
    std::vector<Band> bands;
    std::vector<int> q;
};

std::size_t band_start(const Group& g, int band) {
    return static_cast<std::size_t>(g.offsets[static_cast<std::size_t>(band)]) * g.windows.size();
}

// Quantises a group: each band's scale factor puts its largest line near kTargetQuantised, kept
// within Table A.1's reach of the one before (`previous_sf`, carried across groups).
void quantise(const Group& g, GroupLines& out, double threshold, int& previous_sf) {
    out.bands.assign(static_cast<std::size_t>(g.bands), Band{});
    out.q.assign(band_start(g, g.bands), 0);
    for (int b = 0; b < g.bands; ++b) {
        const std::size_t lo = band_start(g, b);
        const std::size_t hi = band_start(g, b + 1);
        double peak = 0.0;
        for (std::size_t k = lo; k < hi; ++k) {
            peak = std::max(peak, std::abs(out.lines[k]));
        }
        if (peak <= threshold) {
            continue;
        }
        // 2^((sf - 100) / 4) times q^(4/3) is the line.
        int sf = static_cast<int>(
            std::lround(100.0 + 4.0 * std::log2(peak / std::pow(kTargetQuantised, 4.0 / 3.0))));
        sf = std::clamp(sf, 0, 255);
        if (previous_sf >= 0) {
            sf = std::clamp(sf, previous_sf - 60, previous_sf + 60);
        }
        const double gain = std::exp2((sf - 100) / 4.0);
        bool any = false;
        for (std::size_t k = lo; k < hi; ++k) {
            const double magnitude = std::pow(std::abs(out.lines[k]) / gain, 0.75);
            const int q = std::min(static_cast<int>(std::lround(magnitude)), 8191);
            out.q[k] = out.lines[k] < 0 ? -q : q;
            any = any || q != 0;
        }
        if (any) {
            out.bands[static_cast<std::size_t>(b)] = Band{11, sf};
            previous_sf = sf;
        }
    }
}

struct TrackCode {
    BitWriter core;  // sf_data() of Table 36
    BitWriter ext;   // sf_hsf_data() of Table 36a
};

struct Section {
    int cb;
    int start;
    int end;
};

std::vector<Section> sections_of(const GroupLines& lines) {
    std::vector<Section> sections;
    for (std::size_t b = 0; b < lines.bands.size(); ++b) {
        if (!sections.empty() && sections.back().cb == lines.bands[b].cb) {
            sections.back().end = static_cast<int>(b) + 1;
        } else {
            sections.push_back({lines.bands[b].cb, static_cast<int>(b), static_cast<int>(b) + 1});
        }
    }
    return sections;
}

// sf_data() and sf_hsf_data() of one track (Tables 39 to 42c), stage by stage as the syntax has
// them, each over every group: the sections over all bands whether core or extension (Table 39
// splits them at num_sfb_48 itself), then each part's spectral lines, scale factors and noise
// levels in its own substream.
TrackCode encode_track(const FramePlan& plan, const std::vector<GroupLines>& lines,
                       bool noise_fill) {
    TrackCode out;
    // asf_section_data()
    for (std::size_t g = 0; g < plan.groups.size(); ++g) {
        const Group& group = plan.groups[g];
        const int esc = (1 << n_sect_bits(group)) - 1;
        for (const Section& s : sections_of(lines[g])) {
            out.core.put(static_cast<std::uint64_t>(s.cb), 4);
            int remaining = s.end - s.start - 1;
            while (remaining >= esc) {
                out.core.put(static_cast<std::uint64_t>(esc), n_sect_bits(group));
                remaining -= esc;
            }
            out.core.put(static_cast<std::uint64_t>(remaining), n_sect_bits(group));
        }
    }
    // asf_spectral_data() and asf_hsf_spectral_data(): the core's part of every group first.
    for (const bool extension : {false, true}) {
        for (std::size_t g = 0; g < plan.groups.size(); ++g) {
            const Group& group = plan.groups[g];
            const std::size_t core_end = band_start(group, group.n48);
            BitWriter& to = extension ? out.ext : out.core;
            for (const Section& s : sections_of(lines[g])) {
                if (s.cb == 0) {
                    continue;
                }
                const std::size_t first = band_start(group, s.start);
                const std::size_t last = band_start(group, s.end);
                for (std::size_t k = first; k < last; k += 2) {
                    if ((k >= core_end) == extension) {
                        put_pair(to, lines[g].q[k], lines[g].q[k + 1]);
                    }
                }
            }
        }
    }
    // asf_scalefac_data() and asf_hsf_scalefac_data(): the first coded band's scale factor is
    // reference_scale_factor, each later one the difference from the one before, across the groups
    // and from the core's to the extension's.
    int first_sf = 0;
    bool found = false;
    for (const GroupLines& gl : lines) {
        for (const Band& band : gl.bands) {
            if (band.cb != 0 && !found) {
                first_sf = band.sf;
                found = true;
            }
        }
    }
    out.core.put(static_cast<std::uint64_t>(first_sf), 8);
    int previous = first_sf;
    bool first = true;
    for (const bool extension : {false, true}) {
        for (std::size_t g = 0; g < plan.groups.size(); ++g) {
            const Group& group = plan.groups[g];
            for (int b = 0; b < group.bands; ++b) {
                const Band& band = lines[g].bands[static_cast<std::size_t>(b)];
                if (band.cb == 0 || (b >= group.n48) != extension) {
                    continue;
                }
                if (!first) {
                    (extension ? out.ext : out.core)
                        .code(tables::kAsfHcbScalefac, band.sf - previous + 60);
                }
                first = false;
                previous = band.sf;
            }
        }
    }
    // asf_snf_data() and asf_hsf_snf_data(): the escape, no noise, for every band without lines.
    out.core.flag(noise_fill);
    if (noise_fill) {
        for (const bool extension : {false, true}) {
            for (std::size_t g = 0; g < plan.groups.size(); ++g) {
                const Group& group = plan.groups[g];
                for (int b = 0; b < group.bands; ++b) {
                    if (lines[g].bands[static_cast<std::size_t>(b)].cb == 0 &&
                        (b >= group.n48) == extension) {
                        (extension ? out.ext : out.core).code(tables::kAsfHcbSnf, 0);
                    }
                }
            }
        }
    }
    return out;
}

// asf_transform_info() and asf_psy_info() (Tables 37 and 38) with max_sfb the core's every band
// and scale_factor_grouping as the plan has it.
void put_sf_info(BitWriter& w, const FramePlan& plan) {
    if (plan.base >= 1536) {
        w.flag(plan.long_frame);
        if (!plan.long_frame) {
            w.put(static_cast<std::uint64_t>(plan.transf_length0), 2);
            w.put(static_cast<std::uint64_t>(plan.transf_length1), 2);
        }
    } else {
        w.put(static_cast<std::uint64_t>(plan.groups.front().transf_index), 2);
    }
    const int index0 = plan.long_frame ? 4 : plan.transf_length0;
    w.put(static_cast<std::uint64_t>(plan.core_bands[0]),
          n_msfb_bits(plan.long_frame ? plan.base : plan.base >> (4 - index0)));
    if (plan.different) {
        w.put(static_cast<std::uint64_t>(plan.core_bands[1]),
              n_msfb_bits(plan.base >> (4 - plan.transf_length1)));
    }
    for (const bool bit : plan.grouping_bits) {
        w.flag(bit);
    }
}

// metadata() of a substream of sus_ver 1 with nothing in it: 14 bits.
void put_metadata(BitWriter& w) {
    w.flag(false);  // b_more_basic_metadata
    w.flag(false);  // b_dialog
    w.flag(false);  // b_channels_classifier
    w.flag(false);  // b_event_probability
    w.put(1, 7);    // tools_metadata_size_value
    w.flag(false);  // b_more_bits
    w.flag(false);  // b_de_data_present
    w.flag(false);  // b_emdf_payloads_substream
}

std::vector<std::byte> presentation_substream(int dialnorm_bits) {
    BitWriter w;
    w.flag(false);                                        // b_additional_data
    w.put(static_cast<std::uint64_t>(dialnorm_bits), 7);  // dialnorm_bits
    w.flag(false);                                        // b_further_loudness_info
    w.put(1, 5);                                          // drc_metadata_size_value
    w.flag(false);                                        // b_more_bits
    w.flag(false);                                        // b_drc_present
    w.flag(false);                                        // b_associated
    w.align();
    return w.bytes();
}

// frame_rate_multiply_info() (Part 1 Table 7) and frame_rate_fractions_info() (Part 2
// clause 6.2.1.4), both with nothing multiplied or divided.
std::vector<bool> frame_rate_bits(int frame_rate_index) {
    switch (frame_rate_index) {
        case 0:
        case 1:
            return {false};  // b_multiplier
        case 7:
        case 8:
        case 9:
            return {false, false};  // b_multiplier, b_frame_rate_fraction
        case 5:
        case 6:
        case 10:
        case 11:
        case 12:
            return {false};  // b_frame_rate_fraction
        case 13:
            return {};
        default:
            throw std::invalid_argument(
                "the HSF builder has no frame_rate_multiply_info() for this index");
    }
}

double tone_phase(int channel) {
    return 0.4 + 1.1 * channel;
}

// Sample n of channel `channel`'s tone, at full scale 32 768.
double tone_sample(const HsfChannel& tone, int channel, double rate, long n) {
    if (tone.hz == 0.0) {
        return 0.0;
    }
    return tone.amplitude * 32768.0 *
           std::sin(2.0 * std::numbers::pi * tone.hz * static_cast<double>(n) / rate +
                    tone_phase(channel));
}

FramePlan make_plan(const HsfCase& c, int base, int multiplier) {
    FramePlan plan;
    plan.base = base;
    plan.multiplier = multiplier;
    const int full = base * multiplier;
    const auto make_group = [&](std::vector<int> windows, int length, int half, int transf_index) {
        Group g;
        g.windows = std::move(windows);
        g.length = length;
        g.half = half;
        g.transf_index = transf_index;
        g.n48 = tables::num_sfb_48(length / multiplier);
        g.max_sfb = g.n48;
        g.total_bands = multiplier == 2 ? tables::num_sfb_96(length) : tables::num_sfb_192(length);
        g.offsets =
            multiplier == 2 ? tables::sfb_offsets_96(length) : tables::sfb_offsets_192(length);
        if (g.offsets.empty() || g.n48 > 63) {
            throw std::invalid_argument("no band table for this length");
        }
        return g;
    };
    plan.long_frame = c.transf_length0 < 0;
    if (plan.long_frame || base < 1536) {
        const int index = base >= 1536 ? 4 : (base == 512 || base == 384 ? 2 : 3);
        plan.block_lengths = {full};
        plan.groups.push_back(make_group({0}, full, 0, index));
        plan.core_bands = {plan.groups[0].max_sfb, 0};
        return plan;
    }
    plan.transf_length0 = c.transf_length0;
    plan.transf_length1 = c.transf_length1 < 0 ? c.transf_length0 : c.transf_length1;
    plan.different = plan.transf_length0 != plan.transf_length1;
    const std::array<int, 2> index = {plan.transf_length0, plan.transf_length1};
    std::array<int, 2> count{};
    std::array<int, 2> length{};
    for (int h = 0; h < 2; ++h) {
        count[static_cast<std::size_t>(h)] = 1 << (3 - index[static_cast<std::size_t>(h)]);
        length[static_cast<std::size_t>(h)] =
            (base >> (4 - index[static_cast<std::size_t>(h)])) * multiplier;
        for (int w = 0; w < count[static_cast<std::size_t>(h)]; ++w) {
            plan.block_lengths.push_back(length[static_cast<std::size_t>(h)]);
        }
    }
    const int windows = count[0] + count[1];
    // The groups: runs of group_size windows, within each half where the framing differs.
    std::vector<int> group_of(static_cast<std::size_t>(windows));
    int next = 0;
    const int size = c.group_size > 0 ? c.group_size : windows;
    int run = 0;
    for (int w = 0; w < windows; ++w) {
        const bool half_boundary = plan.different && w == count[0];
        if (w == 0 || run == size || half_boundary) {
            ++next;
            run = 0;
        }
        group_of[static_cast<std::size_t>(w)] = next - 1;
        ++run;
    }
    for (int g = 0; g < next; ++g) {
        std::vector<int> members;
        for (int w = 0; w < windows; ++w) {
            if (group_of[static_cast<std::size_t>(w)] == g) {
                members.push_back(w);
            }
        }
        const int h = members.at(0) < count[0] ? 0 : 1;
        // Where the framing is the same a group may span the halves; its length is then the one.
        plan.groups.push_back(make_group(members, length[static_cast<std::size_t>(h)],
                                         plan.different ? h : 0,
                                         index[static_cast<std::size_t>(h)]));
    }
    // scale_factor_grouping: a bit for each pair of neighbouring windows but the pair across the
    // halves where the framing differs.
    for (int w = 0; w + 1 < windows; ++w) {
        if (plan.different && w + 1 == count[0]) {
            continue;
        }
        plan.grouping_bits.push_back(group_of[static_cast<std::size_t>(w)] ==
                                     group_of[static_cast<std::size_t>(w) + 1]);
    }
    if (static_cast<int>(plan.grouping_bits.size()) !=
        kGroupingBits[static_cast<std::size_t>(plan.transf_length0)]
                     [static_cast<std::size_t>(plan.transf_length1)]) {
        throw std::logic_error("scale_factor_grouping bits disagree with Table 109");
    }
    plan.core_bands = {tables::num_sfb_48(base >> (4 - plan.transf_length0)),
                       tables::num_sfb_48(base >> (4 - plan.transf_length1))};
    return plan;
}

using Windows = std::vector<std::vector<double>>;  // [window][line]
using Abcd = ac4dec_test::Abcd;

constexpr Abcd kIdentity = {1.0, 0.0, 0.0, 1.0};
constexpr Abcd kMidSide = {1.0, 1.0, 1.0, -1.0};

// The inverse of a square matrix, by Gauss-Jordan elimination with partial pivoting.
std::vector<std::vector<double>> inverse(std::vector<std::vector<double>> m) {
    const std::size_t n = m.size();
    std::vector<std::vector<double>> inv(n, std::vector<double>(n, 0.0));
    for (std::size_t i = 0; i < n; ++i) {
        inv[i][i] = 1.0;
    }
    for (std::size_t col = 0; col < n; ++col) {
        std::size_t pivot = col;
        for (std::size_t r = col + 1; r < n; ++r) {
            if (std::abs(m[r][col]) > std::abs(m[pivot][col])) {
                pivot = r;
            }
        }
        if (std::abs(m[pivot][col]) < 1e-12) {
            throw std::logic_error("a singular printed matrix");
        }
        std::swap(m[col], m[pivot]);
        std::swap(inv[col], inv[pivot]);
        const double d = m[col][col];
        for (std::size_t j = 0; j < n; ++j) {
            m[col][j] /= d;
            inv[col][j] /= d;
        }
        for (std::size_t r = 0; r < n; ++r) {
            if (r == col) {
                continue;
            }
            const double f = m[r][col];
            for (std::size_t j = 0; j < n; ++j) {
                m[r][j] -= f * m[col][j];
                inv[r][j] -= f * inv[col][j];
            }
        }
    }
    return inv;
}

// What one channel data element of the syntax is, and the channels its outputs are.
enum class Kind { kMono, kLfe, kStereoData, kTwoChannelData, kThree, kFour, kFive };

struct Element {
    Kind kind = Kind::kMono;
    std::vector<std::size_t> outs;  // the decoder's channel of each output O0, O1, ...
    bool stereo_proc = false;       // b_enable_mdct_stereo_proc of a pair
    int mode = 0;                   // the sap_mode of each of its chparam_info()
};

struct Script {
    std::vector<Element> elements;  // in syntax order, an LFE first
    bool lfe = false;
    // Table 183's steps of a 7.X element: pairs of channels (first, second) mixed after the
    // elements.
    std::vector<std::array<std::size_t, 2>> steps;
};

// The elements of a channel mode, with the channels as the decoder orders them.
Script script_of(const HsfCase& c) {
    Script s;
    const int mode = c.sap_mode;
    const bool proc = c.stereo_proc;
    const auto pair = [&](Kind kind, std::size_t a, std::size_t b) {
        s.elements.push_back({kind, {a, b}, proc, mode});
    };
    switch (c.ch_mode) {
        case 0:
            s.elements.push_back({Kind::kMono, {0}, false, 0});
            break;
        case 1:
            pair(Kind::kStereoData, 0, 1);
            break;
        case 2:  // 3.0: L R C
            if (c.coding_config == 1) {
                s.elements.push_back({Kind::kThree, {0, 1, 2}, true, mode});
            } else {
                pair(Kind::kStereoData, 0, 1);
                s.elements.push_back({Kind::kMono, {2}, false, 0});
            }
            break;
        case 3:  // 5.0: L R C Ls Rs
        case 4:  // 5.1: L R C LFE Ls Rs
        {
            const bool lfe = c.ch_mode == 4;
            const std::size_t ls = lfe ? 4 : 3;
            const std::size_t rs = lfe ? 5 : 4;
            if (lfe) {
                s.elements.push_back({Kind::kLfe, {3}, false, 0});
                s.lfe = true;
            }
            if (c.coding_config == 3) {
                s.elements.push_back({Kind::kFive, {0, 1, 2, ls, rs}, true, mode});
            } else {
                pair(Kind::kTwoChannelData, 0, 1);
                pair(Kind::kTwoChannelData, ls, rs);
                s.elements.push_back({Kind::kMono, {2}, false, 0});
            }
            break;
        }
        case 5:  // 7.0 3/4/0: L R C Ls Rs Lb Rb, coding_config 0
            pair(Kind::kTwoChannelData, 0, 1);
            pair(Kind::kTwoChannelData, 3, 4);
            pair(Kind::kTwoChannelData, 5, 6);  // the additional channels
            s.elements.push_back({Kind::kMono, {2}, false, 0});
            if (c.use_sap_add_ch) {
                s.steps = {{3, 5}, {4, 6}};
            }
            break;
        default:
            throw std::invalid_argument("the HSF builder has no such channel mode");
    }
    return s;
}

std::size_t channel_count(int ch_mode) {
    constexpr std::array<std::size_t, 6> kCounts = {1, 2, 3, 5, 6, 7};
    return kCounts.at(static_cast<std::size_t>(ch_mode));
}

// The matrix a channel data element applies to its tracks at a line: its chparam_info() parameters
// where the line is in the bands they cover (the core's), and where it is not the parameters Table
// 114 gives all bands (M/S in sap_mode 2).
Abcd params_at(int mode, bool covered) {
    if (covered) {
        return mode == 0 ? kIdentity : kMidSide;
    }
    return mode == 2 ? kMidSide : kIdentity;
}

std::vector<std::vector<double>> element_matrix(const Element& e, int chel_matsel, bool covered) {
    const Abcd p = params_at(e.mode, covered);
    switch (e.kind) {
        case Kind::kThree: {
            const std::array<Abcd, 2> sets = {p, p};
            return ac4dec_test::printed_matrix(
                ac4dec_test::kTable178[static_cast<std::size_t>(chel_matsel)], sets);
        }
        case Kind::kFive: {
            const std::array<Abcd, 5> sets = {p, p, p, p, p};
            return ac4dec_test::printed_matrix(
                ac4dec_test::kTable179[static_cast<std::size_t>(chel_matsel)], sets);
        }
        case Kind::kFour: {
            const std::array<Abcd, 4> sets = {p, p, p, p};
            return ac4dec_test::printed_matrix(ac4dec_test::kFourChannel, sets);
        }
        case Kind::kStereoData:
        case Kind::kTwoChannelData:
            if (e.stereo_proc) {
                return {{p[0], p[1]}, {p[2], p[3]}};
            }
            return {{1.0, 0.0}, {0.0, 1.0}};
        default:
            return {{1.0}};
    }
}

// The tracks an element codes for the given outputs: the outputs through the inverse of its matrix,
// line by line (its matrix differs between the lines its chparam_info() covers and those it does
// not).
std::vector<Windows> tracks_of(const Element& e, int chel_matsel,
                               const std::vector<const Windows*>& outputs,
                               const std::vector<int>& covered_lines) {
    const std::size_t n = e.outs.size();
    std::vector<Windows> tracks(n);
    for (std::size_t w = 0; w < outputs[0]->size(); ++w) {
        const std::size_t lines = (*outputs[0])[w].size();
        for (std::size_t t = 0; t < n; ++t) {
            tracks[t].emplace_back(lines);
        }
        const auto inside = inverse(element_matrix(e, chel_matsel, true));
        const auto outside = inverse(element_matrix(e, chel_matsel, false));
        for (std::size_t k = 0; k < lines; ++k) {
            const auto& inv = k < static_cast<std::size_t>(covered_lines[w]) ? inside : outside;
            for (std::size_t t = 0; t < n; ++t) {
                double sum = 0.0;
                for (std::size_t o = 0; o < n; ++o) {
                    sum += inv[t][o] * (*outputs[o])[w][k];
                }
                tracks[t][w][k] = sum;
            }
        }
    }
    return tracks;
}

// Table 183's step on two channels: with the step's sap_mode 2 it is the M/S matrix on the lines
// the chparam_info() covers and, by Table 114, on the rest; inverted, the channels before it.
void invert_step(int mode, Windows& first, Windows& second, const std::vector<int>& covered_lines) {
    if (mode == 0) {
        return;
    }
    for (std::size_t w = 0; w < first.size(); ++w) {
        for (std::size_t k = 0; k < first[w].size(); ++k) {
            const bool covered = k < static_cast<std::size_t>(covered_lines[w]);
            if (!(covered || mode == 2)) {
                continue;
            }
            const double a = first[w][k];
            const double b = second[w][k];
            first[w][k] = 0.5 * (a + b);
            second[w][k] = 0.5 * (a - b);
        }
    }
}

std::size_t n_msfbl_bits(int base) {
    return base >= 1536 ? 3 : 2;
}

}  // namespace

HsfStream build_hsf_stream(const HsfCase& c, int frames) {
    HsfStream stream;
    const int base = kBaseLength.at(static_cast<std::size_t>(c.frame_rate_index));
    const int multiplier = 2 << c.sf_multiplier;
    const int length = base * multiplier;
    stream.multiplier = multiplier;
    stream.frame_length = length;
    stream.sample_rate_hz = 48000.0 * multiplier;
    const std::size_t channels = channel_count(c.ch_mode);
    if (c.channels.size() != channels) {
        throw std::invalid_argument("one tone per channel");
    }
    const Script script = script_of(c);
    if (c.ch_mode >= 2 && (c.transf_length0 >= 0 || base < 1536)) {
        throw std::invalid_argument("the HSF builder switches blocks in mono and stereo alone");
    }
    FramePlan plan = make_plan(c, base, multiplier);
    const std::size_t per_frame = plan.block_lengths.size();

    // The blocks' places in time. Block j has `N_j` lines and its window of 2 N_j samples starts at
    // W_j; the centres of neighbouring windows are (N_j + N_j+1) / 2 apart, which is where the
    // overlap-add of Pseudocode 64 puts them. The decoder's output block j starts (full - N_j) / 2
    // before the window.
    const std::size_t blocks = per_frame * static_cast<std::size_t>(frames);
    const auto block_length = [&](std::size_t j) { return plan.block_lengths[j % per_frame]; };
    std::vector<long> window_start(blocks + 1);
    window_start.at(0) = -block_length(0);
    for (std::size_t j = 0; j < blocks; ++j) {
        window_start[j + 1] = window_start[j] + (3L * block_length(j) - block_length(j + 1)) / 2;
    }
    stream.origin = window_start[0] - (length - block_length(0)) / 2;

    // The forward transform of each block, with the window the decoder uses over it: the rising
    // part is KBD_LEFT of the shorter of the block and the one before, centred in the first half,
    // and the falling part that of the shorter of the block and the one after, in the second.
    std::vector<std::vector<std::vector<double>>> spectra(channels,
                                                          std::vector<std::vector<double>>(blocks));
    for (std::size_t j = 0; j < blocks; ++j) {
        const int n = block_length(j);
        const int n_before = j == 0 ? n : block_length(j - 1);
        const int n_after = block_length(j + 1);
        const int nw_left = std::min(n, n_before);
        const int nw_right = std::min(n, n_after);
        const std::vector<double> left =
            dsp::kbd_left(nw_left, dsp::kbd_alpha(nw_left, multiplier));
        const std::vector<double> right =
            dsp::kbd_left(nw_right, dsp::kbd_alpha(nw_right, multiplier));
        std::vector<double> window(2 * static_cast<std::size_t>(n), 0.0);
        const auto un = static_cast<std::size_t>(n);
        const auto skip_left = static_cast<std::size_t>(n - nw_left) / 2;
        const auto skip_right = static_cast<std::size_t>(n - nw_right) / 2;
        for (std::size_t i = 0; i < static_cast<std::size_t>(nw_left); ++i) {
            window[skip_left + i] = left[i];
        }
        for (std::size_t i = skip_left + static_cast<std::size_t>(nw_left); i < un; ++i) {
            window[i] = 1.0;
        }
        for (std::size_t i = 0; i < skip_right; ++i) {
            window[un + i] = 1.0;
        }
        for (std::size_t i = 0; i < static_cast<std::size_t>(nw_right); ++i) {
            window[un + skip_right + i] = right[static_cast<std::size_t>(nw_right) - 1 - i];
        }
        dsp::Mdct<double> mdct(un);
        for (std::size_t ch = 0; ch < channels; ++ch) {
            std::vector<double> block(window.size());
            for (std::size_t i = 0; i < block.size(); ++i) {
                block[i] = window[i] * tone_sample(c.channels[ch], static_cast<int>(ch),
                                                   stream.sample_rate_hz,
                                                   window_start[j] + static_cast<long>(i));
            }
            spectra[ch][j].resize(un);
            mdct.forward(block, spectra[ch][j]);
            for (double& v : spectra[ch][j]) {
                v *= 2.0;  // the round trip through the decoder's transform has a gain of 1/2
            }
        }
    }

    // The LFE's one group: max_sfb of a few bands (n_msfbl_bits, Table 106), all long block.
    constexpr int kLfeBands = 5;
    FramePlan lfe_plan;
    if (script.lfe) {
        lfe_plan = plan;
        lfe_plan.long_frame = true;
        lfe_plan.groups.assign(1, plan.groups.front());
        lfe_plan.groups.front().max_sfb = kLfeBands;
        lfe_plan.lfe = true;
    }

    for (int f = 0; f < frames; ++f) {
        // This frame's channels, by window.
        std::vector<Windows> channel_windows(channels);
        for (std::size_t ch = 0; ch < channels; ++ch) {
            for (std::size_t w = 0; w < per_frame; ++w) {
                channel_windows[ch].push_back(
                    spectra[ch][static_cast<std::size_t>(f) * per_frame + w]);
            }
        }
        std::vector<int> covered(per_frame);
        for (std::size_t w = 0; w < per_frame; ++w) {
            covered[w] = plan.block_lengths[w] /
                         multiplier;  // the lines of the bands chparam_info() reaches
        }
        // Table 183's steps come last in the decoder, so the channels the elements code are what
        // the steps' inverses make of the outputs.
        for (const auto& step : script.steps) {
            invert_step(c.sap_add_mode, channel_windows[step[0]], channel_windows[step[1]],
                        covered);
        }
        // Each element's tracks.
        struct Coded {
            Element element;
            std::vector<Windows> tracks;
        };
        std::vector<Coded> coded;
        for (const Element& e : script.elements) {
            std::vector<const Windows*> outs;
            for (const std::size_t ch : e.outs) {
                outs.push_back(&channel_windows[ch]);
            }
            coded.push_back({e, tracks_of(e, c.chel_matsel, outs, covered)});
        }
        double largest = 0.0;
        for (const Coded& e : coded) {
            for (const Windows& track : e.tracks) {
                for (const auto& window : track) {
                    for (const double v : window) {
                        largest = std::max(largest, std::abs(v));
                    }
                }
            }
        }
        const double threshold = kFloor * largest;
        const auto gather = [&](const Group& g, const Windows& track) {
            GroupLines out;
            for (std::size_t b = 0; b < static_cast<std::size_t>(g.total_bands); ++b) {
                for (const int w : g.windows) {
                    for (auto k = g.offsets[b]; k < g.offsets[b + 1]; ++k) {
                        out.lines.push_back(track[static_cast<std::size_t>(w)][k]);
                    }
                }
            }
            return out;
        };
        const auto plan_of = [&](const Coded& e) -> FramePlan& {
            return e.element.kind == Kind::kLfe ? lfe_plan : plan;
        };

        // How many extension bands each half needs: max_sfb_ext_hsf is one value for the substream.
        std::array<int, 2> ext_bands{};
        for (const Coded& e : coded) {
            for (const Windows& track : e.tracks) {
                int previous = -1;
                FramePlan& tp = plan_of(e);
                for (Group& g : tp.groups) {
                    g.bands = g.total_bands;
                    GroupLines gl = gather(g, track);
                    quantise(g, gl, threshold, previous);
                    for (int b = g.total_bands - 1; b >= g.n48; --b) {
                        if (gl.bands[static_cast<std::size_t>(b)].cb != 0) {
                            auto& v = ext_bands[static_cast<std::size_t>(g.half)];
                            v = std::max(v, b + 1 - g.n48);
                            break;
                        }
                    }
                }
            }
        }
        std::array<int, 2> header{};
        for (FramePlan* tp : {&plan, &lfe_plan}) {
            for (Group& g : tp->groups) {
                const int wanted =
                    c.ext_bands >= 0 ? c.ext_bands : ext_bands[static_cast<std::size_t>(g.half)];
                g.bands = g.max_sfb + wanted;
                if (g.bands > g.total_bands || wanted > 63) {
                    throw std::invalid_argument("more extension bands than the transform has");
                }
                if (tp == &plan) {
                    header[static_cast<std::size_t>(g.half)] = wanted;
                }
            }
        }
        if (script.lfe) {
            // The LFE's own sections cover max_sfb + the shared extension's bands.
            lfe_plan.groups.front().bands = kLfeBands + header[0];
        }

        // Every track's code, element by element.
        std::vector<std::vector<TrackCode>> codes;
        for (const Coded& e : coded) {
            codes.emplace_back();
            for (const Windows& track : e.tracks) {
                const FramePlan& tp = plan_of(e);
                std::vector<GroupLines> lines;
                int previous = -1;
                for (const Group& g : tp.groups) {
                    GroupLines gl = gather(g, track);
                    gl.lines.resize(band_start(g, g.bands));
                    quantise(g, gl, threshold, previous);
                    lines.push_back(std::move(gl));
                }
                codes.back().push_back(encode_track(tp, lines, c.noise_fill));
            }
        }

        // audio_data(): the element, then its channel data
        BitWriter audio;
        const auto put_chparam = [&](int mode, const FramePlan& tp) {
            audio.put(static_cast<std::uint64_t>(mode), 2);  // sap_mode
            if (mode == 1) {
                for (const Group& g : tp.groups) {
                    for (int b = 0; b < g.max_sfb; ++b) {
                        audio.flag(true);  // ms_used
                    }
                }
            }
        };
        switch (c.ch_mode) {
            case 0:
                audio.put(0, 1);  // mono_codec_mode: SIMPLE
                break;
            case 1:
                audio.put(0, 2);  // stereo_codec_mode
                break;
            case 2:
                audio.put(0, 1);  // 3_0_codec_mode
                audio.put(static_cast<std::uint64_t>(c.coding_config), 1);
                break;
            case 3:
            case 4:
                audio.put(0, 3);  // 5_X_codec_mode
                break;
            default:
                audio.put(0, 2);  // 7_X_codec_mode
                break;
        }
        const auto put_config = [&] {
            // coding_config of the 5.X and 7.X elements, after the LFE
            audio.put(static_cast<std::uint64_t>(c.ch_mode == 5 ? 0 : c.coding_config), 2);
            if (c.coding_config == 0 || c.ch_mode == 5) {
                audio.put(0, 1);  // 2ch_mode
            }
        };
        bool config_written = c.ch_mode < 3;
        for (std::size_t e = 0; e < coded.size(); ++e) {
            const Element& el = coded[e].element;
            const std::vector<TrackCode>& tc = codes[e];
            if (el.kind != Kind::kLfe && !config_written) {
                put_config();
                config_written = true;
            }
            switch (el.kind) {
                case Kind::kMono:
                    audio.put(0, 1);  // spec_frontend
                    put_sf_info(audio, plan);
                    audio.append(tc[0].core);
                    break;
                case Kind::kLfe: {
                    audio.put(static_cast<std::uint64_t>(kLfeBands),
                              static_cast<int>(n_msfbl_bits(base)));
                    audio.append(tc[0].core);
                    break;
                }
                case Kind::kStereoData:
                    audio.flag(el.stereo_proc);
                    if (el.stereo_proc) {
                        put_sf_info(audio, plan);
                        put_chparam(el.mode, plan);
                    } else {
                        audio.put(0, 1);
                        put_sf_info(audio, plan);
                        audio.put(0, 1);
                        put_sf_info(audio, plan);
                    }
                    audio.append(tc[0].core);
                    audio.append(tc[1].core);
                    break;
                case Kind::kTwoChannelData:
                    if (c.ch_mode == 5 && e == 2) {
                        // 7.X: b_use_sap_add_ch, then the steps' chparam_info(), before the
                        // additional pair
                        audio.flag(c.use_sap_add_ch);
                        if (c.use_sap_add_ch) {
                            put_chparam(c.sap_add_mode, plan);
                            put_chparam(c.sap_add_mode, plan);
                        }
                    }
                    audio.flag(el.stereo_proc);
                    put_sf_info(audio, plan);
                    if (el.stereo_proc) {
                        put_chparam(el.mode, plan);
                    } else {
                        put_sf_info(audio, plan);
                    }
                    audio.append(tc[0].core);
                    audio.append(tc[1].core);
                    break;
                case Kind::kThree:
                    put_sf_info(audio, plan);
                    audio.put(static_cast<std::uint64_t>(c.chel_matsel), 4);
                    put_chparam(el.mode, plan);
                    put_chparam(el.mode, plan);
                    for (const TrackCode& t : tc) {
                        audio.append(t.core);
                    }
                    break;
                case Kind::kFour:
                    break;
                case Kind::kFive:
                    put_sf_info(audio, plan);
                    audio.put(static_cast<std::uint64_t>(c.chel_matsel), 4);
                    for (int i = 0; i < 5; ++i) {
                        put_chparam(el.mode, plan);
                    }
                    for (const TrackCode& t : tc) {
                        audio.append(t.core);
                    }
                    break;
            }
        }
        const std::size_t audio_bytes = (audio.size() + 7) / 8;
        if (audio_bytes >= 32768) {
            throw std::invalid_argument("audio_size needs more than 15 bits");
        }
        BitWriter owner;
        owner.put(audio_bytes, 15);  // audio_size_value
        owner.flag(false);           // b_more_bits
        owner.append(audio);
        while (owner.size() < 16 + 8 * audio_bytes) {
            owner.flag(false);  // fill_bits
        }
        put_metadata(owner);
        owner.align();

        // ac4_hsf_ext_substream(): Table 17, each track's sf_hsf_data() in syntax order
        BitWriter ext;
        ext.put(static_cast<std::uint64_t>(header[0]), 6);  // max_sfb_ext_hsf[0]
        if (plan.different) {
            ext.put(static_cast<std::uint64_t>(header[1]), 6);  // max_sfb_ext_hsf[1]
        }
        for (const auto& element_codes : codes) {
            for (const TrackCode& code : element_codes) {
                ext.append(code.ext);
            }
        }
        ext.align();

        std::vector<std::vector<std::byte>> substreams = {owner.bytes(), ext.bytes(),
                                                          presentation_substream(c.dialnorm_bits)};
        ac4_toc_test::PresV1 p;
        p.presentation_substream = 2;
        for (const bool bit : frame_rate_bits(c.frame_rate_index)) {
            p.frame_rate_bits.push_back(bit);
        }
        ac4_toc_test::ChanInfo info;
        info.ch_mode = c.ch_mode;
        info.sf_multiplier = c.sf_multiplier;
        info.substream_index = 0;
        info.hsf_ext_substream_index = 1;
        BitWriter toc;
        ac4_toc_test::toc_start(toc, {.bitstream_version = 2,
                                      .sequence_counter = c.first_counter + f,
                                      .fs_index = 1,
                                      .frame_rate_index = c.frame_rate_index,
                                      .b_iframe_global = true});
        ac4_toc_test::presentation_v1(toc, p);
        ac4_toc_test::chan_group(toc, {info}, 1);
        ac4_toc_test::index_table(toc, ac4_toc_test::sizes_of(substreams));
        toc.align();
        stream.frames.push_back(ac4_toc_test::assemble(toc, substreams));
    }
    return stream;
}

std::vector<std::byte> hsf_sync_framed(const HsfStream& stream) {
    std::vector<std::byte> out;
    for (const auto& frame : stream.frames) {
        const std::vector<std::byte> framed = iclforge::ac4::sync_frame(frame, true);
        out.insert(out.end(), framed.begin(), framed.end());
    }
    return out;
}

std::vector<HsfCase> committed_hsf_cases() {
    std::vector<HsfCase> cases;
    const auto tone = [](double hz, double amplitude = 0.1) {
        return HsfChannel{.hz = hz, .amplitude = amplitude};
    };
    {
        HsfCase c;
        c.name = "mono-96-long";
        c.channels = {tone(30006.25)};
        cases.push_back(c);
    }
    {
        HsfCase c;
        c.name = "mono-96-24fps";
        c.frame_rate_index = 1;
        c.channels = {tone(41018.75)};
        cases.push_back(c);
    }
    {
        HsfCase c;
        c.name = "mono-192-switched-snf";
        c.sf_multiplier = 1;
        c.transf_length0 = 3;
        c.transf_length1 = 1;
        c.group_size = 2;
        c.noise_fill = true;
        c.channels = {tone(65010.0)};
        cases.push_back(c);
    }
    {
        HsfCase c;
        c.name = "stereo-96-sap2";
        c.ch_mode = 1;
        c.stereo_proc = true;
        c.sap_mode = 2;
        c.channels = {tone(9812.5), tone(37506.25, 0.05)};
        cases.push_back(c);
    }
    {
        HsfCase c;
        c.name = "stereo-192-sap1-switched";
        c.ch_mode = 1;
        c.sf_multiplier = 1;
        c.stereo_proc = true;
        c.sap_mode = 1;
        c.transf_length0 = 1;
        c.transf_length1 = 1;
        c.group_size = 2;
        c.channels = {tone(15012.5), tone(72531.25, 0.05)};
        cases.push_back(c);
    }
    {
        HsfCase c;
        c.name = "3_0-96-matsel5";
        c.ch_mode = 2;
        c.coding_config = 1;
        c.chel_matsel = 5;
        c.sap_mode = 2;
        c.channels = {tone(5006.25, 0.05), tone(17512.5, 0.06), tone(31018.75, 0.07)};
        cases.push_back(c);
    }
    {
        HsfCase c;
        c.name = "5_1-96-config3-matsel7";
        c.ch_mode = 4;
        c.coding_config = 3;
        c.chel_matsel = 7;
        c.sap_mode = 2;
        c.channels = {tone(5006.25, 0.04), tone(17512.5, 0.05), tone(31018.75, 0.06),
                      tone(87.5, 0.07),    tone(26025.0, 0.08), tone(38531.25, 0.09)};
        cases.push_back(c);
    }
    {
        HsfCase c;
        c.name = "7_0-192-add-ch";
        c.ch_mode = 5;
        c.sf_multiplier = 1;
        c.stereo_proc = true;
        c.sap_mode = 2;
        c.use_sap_add_ch = true;
        c.sap_add_mode = 2;
        c.channels = {tone(5006.25, 0.04), tone(17512.5, 0.05), tone(31018.75, 0.06),
                      tone(3750.0, 0.07),  tone(55025.0, 0.08), tone(71531.25, 0.09),
                      tone(12506.25, 0.1)};
        cases.push_back(c);
    }
    return cases;
}

std::vector<float> hsf_tone(const HsfCase& c, const HsfStream& stream, std::size_t channel,
                            long first, std::size_t count) {
    std::vector<float> out(count);
    for (std::size_t i = 0; i < count; ++i) {
        out[i] =
            static_cast<float>(tone_sample(c.channels.at(channel), static_cast<int>(channel),
                                           stream.sample_rate_hz, first + static_cast<long>(i)) /
                               32768.0);
    }
    return out;
}

}  // namespace ac4dec_test
