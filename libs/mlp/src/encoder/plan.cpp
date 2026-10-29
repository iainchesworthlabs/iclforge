#include "encoder/plan.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <string_view>

#include "encoder/analysis.hpp"

namespace iclforge::mlp::detail {

namespace {

constexpr double kMinus3dB = 0.70710678118654752;

// The restart sync word of substream 2 (P5).
constexpr std::uint16_t kSubstream2Sync = 0x31EB;

// Which side a speaker label is on, for the fold-downs: -1 left, 1 right, 0 centre, 2 LFE.
int side(std::string_view label) {
    if (label == "LFE" || label == "LFE2") {
        return 2;
    }
    if (label.size() > 1 && label[0] == 'L') {
        return -1;
    }
    if (label.size() > 1 && label[0] == 'R') {
        return 1;
    }
    if (label == "L") {
        return -1;
    }
    if (label == "R") {
        return 1;
    }
    return 0;
}

std::uint16_t default_assignment(int channels) {
    switch (channels) {
        case 1:
            return 0x002;  // C
        case 2:
            return kAssignStereo;
        case 3:
            return 0x003;  // L R C
        case 4:
            return 0x009;  // L R Ls Rs
        case 5:
            return 0x00B;  // L R C Ls Rs
        case 6:
            return kAssign51;
        case 7:
            return 0x08F;  // 5.1 + Cs
        default:
            return kAssign71;
    }
}

// floor(a / 2^14) for any sign.
constexpr std::int64_t floor14(std::int64_t a) noexcept {
    return a >> 14;
}

}  // namespace

std::expected<Plan, Error> make_plan(int channels, std::uint16_t assignment, bool downmix) {
    Plan p;
    p.channels = channels;
    const int bed = std::min(channels, 8);
    p.assignment = assignment != 0 ? assignment : default_assignment(bed);
    if (channel_count(p.assignment) != bed) {
        return std::unexpected(Error::kInvalidConfiguration);
    }
    const auto labels = speakers(p.assignment);
    auto add = [&](int min, int max, std::uint16_t sync, int matrices) {
        auto& s = p.sub[static_cast<std::size_t>(p.substreams++)];
        s.min_chan = min;
        s.max_chan = max;
        s.max_matrix_chan = max;
        s.sync_word = sync;
        s.max_matrices = matrices;
    };
    p.substreams = 0;
    // 10.3: substream 0 takes at most two primitive matrices, the 16-channel substream fifteen.
    add(0, std::min(channels, 2) - 1, kRestartSyncA, 2);
    if (channels > 2) {
        add(2, std::min(channels, 6) - 1, kRestartSyncA, 8);
    }
    if (channels > 6) {
        add(6, std::min(channels, 8) - 1, kSubstream2Sync, 8);
    }
    if (channels > 8) {
        add(8, channels - 1, kRestartSyncC, 15);
    }
    // format_info (5.1): FFmpeg writes 1 in both the six- and eight-channel assignments of a
    // stereo stream, and the 5.1 assignment in both for 5.1 (P25). A mono stream names its one
    // channel in both: FFmpeg's decoder checks a presentation's channel count against them.
    p.sixch_assignment =
        static_cast<std::uint8_t>((channels <= 6 ? p.assignment : kAssign51) & 0x1FU);
    p.eightch_assignment = p.assignment;
    // 6.4's substream_info: FFmpeg writes 0x14 with one substream and 0x3C with two. The values
    // for three and four are this encoder's (P34): bit 6 for the 8-channel presentation and bit 7
    // for the 16-channel one, which 6.4 names.
    static constexpr std::array<std::uint8_t, 4> kInfo{0x14, 0x3C, 0x7C, 0xFC};
    p.substream_info = kInfo[static_cast<std::size_t>(p.substreams - 1)];

    if (!downmix) {
        return p;
    }
    // Stage 1: everything past L and R into L and R (5.1 to stereo: C and the surrounds at
    // -3 dB, the LFE dropped; 10.5).
    if (channels > 2 && bed >= 3) {
        FoldStep left{0, {}};
        FoldStep right{1, {}};
        for (int ch = 2; ch < std::min(bed, 6); ++ch) {
            const int s = side(labels.labels[static_cast<std::size_t>(ch)]);
            if (s == 0 || s == -1) {
                left.gains[static_cast<std::size_t>(ch)] = kMinus3dB;
            }
            if (s == 0 || s == 1) {
                right.gains[static_cast<std::size_t>(ch)] = kMinus3dB;
            }
        }
        p.stages[1] = {left, right};
    }
    // Stage 2: the 7.1 rears into the 5.1 surrounds, -3 dB, side by side; a centre rear into C.
    if (channels > 6) {
        std::vector<FoldStep> steps;
        for (int target : {2, 4, 5}) {
            FoldStep f{target, {}};
            bool any = false;
            for (int ch = 6; ch < bed; ++ch) {
                const int s = side(labels.labels[static_cast<std::size_t>(ch)]);
                const bool match =
                    (target == 4 && s == -1) || (target == 5 && s == 1) || (target == 2 && s == 0);
                if (match) {
                    f.gains[static_cast<std::size_t>(ch)] = kMinus3dB;
                    any = true;
                }
            }
            if (any) {
                steps.push_back(f);
            }
        }
        p.stages[2] = std::move(steps);
    }
    // Stage 3 (16 channels into the 7.1 bed) has no nominal gains: an Atmos encode supplies the
    // objects' render (encoder/atmos), a channel-based one leaves the bed as it is.
    return p;
}

std::expected<void, Error> plan_interval(const Plan& plan, const EncoderConfig& config,
                                         const std::vector<std::vector<std::int32_t>>& input,
                                         int spa, std::span<const ObjectRender> renders,
                                         IntervalPlan& out) {
    const int channels = plan.channels;
    const std::size_t n = input.empty() ? 0 : input[0].size();
    out = IntervalPlan{};

    // The output shift: the trailing zeros every sample shares (a 16-bit source has 8), at most
    // 15 (max_shift is 4 bits).
    std::uint32_t any = 0;
    std::int64_t peak = 0;
    for (const auto& ch : input) {
        for (const std::int32_t v : ch) {
            any |= static_cast<std::uint32_t>(v);
            peak = std::max<std::int64_t>(peak, std::abs(std::int64_t{v}));
        }
    }
    const int zeros = any == 0 ? 0 : std::min(22, std::countr_zero(any));
    out.shift = std::min(zeros, 7);
    out.quant = std::min(zeros - out.shift, 15);
    const int q = out.quant;
    // A matrix output is rounded down to its channel's quantiser step (P21), so the encoder's
    // forward steps round alike.
    const auto rq = [q](std::int64_t v) { return (v >> q) << q; };
    out.max_bits = std::bit_width(static_cast<std::uint64_t>(peak));
    const std::int64_t limit = std::int64_t{1} << (23 - out.shift);

    std::vector<std::vector<std::int32_t>> level(static_cast<std::size_t>(channels),
                                                 std::vector<std::int32_t>(n));
    for (int c = 0; c < channels; ++c) {
        for (std::size_t i = 0; i < n; ++i) {
            level[static_cast<std::size_t>(c)][i] =
                input[static_cast<std::size_t>(c)][i] >> out.shift;
        }
    }

    // The presentations, top down, and the matrices that undo each fold.
    struct Undo {
        int target;
        std::array<std::int32_t, kMaxChannels> coeffs;  // Q14 gains on the sources
    };
    // undo[k][u]: the matrices that undo stage k in access unit u. Every stage's gains are the
    // same in every access unit except stage 3's in an Atmos stream, which follow the objects.
    const std::size_t units = spa > 0 ? n / static_cast<std::size_t>(spa) : 0;
    std::array<std::vector<std::vector<Undo>>, kMaxSubstreams> undo{};
    out.presentations[static_cast<std::size_t>(plan.substreams - 1)] = level;
    for (int k = plan.substreams - 1; k >= 1; --k) {
        const bool rendered = k == 3 && !renders.empty();
        auto steps_for = [&](std::size_t u) {
            if (!rendered) {
                return plan.stages[static_cast<std::size_t>(k)];
            }
            // An object's render into the bed: object o (channel 8 + o) into bed channel c.
            std::vector<FoldStep> steps;
            const auto& render = renders[std::min(u, renders.size() - 1)];
            for (int c = 0; c < 8; ++c) {
                FoldStep step{c, {}};
                for (int o = 0; o + 8 < channels; ++o) {
                    step.gains[static_cast<std::size_t>(8 + o)] =
                        render[static_cast<std::size_t>(o)][static_cast<std::size_t>(c)];
                }
                steps.push_back(step);
            }
            return steps;
        };
        std::vector<std::vector<Undo>> chosen;
        std::vector<std::vector<std::int32_t>> folded;
        for (const double f : {1.0, 0.5, 0.25, 0.0}) {
            chosen.assign(units, {});
            folded = level;
            bool fits = true;
            bool any_steps = false;
            for (std::size_t u = 0; u < units && fits; ++u) {
                const std::size_t first = u * static_cast<std::size_t>(spa);
                const std::size_t last = first + static_cast<std::size_t>(spa);
                for (const FoldStep& step : steps_for(u)) {
                    Undo un{step.target, {}};
                    bool nonzero = false;
                    for (int s = 0; s < channels; ++s) {
                        un.coeffs[static_cast<std::size_t>(s)] = static_cast<std::int32_t>(
                            std::lround(step.gains[static_cast<std::size_t>(s)] * f * 16384.0));
                        nonzero = nonzero || un.coeffs[static_cast<std::size_t>(s)] != 0;
                    }
                    any_steps = true;
                    if (!nonzero) {
                        continue;
                    }
                    auto& tgt = folded[static_cast<std::size_t>(step.target)];
                    for (std::size_t i = first; i < last && fits; ++i) {
                        std::int64_t acc = 0;
                        for (int s = 0; s < channels; ++s) {
                            acc += std::int64_t{un.coeffs[static_cast<std::size_t>(s)]} *
                                   level[static_cast<std::size_t>(s)][i];
                        }
                        const std::int64_t d = std::int64_t{tgt[i]} - rq(floor14(-acc));
                        if (d >= limit || d < -limit) {
                            fits = false;
                        }
                        tgt[i] = static_cast<std::int32_t>(d);
                    }
                    chosen[u].push_back(un);
                }
            }
            if (fits) {
                if (f < 1.0 && any_steps) {
                    out.limited = true;
                }
                break;
            }
        }
        undo[static_cast<std::size_t>(k)] = chosen;
        level = std::move(folded);
        const int mmc = plan.sub[static_cast<std::size_t>(k - 1)].max_matrix_chan;
        out.presentations[static_cast<std::size_t>(k - 1)].assign(level.begin(),
                                                                  level.begin() + mmc + 1);
    }

    // Decorrelation, on the fully folded channels.
    struct Dec {
        int target;
        int frac;
        std::array<std::int32_t, kMaxChannels> coded;  // in frac_bits + 2 bits
        double saving;
    };
    std::vector<Dec> decs;
    std::vector<std::vector<std::int32_t>> coded = level;
    if (config.decorrelate) {
        for (int j = 1; j < channels; ++j) {
            std::vector<std::span<const std::int32_t>> inputs;
            for (int i = 0; i < j; ++i) {
                inputs.emplace_back(level[static_cast<std::size_t>(i)]);
            }
            auto w = least_squares(inputs, level[static_cast<std::size_t>(j)]);
            if (!w) {
                continue;
            }
            double peak_w = 0.0;
            for (const double v : *w) {
                peak_w = std::max(peak_w, std::abs(v));
            }
            // frac_bits + 2 bits signed hold weights below 2 in magnitude at any frac_bits; a
            // heavier weight cannot be coded (10.2).
            if (peak_w >= 1.99) {
                continue;
            }
            int frac = 14;
            Dec d{j, frac, {}, 0.0};
            std::array<std::int32_t, kMaxChannels> eff{};
            bool nonzero = false;
            for (int i = 0; i < j; ++i) {
                d.coded[static_cast<std::size_t>(i)] = static_cast<std::int32_t>(
                    std::lround((*w)[static_cast<std::size_t>(i)] * std::ldexp(1.0, frac)));
                eff[static_cast<std::size_t>(i)] = d.coded[static_cast<std::size_t>(i)]
                                                   << (14 - frac);
                nonzero = nonzero || eff[static_cast<std::size_t>(i)] != 0;
            }
            if (!nonzero) {
                continue;
            }
            // The saving, judged on first differences, a stand-in for what prediction leaves.
            std::vector<std::int32_t> trial(n);
            double before = 0.0;
            double after = 0.0;
            bool fits = true;
            for (std::size_t s = 0; s < n; ++s) {
                std::int64_t acc = 0;
                for (int i = 0; i < j; ++i) {
                    acc += std::int64_t{eff[static_cast<std::size_t>(i)]} *
                           level[static_cast<std::size_t>(i)][s];
                }
                const std::int64_t c =
                    std::int64_t{level[static_cast<std::size_t>(j)][s]} - rq(floor14(acc));
                if (c >= (std::int64_t{1} << 23) || c < -(std::int64_t{1} << 23)) {
                    fits = false;
                    break;
                }
                trial[s] = static_cast<std::int32_t>(c);
                if (s > 0) {
                    const double db =
                        level[static_cast<std::size_t>(j)][s] -
                        static_cast<double>(level[static_cast<std::size_t>(j)][s - 1]);
                    const double da = static_cast<double>(trial[s]) - trial[s - 1];
                    before += db * db;
                    after += da * da;
                }
            }
            if (!fits || after >= before * 0.7) {
                continue;
            }
            d.saving = 0.5 * static_cast<double>(n) * std::log2((before + 1.0) / (after + 1.0));
            if (d.saving < 64.0) {
                continue;
            }
            decs.push_back(d);
        }
    }
    // The budgets: a substream's stack holds every decorrelation of its presentation's channels
    // and the undoing of each of its fold stages.
    std::sort(decs.begin(), decs.end(),
              [](const Dec& a, const Dec& b) { return a.saving > b.saving; });
    std::vector<Dec> kept;
    for (const Dec& d : decs) {
        bool ok = true;
        for (int s = plan.substream_of(d.target); s < plan.substreams; ++s) {
            int count = 1;
            for (const Dec& k : kept) {
                count += k.target <= plan.sub[static_cast<std::size_t>(s)].max_matrix_chan ? 1 : 0;
            }
            for (int k = 1; k <= s; ++k) {
                std::size_t most = 0;
                for (const auto& per_unit : undo[static_cast<std::size_t>(k)]) {
                    most = std::max(most, per_unit.size());
                }
                count += static_cast<int>(most);
            }
            if (count > plan.sub[static_cast<std::size_t>(s)].max_matrices) {
                ok = false;
            }
        }
        if (ok) {
            kept.push_back(d);
        }
    }
    std::sort(kept.begin(), kept.end(),
              [](const Dec& a, const Dec& b) { return a.target < b.target; });
    // Encoder order is the reverse of the decoder's: the highest channel first, each from the
    // channels below it as they stand (those are what the decoder has restored by then).
    for (auto it = kept.rbegin(); it != kept.rend(); ++it) {
        const Dec& d = *it;
        for (std::size_t s = 0; s < n; ++s) {
            std::int64_t acc = 0;
            for (int i = 0; i < d.target; ++i) {
                acc += std::int64_t{d.coded[static_cast<std::size_t>(i)] << (14 - d.frac)} *
                       level[static_cast<std::size_t>(i)][s];
            }
            coded[static_cast<std::size_t>(d.target)][s] = static_cast<std::int32_t>(
                level[static_cast<std::size_t>(d.target)][s] - rq(floor14(acc)));
        }
    }
    out.decorrelation_count = static_cast<int>(kept.size());
    out.coded = std::move(coded);

    // Each substream's stack, per access unit: the decorrelations of its channels in increasing
    // order, then the fold stages 1 to k as that access unit folded them.
    for (int s = 0; s < plan.substreams; ++s) {
        const int mmc = plan.sub[static_cast<std::size_t>(s)].max_matrix_chan;
        const int inputs_limit = plan.sub[static_cast<std::size_t>(s)].max_chan;
        auto& per_unit = out.stacks[static_cast<std::size_t>(s)];
        per_unit.assign(units, MatrixParams{});
        for (std::size_t u = 0; u < units; ++u) {
            MatrixParams& m = per_unit[u];
            auto push = [&](int target, int frac, auto coded_of) {
                PrimitiveMatrix p;
                p.output_channel = static_cast<std::uint8_t>(target);
                p.frac_bits = static_cast<std::uint8_t>(frac);
                for (int ch = 0; ch <= inputs_limit; ++ch) {
                    const std::int32_t v = ch == target ? (std::int32_t{1} << frac) : coded_of(ch);
                    if (v != 0) {
                        p.coeff_present |= 1U << ch;
                        p.coeffs[static_cast<std::size_t>(ch)] = v;
                    }
                }
                m.matrices[m.count++] = p;
            };
            for (const Dec& d : kept) {
                if (d.target <= mmc) {
                    push(d.target, d.frac, [&](int ch) {
                        return ch < d.target ? d.coded[static_cast<std::size_t>(ch)] : 0;
                    });
                }
            }
            for (int k = 1; k <= s; ++k) {
                const auto& stage = undo[static_cast<std::size_t>(k)];
                if (u < stage.size()) {
                    for (const auto& un : stage[u]) {
                        push(un.target, 14,
                             [&](int ch) { return -un.coeffs[static_cast<std::size_t>(ch)]; });
                    }
                }
            }
        }
    }
    return {};
}

}  // namespace iclforge::mlp::detail
