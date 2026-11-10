#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>

#include "iclforge/ac3/core/exponents.hpp"
#include "iclforge/ac3/core/tables.hpp"

// Exponent-run planning, shared by both encoders' §8.2.8 reuse-span decisions
// (AC-3's encoder.cpp and E-AC-3's eac3_frame.cpp).
//
// The strategy a chosen span earns under Table E2.10 is a spec rule and lives
// with the rest of the exponent pipeline (iclforge::ac3::strategy_for_span,
// core/exponents.hpp). What follows is the other half of the plan - WHERE the
// spans end - and that is a judgement about cost rather than anything the
// standard states, which is why it sits here in the encoder's own headers.
//
// Internal to libs/ac3/src/encoder/ on purpose, the same way snr_search.hpp
// is: plumbing between the two encoder translation units, not library surface.
// libs/ac3/tests/encoder/test_exp_strategy.cpp includes it directly.

namespace iclforge::ac3::internal {

// §8.2.8: "when the variation exceeds a threshold, new exponents will be
// sent".
//
// The threshold is a judgement about COST, so it is not one number. A full-
// bandwidth channel's set is 4 + 7*ngrps bits - about 590 at D15 over a
// 250-coefficient band - and spending that mid-frame has to buy back more
// than it costs, so it waits for the exponents to have really moved: a mean
// change above two steps, 12 dB per bin.
//
// The LFE's set is always two groups, 18 bits, thirty times cheaper. Holding
// it to the same bar means almost never refreshing it, and the frame's one
// set is then the per-bin minimum across six blocks - a scale chosen by the
// loudest of them. Any block quieter than that is quantized against the wrong
// scale for the sake of not spending 18 bits. So the LFE refreshes as soon as
// its exponents move at all, which is the trade its own cost argues for.
//
// This is the AC-3 encoder's rule, kept because that is the encoder it was
// measured on. plan_exponent_runs below replaces it for E-AC-3 with an
// explicit accounting of the same trade - see its own comment for why a fixed
// threshold turned out not to travel.
[[nodiscard]] inline bool needs_new_exponents(std::span<const std::uint8_t> current,
                                              std::span<const std::uint8_t> reference,
                                              bool is_lfe) {
    long long diff = 0;
    for (std::size_t i = 0; i < current.size(); ++i) {
        diff += std::abs(static_cast<int>(current[i]) - static_cast<int>(reference[i]));
    }
    return diff > (is_lfe ? 0 : 2 * static_cast<long long>(current.size()));
}

// The largest number of blocks a plan covers: a full six-block syncframe.
inline constexpr int kMaxPlanBlocks = kBlocksPerFrame;

// One stream's plan. runs tile [0, blocks): run i covers starts[i] up to
// starts[i + 1], and states strategy[i].
struct ExponentRunPlan {
    int count = 0;
    std::array<int, kMaxPlanBlocks + 1> starts{};
    std::array<ExpStrategy, kMaxPlanBlocks> strategy{};
    // What the plan costs, in bits: the exponent sets it transmits plus the
    // mantissa precision it gives up. Comparable across plans of the same
    // stream, and (bar the per-stream strategy field, which does not depend
    // on the plan) across streams too.
    long long score = 0;
};

// What the planner needs to know about one stream.
struct ExponentRunInput {
    // [block][bin], row stride `bins`, indexed from the stream's own start
    // bin. These are the RAW per-block exponents, before any grouping.
    std::span<const std::uint8_t> exps;
    int bins = 0;
    int blocks = kMaxPlanBlocks;
    // Per bin, from a provisional allocation: how many bits of precision that
    // bin's mantissa actually resolves. It bounds how much a coarser exponent
    // set can cost the bin - see the waste model in plan_exponent_runs. Empty
    // leaves it unbounded, which is only useful for tests.
    std::span<const std::uint8_t> precision;
    // Blocks that must start a run whatever it costs. §7.9's block-switched
    // block is isolated on both sides: its 256 coefficients are two
    // interleaved short spectra, so one exponent set cannot describe it and a
    // long block together.
    std::array<bool, kMaxPlanBlocks> boundary{};
    // §5.4.3.25: the coupling channel groups from its first coded bin and its
    // group count must divide the region exactly. A full-bandwidth or LFE
    // channel keeps bin 0 as its absolute exponent and groups from bin 1
    // (§7.1.3).
    bool coupling = false;
    // §5.4.3.15 makes lfeexpstr one bit - D15 or reuse - so the LFE has no
    // banding to choose.
    bool lfe = false;
    // Table E2.10 fixes each run's strategy from its span, so a frame that
    // hoists its strategies (expstre == 0) has no strategy choice left once
    // the partition is picked. Only the per-block form leaves it free.
    bool free_strategy = false;
};

// The exponent set a run would transmit, as the decoder would reconstruct it:
// exactly what encode_exponents followed by decode_exponents produces for the
// per-bin minimum over the run's blocks, without building either side's
// vectors. §8.2.10's whole preprocessing chain is here, not just the grouping
// minimum, because the part it would be tempting to leave out is the part that
// decides between the strategies: differentials are limited to +-2 and the
// limiter only ever DECREASES exponents, so a spectrum that falls steeply
// costs a D45 set - whose groups are four bins apart - four times the ground a
// D15 set gives up over the same span. Modelling the grouping alone makes the
// coarse strategies look nearly free, and they are not.
//
// This is the whole-spectrum statement of the model. The planner itself builds
// only the groups its coded bins read (limited_group_exponents below), and the
// tests hold the two to each other.
inline void banded_run_exponents(std::span<const std::uint8_t> run_min, ExpStrategy strategy,
                                 bool coupling, std::span<std::uint8_t> out) {
    const auto bins = static_cast<int>(run_min.size());
    const int group = exponent_group_size(strategy);
    // pre[0] is the transmitted absolute exponent; pre[1 + i] covers the i-th
    // group of `group` bins. A coupling channel's reference does not
    // correspond to a coefficient and must stay even (§5.4.3.25), so its
    // groups start at bin 0; a full-bandwidth or LFE channel's reference IS
    // bin 0 and its groups start at bin 1 (§7.1.3).
    std::array<int, 257> pre{};
    const int first = coupling ? 0 : 1;
    const int real_diffs = (bins - first + group - 1) / group;
    for (int i = 0; i < real_diffs; ++i) {
        const int begin = first + i * group;
        int value = kMaxExponent;
        for (int bin = begin; bin < begin + group && bin < bins; ++bin) {
            value = std::min(value, static_cast<int>(run_min[static_cast<std::size_t>(bin)]));
        }
        pre[static_cast<std::size_t>(i) + 1] = value;
    }
    pre[0] = coupling ? std::clamp(pre[1] & ~1, 0, kMaxExponent)
                      : std::min<int>(run_min[0], kMaxAbsoluteExponent);
    for (int i = 1; i <= real_diffs; ++i) {
        pre[static_cast<std::size_t>(i)] =
            std::min(pre[static_cast<std::size_t>(i)], pre[static_cast<std::size_t>(i) - 1] + 2);
    }
    for (int i = real_diffs; i-- > 0;) {
        pre[static_cast<std::size_t>(i)] =
            std::min(pre[static_cast<std::size_t>(i)], pre[static_cast<std::size_t>(i) + 1] + 2);
        if (i == 0 && coupling) {
            pre[0] &= ~1;  // the transmitted reference stays even
        }
    }
    if (!coupling) {
        out[0] = static_cast<std::uint8_t>(pre[0]);
    }
    for (int i = 0; i < real_diffs; ++i) {
        const int begin = first + i * group;
        for (int bin = begin; bin < begin + group && bin < bins; ++bin) {
            out[static_cast<std::size_t>(bin)] =
                static_cast<std::uint8_t>(pre[static_cast<std::size_t>(i) + 1]);
        }
    }
}

// How far one group's exponent can reach through §8.2.10's +-2 limiter. A
// limited exponent is the smaller of its own group's minimum and every other
// group's minimum plus twice the distance to it; no group minimum exceeds
// kMaxExponent, so a group 12 or more away adds at least 24 and never wins.
inline constexpr int kLimiterReach = (kMaxExponent - 1) / 2;

// banded_run_exponents' arithmetic for the groups the planner reads, and no
// more: the limited exponent of group i (pre[0] is the transmitted absolute
// exponent or coupling reference, pre[1 + n] the n-th group, as there), for
// every group up to `needed` inclusive. `needed` is the last group any coded
// bin falls in, so what lies beyond it, and beyond the limiter's reach of it,
// is neither grouped nor limited. The result at every group <= needed is the
// one banded_run_exponents gives; groups past it are not written.
//
// The template parameter is log2 of the group size (D15, D25, D45 = 0, 1, 2).
template <int Shift>
inline void limited_group_exponents(const std::uint8_t* run_min, int bins, bool coupling,
                                    int needed, std::uint8_t* pre) {
    constexpr int group = 1 << Shift;
    const int first = coupling ? 0 : 1;
    const int real_diffs = (bins - first + group - 1) >> Shift;
    const int top = std::min(real_diffs, needed + kLimiterReach);
    const int full = (bins - first) >> Shift;  // groups with all `group` bins inside
    // The minimum of group i (1-based) when every bin of it is inside.
    const auto group_min = [&](int i) {
        const std::uint8_t* p = run_min + first + ((i - 1) << Shift);
        if constexpr (Shift == 0) {
            return static_cast<int>(p[0]);
        } else if constexpr (Shift == 1) {
            return static_cast<int>(std::min(p[0], p[1]));
        } else {
            return static_cast<int>(std::min(std::min(p[0], p[1]), std::min(p[2], p[3])));
        }
    };
    const auto tail_min = [&](int i) {  // the last group, when it is cut short
        int value = kMaxExponent;
        for (int bin = first + ((i - 1) << Shift); bin < bins; ++bin) {
            value = std::min(value, static_cast<int>(run_min[bin]));
        }
        return value;
    };
    int value = 0;
    if (coupling) {
        if (real_diffs > 0) {
            const int group_one = full >= 1 ? group_min(1) : tail_min(1);
            value = std::clamp(group_one & ~1, 0, kMaxExponent);
        }
    } else {
        value = std::min<int>(run_min[0], kMaxAbsoluteExponent);
    }
    pre[0] = static_cast<std::uint8_t>(value);
    // Forward: no group more than 2 above its predecessor.
    const int body = std::min(top, full);
    for (int i = 1; i <= body; ++i) {
        value = std::min(group_min(i), value + 2);
        pre[i] = static_cast<std::uint8_t>(value);
    }
    if (top > body) {
        value = std::min(tail_min(top), value + 2);
        pre[top] = static_cast<std::uint8_t>(value);
    }
    // Backward: no group more than 2 above its successor.
    for (int i = top; i-- > 0;) {
        value = std::min(static_cast<int>(pre[i]), value + 2);
        pre[i] = static_cast<std::uint8_t>(value);
    }
    if (coupling && top > 0) {
        pre[0] &= static_cast<std::uint8_t>(~1);  // the transmitted reference stays even
    }
}

// Eight bytes at a time, with no instruction set behind it: the lane-wise
// unsigned minimum of two words. Each lane's difference is taken without a
// borrow crossing into its neighbour, and the borrow out of the top bit is
// the "x is less than y" mask the select needs.
[[nodiscard]] inline std::uint64_t min_of_bytes(std::uint64_t x, std::uint64_t y) {
    constexpr std::uint64_t kHigh = 0x8080808080808080ULL;
    const std::uint64_t diff = ((x | kHigh) - (y & ~kHigh)) ^ ((x ^ ~y) & kHigh);
    const std::uint64_t below = ((~x & y) | (~(x ^ y) & diff)) & kHigh;
    // 0xFF in every lane whose top bit is set: 0x100 - 0x01 per lane.
    const std::uint64_t mask = (below << 1) - (below >> 7);
    return (x & mask) | (y & ~mask);
}

// run_min[0, n) = the lane-wise minimum of itself and `row`; true when any
// entry moved.
inline bool lower_minimum(const std::uint8_t* row, std::uint8_t* run_min, int n) {
    std::uint64_t moved = 0;
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        std::uint64_t held = 0;
        std::uint64_t incoming = 0;
        std::memcpy(&held, run_min + i, 8);
        std::memcpy(&incoming, row + i, 8);
        const std::uint64_t lowest = min_of_bytes(held, incoming);
        moved |= held ^ lowest;
        std::memcpy(run_min + i, &lowest, 8);
    }
    for (; i < n; ++i) {
        if (row[i] < run_min[i]) {
            run_min[i] = row[i];
            moved = 1;
        }
    }
    return moved != 0;
}

// One block's contribution to a run's waste over the bins [0, n): what each bin
// gives up between its own exponent and the set exponent `held` that stands in
// for it, bounded by the precision it was given. A bin with no precision has a
// cap of 0 and adds nothing, so the sum can run straight along the bins - one
// byte per bin, no list of which are coded, and a form compilers can run
// several bins at a time.
[[nodiscard]] inline int row_waste(const std::uint8_t* exps, const std::uint8_t* held,
                                   const std::uint8_t* cap, int n) {
    unsigned waste = 0;
    for (int bin = 0; bin < n; ++bin) {
        const auto lost = static_cast<std::uint8_t>(exps[bin] - held[bin]);
        waste += lost < cap[bin] ? lost : cap[bin];
    }
    return static_cast<int>(waste);
}

// The best plan for one stream, by exhaustive dynamic programming over every
// partition of its blocks and (where the frame form allows it) every strategy
// for every run.
//
// Both halves of the trade are counted in bits, which is what makes them
// comparable at all:
//
//   cost   - the exponent set itself, 4 + 7 * ngrps.
//   waste  - the mantissa precision the set gives up. One exponent step is a
//            factor of two of scale, so a bin quantized against a set one step
//            coarser than its own block wanted carries about 6 dB more
//            quantization noise - one bit of accuracy.
//
// The waste is bounded, per bin, by what that bin was actually given
// (`precision`). Without that bound the model is badly wrong in the direction
// that matters: a quiet block's bin sits 18 exponent steps below the loud
// block that set the run's scale, and counting all 18 as recoverable would
// value a refresh at thousands of bits when the bin is allocated three. It
// cannot lose precision it never had - a bin with no bits at all loses
// nothing, and reconstructs to zero either way.
//
// That bound is the whole reason this is not a fixed threshold. At 192 kbit/s
// stereo a frame has roughly 1.5 mantissa bits per bin-block to spend, so most
// bins are allocated nothing; a rule that counts every bin's headroom as a
// saving will happily spend a tenth of the frame on exponent sets that buy
// silence. The AC-3 encoder's fixed "mean two steps" threshold is right for
// AC-3, which is where it was measured - this is not a claim that it is wrong
// there, only that it does not travel to a different rate regime by itself.
//
// Annex E has two frame forms, and the encoder wants the best plan under
// each: hoisted (expstre == 0, every run's strategy fixed by Table E2.10 from
// its span) and per-block (every strategy free). Both are the same dynamic
// programme over the same candidate runs, differing only in which strategies
// a run may take, so plan_exponent_runs_both scores both in one pass: each
// candidate's exponent set and its waste are computed once and offered to
// whichever form admits it.
//
// The plan each form ends with is the cheapest, and among equally cheap ones
// the one whose last run starts earliest (and, for the same start, the finer
// strategy): exactly the plan a pass that tried every run in order of start
// block, strategies finest first, and took a candidate only when it scored
// strictly below the best so far would have kept. The runs are tried in the
// opposite order here - by end block, nearest start first - and a candidate
// that ties the best so far takes over when it starts earlier, which comes to
// the same plan.
//
// Four things keep the pass short without changing its answer. A bin
// allocated no precision loses none, so no sum runs past the last coded bin,
// and a strategy's set is only built as far as that bin needs. A run's set
// depends only on its per-bin minimum, so while extending the run by a block
// moves no minimum the set stands, and the extended run's waste is the waste
// so far plus the new block's row. The waste of a run is never less than
//
//   - its waste under a finer strategy (a coarser set is never higher: a
//     group's exponent is at most each of its bins' minima, and limiting only
//     lowers it), nor
//   - the sum of the wastes of any two runs it splits into under the same
//     strategy (each piece's minimum is at least the whole's, so the piece's
//     blocks lose no more than they do in the whole),
//
// and both are known for something already scored; so is its set's cost, and
// the two together bound a candidate's score from below before its set is
// built. A candidate whose bound is already above the best for its end block
// can neither win nor tie, and is skipped. Last, a block's row is summed along
// its own exponents a byte at a time, which a compiler can do several at once.
//
// Complexity was blocks^2 * strategies passes over the bins per form - about
// 32k operations for a 253-bin channel, twice per stream per frame.
struct ExponentRunPlans {
    ExponentRunPlan hoisted;
    ExponentRunPlan per_block;
};

[[nodiscard]] inline ExponentRunPlans plan_exponent_runs_both(const ExponentRunInput& in) {
    const auto bins = static_cast<std::size_t>(in.bins);
    const int blocks = in.blocks;
    constexpr std::array<ExpStrategy, 3> kCandidates{ExpStrategy::kD15, ExpStrategy::kD25,
                                                     ExpStrategy::kD45};

    // One table per form. best[b]: the cheapest way to cover blocks [0, b)
    // with whole runs; from[b]/via[b]: the run that ends at b in that plan.
    constexpr long long kUnreachable = (1LL << 60);
    struct Table {
        std::array<long long, kMaxPlanBlocks + 1> best{};
        std::array<int, kMaxPlanBlocks + 1> from{};
        std::array<ExpStrategy, kMaxPlanBlocks + 1> via{};
    };
    Table hoisted;
    Table per_block;
    hoisted.best.fill(kUnreachable);
    per_block.best.fill(kUnreachable);
    hoisted.best[0] = 0;
    per_block.best[0] = 0;

    // Each bin's precision cap: a bin allocated none (cap 0) loses nothing
    // whatever the set, which is what lets the waste sums run along the bins
    // without a list of the coded ones. The last coded bin bounds all the work:
    // nothing past it is summed, and a strategy's set is only built as far as
    // the group that bin falls in (plus the limiter's reach, see
    // limited_group_exponents). At low rates that is a fraction of the bins.
    std::array<std::uint8_t, 256> cap{};
    int coded_end = 0;  // one past the last coded bin
    for (int bin = 0; bin < in.bins; ++bin) {
        const int c =
            in.precision.empty() ? kMaxExponent : in.precision[static_cast<std::size_t>(bin)];
        if (c > 0) {
            cap[static_cast<std::size_t>(bin)] = static_cast<std::uint8_t>(c);
            coded_end = bin + 1;
        }
    }
    // Strategy k's group size is 1 << k. Bin b falls in group
    // (b + (1 << k) - first) >> k, so the last group anything is read from is:
    static_assert(exponent_group_size(ExpStrategy::kD15) == 1 &&
                  exponent_group_size(ExpStrategy::kD25) == 2 &&
                  exponent_group_size(ExpStrategy::kD45) == 4);
    const int first_bin = in.coupling ? 0 : 1;
    std::array<int, 3> needed_groups{};
    // How many bins of run_min the sets need: the planner keeps the running
    // minimum no further than the longest of them reaches.
    int min_end = 0;
    for (int k = 0; k < 3; ++k) {
        const int group = 1 << k;
        needed_groups[static_cast<std::size_t>(k)] =
            coded_end == 0 ? -1 : (coded_end - 1 + group - first_bin) >> k;
        if (coded_end > 0) {
            const int real_diffs = (in.bins - first_bin + group - 1) >> k;
            const int top = std::min(
                real_diffs, needed_groups[static_cast<std::size_t>(k)] + kLimiterReach);
            min_end = std::max(min_end, std::min(in.bins, first_bin + top * group));
        }
    }

    std::array<std::uint8_t, 256> run_min{};
    // The group exponents of whichever strategy is being built, before they
    // are laid out along the bins.
    std::array<std::uint8_t, 264> groups{};
    // Per strategy, the set laid out along the bins (held[bin] is the
    // exponent bin `bin` is transmitted at) and the waste for the run as it
    // stood the last time the strategy was scored for this end block: the
    // set depends only on run_min, so while the minimum has not moved it
    // holds, and the waste of the run extended by a block in front is the
    // waste so far plus that block's row. The cached waste spans the blocks
    // [covered_from, b). held starts 8 bytes in so D15's coupling form, whose
    // groups sit one before the bins they cover, can be built in place.
    struct StrategyCache {
        std::array<std::uint8_t, 8 + 256 + 8> storage{};
        long long waste = 0;
        int covered_from = 0;
        bool valid = false;
    };
    std::array<StrategyCache, 3> cache{};
    // Build strategy k's set for the current run_min into its cache entry.
    const auto build_set = [&](std::size_t k, StrategyCache& c) {
        const int needed = needed_groups[k];
        if (needed < 0) {
            return;  // nothing is coded: no waste to measure
        }
        std::uint8_t* held = c.storage.data() + 8;
        if (k == 0) {
            // One group per bin: the groups ARE the bins, shifted by one for a
            // coupling channel (whose reference sits before bin 0).
            limited_group_exponents<0>(run_min.data(), in.bins, in.coupling, needed,
                                       in.coupling ? held - 1 : held);
            return;
        }
        // D25 / D45: build the groups, then repeat each across its bins. The
        // last group may reach a little past the bins; the storage has room.
        const int group = 1 << k;
        std::uint8_t* pre = groups.data();
        if (k == 1) {
            limited_group_exponents<1>(run_min.data(), in.bins, in.coupling, needed, pre);
        } else {
            limited_group_exponents<2>(run_min.data(), in.bins, in.coupling, needed, pre);
        }
        int bin = 0;
        if (!in.coupling) {
            held[0] = pre[0];  // the absolute exponent is bin 0's own
            bin = 1;
        }
        for (int g = 1; g <= needed; ++g, bin += group) {
            if (k == 1) {
                const auto pair = static_cast<std::uint16_t>(pre[g] * 0x0101U);
                std::memcpy(held + bin, &pair, sizeof pair);
            } else {
                const std::uint32_t quad = pre[g] * 0x01010101U;
                std::memcpy(held + bin, &quad, sizeof quad);
            }
        }
    };

    // The waste of every run [a, b) under strategy k, as far as it is known:
    // exact where the candidate was scored, a lower bound where it was
    // skipped, 0 where nothing is known. A waste is at most 6 blocks * 256
    // bins * 24 steps, so 16 bits hold it.
    std::array<std::array<std::array<std::uint16_t, 3>, kMaxPlanBlocks + 1>, kMaxPlanBlocks + 1>
        known{};

    // Runs are scored by their END block, nearest start first: best[b] is
    // final before any run starting at b is scored, and a run that starts
    // earlier is the previous run with one more block in front.
    for (int b = 1; b <= blocks; ++b) {
        const auto ub = static_cast<std::size_t>(b);
        for (int bin = 0; bin < min_end; ++bin) {
            run_min[static_cast<std::size_t>(bin)] = kMaxExponent;
        }
        for (auto& c : cache) {
            c.valid = false;
        }
        for (int a = b - 1; a >= 0; --a) {
            const auto ua = static_cast<std::size_t>(a);
            // A forced boundary inside the run rules it out, and every longer
            // run too.
            if (a < b - 1 && in.boundary[ua + 1]) {
                break;
            }
            // Extend the run to cover block a, noting whether any bin's
            // minimum moved (then every cached set is stale).
            const std::uint8_t* row = in.exps.data() + ua * bins;
            if (lower_minimum(row, run_min.data(), min_end)) {
                for (auto& c : cache) {
                    c.valid = false;
                }
            }
            const bool hoisted_open = hoisted.best[ua] < kUnreachable;
            const bool per_block_open = per_block.best[ua] < kUnreachable;
            if (!hoisted_open && !per_block_open) {
                continue;
            }
            const int span = b - a;
            const ExpStrategy fixed = strategy_for_span(span);
            for (std::size_t k = 0; k < kCandidates.size(); ++k) {
                const ExpStrategy strategy = kCandidates[k];
                if (in.lfe) {
                    // lfeexpstr states D15 or nothing, in either frame form -
                    // the LFE has no Table E2.10 code of its own, so the
                    // hoisted form does not constrain its run layout either.
                    if (strategy != ExpStrategy::kD15) {
                        continue;
                    }
                }
                // Which forms admit this strategy for this span. The hoisted
                // form takes only Table E2.10's; the per-block form takes any;
                // the LFE's D15 is the one strategy either form has for it.
                const bool for_hoisted = hoisted_open && (in.lfe || strategy == fixed);
                const bool for_per_block = per_block_open;
                if (!for_hoisted && !for_per_block) {
                    continue;
                }
                const int group = exponent_group_size(strategy);
                int ngrps = 0;
                if (in.coupling) {
                    // §5.4.3.25: ncplgrps covers the region exactly, with none
                    // of §7.1.3's round-up slack, so a strategy that does not
                    // divide it cannot be stated at all.
                    if (in.bins % (3 * group) != 0) {
                        continue;
                    }
                    ngrps = in.bins / (3 * group);
                } else {
                    ngrps = exponent_group_count(strategy, in.bins);
                }
                const long long set_cost = 4 + 7LL * ngrps;
                // What the run's waste is at least: the same run under a
                // finer strategy (its set is never coarser), and any two
                // pieces of it scored apart (each piece's minimum is higher,
                // so its blocks lose less than they do in the whole).
                int least = k > 0 ? known[ua][ub][k - 1] : 0;
                for (int m = a + 1; m < b; ++m) {
                    least = std::max(least, known[ua][static_cast<std::size_t>(m)][k] +
                                                known[static_cast<std::size_t>(m)][ub][k]);
                }
                // The lower bound: this candidate scores at least
                // best[a] + least + set_cost. It is taken when that is below
                // best[b], or equal to it: runs are scored nearest start
                // first, and the plan wanted is the one with the earliest
                // start among equals.
                const bool hoisted_can_win =
                    for_hoisted && hoisted.best[ua] + least + set_cost <= hoisted.best[ub];
                const bool per_block_can_win =
                    for_per_block && per_block.best[ua] + least + set_cost <= per_block.best[ub];
                if (!hoisted_can_win && !per_block_can_win) {
                    known[ua][ub][k] = static_cast<std::uint16_t>(std::min(least, 0xFFFF));
                    continue;
                }
                StrategyCache& c = cache[k];
                if (!c.valid) {
                    build_set(k, c);
                    c.waste = 0;
                    c.covered_from = b;
                    c.valid = true;
                }
                for (int blk = a; blk < c.covered_from; ++blk) {
                    c.waste += row_waste(in.exps.data() + static_cast<std::size_t>(blk) * bins,
                                         c.storage.data() + 8, cap.data(), coded_end);
                }
                c.covered_from = a;
                const long long waste = c.waste;
                known[ua][ub][k] = static_cast<std::uint16_t>(std::min<long long>(waste, 0xFFFF));
                const auto offer = [&](Table& table, bool can_win) {
                    if (!can_win) {
                        return;
                    }
                    const long long score = table.best[ua] + waste + set_cost;
                    if (score < table.best[ub] || (score == table.best[ub] && a < table.from[ub])) {
                        table.best[ub] = score;
                        table.from[ub] = a;
                        table.via[ub] = strategy;
                    }
                };
                offer(hoisted, hoisted_can_win);
                offer(per_block, per_block_can_win);
            }
        }
    }

    // Walk each form's choices back. The DP always reaches `blocks`: a single
    // run over every block is legal for every stream (D15 divides the
    // coupling region whatever its width, and no forced boundary can sit
    // inside a run that starts at block 0 and never stops).
    const auto unwind = [blocks](const Table& table) {
        ExponentRunPlan plan;
        std::array<int, kMaxPlanBlocks + 1> reversed{};
        std::array<ExpStrategy, kMaxPlanBlocks> reversed_strategy{};
        int count = 0;
        for (int b = blocks; b > 0; b = table.from[static_cast<std::size_t>(b)]) {
            reversed_strategy[static_cast<std::size_t>(count)] =
                table.via[static_cast<std::size_t>(b)];
            reversed[static_cast<std::size_t>(count)] = table.from[static_cast<std::size_t>(b)];
            ++count;
        }
        plan.count = count;
        for (int i = 0; i < count; ++i) {
            plan.starts[static_cast<std::size_t>(i)] =
                reversed[static_cast<std::size_t>(count - 1 - i)];
            plan.strategy[static_cast<std::size_t>(i)] =
                reversed_strategy[static_cast<std::size_t>(count - 1 - i)];
        }
        plan.starts[static_cast<std::size_t>(count)] = blocks;
        plan.score = table.best[static_cast<std::size_t>(blocks)];
        return plan;
    };
    return {.hoisted = unwind(hoisted), .per_block = unwind(per_block)};
}

// One form on its own, for callers and tests that want just that:
// free_strategy picks the per-block form, otherwise the hoisted one.
[[nodiscard]] inline ExponentRunPlan plan_exponent_runs(const ExponentRunInput& in) {
    const ExponentRunPlans both = plan_exponent_runs_both(in);
    return in.free_strategy ? both.per_block : both.hoisted;
}

}  // namespace iclforge::ac3::internal