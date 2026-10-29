#include "iclforge/truehd/encoder/encoder.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <optional>

#include "encoder/coding.hpp"
#include "encoder/verify.hpp"
#include "iclforge/base/bitwriter.hpp"
#include "iclforge/truehd/core/crc.hpp"
#include "iclforge/truehd/core/syntax.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/access_unit.hpp"

namespace iclforge::truehd {

namespace {

using detail::ChannelCoding;
using Plane = std::vector<std::int32_t>;

// TH spec §8.4 and ERRATA.md entry 19: the block size after a restart is 8 until a header says
// otherwise. FFmpeg's streams start each restart interval with a block of 8 that is not filtered
// and then use blocks of a whole access unit. The decoder keeps the filter histories across a
// restart (ERRATA.md entry 13), but those 8 samples fill the longest history a filter has (8
// entries) before the first filtered sample, so the encoder's prediction never reaches back past
// the restart and does not depend on which reading of the specification is right.
constexpr int kFirstBlock = 8;
constexpr int kMatrixShift = 14;    // ERRATA.md entry 8: coefficients are scaled to 2^-14
constexpr int kMatrixFracBits = 4;  // a coefficient is sent in 1/16 steps, 6 bits
constexpr int kMaxOutputShift = 7;  // TH spec §8.2: output_shift is a signed 4 bit field
constexpr int kMaxQuantStep = 15;
constexpr std::uint32_t kTerminatorA = 0xD234E;  // TH spec §16.5; ERRATA.md entry 14
constexpr std::uint8_t kExtraDataSeed = 0xA9;    // TH spec §16.4
constexpr int kMaxUnitWords = 4095;              // the 12 bit lengths of TH spec §3.1, §6.1
constexpr int kFixedUnitBytes = 4;
constexpr int kLatencyMs = 75;  // TH spec §3.4, §18.1

[[nodiscard]] constexpr EncodeError make_error(EncodeErrorKind kind, std::string_view detail) {
    return EncodeError{kind, detail};
}

// The matrix channel output channel `c` of a stage takes: the encoder writes the identity ch_assign
// (restart header and fold both read it here).
[[nodiscard]] constexpr std::uint8_t stage_ch_assign(int c) noexcept {
    return static_cast<std::uint8_t>(c);
}

[[nodiscard]] bool is_sample_rate(std::uint32_t rate) noexcept {
    return rate == 44100 || rate == 48000 || rate == 88200 || rate == 96000 || rate == 176400 ||
           rate == 192000;
}

[[nodiscard]] unsigned ratebits_of(std::uint32_t rate) noexcept {
    switch (rate) {
        case 48000:
            return 0;
        case 96000:
            return 1;
        case 192000:
            return 2;
        case 44100:
            return 8;
        case 88200:
            return 9;
        default:
            return 10;
    }
}

struct StageSpec {
    int first_row = 0;
    int last_row = 0;
    [[nodiscard]] int out_rows() const noexcept { return last_row + 1; }
    [[nodiscard]] int coded() const noexcept { return last_row - first_row + 1; }
};

// How the channels are spread over substreams (ERRATA.md entry 31): FFmpeg's TrueHD encoder puts
// two channels in substream 0 and the rest in substream 1; a third substream takes the last two of
// an eight channel stream. At 176.4 and 192 kHz TH spec §9.6 allows one substream only.
[[nodiscard]] std::vector<StageSpec> stage_layout(int channels, unsigned ratebits,
                                                  std::uint32_t assignment) {
    if (rate_factor(ratebits) >= 2 || channels <= 2) {
        return {StageSpec{0, channels - 1}};
    }
    if (channels <= 6) {
        return {StageSpec{0, 1}, StageSpec{2, channels - 1}};
    }
    // The 6 channel presentation is the channels of the major sync's six channel field, the low
    // four bits of the assignment (L R, C, LFE, Ls Rs): its substream must hold exactly that many
    // rows, or FFmpeg's decoder rejects the stream (ERRATA.md entry 33).
    int six = channel_count_of_assignment(assignment & 0xFU);
    if (six < 3 || six >= channels) {
        six = 6;
    }
    return {StageSpec{0, 1}, StageSpec{2, six - 1}, StageSpec{six, channels - 1}};
}

// FFmpeg's decoder takes a substream of more than 6 channels (a channel above 5) only when its
// restart sync has noise_type set, 0x31EB, whether or not the stream uses noise (ERRATA.md entry
// 32; provisional, owner to confirm). The noise shift and seed are zero and no matrix draws on a
// noise channel.
[[nodiscard]] bool noise_flag(const StageSpec& stage) noexcept {
    return stage.last_row > 5;
}

[[nodiscard]] bool is_pair(Channel left, Channel right) noexcept {
    switch (left) {
        case Channel::kL:
            return right == Channel::kR;
        case Channel::kLs:
            return right == Channel::kRs;
        case Channel::kLvh:
            return right == Channel::kRvh;
        case Channel::kLc:
            return right == Channel::kRc;
        case Channel::kLrs:
            return right == Channel::kRrs;
        case Channel::kLsd:
            return right == Channel::kRsd;
        case Channel::kLw:
            return right == Channel::kRw;
        default:
            return false;
    }
}

// --- the primitive matrices (TH spec §10) -------------------------------------------------

// Applies the inverse of `matrix` to `rows`: the decoder computes
//   row[out] = ((sum over c of g[c] * row[c]) >> 14) & keep
// with g[out] = 1.0, so the row before the matrix is row[out] - (F & keep) with F the sum without
// the out row's own term. The other rows are the same before and after.
void invert_matrix(const MatrixSpec& matrix, std::vector<Plane>& rows, int quant) {
    Plane& out = rows[matrix.out_ch];
    const std::size_t n = out.size();
    const std::int64_t keep = ~((std::int64_t{1} << quant) - 1);
    const int scale = kMatrixShift - matrix.frac_bits;
    for (std::size_t t = 0; t < n; ++t) {
        std::int64_t sum = 0;
        for (std::size_t c = 0; c < rows.size(); ++c) {
            if (c != matrix.out_ch && matrix.present[c] && matrix.coeff[c] != 0) {
                sum += std::int64_t{rows[c][t]} * (std::int64_t{matrix.coeff[c]} << scale);
            }
        }
        out[t] = static_cast<std::int32_t>(out[t] - ((sum >> kMatrixShift) & keep));
    }
}

// The decoder's side of a matrix, applied to `rows` in place (TH spec §10.2).
void apply_matrix(const MatrixSpec& matrix, std::vector<Plane>& rows, int quant) {
    Plane& out = rows[matrix.out_ch];
    const std::size_t n = out.size();
    const std::int64_t keep = ~((std::int64_t{1} << quant) - 1);
    const int scale = kMatrixShift - matrix.frac_bits;
    for (std::size_t t = 0; t < n; ++t) {
        std::int64_t sum = 0;
        for (std::size_t c = 0; c < rows.size(); ++c) {
            if (matrix.present[c] && matrix.coeff[c] != 0) {
                sum += std::int64_t{rows[c][t]} * (std::int64_t{matrix.coeff[c]} << scale);
            }
        }
        out[t] = static_cast<std::int32_t>((sum >> kMatrixShift) & keep);
    }
}

// --- the downmix presentations (TH spec §2.2, §10.5) ---------------------------------------
//
// Substream 0 carries a stereo downmix instead of L and R, and the matrices of the later
// substreams take the folded-in channels out of it again, exactly. An 8 channel stream is a
// cascade: substream 1's 5.1 has the rear surrounds folded into its surrounds, and substream 2's
// matrices take them out. Which gains the standard downmix uses is the encoder's choice (the
// specification gives none; ERRATA.md entry 38): no decoder here checks the downmix itself, only
// that the full presentation comes back as it went in.

struct DownmixTerm {
    int source = 0;
    double gain = 0.0;
};

struct DownmixStep {
    int out_row = 0;
    std::vector<DownmixTerm> terms;
};

struct DownmixPlan {
    bool active = false;
    std::vector<DownmixStep> stereo;  // L and R, from the rows of substream 1
    std::vector<DownmixStep> rear;    // Ls and Rs, from the rows of substream 2
};

// The sentence for a layout the downmix cannot be built for, empty if it can.
[[nodiscard]] std::string_view downmix_problem(const std::vector<Channel>& layout,
                                               const std::vector<StageSpec>& stages) {
    if (stages.size() < 2) {
        return "a downmix presentation needs a second substream, and a stream of 176.4 or 192 kHz "
               "has one (TH spec §9.6)";
    }
    if (layout.size() < 3 || layout[0] != Channel::kL || layout[1] != Channel::kR) {
        return "the downmix needs the first two channels to be L and R";
    }
    for (int row = stages[1].first_row; row <= stages[1].last_row; ++row) {
        switch (layout[static_cast<std::size_t>(row)]) {
            case Channel::kC:
            case Channel::kLfe:
            case Channel::kLs:
            case Channel::kRs:
            case Channel::kLrs:
            case Channel::kRrs:
                break;
            default:
                return "the downmix is written for the channels L R C LFE Ls Rs Lrs Rrs only";
        }
    }
    if (stages.size() == 3) {
        bool has_ls = false;
        bool has_rs = false;
        for (int row = stages[1].first_row; row <= stages[1].last_row; ++row) {
            has_ls = has_ls || layout[static_cast<std::size_t>(row)] == Channel::kLs;
            has_rs = has_rs || layout[static_cast<std::size_t>(row)] == Channel::kRs;
        }
        const auto first = static_cast<std::size_t>(stages[2].first_row);
        if (stages[2].coded() != 2 || layout[first] != Channel::kLrs ||
            layout[first + 1] != Channel::kRrs || !has_ls || !has_rs) {
            return "an 8 channel downmix needs Lrs and Rrs last, after an Ls and Rs";
        }
    }
    return {};
}

[[nodiscard]] DownmixPlan make_downmix_plan(const std::vector<Channel>& layout,
                                            const std::vector<StageSpec>& stages,
                                            const DownmixGains& gains) {
    DownmixPlan plan;
    plan.active = true;
    DownmixStep left;
    DownmixStep right;
    left.out_row = 0;
    right.out_row = 1;
    for (int row = stages[1].first_row; row <= stages[1].last_row; ++row) {
        switch (layout[static_cast<std::size_t>(row)]) {
            case Channel::kC:
                left.terms.push_back({row, gains.centre});
                right.terms.push_back({row, gains.centre});
                break;
            case Channel::kLfe:
                left.terms.push_back({row, gains.lfe});
                right.terms.push_back({row, gains.lfe});
                break;
            case Channel::kLs:
            case Channel::kLrs:
                left.terms.push_back({row, gains.surround});
                break;
            default:  // Rs and Rrs
                right.terms.push_back({row, gains.surround});
                break;
        }
    }
    plan.stereo = {left, right};
    if (stages.size() == 3) {
        for (int row = stages[1].first_row; row <= stages[1].last_row; ++row) {
            const Channel channel = layout[static_cast<std::size_t>(row)];
            if (channel == Channel::kLs) {
                DownmixStep step;
                step.out_row = row;
                step.terms.push_back({stages[2].first_row, gains.rear_to_surround});
                plan.rear.push_back(step);
            } else if (channel == Channel::kRs) {
                DownmixStep step;
                step.out_row = row;
                step.terms.push_back({stages[2].first_row + 1, gains.rear_to_surround});
                plan.rear.push_back(step);
            }
        }
    }
    return plan;
}

// The matrices of `steps` with every gain multiplied by `scale`: a coefficient of -gain on each
// source and 1.0 on the row itself (the decoder takes the sources out). A step none of whose
// gains survive the quantisation to 2^-14 is left out.
[[nodiscard]] std::vector<MatrixSpec> make_fold_matrices(const std::vector<DownmixStep>& steps,
                                                         double scale) {
    constexpr int kUnity = 1 << kMatrixShift;
    std::vector<MatrixSpec> matrices;
    for (const DownmixStep& step : steps) {
        MatrixSpec matrix;
        matrix.out_ch = static_cast<std::uint8_t>(step.out_row);
        matrix.frac_bits = kMatrixShift;
        matrix.present[static_cast<std::size_t>(step.out_row)] = true;
        matrix.coeff[static_cast<std::size_t>(step.out_row)] = kUnity;
        bool any = false;
        for (const DownmixTerm& term : step.terms) {
            const long q = std::lround(-term.gain * scale * kUnity);
            if (q == 0) {
                continue;
            }
            matrix.present[static_cast<std::size_t>(term.source)] = true;
            matrix.coeff[static_cast<std::size_t>(term.source)] =
                static_cast<std::int32_t>(std::clamp<long>(q, -2 * kUnity, 2 * kUnity - 1));
            any = true;
        }
        if (any) {
            matrices.push_back(matrix);
        }
    }
    return matrices;
}

[[nodiscard]] bool within_24_bits(const Plane& plane, int shift) {
    const std::int64_t limit = std::int64_t{1} << (23 - shift);
    return std::all_of(plane.begin(), plane.end(),
                       [limit](std::int32_t v) { return v >= -limit && v < limit; });
}

// Folds the channels into `rows` (the encoder's side of the downmix matrices) and returns the
// matrices the decoder undoes it with, stereo steps first. Each fold uses the largest of a few
// gain scales whose downmix rows stay within the 24 bit range of a presentation (TH spec §10.5);
// with none left the rows stay as they are. True if a fold was weakened or dropped.
[[nodiscard]] bool fold_downmix(const DownmixPlan& plan, int quant, int shift,
                                std::vector<Plane>& rows, std::vector<MatrixSpec>& matrices) {
    constexpr std::array<double, 5> kScales{1.0, 0.75, 0.5, 0.25, 0.0};
    bool reduced = false;
    const auto fold = [&](const std::vector<DownmixStep>& steps, std::vector<MatrixSpec>& chosen) {
        const bool wanted = !make_fold_matrices(steps, 1.0).empty();
        for (const double scale : kScales) {
            std::vector<MatrixSpec> candidate = make_fold_matrices(steps, scale);
            std::vector<Plane> trial = rows;
            for (auto it = candidate.rbegin(); it != candidate.rend(); ++it) {
                invert_matrix(*it, trial, quant);
            }
            const bool fits = std::all_of(
                candidate.begin(), candidate.end(),
                [&](const MatrixSpec& m) { return within_24_bits(trial[m.out_ch], shift); });
            if (fits) {
                rows = std::move(trial);
                chosen = std::move(candidate);
                reduced = reduced || (wanted && scale < 1.0);
                return;
            }
        }
    };
    std::vector<MatrixSpec> rear;
    std::vector<MatrixSpec> stereo;
    fold(plan.rear, rear);  // the decoder undoes the stereo steps first, so they are folded last
    fold(plan.stereo, stereo);
    matrices = stereo;
    matrices.insert(matrices.end(), rear.begin(), rear.end());
    return reduced;
}

// The bit cost, up to a constant, of coding `x` with an order 8 predictor: half the log of the
// prediction error power per sample, for comparing two ways of presenting the same channel.
[[nodiscard]] double prediction_cost(const Plane& x) {
    if (x.size() < 16) {
        return 0.0;
    }
    const std::vector<double> r = detail::autocorrelation(x, kMaxFirOrder);
    if (r[0] < 1.0) {
        return 0.0;
    }
    const detail::Predictors p = detail::levinson(r, kMaxFirOrder);
    const auto n = static_cast<double>(x.size());
    return 0.5 * n * std::log2(p.error[kMaxFirOrder] / n + 0.25);
}

struct PairMatrix {
    MatrixSpec spec;
    double saving = 0.0;  // bits
};

// A matrix that takes `source` out of `target` (out = target) if that makes the pair smaller by
// more than the matrix costs.
[[nodiscard]] std::optional<PairMatrix> try_pair(const std::vector<Plane>& rows, int source,
                                                 int target, int quant, double baseline) {
    const Plane& s = rows[static_cast<std::size_t>(source)];
    const Plane& t = rows[static_cast<std::size_t>(target)];
    double dot = 0.0;
    double power = 0.0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        dot += static_cast<double>(s[i]) * static_cast<double>(t[i]);
        power += static_cast<double>(s[i]) * static_cast<double>(s[i]);
    }
    if (power < 1.0) {
        return std::nullopt;
    }
    const int ideal = static_cast<int>(std::lround(dot / power * (1 << kMatrixFracBits)));
    const std::array<int, 5> candidates{ideal, ideal - 1, ideal + 1, 1 << kMatrixFracBits,
                                        1 << (kMatrixFracBits - 1)};
    const std::int64_t keep = ~((std::int64_t{1} << quant) - 1);
    constexpr int kLimit = (1 << (kMatrixFracBits + 1)) - 1;
    const double source_cost = prediction_cost(s);
    std::optional<PairMatrix> best;
    double best_cost = baseline;
    Plane trial(t.size());
    for (const int q : candidates) {
        if (q == 0 || q > kLimit || q < -kLimit - 1) {
            continue;
        }
        bool in_range = true;
        for (std::size_t i = 0; i < t.size(); ++i) {
            const std::int64_t folded = ((std::int64_t{q} * s[i]) >> kMatrixFracBits) & keep;
            const std::int64_t value = t[i] - folded;
            if (value > (std::int64_t{1} << 30) || value < -(std::int64_t{1} << 30)) {
                in_range = false;
                break;
            }
            trial[i] = static_cast<std::int32_t>(value);
        }
        if (!in_range) {
            continue;
        }
        const double cost = source_cost + prediction_cost(trial);
        if (cost < best_cost) {
            best_cost = cost;
            PairMatrix found;
            found.spec.out_ch = static_cast<std::uint8_t>(target);
            found.spec.frac_bits = kMatrixFracBits;
            found.spec.present[static_cast<std::size_t>(source)] = true;
            found.spec.coeff[static_cast<std::size_t>(source)] = q;
            found.spec.present[static_cast<std::size_t>(target)] = true;
            found.spec.coeff[static_cast<std::size_t>(target)] = 1 << kMatrixFracBits;
            found.saving = baseline - cost;
            best = found;
        }
    }
    constexpr double kMatrixOverheadBits = 64.0;
    if (best && best->saving > kMatrixOverheadBits) {
        return best;
    }
    return std::nullopt;
}

// --- prediction and residuals --------------------------------------------------------------

// The residual of `u` under `fir`, in units of 2^quant, with the first block unfiltered. False if
// a residual does not fit the 31 bits the block coder works in.
[[nodiscard]] bool compute_residual(const Plane& u, const FilterSpec& fir, int quant, Plane& out) {
    const std::size_t n = u.size();
    out.assign(n, 0);
    const std::int64_t keep = ~((std::int64_t{1} << quant) - 1);
    for (std::size_t t = 0; t < n; ++t) {
        std::int64_t prediction = 0;
        if (fir.order > 0 && t >= static_cast<std::size_t>(kFirstBlock)) {
            prediction = detail::fir_prediction(fir, u, t) & keep;
        }
        const std::int64_t e = std::int64_t{u[t]} - prediction;
        if (e > (std::int64_t{1} << 30) || e < -(std::int64_t{1} << 30)) {
            return false;
        }
        out[t] = static_cast<std::int32_t>(e >> quant);
    }
    return true;
}

[[nodiscard]] FilterSpec choose_fir(const Plane& u, int quant, int max_order, int nominal) {
    if (max_order <= 0 || u.size() <= static_cast<std::size_t>(2 * kFirstBlock)) {
        return {};
    }
    const std::vector<double> r = detail::autocorrelation(u, max_order);
    if (r[0] < 1.0) {
        return {};
    }
    const detail::Predictors predictors = detail::levinson(r, max_order);
    FilterSpec best;
    double best_cost = -1.0;
    Plane residual;
    for (int order = 0; order <= max_order; ++order) {
        FilterSpec candidate;
        if (order > 0) {
            candidate = detail::quantise_fir(
                std::span<const double>(predictors.coeff[static_cast<std::size_t>(order)].data(),
                                        static_cast<std::size_t>(order)));
        }
        if (!compute_residual(u, candidate, quant, residual)) {
            continue;
        }
        double cost = detail::filter_header_bits(candidate);
        for (std::size_t start = static_cast<std::size_t>(kFirstBlock); start < residual.size();
             start += static_cast<std::size_t>(nominal)) {
            const std::size_t size =
                std::min<std::size_t>(static_cast<std::size_t>(nominal), residual.size() - start);
            cost += detail::estimated_bits(
                std::span<const std::int32_t>(residual).subspan(start, size));
        }
        if (best_cost < 0.0 || cost < best_cost) {
            best_cost = cost;
            best = candidate;
        }
    }
    return best;
}

// --- one block, planned ---------------------------------------------------------------------

struct BlockPlan {
    std::size_t start = 0;
    int size = 0;
    bool has_header = false;
    BlockHeaderSpec header;
};

struct StageWork {
    StageSpec spec;
    std::vector<MatrixSpec> matrices;  // in the order the decoder applies them
    std::vector<Plane> coded;          // the rows this stage codes, before its matrices
    std::vector<Plane> outputs;        // the stage's output rows, in the 24 bit domain
    std::vector<FilterSpec> fir;       // per coded row
    std::vector<Plane> residual;       // per coded row, in units of 2^quant
    std::vector<BlockPlan> blocks;
    int max_lsbs = 0;
    std::uint8_t check = 0;  // the fold of this interval's output
};

struct IntervalShape {
    int units = 0;
    int nominal = 0;
    [[nodiscard]] std::size_t samples() const noexcept {
        return static_cast<std::size_t>(units) * static_cast<std::size_t>(nominal);
    }
};

// The blocks of an interval: the first access unit is cut after 8 samples, the rest are whole.
[[nodiscard]] std::vector<std::pair<std::size_t, int>> block_bounds(const IntervalShape& shape) {
    std::vector<std::pair<std::size_t, int>> bounds;
    for (int au = 0; au < shape.units; ++au) {
        const auto start = static_cast<std::size_t>(au) * static_cast<std::size_t>(shape.nominal);
        if (au == 0) {
            bounds.emplace_back(start, kFirstBlock);
            bounds.emplace_back(start + kFirstBlock, shape.nominal - kFirstBlock);
        } else {
            bounds.emplace_back(start, shape.nominal);
        }
    }
    return bounds;
}

[[nodiscard]] std::size_t round_up_even(std::size_t value) noexcept {
    return value + (value & 1U);
}

}  // namespace

std::string_view to_string(EncodeErrorKind kind) noexcept {
    switch (kind) {
        case EncodeErrorKind::kInvalidConfig:
            return "invalid configuration";
        case EncodeErrorKind::kBadInput:
            return "bad input";
        case EncodeErrorKind::kUnsupported:
            return "unsupported";
        case EncodeErrorKind::kRateExceeded:
            return "rate exceeded";
        case EncodeErrorKind::kVerifyFailed:
            return "verification failed";
        case EncodeErrorKind::kInternal:
            return "internal error";
    }
    return "unknown";
}

std::uint32_t default_channel_assignment(int channels) noexcept {
    switch (channels) {
        case 1:
            return 0x2;  // C
        case 2:
            return 0x1;  // L R
        case 3:
            return 0x3;  // L R C
        case 4:
            return 0x9;  // L R Ls Rs
        case 5:
            return 0xB;  // L R C Ls Rs
        case 6:
            return 0xF;  // L R C LFE Ls Rs
        case 7:
            return 0x4B;  // L R C Ls Rs Lrs Rrs
        case 8:
            return 0x4F;  // L R C LFE Ls Rs Lrs Rrs
        default:
            return 0;
    }
}

struct Encoder::Impl {
    explicit Impl(const EncoderConfig& configuration)
        : config(configuration),
          assignment(configuration.channel_assignment != 0
                         ? configuration.channel_assignment
                         : default_channel_assignment(configuration.channels)),
          layout(channels_of_assignment(assignment)),
          ratebits(ratebits_of(configuration.sample_rate)),
          nominal(nominal_samples_per_access_unit(ratebits)),
          interval_units(configuration.restart_interval_units),
          stages(stage_layout(configuration.channels, ratebits, assignment)) {
        previous_check.assign(stages.size(), 0);
        // One restart interval of input is what the encoder holds before it writes anything: take
        // the room once, so that a caller giving it an access unit a call allocates nothing
        // after this and the buffer is not a figure that grows over a stream's first interval.
        pending.reserve(interval_frames() * static_cast<std::size_t>(configuration.channels));
        if (config.downmix == DownmixMode::kStandard && config.channels > 2) {
            downmix_plan = make_downmix_plan(layout, stages, config.downmix_gains);
        }
        if (config.verify) {
            DecoderConfig decoder_config;
            decoder_config.presentation = Presentation::kBest;
            decoder_config.strictness = Strictness::kStrict;
            verifier = std::make_unique<Decoder>(decoder_config);
        }
    }

    EncoderConfig config;
    std::uint32_t assignment;
    std::vector<Channel> layout;
    unsigned ratebits;
    int nominal;
    int interval_units;
    std::vector<StageSpec> stages;
    DownmixPlan downmix_plan;

    std::vector<std::int32_t> pending;  // interleaved, in the 24 bit domain
    std::size_t pending_start = 0;      // index of the first unread value
    std::uint64_t unit_index = 0;       // access units written in this stream
    std::uint64_t sample_position = 0;  // samples per channel written in this stream
    std::vector<std::uint8_t> previous_check;
    std::int64_t last_start = 0;
    std::int64_t last_arrival_end = 0;
    EncoderStats stats;
    std::unique_ptr<Decoder> verifier;

    [[nodiscard]] std::size_t pending_frames() const noexcept {
        return (pending.size() - pending_start) / static_cast<std::size_t>(config.channels);
    }
    [[nodiscard]] std::size_t interval_frames() const noexcept {
        return static_cast<std::size_t>(interval_units) * static_cast<std::size_t>(nominal);
    }

    [[nodiscard]] std::expected<std::vector<EncodedAccessUnit>, EncodeError> process_interval(
        std::size_t valid_frames, int units);
    [[nodiscard]] MajorSync make_major_sync() const;
    [[nodiscard]] std::vector<std::byte> frame_extra_data(std::span<const std::byte> payload) const;
    void reset_stream();
};

std::string_view Encoder::refusal_reason(const EncoderConfig& config) {
    if (config.kind != StreamKind::kTrueHd) {
        return "only TrueHD (0xF8726FBA) is written; MLP is not (ERRATA.md)";
    }
    if (!is_sample_rate(config.sample_rate)) {
        return "the sample rate is not 44.1, 48, 88.2, 96, 176.4 or 192 kHz (TH spec §1.4)";
    }
    if (config.channels < 1 || config.channels > 8) {
        return "1 to 8 channels are written; 16 channel (Atmos) streams are phase T2b";
    }
    if (config.bits != 16 && config.bits != 20 && config.bits != 24) {
        return "bits must be 16, 20 or 24 (TH spec §1.4)";
    }
    if (config.channel_assignment != 0 &&
        channel_count_of_assignment(config.channel_assignment) != config.channels) {
        return "channel_assignment names a different number of channels";
    }
    if (config.channel_assignment >= (1U << kChannelAssignmentBits)) {
        return "channel_assignment is wider than 13 bits (TH spec §13.1)";
    }
    if (config.restart_interval_units != 1 && config.restart_interval_units < 8) {
        return "the restart interval must be 1 or at least 8 access units (TH spec §7.5)";
    }
    if (config.restart_interval_units > 4096) {
        return "restart_interval_units above 4096";
    }
    if (config.max_fir_order < 0 || config.max_fir_order > kMaxFirOrder) {
        return "max_fir_order must be 0..8 (TH spec §9.3)";
    }
    if (config.peak_data_rate_bps < 1'000'000) {
        return "peak_data_rate_bps below 1 Mbps";
    }
    if (config.stereo_mode > 3) {
        return "stereo_mode must be 0..3 (TH spec §5.4)";
    }
    if (config.experimental.iir || config.experimental.lsb_bypass) {
        return "ExperimentalTools::iir and lsb_bypass are not written yet";
    }
    if (config.downmix == DownmixMode::kStandard) {
        for (const double gain :
             {config.downmix_gains.centre, config.downmix_gains.surround, config.downmix_gains.lfe,
              config.downmix_gains.rear_to_surround}) {
            if (!(gain >= 0.0 && gain <= 1.99)) {
                return "downmix gains must be between 0 and 1.99";
            }
        }
    }
    const unsigned bits = ratebits_of(config.sample_rate);
    if (rate_factor(bits) >= 2 && config.channels > 6) {
        return "at 176.4 and 192 kHz at most 6 channels are written (TH spec §9.6, §21.6)";
    }
    if (config.downmix == DownmixMode::kStandard && config.channels > 2) {
        const std::uint32_t used = config.channel_assignment != 0
                                       ? config.channel_assignment
                                       : default_channel_assignment(config.channels);
        return downmix_problem(channels_of_assignment(used),
                               stage_layout(config.channels, bits, used));
    }
    return {};
}

std::expected<Encoder, EncodeError> Encoder::create(const EncoderConfig& config) {
    if (const std::string_view reason = refusal_reason(config); !reason.empty()) {
        return std::unexpected(make_error(EncodeErrorKind::kInvalidConfig, reason));
    }
    return Encoder(std::make_unique<Impl>(config));
}

Encoder::Encoder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Encoder::Encoder(Encoder&&) noexcept = default;
Encoder& Encoder::operator=(Encoder&&) noexcept = default;
Encoder::~Encoder() = default;

const EncoderStats& Encoder::stats() const noexcept {
    return impl_->stats;
}

int Encoder::samples_per_access_unit() const noexcept {
    return impl_->nominal;
}

int Encoder::substreams() const noexcept {
    return static_cast<int>(impl_->stages.size());
}

void Encoder::Impl::reset_stream() {
    unit_index = 0;
    sample_position = 0;
    std::fill(previous_check.begin(), previous_check.end(), std::uint8_t{0});
    last_start = 0;
    last_arrival_end = 0;
    if (verifier) {
        verifier->reset();
    }
}

MajorSync Encoder::Impl::make_major_sync() const {
    MajorSync sync;
    sync.kind = StreamKind::kTrueHd;
    // TH spec §5.1. FFmpeg's streams put the same channel assignment in the six channel field (as
    // far as 6 channels fit) and in the eight channel field; the modifiers are 0 except that a
    // mono stream has 3 in all three ("not used") and a stereo one the 2 channel mode.
    const int channels = config.channels;
    std::uint32_t six = assignment;
    if (channels > 6) {
        six = assignment & 0xFU;
    }
    unsigned two_mod = 0;
    unsigned six_mod = 0;
    unsigned eight_mod = 0;
    if (channels == 1) {
        two_mod = six_mod = eight_mod = 3;
    } else if (channels == 2) {
        two_mod = six_mod = eight_mod = config.stereo_mode;
    } else {
        two_mod = config.stereo_mode;
    }
    sync.format_info = (ratebits << 28U) | (two_mod << 22U) | (six_mod << 20U) |
                       ((six & 0x1FU) << 15U) | (eight_mod << 13U) | (assignment & 0x1FFFU);
    sync.signature = kSignature;
    sync.is_vbr = true;
    // TH spec §4.4 says raw * 200 * a rate factor; FFmpeg's streams hold 3199 at 48 kHz, 1599 at
    // 96, 799 at 192 and 3482 at 44.1: raw * rate / 16 is 9.6 Mbps in all four (ERRATA.md entry
    // 30). Provisional, owner to confirm.
    const std::uint64_t raw = std::min<std::uint64_t>(
        0x7FFF, (std::uint64_t{config.peak_data_rate_bps} * 16U) / config.sample_rate);
    sync.peak_data_rate = static_cast<std::uint16_t>(raw);
    sync.num_substreams = static_cast<std::uint8_t>(stages.size());
    sync.extended_substream_info = 0;
    // TH spec §6.4's bit table does not fit the streams (ERRATA.md entry 17): 0x14 with one
    // substream and 0x3C with two. Nothing shows the byte for three; it repeats the two substream
    // value.
    sync.substream_info = stages.size() == 1 ? 0x14 : 0x3C;
    sync.channel_meaning = config.channel_meaning;
    sync.size_bytes = kMajorSyncBaseBytes;
    return sync;
}

std::vector<std::byte> Encoder::Impl::frame_extra_data(std::span<const std::byte> payload) const {
    // TH spec §16.1: check nibble and length (in 16-bit words, less one), evo_reserved and the
    // payload length, the payload, zero padding to a whole word, and the parity byte (§16.4).
    const std::size_t content = 4 + payload.size() + 1;
    const std::size_t words = (content + 1) / 2;
    const std::size_t total = words * 2;
    std::vector<std::byte> out(total, std::byte{0});
    const auto length = static_cast<std::uint32_t>(words - 1);
    const auto word0 = static_cast<std::uint32_t>(
        (std::uint32_t{extra_data_check_nibble(length)} << 12U) | (length & 0xFFFU));
    out[0] = static_cast<std::byte>(word0 >> 8U);
    out[1] = static_cast<std::byte>(word0 & 0xFFU);
    const auto size = static_cast<std::uint32_t>(payload.size());
    out[2] = static_cast<std::byte>((size >> 8U) & 0xFU);
    out[3] = static_cast<std::byte>(size & 0xFFU);
    std::copy(payload.begin(), payload.end(), out.begin() + 4);
    unsigned accumulator = size ^ kExtraDataSeed;
    for (std::size_t i = 4; i + 1 < total; ++i) {
        accumulator ^= std::to_integer<unsigned>(out[i]);
    }
    out[total - 1] = static_cast<std::byte>(((accumulator >> 8U) ^ accumulator) & 0xFFU);
    return out;
}

std::expected<std::vector<EncodedAccessUnit>, EncodeError> Encoder::encode(
    std::span<const std::int32_t> pcm) {
    Impl& impl = *impl_;
    const auto channels = static_cast<std::size_t>(impl.config.channels);
    if (pcm.size() % channels != 0) {
        return std::unexpected(
            make_error(EncodeErrorKind::kBadInput, "the PCM is not a whole number of frames"));
    }
    const int bits = impl.config.bits;
    const std::int32_t lowest = -(std::int32_t{1} << (bits - 1));
    const std::int32_t highest = (std::int32_t{1} << (bits - 1)) - 1;
    const int up = 24 - bits;
    // The whole call is checked before any of it is kept, so that a refused call leaves the
    // encoder as it was.
    for (const std::int32_t sample : pcm) {
        if (sample < lowest || sample > highest) {
            return std::unexpected(
                make_error(EncodeErrorKind::kBadInput,
                           "a sample is outside the range of EncoderConfig::bits"));
        }
    }
    // Geometric growth, not an exact reserve on every call: libstdc++ reserves exactly what it is
    // asked for, so a caller that gives less than an interval a call (one access unit at a time, as
    // a live caller does) would copy the whole of the pending input 128 times an interval, which
    // measured 7.5 times the time of one call with the lot (tests/performance, truehd_51_encode).
    if (const std::size_t needed = impl.pending.size() + pcm.size(); needed > impl.pending.capacity()) {
        impl.pending.reserve(std::max(needed, 2 * impl.pending.capacity()));
    }
    for (const std::int32_t sample : pcm) {
        impl.pending.push_back(static_cast<std::int32_t>(static_cast<std::uint32_t>(sample) << up));
    }
    impl.stats.input_samples += pcm.size() / channels;

    std::vector<EncodedAccessUnit> out;
    while (impl.pending_frames() >= impl.interval_frames()) {
        auto units = impl.process_interval(impl.interval_frames(), impl.interval_units);
        if (!units) {
            return std::unexpected(units.error());
        }
        impl.pending_start += impl.interval_frames() * channels;
        for (EncodedAccessUnit& unit : *units) {
            out.push_back(std::move(unit));
        }
    }
    // What the intervals just written consumed leaves with them: the input held is then never more
    // than the interval being filled, where it used to keep up to a mebibyte of PCM it had coded.
    if (impl.pending_start > 0) {
        impl.pending.erase(impl.pending.begin(),
                           impl.pending.begin() + static_cast<std::ptrdiff_t>(impl.pending_start));
        impl.pending_start = 0;
    }
    return out;
}

std::expected<std::vector<EncodedAccessUnit>, EncodeError> Encoder::flush() {
    Impl& impl = *impl_;
    std::vector<EncodedAccessUnit> out;
    const std::size_t frames = impl.pending_frames();
    if (frames > 0) {
        const int units = static_cast<int>((frames + static_cast<std::size_t>(impl.nominal) - 1) /
                                           static_cast<std::size_t>(impl.nominal));
        auto encoded = impl.process_interval(frames, units);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        out = std::move(*encoded);
    }
    impl.pending.clear();
    impl.pending_start = 0;
    impl.reset_stream();
    return out;
}

std::expected<std::vector<EncodedAccessUnit>, EncodeError> Encoder::Impl::process_interval(
    std::size_t valid_frames, int units) {
    const int channels = config.channels;
    IntervalShape shape;
    shape.units = units;
    shape.nominal = nominal;
    const std::size_t n = shape.samples();
    const int padding = static_cast<int>(n - valid_frames);
    const bool final_unit_padded = padding > 0;

    // --- the rows, planar, with the output shift and quantiser step found -------------------
    std::vector<Plane> original(static_cast<std::size_t>(channels), Plane(n, 0));
    std::uint32_t seen = 0;
    for (std::size_t t = 0; t < valid_frames; ++t) {
        for (int c = 0; c < channels; ++c) {
            const std::int32_t v = pending[pending_start + t * static_cast<std::size_t>(channels) +
                                           static_cast<std::size_t>(c)];
            original[static_cast<std::size_t>(c)][t] = v;
            seen |= static_cast<std::uint32_t>(v);
        }
    }
    // The low bits every sample leaves clear are sent as the output shift (at most 7: the field is
    // 4 bits signed) and the quantiser step (TH spec §8.2). FFmpeg's stream of a 16 bit source
    // has shift 7 and step 1.
    const int zero_bits =
        seen == 0 ? 0 : std::min(std::countr_zero(seen), kMaxOutputShift + kMaxQuantStep);
    const int output_shift = std::min(zero_bits, kMaxOutputShift);
    const int quant = zero_bits - output_shift;
    std::vector<Plane> rows = original;
    if (output_shift > 0) {
        for (Plane& plane : rows) {
            for (std::int32_t& v : plane) {
                v >>= output_shift;
            }
        }
    }

    // --- matrices, then the rows each stage codes --------------------------------------------
    // FFmpeg's decoder gives a later substream the rows an earlier one decoded as they were
    // before that earlier substream's matrices (ERRATA.md entry 34): a matrix of
    // substream 0 shapes substream 0's own output and is invisible to substream 1. So a pair
    // decorrelated by a matrix has the matrix in every substream that decodes both its rows, and
    // each substream's own decode (a 2 channel decode reads substream 0 alone) still gets the
    // source channels.
    const std::size_t stage_count = stages.size();
    std::vector<StageWork> work(stage_count);
    std::vector<Plane> coded_rows = rows;  // becomes the rows as they are coded, before matrices
    std::vector<MatrixSpec> fold_matrices;
    if (downmix_plan.active &&
        fold_downmix(downmix_plan, quant, output_shift, coded_rows, fold_matrices)) {
        ++stats.downmix_reduced_intervals;
    }
    std::vector<MatrixSpec> pair_matrices;
    if (config.matrix_search) {
        for (const StageSpec& stage : stages) {
            for (int a = stage.first_row; a < stage.last_row; ++a) {
                if (static_cast<std::size_t>(a + 1) >= layout.size() ||
                    !is_pair(layout[static_cast<std::size_t>(a)],
                             layout[static_cast<std::size_t>(a + 1)])) {
                    continue;
                }
                const double baseline =
                    prediction_cost(coded_rows[static_cast<std::size_t>(a)]) +
                    prediction_cost(coded_rows[static_cast<std::size_t>(a + 1)]);
                const auto forward = try_pair(coded_rows, a, a + 1, quant, baseline);
                const auto backward = try_pair(coded_rows, a + 1, a, quant, baseline);
                const std::optional<PairMatrix>* chosen = nullptr;
                if (forward && (!backward || forward->saving >= backward->saving)) {
                    chosen = &forward;
                } else if (backward) {
                    chosen = &backward;
                }
                if (chosen != nullptr) {
                    invert_matrix((*chosen)->spec, coded_rows, quant);
                    pair_matrices.push_back((*chosen)->spec);
                }
            }
        }
    }
    for (std::size_t s = 0; s < stage_count; ++s) {
        StageWork& stage = work[s];
        stage.spec = stages[s];
        stage.outputs.assign(coded_rows.begin(), coded_rows.begin() + stage.spec.out_rows());
        // The decoder applies the pair matrices first, then the downmix (stereo steps, rear steps).
        const auto inside = [&stage](const MatrixSpec& matrix) {
            if (matrix.out_ch >= stage.spec.out_rows()) {
                return false;
            }
            for (int c = stage.spec.out_rows(); c < static_cast<int>(matrix.present.size()); ++c) {
                if (matrix.present[static_cast<std::size_t>(c)]) {
                    return false;
                }
            }
            return true;
        };
        for (const MatrixSpec& matrix : pair_matrices) {
            if (inside(matrix)) {
                stage.matrices.push_back(matrix);
            }
        }
        for (const MatrixSpec& matrix : fold_matrices) {
            if (inside(matrix)) {
                stage.matrices.push_back(matrix);
            }
        }
        for (const MatrixSpec& matrix : stage.matrices) {
            apply_matrix(matrix, stage.outputs, quant);
        }
        stats.matrices_used += stage.matrices.size();
        stage.coded.assign(coded_rows.begin() + stage.spec.first_row,
                           coded_rows.begin() + stage.spec.out_rows());
    }
    // The last substream's decode is the source, whatever the matrices before it did.
    for (std::size_t k = 0; k < work.back().outputs.size(); ++k) {
        if (work.back().outputs[k] != rows[k]) {
            return std::unexpected(make_error(EncodeErrorKind::kInternal,
                                              "the matrices do not give back the source channels"));
        }
    }

    // --- prediction filters and residuals ----------------------------------------------------
    const int max_order =
        rate_factor(ratebits) >= 2 ? std::min(config.max_fir_order, 4) : config.max_fir_order;
    for (StageWork& stage : work) {
        const auto coded = static_cast<std::size_t>(stage.spec.coded());
        stage.fir.assign(coded, FilterSpec{});
        stage.residual.assign(coded, Plane());
        for (std::size_t k = 0; k < coded; ++k) {
            stage.fir[k] = choose_fir(stage.coded[k], quant, max_order, nominal);
            if (!compute_residual(stage.coded[k], stage.fir[k], quant, stage.residual[k])) {
                stage.fir[k] = FilterSpec{};
                if (!compute_residual(stage.coded[k], stage.fir[k], quant, stage.residual[k])) {
                    return std::unexpected(make_error(EncodeErrorKind::kInternal,
                                                      "a channel row does not fit 31 bits"));
                }
            }
        }
    }

    // --- the blocks of each stage: coding choices and headers --------------------------------
    const auto bounds = block_bounds(shape);
    for (StageWork& stage : work) {
        const auto coded = static_cast<std::size_t>(stage.spec.coded());
        bool retry = true;
        while (retry) {
            retry = false;
            std::vector<ChannelCoding> previous(coded);
            stage.blocks.clear();
            stage.max_lsbs = 0;
            int current_size = kFirstBlock;
            for (std::size_t b = 0; b < bounds.size() && !retry; ++b) {
                BlockPlan plan;
                plan.start = bounds[b].first;
                plan.size = bounds[b].second;
                BlockHeaderSpec& header = plan.header;
                bool any = false;
                if (plan.size != current_size) {
                    header.block_size = static_cast<std::uint16_t>(plan.size);
                    current_size = plan.size;
                    any = true;
                }
                if (b == 0) {
                    if (!stage.matrices.empty()) {
                        MatrixSet set;
                        set.count = static_cast<std::uint8_t>(stage.matrices.size());
                        for (std::size_t m = 0; m < stage.matrices.size(); ++m) {
                            set.items[m] = stage.matrices[m];
                        }
                        header.matrices = set;
                        any = true;
                    }
                    if (output_shift != 0) {
                        std::array<std::int8_t, kMaxChannels> shifts{};
                        for (int c = 0; c < stage.spec.out_rows(); ++c) {
                            shifts[static_cast<std::size_t>(c)] =
                                static_cast<std::int8_t>(output_shift);
                        }
                        header.output_shift = shifts;
                        any = true;
                    }
                    if (quant != 0) {
                        std::array<std::uint8_t, kMaxChannels> steps{};
                        for (int c = 0; c <= stage.spec.last_row; ++c) {
                            steps[static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(quant);
                        }
                        header.quant_step = steps;
                        any = true;
                    }
                }
                for (std::size_t k = 0; k < coded && !retry; ++k) {
                    const int fir_bits = (b == 1 && stage.fir[k].order > 0)
                                             ? detail::filter_header_bits(stage.fir[k])
                                             : 0;
                    const std::span<const std::int32_t> block =
                        std::span<const std::int32_t>(stage.residual[k])
                            .subspan(plan.start, static_cast<std::size_t>(plan.size));
                    const detail::CodingChoice choice =
                        detail::choose_coding(block, quant, previous[k], fir_bits);
                    if (choice.data_bits >= detail::kInfeasible) {
                        // The filter's residual does not fit any coding: drop the filter. A row
                        // that does not fit without one has nothing left to drop.
                        if (stage.fir[k].order == 0) {
                            return std::unexpected(make_error(EncodeErrorKind::kInternal,
                                                              "a channel row fits no coding"));
                        }
                        stage.fir[k] = FilterSpec{};
                        if (!compute_residual(stage.coded[k], stage.fir[k], quant,
                                              stage.residual[k])) {
                            return std::unexpected(make_error(
                                EncodeErrorKind::kInternal, "a channel row does not fit 31 bits"));
                        }
                        retry = true;
                        break;
                    }
                    stage.max_lsbs = std::max<int>(stage.max_lsbs, choice.coding.lsbs);
                    const bool changed = choice.header_bits > 1 || fir_bits > 0;
                    if (changed) {
                        ChannelUpdate& update =
                            header.channels[static_cast<std::size_t>(stage.spec.first_row) + k];
                        update.present = true;
                        if (fir_bits > 0) {
                            update.fir = stage.fir[k];
                        }
                        if (choice.coding.offset != previous[k].offset) {
                            update.huff_offset = choice.coding.offset;
                        }
                        update.huff_type = choice.coding.type;
                        update.huff_lsbs = choice.coding.lsbs;
                        any = true;
                    }
                    previous[k] = choice.coding;
                }
                plan.has_header = any;
                stage.blocks.push_back(std::move(plan));
            }
        }
    }

    // --- the lossless check of this interval, per stage --------------------------------------
    for (StageWork& stage : work) {
        std::uint32_t fold = 0;
        for (std::size_t t = 0; t < n; ++t) {
            for (int k = 0; k < stage.spec.out_rows(); ++k) {
                const auto value =
                    static_cast<std::uint32_t>(stage.outputs[static_cast<std::size_t>(k)][t])
                    << output_shift;
                fold = lossless_fold_step(fold, value, stage_ch_assign(k));
            }
        }
        stage.check = lossless_check_of(fold);
    }

    // --- the access units --------------------------------------------------------------------
    std::vector<EncodedAccessUnit> units_out;
    units_out.reserve(static_cast<std::size_t>(units));
    for (int au = 0; au < units; ++au) {
        const bool first = au == 0;
        const bool last = au == units - 1;
        const std::uint64_t this_unit = unit_index;
        const std::uint64_t first_sample = sample_position;
        std::vector<std::vector<std::byte>> segments;
        std::array<SubstreamEntry, kMaxSubstreams> entries{};
        std::size_t words = 0;
        for (std::size_t s = 0; s < stage_count; ++s) {
            StageWork& stage = work[s];
            BitWriter w;
            // The blocks that belong to this access unit.
            std::size_t first_block = 0;
            std::size_t block_count = 0;
            const std::size_t au_start =
                static_cast<std::size_t>(au) * static_cast<std::size_t>(nominal);
            for (std::size_t b = 0; b < stage.blocks.size(); ++b) {
                if (stage.blocks[b].start >= au_start &&
                    stage.blocks[b].start < au_start + static_cast<std::size_t>(nominal)) {
                    if (block_count == 0) {
                        first_block = b;
                    }
                    ++block_count;
                }
            }
            BlockContext context;
            context.min_chan = stage.spec.first_row;
            context.max_chan = stage.spec.last_row;
            context.max_matrix_chan = stage.spec.last_row;
            context.noise_type = noise_flag(stage.spec);
            context.presence_flags = presence::kAll;

            // The coding in force at the start of this access unit's first block is rebuilt by the
            // writer from the headers of all blocks before it.
            std::vector<ChannelCoding> in_force(static_cast<std::size_t>(stage.spec.coded()));
            for (std::size_t b = 0; b < first_block; ++b) {
                for (std::size_t k = 0; k < in_force.size(); ++k) {
                    const ChannelUpdate& u =
                        stage.blocks[b]
                            .header.channels[static_cast<std::size_t>(stage.spec.first_row) + k];
                    if (u.present) {
                        in_force[k].type = u.huff_type;
                        in_force[k].lsbs = u.huff_lsbs;
                        if (u.huff_offset) {
                            in_force[k].offset = *u.huff_offset;
                        }
                    }
                }
            }
            for (std::size_t bi = 0; bi < block_count; ++bi) {
                const BlockPlan& plan = stage.blocks[first_block + bi];
                const bool restart_here = first && bi == 0;
                if (plan.has_header || restart_here) {
                    w.put(1, 1);
                    w.put(restart_here ? 1U : 0U, 1);
                    if (restart_here) {
                        RestartHeader restart;
                        restart.sync =
                            noise_flag(stage.spec) ? kRestartSyncNoise : kRestartSyncStandard;
                        restart.output_timing = static_cast<std::uint16_t>(first_sample & 0xFFFFU);
                        restart.min_chan = static_cast<std::uint8_t>(stage.spec.first_row);
                        restart.max_chan = static_cast<std::uint8_t>(stage.spec.last_row);
                        restart.max_matrix_chan = static_cast<std::uint8_t>(stage.spec.last_row);
                        restart.max_shift = static_cast<std::uint8_t>(output_shift);
                        restart.max_lsbs = static_cast<std::uint8_t>(std::min(stage.max_lsbs, 31));
                        restart.max_bits = 24;
                        restart.max_bits_repeat = 24;
                        restart.data_check_present = false;
                        restart.lossless_check = previous_check[s];
                        for (int c = 0; c <= stage.spec.last_row; ++c) {
                            restart.ch_assign[static_cast<std::size_t>(c)] = stage_ch_assign(c);
                        }
                        write_restart_header(w, restart);
                    }
                    write_block_header(w, context, plan.header);
                } else {
                    w.put(0, 1);
                }
                for (std::size_t k = 0; k < in_force.size(); ++k) {
                    const ChannelUpdate& u =
                        plan.header.channels[static_cast<std::size_t>(stage.spec.first_row) + k];
                    if (u.present) {
                        in_force[k].type = u.huff_type;
                        in_force[k].lsbs = u.huff_lsbs;
                        if (u.huff_offset) {
                            in_force[k].offset = *u.huff_offset;
                        }
                    }
                }
                for (int i = 0; i < plan.size; ++i) {
                    const std::size_t t = plan.start + static_cast<std::size_t>(i);
                    for (std::size_t k = 0; k < in_force.size(); ++k) {
                        detail::put_residual(w, stage.residual[k][t], quant, in_force[k]);
                    }
                }
                w.put(bi + 1 == block_count ? 1U : 0U, 1);
            }
            // The segment ends with zero padding to an even size, and in the last unit of a
            // stream that does not end on a unit boundary with the end-of-stream marker
            // (ERRATA.md entry 14).
            const std::size_t used = w.bit_count();
            const bool marker = last && final_unit_padded;
            std::size_t payload_bytes = round_up_even((used + (marker ? 32U : 0U) + 7U) / 8U);
            std::size_t fill = payload_bytes * 8U - used - (marker ? 32U : 0U);
            while (fill > 0) {
                const std::size_t step = std::min<std::size_t>(fill, 32);
                w.put(0, static_cast<int>(step));
                fill -= step;
            }
            if (marker) {
                w.put(kTerminatorA, 20);
                w.put(static_cast<std::uint32_t>(padding), 12);
            }
            std::vector<std::byte> payload = w.take();
            const std::uint8_t parity = substream_parity(payload);
            const std::uint8_t crc = substream_crc(payload);
            payload.push_back(static_cast<std::byte>(parity));
            payload.push_back(static_cast<std::byte>(crc));
            words += payload.size() / 2;
            SubstreamEntry& entry = entries[s];
            entry.restart_nonexistent = !first;
            entry.crc_present = true;
            entry.end_ptr = static_cast<std::uint16_t>(words);
            segments.push_back(std::move(payload));
        }
        std::vector<std::byte> extra;
        if (config.extra_data) {
            const std::vector<std::byte> payload = config.extra_data(this_unit);
            if (!payload.empty()) {
                extra = frame_extra_data(payload);
            }
        }
        const std::optional<MajorSync> major_sync =
            first ? std::optional<MajorSync>(make_major_sync()) : std::nullopt;
        const std::size_t prefix_bytes =
            kFixedUnitBytes + (first ? kMajorSyncBaseBytes : 0U) + 2U * stage_count;
        const std::size_t total = prefix_bytes + words * 2U + extra.size();
        if (total / 2U > static_cast<std::size_t>(kMaxUnitWords)) {
            return std::unexpected(make_error(EncodeErrorKind::kRateExceeded,
                                              "an access unit longer than 4095 words"));
        }

        // The rate model: the unit may start to arrive when its slot opens, when the previous unit
        // has arrived and no sooner than one unit duration after the previous start (TH spec §3.4).
        const std::int64_t slot = static_cast<std::int64_t>(first_sample) - nominal;
        std::int64_t start = slot;
        if (this_unit > 0) {
            start = std::max({slot, last_arrival_end, last_start + nominal});
        }
        const std::int64_t arrival =
            static_cast<std::int64_t>((static_cast<std::uint64_t>(total) * 8U * config.sample_rate +
                                       config.peak_data_rate_bps - 1U) /
                                      config.peak_data_rate_bps);
        const std::int64_t latency_limit =
            static_cast<std::int64_t>(config.sample_rate) * kLatencyMs / 1000;
        if (start + arrival - slot > latency_limit) {
            ++stats.rate_violations;
            if (!config.allow_rate_violations) {
                return std::unexpected(make_error(
                    EncodeErrorKind::kRateExceeded,
                    "the stream cannot be sent within the peak data rate and 75 ms (TH spec §18)"));
            }
        }
        last_start = start;
        last_arrival_end = start + arrival;
        stats.max_unit_bytes =
            std::max<std::uint32_t>(stats.max_unit_bytes, static_cast<std::uint32_t>(total));
        stats.max_unit_rate_bps = std::max<std::uint64_t>(
            stats.max_unit_rate_bps, static_cast<std::uint64_t>(total) * 8U * config.sample_rate /
                                         static_cast<std::uint64_t>(nominal));
        const auto input_timing =
            static_cast<std::uint16_t>(static_cast<std::uint64_t>(start) & 0xFFFFU);

        auto prefix = write_access_unit_prefix(
            major_sync, std::span<const SubstreamEntry>(entries.data(), stage_count), input_timing,
            total);
        if (!prefix) {
            return std::unexpected(
                make_error(EncodeErrorKind::kInternal, "the access unit prefix does not write"));
        }
        EncodedAccessUnit unit;
        unit.bytes = std::move(*prefix);
        for (const auto& segment : segments) {
            unit.bytes.insert(unit.bytes.end(), segment.begin(), segment.end());
        }
        unit.bytes.insert(unit.bytes.end(), extra.begin(), extra.end());
        unit.samples = nominal;
        unit.padding_samples = last ? padding : 0;
        unit.has_major_sync = first;
        stats.bytes += unit.bytes.size();
        ++stats.access_units;
        ++unit_index;
        sample_position += static_cast<std::uint64_t>(nominal);

        if (verifier) {
            auto decoded = verifier->decode(unit.bytes);
            if (!decoded || !*decoded) {
                return std::unexpected(make_error(EncodeErrorKind::kVerifyFailed,
                                                  "the decoder refused the unit it was given"));
            }
            const DecodedAccessUnit& got = **decoded;
            const int expected_samples = nominal - unit.padding_samples;
            const std::size_t base =
                static_cast<std::size_t>(au) * static_cast<std::size_t>(nominal);
            if (const auto mismatch =
                    detail::verify_mismatch(got, channels, expected_samples, original, base);
                !mismatch.empty()) {
                return std::unexpected(make_error(EncodeErrorKind::kVerifyFailed, mismatch));
            }
            ++stats.verified_units;
        }
        units_out.push_back(std::move(unit));
    }
    for (std::size_t s = 0; s < stage_count; ++s) {
        previous_check[s] = work[s].check;
    }
    ++stats.restart_intervals;
    return units_out;
}

}  // namespace iclforge::truehd
