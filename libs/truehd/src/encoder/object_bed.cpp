#include "encoder/object_bed.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>

#include "core/noise.hpp"
#include "iclforge/truehd/core/format.hpp"

namespace iclforge::truehd::detail {

namespace {

constexpr int kObjectShift = kernels::kObjectShift;
constexpr int kFirstCodedRow = kBedChannels;
constexpr int kMixFracBits = 12;
constexpr std::int64_t kMixPlaces = std::int64_t{1} << kMixFracBits;  // coefficient 1.0 as sent
constexpr std::int64_t kGainOne = std::int64_t{1} << kObjectGainBits;
constexpr std::int32_t kMaxGain = 130000;  // +-1.98: the coefficient is 14 bits, signed, at 2^-12
constexpr std::size_t kMatrixChannels = 16;

// The shift that takes a raw coefficient to the decoder's running 2^-18.
[[nodiscard]] int place_bits(const MatrixSpec& matrix) noexcept {
    return kObjectShift - matrix.frac_bits + matrix.f_shift;
}

// S of SUBSTREAM3.md section 3: the slope's delta is worth 2^-ext_field2 raw places a unit.
[[nodiscard]] int slope_scale(const MatrixSpec& matrix) noexcept {
    return matrix.ext_field2 - matrix.f_shift + matrix.frac_bits;
}

// The number of bypass bits a sample of a matrix (0 when it has none).
[[nodiscard]] int bypass_bits_of(const MatrixSpec& matrix) noexcept {
    return matrix.lsb_bypass ? std::max<int>(matrix.field_2b, 1) : 0;
}

// The ramp of the slopes grows by this much a sample inside an access unit of `nominal` samples
// (SUBSTREAM3.md section 3: 6552 at 48 kHz, 3276 at 96 kHz and 1636 at 192 kHz, where the unit
// holds 40, 80 and 160 samples; the last two are PROVISIONAL, see object_ramp_step).
[[nodiscard]] std::int32_t ramp_step_of(int nominal) noexcept {
    const int units = nominal >= 40 && nominal % 40 == 0 ? nominal / 40 : 1;
    return object_ramp_step(std::countr_zero(static_cast<unsigned>(units)));
}

// Generator 1's buffers for every access unit of an interval (ERRATA.md, noise generator 1): the
// buffer of an access unit is filled from the seed the one before it left, the first from the
// restart header's.
class IntervalNoise {
   public:
    explicit IntervalNoise(const ObjectChainShape& shape)
        : size_(static_cast<std::size_t>(shape.noise_size)),
          nominal_(static_cast<std::size_t>(shape.nominal)),
          buffer_(static_cast<std::size_t>(shape.units) * size_) {
        std::uint32_t seed = shape.noise_seed;
        for (int unit = 0; unit < shape.units; ++unit) {
            seed = fill_noise_buffer(seed, size_,
                                     buffer_.data() + static_cast<std::size_t>(unit) * size_);
        }
    }

    // What the matrix at `index` of `count` adds to its sum (at 2^-18) for sample `t` of the
    // interval: the decoder starts the access unit at the index of the matrices left (this one
    // included) and steps by twice that, plus one, a sample.
    [[nodiscard]] std::int64_t term(std::size_t index, std::size_t count, int shift,
                                    std::size_t t) const noexcept {
        if (shift == 0) {
            return 0;
        }
        const std::size_t left = count - index;
        const std::size_t unit = t / nominal_;
        const std::size_t position = t % nominal_;
        const std::size_t slot = (left + (2 * left + 1) * position) & (size_ - 1);
        return std::int64_t{buffer_[unit * size_ + slot]} *
               (std::int64_t{1} << (shift + kObjectShift - kernels::kMatrixShift +
                                    kernels::kNoiseScaleShift));
    }

   private:
    std::size_t size_;
    std::size_t nominal_;
    std::vector<std::int8_t> buffer_;
};

// What the decoder's kernels::apply_object_matrix makes of one matrix at sample `i` of a block
// (`t` of the interval), before the quantiser step's mask and the bypass bits: the channels it
// reads times their running coefficients, less the matrix's own channel when `skip_own`, plus its
// noise, plus with the extension the slopes' share (sum of channel times slope shifted down by the
// scale) times the ramp, at 2^-18.
[[nodiscard]] std::int64_t matrix_value(const MatrixSpec& matrix, const ObjectRun& run,
                                        const std::vector<ObjectPlane>& rows, std::size_t t, int i,
                                        std::int32_t ramp_step, bool skip_own,
                                        std::int64_t noise) noexcept {
    std::int64_t sum = noise;
    std::int64_t fine = 0;
    for (std::size_t c = 0; c < rows.size(); ++c) {
        if (!matrix.present[c] || (skip_own && c == matrix.out_ch)) {
            continue;
        }
        const std::int64_t row = rows[c][t];
        sum += row * run.coeff[c];
        fine += row * run.slope[c];
    }
    if (run.extension) {
        // 64-bit `fine`, truncated to 32 bits as the decoder does (kernels.hpp): the width is
        // PROVISIONAL at 96 and 192 kHz, where no stream shows it.
        const std::int64_t shared =
            run.scale >= 0
                ? fine >> run.scale
                : static_cast<std::int64_t>(static_cast<std::uint64_t>(fine) << -run.scale);
        sum += std::int64_t{static_cast<std::int32_t>(shared)} *
               (std::int64_t{run.ramp} + std::int64_t{ramp_step} * i);
    }
    return sum >> kObjectShift;
}

// The matrix is primitive when its own coefficient is +1.0 or -1.0 (or 2^n with the n bits of LSB
// bypass that give back the low bits the multiplication loses) and carries no slope, the one thing
// that makes it invertible in integers. Returns the own coefficient at 2^-18, or 0 if the matrix
// is not primitive.
[[nodiscard]] std::int64_t own_coefficient(const MatrixSpec& matrix, const ObjectRun& run,
                                           std::size_t rows) noexcept {
    if (matrix.out_ch >= rows || !matrix.present[matrix.out_ch] || run.slope[matrix.out_ch] != 0) {
        return 0;
    }
    const std::int64_t own = run.coeff[matrix.out_ch];
    const std::int64_t unit = std::int64_t{1} << kObjectShift;
    if (own == unit || own == -unit) {
        return own;
    }
    if (matrix.lsb_bypass && own == (unit << bypass_bits_of(matrix))) {
        return own;
    }
    return 0;
}

[[nodiscard]] bool same_headers(const MatrixSpec& a, const MatrixSpec& b) noexcept {
    return a.out_ch == b.out_ch && a.frac_bits == b.frac_bits && a.f_shift == b.f_shift &&
           a.field_2b == b.field_2b && a.lsb_bypass == b.lsb_bypass && a.field_m4 == b.field_m4 &&
           a.present == b.present;
}

[[nodiscard]] bool same_list_headers(const std::vector<MatrixSpec>& a,
                                     const std::vector<MatrixSpec>& b) noexcept {
    return a.size() == b.size() &&
           std::ranges::all_of(std::views::iota(std::size_t{0}, a.size()),
                               [&](std::size_t m) { return same_headers(a[m], b[m]); });
}

// What the decoder's kernel does to the coefficients when a matrix has played the last sample of
// an access unit (kernels::apply_object_matrix): each moves by its slope.
void end_of_unit(const MatrixSpec& matrix, ObjectRun& run) noexcept {
    if (!run.extension) {
        return;
    }
    const int up = kObjectShift - run.scale;
    for (std::size_t c = 0; c < kMatrixChannels; ++c) {
        if (!matrix.present[c]) {
            continue;
        }
        const std::int32_t slope = run.slope[c];
        run.coeff[c] =
            static_cast<std::int32_t>(static_cast<std::uint32_t>(run.coeff[c]) +
                                      (up >= 0 ? static_cast<std::uint32_t>(slope) << up
                                               : static_cast<std::uint32_t>(slope >> -up)));
    }
}

// The state at the start of the next block after `block` has played from `run`.
void advance(const MatrixSpec& matrix, ObjectRun& run, int size, bool ends_unit,
             std::int32_t ramp_step) noexcept {
    if (run.extension) {
        run.ramp += ramp_step * size;
    }
    if (ends_unit) {
        end_of_unit(matrix, run);
    }
}

struct MixRow {
    int row = 0;               // the bed row the matrix writes
    std::vector<int> objects;  // the objects (from 0 = channel 8) with a gain into it
    int width = 0;             // the width of the slopes over the interval
};

// The smallest width whose deltas (width + 1 bits, signed) hold magnitudes up to `bound`.
[[nodiscard]] int width_for(std::int64_t bound) noexcept {
    if (bound <= 0) {
        return 0;
    }
    return std::min(kMaxDeltaWidth, std::max(1, std::bit_width(static_cast<std::uint64_t>(bound))));
}

}  // namespace

GainFraction path_gain(const ObjectPath& path, int row, std::int64_t sample) noexcept {
    const auto& knots = path.knots;
    if (knots.empty()) {
        return {0, 1};
    }
    const auto at = static_cast<std::size_t>(row);
    const auto next = std::ranges::upper_bound(knots, sample, {}, &ObjectGainKnot::sample);
    if (next == knots.begin()) {
        return {knots.front().gain[at], 1};
    }
    const ObjectGainKnot& a = *(next - 1);
    if (next == knots.end()) {
        return {a.gain[at], 1};
    }
    const std::int64_t span = next->sample - a.sample;
    return {std::int64_t{a.gain[at]} * span +
                (std::int64_t{next->gain[at]} - a.gain[at]) * (sample - a.sample),
            span};
}

std::string_view object_path_problem(const ObjectPath& path, int objects) {
    if (path.object < kFirstCodedRow || path.object >= objects) {
        return "an object path names an object that is not above the bed";
    }
    if (path.knots.empty()) {
        return "an object path has no knots";
    }
    for (std::size_t k = 0; k < path.knots.size(); ++k) {
        const ObjectGainKnot& knot = path.knots[k];
        if (knot.sample < 0) {
            return "an object path's knot is before the stream's first sample";
        }
        if (k > 0 && knot.sample < path.knots[k - 1].sample) {
            return "an object path's knots are not in ascending order of sample";
        }
        if (k > 0 && knot.sample - path.knots[k - 1].sample > kMaxKnotSpan) {
            return "an object path's knots are more than 2^26 samples apart";
        }
        for (const std::int32_t gain : knot.gain) {
            if (gain > kMaxGain || gain < -kMaxGain) {
                return "an object path's gain is beyond +-1.98";
            }
        }
    }
    return {};
}

ObjectState apply_object_section(const ObjectState& before, const MatrixSet& section) {
    const ObjectSectionForm& form = section.form;
    ObjectState after = before;
    const std::size_t count = section.count;
    if (count == 0) {
        return after;
    }
    if (form.change && form.headers) {
        // New headers keep the widths and scales of the matrix at the same place
        after.matrices.assign(section.items.begin(), section.items.begin() + count);
        for (std::size_t m = 0; m < count; ++m) {
            after.matrices[m].coeff = {};
            after.matrices[m].ext_value = {};
            after.matrices[m].ext_width =
                m < before.matrices.size() ? before.matrices[m].ext_width : 0;
            after.matrices[m].ext_field2 =
                m < before.matrices.size() ? before.matrices[m].ext_field2 : 0;
        }
    }
    after.run.resize(count);
    for (std::size_t m = 0; m < count; ++m) {
        MatrixSpec& matrix = after.matrices.at(m);
        ObjectRun& run = after.run[m];
        if (form.deltas && form.widths) {
            matrix.ext_width = section.items[m].ext_width;
            matrix.ext_field2 = section.items[m].ext_field2;
        }
        if (form.change) {
            run.coeff = {};
            const int up = place_bits(matrix);
            for (std::size_t c = 0; c < kMatrixChannels; ++c) {
                if (matrix.present[c]) {
                    run.coeff[c] = section.items[m].coeff[c] * (1 << std::max(up, 0));
                }
            }
        }
        if (!form.extension) {
            run.slope = {};
        } else if (form.deltas) {
            run.slope = {};
            for (std::size_t c = 0; c < kMatrixChannels; ++c) {
                if (matrix.present[c] && matrix.ext_width != 0) {
                    run.slope[c] = section.items[m].ext_value[c];
                }
            }
        }
        run.extension = form.extension;
        run.scale = slope_scale(matrix);
    }
    return after;
}

std::expected<ObjectTimeline, std::string_view> build_object_timeline(
    const ObjectTimelineRequest& request) {
    if (request.objects <= kBedChannels || request.objects > kMaxChannels) {
        return std::unexpected("the object matrices need 9 to 16 channels");
    }
    if (request.noise_shift < 0 || request.noise_shift > 15 || request.bypass_bits < 0 ||
        request.bypass_bits > 3 || request.mix_step < 0 || request.mix_step >= kObjectMixSteps ||
        request.update_units < 1 || request.nominal < 1) {
        return std::unexpected("an object matrix parameter is out of range");
    }
    const bool slopes = request.extension == ObjectExtension::kSlopes;
    if (slopes && request.update_units != 1) {
        return std::unexpected("the slopes need an update at every access unit");
    }
    const int above = request.objects - kFirstCodedRow;

    // The paths of the objects above the bed: the caller's, else a constant gain into the object's
    // own bed row.
    std::vector<ObjectPath> paths(static_cast<std::size_t>(above));
    std::vector<bool> given(static_cast<std::size_t>(above), false);
    for (const ObjectPath& path : request.paths) {
        if (const std::string_view problem = object_path_problem(path, request.objects);
            !problem.empty()) {
            return std::unexpected(problem);
        }
        const auto k = static_cast<std::size_t>(path.object - kFirstCodedRow);
        if (given[k]) {
            return std::unexpected("two object paths name the same object");
        }
        given[k] = true;
        paths[k] = path;
    }
    for (int k = 0; k < above; ++k) {
        const auto at = static_cast<std::size_t>(k);
        if (!given[at]) {
            ObjectGainKnot knot;
            knot.gain[static_cast<std::size_t>(kBedRowOfObject[k])] = kDefaultMixGain;
            paths[at].object = kFirstCodedRow + k;
            paths[at].knots.push_back(knot);
        }
    }

    // The bed rows that get a mixing matrix, in the order of the first object that reaches them
    // (or, split, a matrix for each object and row it reaches, objects in order).
    std::vector<MixRow> mixes;
    if (request.mix_step < kObjectMixSteps - 1) {
        for (int k = 0; k < above; ++k) {
            for (int row = 0; row < kBedChannels; ++row) {
                const bool reaches = std::ranges::any_of(
                    paths[static_cast<std::size_t>(k)].knots, [row](const ObjectGainKnot& knot) {
                        return knot.gain[static_cast<std::size_t>(row)] != 0;
                    });
                if (!reaches) {
                    continue;
                }
                if (request.split_mixes) {
                    mixes.push_back(MixRow{row, {k}, 0});
                    continue;
                }
                auto it = std::ranges::find(mixes, row, &MixRow::row);
                if (it == mixes.end()) {
                    mixes.push_back(MixRow{row, {}, 0});
                    it = mixes.end() - 1;
                }
                it->objects.push_back(k);
            }
        }
    }
    const std::int64_t halves = std::int64_t{1} << request.mix_step;

    // A path's value for a mixing matrix's coefficient at an absolute sample: the raw value (minus
    // the gain at 2^-12, rounded) and the same at the decoder's 2^-18.
    const auto gain_at = [&](const MixRow& mix, std::size_t o, std::int64_t sample) {
        return path_gain(paths[static_cast<std::size_t>(mix.objects[o])], mix.row, sample);
    };
    const auto raw_of = [&](const GainFraction& g) -> std::int32_t {
        return static_cast<std::int32_t>(
            round_div(-g.numerator * kMixPlaces, g.denominator * kGainOne * halves));
    };
    const auto at_18 = [&](const GainFraction& g) -> std::int32_t {
        return static_cast<std::int32_t>(
            round_div(-g.numerator * (std::int64_t{1} << (kObjectShift - kObjectGainBits)),
                      g.denominator * halves));
    };

    const std::size_t block_count = request.blocks.size();
    const auto nominal = static_cast<std::size_t>(request.nominal);
    const std::int32_t ramp_step = ramp_step_of(request.nominal);
    const std::size_t end_of_interval =
        block_count == 0
            ? 0
            : request.blocks.back().first + static_cast<std::size_t>(request.blocks.back().second);
    const std::size_t unit_count = (end_of_interval + nominal - 1) / nominal;

    // The matrices every block has: the sign flip, and the bypass matrix; their coefficients never
    // move.
    MatrixSpec flip;
    flip.out_ch = kFirstCodedRow;
    flip.frac_bits = 0;
    flip.f_shift = -1;
    flip.present[kFirstCodedRow] = true;
    flip.coeff[kFirstCodedRow] = -2;  // -2 / 2^(0 + 1): -1.0 in two bits
    MatrixSpec doubled;
    if (request.bypass_bits > 0) {
        doubled.out_ch = static_cast<std::uint8_t>(request.objects - 1);
        doubled.frac_bits = kMixFracBits;
        doubled.f_shift = static_cast<std::int8_t>(request.bypass_bits);
        doubled.lsb_bypass = true;
        doubled.field_2b = static_cast<std::uint8_t>(request.bypass_bits);
        doubled.field_m4 = static_cast<std::uint8_t>(request.noise_shift);
        doubled.present[doubled.out_ch] = true;
        doubled.coeff[doubled.out_ch] = 1 << kMixFracBits;  // 2^12 / 2^(12 - n): 2^n
    }
    const auto mix_head = [&](const MixRow& mix) {
        MatrixSpec head;
        head.out_ch = static_cast<std::uint8_t>(mix.row);
        head.frac_bits = kMixFracBits;
        head.field_m4 = static_cast<std::uint8_t>(request.noise_shift);
        head.present[head.out_ch] = true;
        for (const int k : mix.objects) {
            head.present[static_cast<std::size_t>(kFirstCodedRow + k)] = true;
        }
        return head;
    };

    // The slopes' width of each mixing matrix: the largest move of any of its coefficients in an
    // access unit that is a slope and not a step, plus what a jump leaves, in deltas.
    const int slope_up = kObjectShift - (kSlopeScaleField + kMixFracBits);  // 18 - S
    if (slopes) {
        for (MixRow& mix : mixes) {
            std::int64_t bound = 0;
            for (std::size_t u = 0; u < unit_count; ++u) {
                for (std::size_t o = 0; o < mix.objects.size(); ++o) {
                    const auto from = request.first_sample + static_cast<std::int64_t>(u * nominal);
                    const std::int64_t move =
                        std::abs(std::int64_t{at_18(gain_at(mix, o, from + request.nominal))} -
                                 at_18(gain_at(mix, o, from)));
                    if (move > 0 && move <= kStepLimit) {
                        bound = std::max(
                            bound, (move + kJumpTolerance + (std::int64_t{1} << slope_up) - 1) >>
                                       slope_up);
                    }
                }
            }
            mix.width = width_for(bound);
        }
    }

    ObjectTimeline timeline;
    ObjectState state;                                // what the decoder holds, as it goes
    std::vector<std::vector<std::int32_t>> held_raw;  // without slopes: the raw values held
    for (std::size_t j = 0; j < block_count; ++j) {
        const std::size_t start = request.blocks[j].first;
        const int size = request.blocks[j].second;
        const bool unit_start = start % nominal == 0;
        const std::int64_t from = request.first_sample + static_cast<std::int64_t>(start);
        const std::int64_t to = from + request.nominal;

        if (unit_start) {
            // --- the matrices wanted in this access unit --------------------------------------
            const bool update = j == 0 || slopes ||
                                (from % request.nominal == 0 &&
                                 (from / request.nominal) % request.update_units == 0);
            // raw values at the unit's start (and its end, for the idle test)
            std::vector<std::vector<std::int32_t>> raw0(mixes.size());
            std::vector<std::vector<std::int32_t>> raw1(mixes.size());
            std::vector<std::vector<std::int32_t>> at0(mixes.size());
            std::vector<std::vector<std::int32_t>> at1(mixes.size());
            for (std::size_t r = 0; r < mixes.size(); ++r) {
                for (std::size_t o = 0; o < mixes[r].objects.size(); ++o) {
                    const GainFraction g0 = gain_at(mixes[r], o, from);
                    const GainFraction g1 = gain_at(mixes[r], o, to);
                    raw0[r].push_back(raw_of(g0));
                    raw1[r].push_back(raw_of(g1));
                    at0[r].push_back(at_18(g0));
                    at1[r].push_back(at_18(g1));
                }
            }
            if (!slopes && update) {
                held_raw = raw0;
            }
            const auto& wanted = slopes ? raw0 : held_raw;

            std::vector<MatrixSpec> head;  // the list wanted
            std::vector<int> mix_of;       // for each matrix of the list, its mix (or -1)
            head.push_back(flip);
            mix_of.push_back(-1);
            if (request.bypass_bits > 0) {
                head.push_back(doubled);
                mix_of.push_back(-1);
            }
            for (std::size_t r = 0; r < mixes.size(); ++r) {
                bool idle = true;
                for (std::size_t o = 0; o < mixes[r].objects.size(); ++o) {
                    idle = idle && wanted[r][o] == 0 && (!slopes || raw1[r][o] == 0);
                }
                if (idle && request.drop_idle) {
                    continue;
                }
                MatrixSpec spec = mix_head(mixes[r]);
                if (slopes && mixes[r].width != 0) {
                    spec.ext_width = static_cast<std::uint8_t>(mixes[r].width);
                    spec.ext_field2 = kSlopeScaleField;
                }
                head.push_back(spec);
                mix_of.push_back(static_cast<int>(r));
            }
            if (head.size() > static_cast<std::size_t>(kMaxObjectMatrices)) {
                return std::unexpected("more than 16 object matrices");
            }
            bool extension = false;
            for (const MatrixSpec& spec : head) {
                extension = extension || (slopes && spec.ext_width != 0);
            }

            const bool list_changed =
                state.matrices.empty() || !same_list_headers(state.matrices, head);
            // the raw value of coefficient `c` of list place `m`: the fixed matrices' own, a mixing
            // matrix's own 1.0, and its objects' values
            const auto raw_for = [&](std::size_t m, std::size_t channel,
                                     bool jumped) -> std::int32_t {
                const int r = mix_of[m];
                if (r < 0) {
                    return head[m].coeff[channel];
                }
                if (channel == head[m].out_ch) {
                    return 1 << kMixFracBits;
                }
                const auto& objects = mixes[static_cast<std::size_t>(r)].objects;
                const auto o = static_cast<std::size_t>(
                    std::ranges::find(objects, static_cast<int>(channel) - kFirstCodedRow) -
                    objects.begin());
                if (jumped || list_changed) {
                    return slopes ? raw0[static_cast<std::size_t>(r)][o]
                                  : wanted[static_cast<std::size_t>(r)][o];
                }
                // not jumped: the raw value nearest to where the coefficient has got to
                return static_cast<std::int32_t>(
                    round_div(state.run[m].coeff[channel], std::int64_t{1} << place_bits(head[m])));
            };

            MatrixSet section;
            bool send = false;
            std::vector<ObjectRun> intended;
            ObjectSectionForm& form = section.form;
            section.count = static_cast<std::uint8_t>(head.size());
            for (std::size_t m = 0; m < head.size(); ++m) {
                section.items[m] = head[m];
            }
            if (!slopes) {
                // The coefficients hold: new raw values when they differ from what the decoder has.
                bool differs = list_changed;
                for (std::size_t m = 0; m < head.size() && !differs; ++m) {
                    for (std::size_t c = 0; c < kMatrixChannels && !differs; ++c) {
                        if (head[m].present[c]) {
                            differs = (raw_for(m, c, true) * (1 << place_bits(head[m]))) !=
                                      state.run[m].coeff[c];
                        }
                    }
                }
                if (differs) {
                    send = true;
                    form.change = true;
                    form.headers = list_changed;
                    for (std::size_t m = 0; m < head.size(); ++m) {
                        for (std::size_t c = 0; c < kMatrixChannels; ++c) {
                            if (head[m].present[c]) {
                                section.items[m].coeff[c] = raw_for(m, c, true);
                            }
                        }
                    }
                }
            } else {
                // The coefficients run on with their slopes: send new raw values where one has
                // drifted from the path, new slopes where the path turns.
                bool jump = list_changed;
                std::vector<ObjectRun> next(head.size());
                if (!list_changed) {
                    for (std::size_t m = 0; m < head.size(); ++m) {
                        next[m] = state.run[m];
                        const int r = mix_of[m];
                        for (std::size_t o = 0;
                             r >= 0 && o < mixes[static_cast<std::size_t>(r)].objects.size(); ++o) {
                            const auto channel = static_cast<std::size_t>(
                                kFirstCodedRow + mixes[static_cast<std::size_t>(r)].objects[o]);
                            if (std::abs(std::int64_t{state.run[m].coeff[channel]} -
                                         at0[static_cast<std::size_t>(r)][o]) > kJumpTolerance) {
                                jump = true;
                            }
                        }
                    }
                }
                std::vector<bool> jumped(head.size(), false);
                if (jump) {
                    for (std::size_t m = 0; m < head.size(); ++m) {
                        const int r = mix_of[m];
                        for (std::size_t c = 0; c < kMatrixChannels; ++c) {
                            if (!head[m].present[c]) {
                                continue;
                            }
                            bool far = list_changed;
                            if (r >= 0 && c != head[m].out_ch && !list_changed) {
                                const auto& objects = mixes[static_cast<std::size_t>(r)].objects;
                                const auto o = static_cast<std::size_t>(
                                    std::ranges::find(objects,
                                                      static_cast<int>(c) - kFirstCodedRow) -
                                    objects.begin());
                                far =
                                    std::abs(std::int64_t{state.run[m].coeff[c]} -
                                             at0[static_cast<std::size_t>(r)][o]) > kJumpTolerance;
                            }
                            const std::int32_t raw = raw_for(m, c, far);
                            section.items[m].coeff[c] = raw;
                            next[m].coeff[c] = raw * (1 << place_bits(head[m]));
                        }
                    }
                } else {
                    for (std::size_t m = 0; m < head.size(); ++m) {
                        next[m] = state.run[m];
                    }
                }
                // the slopes: keep the standing ones that still reach the path at the unit's end
                bool slope_changed = list_changed;
                for (std::size_t m = 0; m < head.size(); ++m) {
                    const int r = mix_of[m];
                    next[m].extension = extension;
                    next[m].scale = slope_scale(head[m]);
                    if (list_changed) {
                        next[m].slope = {};
                    }
                    if (r < 0 || head[m].ext_width == 0) {
                        continue;
                    }
                    const int up = kObjectShift - next[m].scale;
                    const std::int64_t width_reach = std::int64_t{1} << head[m].ext_width;
                    for (std::size_t o = 0; o < mixes[static_cast<std::size_t>(r)].objects.size();
                         ++o) {
                        const auto channel = static_cast<std::size_t>(
                            kFirstCodedRow + mixes[static_cast<std::size_t>(r)].objects[o]);
                        const std::int64_t c = next[m].coeff[channel];
                        const std::int64_t target = at1[static_cast<std::size_t>(r)][o];
                        const std::int64_t standing = list_changed ? 0 : next[m].slope[channel];
                        const std::int64_t reaches = c + (standing << up);
                        if (!list_changed && std::abs(reaches - target) <= kSlopeTolerance) {
                            continue;
                        }
                        const std::int64_t move = target - c;
                        std::int64_t delta = 0;
                        if (std::abs(move) <= kStepLimit) {
                            delta = std::clamp(round_div(move, std::int64_t{1} << up), -width_reach,
                                               width_reach - 1);
                        }
                        if (delta != standing) {
                            slope_changed = true;
                        }
                        next[m].slope[channel] = static_cast<std::int32_t>(delta);
                    }
                }
                intended = next;
                if (jump || slope_changed || list_changed) {
                    send = true;
                    form.change = jump;
                    form.headers = list_changed;
                    form.extension = extension;
                    form.deltas = extension && slope_changed;
                    for (std::size_t m = 0; m < head.size(); ++m) {
                        const std::size_t before =
                            m < state.matrices.size() ? state.matrices[m].ext_width : 0;
                        const std::size_t before_field =
                            m < state.matrices.size() ? state.matrices[m].ext_field2 : 0;
                        if (head[m].ext_width != before || head[m].ext_field2 != before_field) {
                            form.widths = extension;
                        }
                        for (std::size_t c = 0; c < kMatrixChannels; ++c) {
                            if (head[m].present[c] && head[m].ext_width != 0) {
                                section.items[m].ext_value[c] = next[m].slope[c];
                            }
                        }
                    }
                    if (form.widths) {
                        form.deltas = true;  // C2 is inside B2
                    }
                }
            }
            if (send) {
                section.extension = form.extension;
                section.coeffs_sent = form.change;
                section.deltas_sent = form.deltas;
                state = apply_object_section(state, section);
                // the model of the decoder must give what the generator meant
                if (slopes) {
                    for (std::size_t m = 0; m < head.size(); ++m) {
                        if (state.run[m].coeff != intended[m].coeff ||
                            state.run[m].slope != intended[m].slope ||
                            state.run[m].extension != intended[m].extension) {
                            return std::unexpected(
                                "an object matrix section does not give the state planned");
                        }
                    }
                }
            } else if (state.matrices.empty()) {
                return std::unexpected("an object matrix section is missing at the restart");
            }
            timeline.blocks.emplace_back();
            timeline.blocks.back().send = send;
            timeline.blocks.back().section = section;
        } else {
            timeline.blocks.emplace_back();
        }
        ObjectBlock& block = timeline.blocks.back();
        block.start = start;
        block.size = size;
        if (unit_start) {
            for (ObjectRun& run : state.run) {
                run.ramp = 0;
            }
        }
        block.matrices = state.matrices;
        block.run = state.run;
        const bool ends_unit = (start + static_cast<std::size_t>(size)) % nominal == 0;
        for (std::size_t m = 0; m < state.matrices.size(); ++m) {
            advance(state.matrices[m], state.run[m], size, ends_unit, ramp_step);
        }
    }
    return timeline;
}

bool invert_object_timeline(const ObjectTimeline& timeline, const ObjectChainShape& shape,
                            std::vector<ObjectPlane>& rows, ObjectBypass& bypass) {
    const IntervalNoise noise(shape);
    const std::int32_t ramp_step = ramp_step_of(shape.nominal);
    const std::int64_t keep = ~((std::int64_t{1} << shape.quant) - 1);
    const std::size_t samples = rows.empty() ? 0 : rows.front().size();
    std::size_t slots = 0;
    for (const ObjectBlock& block : timeline.blocks) {
        slots = std::max<std::size_t>(
            slots, static_cast<std::size_t>(std::ranges::count_if(
                       block.matrices, [](const MatrixSpec& m) { return m.lsb_bypass; })));
    }
    bypass.assign(slots, std::vector<std::uint8_t>(samples, 0));
    for (const ObjectBlock& block : timeline.blocks) {
        const std::size_t count = block.matrices.size();
        std::vector<std::int64_t> own(count);
        std::vector<int> ordinal(count, -1);
        int next = 0;
        for (std::size_t m = 0; m < count; ++m) {
            own[m] = own_coefficient(block.matrices[m], block.run[m], rows.size());
            if (own[m] == 0 || (block.matrices[m].lsb_bypass && shape.quant != 0)) {
                return false;
            }
            if (block.matrices[m].lsb_bypass) {
                ordinal[m] = next++;
            }
        }
        for (int i = 0; i < block.size && block.start + static_cast<std::size_t>(i) < samples;
             ++i) {
            const std::size_t t = block.start + static_cast<std::size_t>(i);
            for (std::size_t m = count; m-- > 0;) {
                const MatrixSpec& matrix = block.matrices[m];
                const std::int64_t base =
                    matrix_value(matrix, block.run[m], rows, t, i, ramp_step, true,
                                 noise.term(m, count, matrix.field_m4, t));
                const std::int64_t after = rows[matrix.out_ch][t];
                std::int64_t before = 0;
                if (matrix.lsb_bypass) {
                    // after = 2^n * before + base + bits: the bits are the low bits of what is left
                    const int n = bypass_bits_of(matrix);
                    const std::int64_t rest = after - base;
                    before = rest >> n;
                    bypass[static_cast<std::size_t>(ordinal[m])][t] =
                        static_cast<std::uint8_t>(rest & ((std::int64_t{1} << n) - 1));
                } else if (own[m] > 0) {
                    before = after - (base & keep);
                } else {
                    before = (base & keep) - after;
                }
                rows[matrix.out_ch][t] = static_cast<std::int32_t>(before);
            }
        }
    }
    return true;
}

void apply_object_timeline(const ObjectTimeline& timeline, const ObjectChainShape& shape,
                           const ObjectBypass& bypass, std::vector<ObjectPlane>& rows) {
    const IntervalNoise noise(shape);
    const std::int32_t ramp_step = ramp_step_of(shape.nominal);
    const std::int64_t keep = ~((std::int64_t{1} << shape.quant) - 1);
    const std::size_t samples = rows.empty() ? 0 : rows.front().size();
    for (const ObjectBlock& block : timeline.blocks) {
        const std::size_t count = block.matrices.size();
        for (int i = 0; i < block.size && block.start + static_cast<std::size_t>(i) < samples;
             ++i) {
            const std::size_t t = block.start + static_cast<std::size_t>(i);
            std::size_t next = 0;
            for (std::size_t m = 0; m < count; ++m) {
                const MatrixSpec& matrix = block.matrices[m];
                std::int64_t value = matrix_value(matrix, block.run[m], rows, t, i, ramp_step,
                                                  false, noise.term(m, count, matrix.field_m4, t)) &
                                     keep;
                if (matrix.lsb_bypass) {
                    value += bypass[next++][t];
                }
                rows[matrix.out_ch][t] = static_cast<std::int32_t>(value);
            }
        }
    }
}

namespace {

// One block over all of `rows` for a chain of matrices that do not move.
ObjectTimeline chain_timeline(const std::vector<MatrixSpec>& chain, std::size_t samples) {
    ObjectBlock block;
    block.size = static_cast<int>(samples);
    block.matrices = chain;
    for (const MatrixSpec& matrix : chain) {
        ObjectRun run;
        for (std::size_t c = 0; c < kMatrixChannels; ++c) {
            if (matrix.present[c]) {
                run.coeff[c] = matrix.coeff[c] * (1 << std::max(place_bits(matrix), 0));
            }
        }
        block.run.push_back(run);
    }
    ObjectTimeline timeline;
    timeline.blocks.push_back(std::move(block));
    return timeline;
}

}  // namespace

bool invert_object_chain(const std::vector<MatrixSpec>& chain, const ObjectChainShape& shape,
                         std::vector<ObjectPlane>& rows, ObjectBypass& bypass) {
    const std::size_t samples = rows.empty() ? 0 : rows.front().size();
    return invert_object_timeline(chain_timeline(chain, samples), shape, rows, bypass);
}

void apply_object_chain(const std::vector<MatrixSpec>& chain, const ObjectChainShape& shape,
                        const ObjectBypass& bypass, std::vector<ObjectPlane>& rows) {
    const std::size_t samples = rows.empty() ? 0 : rows.front().size();
    apply_object_timeline(chain_timeline(chain, samples), shape, bypass, rows);
}

void put_object_bypass(BitWriter& writer, const ObjectTimeline& timeline,
                       const ObjectBypass& bypass, std::size_t block, int i) {
    const ObjectBlock& b = timeline.blocks[block];
    const std::size_t t = b.start + static_cast<std::size_t>(i);
    std::size_t next = 0;
    for (const MatrixSpec& matrix : b.matrices) {
        if (matrix.lsb_bypass) {
            writer.put(bypass[next++][t], bypass_bits_of(matrix));
        }
    }
}

std::size_t object_matrices_used(const ObjectTimeline& timeline) noexcept {
    std::size_t used = 0;
    for (const ObjectBlock& block : timeline.blocks) {
        used = std::max(used, block.matrices.size());
    }
    return used;
}

// --- a general downmix of the objects into the bed -------------------------------------------------

namespace {

constexpr int kChainFracBits = 14;  // the precision of a coefficient, 2^-14 (TH spec 10.2.1d)
constexpr int kChainMaxShift = 4;   // f_shift 4 holds up to +-31.99, the bound DEE keeps to
constexpr std::int32_t kChainMaxRaw = 32767;
// A pivot this near 1 needs no tuning primitive (a coefficient is 2^-14 = 6.1e-5 coarse).
constexpr double kChainPivotTolerance = 5e-5;
// The pivot of a row after its tuning primitive, quantised, may be this far from 1.
constexpr double kChainPivotLimit = 2e-3;
// TH spec 10.2.1d: a row joins the shared group only if the pivot moves by at least this.
constexpr double kChainGroupSlope = 0.05;
// A realised bed row is this near the wanted one (the 2^-14 coefficients accumulate along the chain).
constexpr double kChainReach = 1e-3;
constexpr double kChainNegligible = 1e-9;

using Form = std::vector<double>;  // a row as a linear form: its coefficient on each element

// An encoder-side primitive: row[out] += sum of value(term) * row[term row]. The decoder's matrix is
// its inverse: own coefficient +1 and every other coefficient negated.
struct ChainOp {
    int out = 0;
    int f_shift = 0;
    std::vector<std::pair<int, std::int32_t>> terms;  // (row, raw at 2^-(14 - f_shift))

    [[nodiscard]] double value(const std::pair<int, std::int32_t>& term) const {
        return std::ldexp(static_cast<double>(term.second), f_shift - kChainFracBits);
    }
};

// Rounds the coefficients to the finest grid in which every one fits (TH spec 10.2.1d,
// "Coefficient quantisation": f_shift is the smallest that holds the largest).
std::optional<ChainOp> quantise_op(int out, const std::vector<std::pair<int, double>>& wanted) {
    for (int shift = 0; shift <= kChainMaxShift; ++shift) {
        const double scale = std::ldexp(1.0, kChainFracBits - shift);
        ChainOp op;
        op.out = out;
        op.f_shift = shift;
        bool fits = true;
        for (const auto& [row, gain] : wanted) {
            const double raw = std::round(gain * scale);
            if (!(std::abs(raw) <= kChainMaxRaw)) {
                fits = false;
                break;
            }
            if (raw != 0.0) {
                op.terms.emplace_back(row, static_cast<std::int32_t>(raw));
            }
        }
        if (fits) {
            return op;
        }
    }
    return std::nullopt;
}

void apply_op(std::vector<Form>& forms, const ChainOp& op) {
    Form& out = forms[static_cast<std::size_t>(op.out)];
    for (const auto& term : op.terms) {
        const double gain = op.value(term);
        const Form& source = forms[static_cast<std::size_t>(term.first)];
        for (std::size_t e = 0; e < out.size(); ++e) {
            out[e] += gain * source[e];
        }
    }
}

// The coefficients m with sum_t m[t] * forms[t] = target; the forms are always independent (the
// chain is a product of elementary matrices), so this only fails on a numerical accident.
std::optional<std::vector<double>> coordinates(const std::vector<Form>& forms,
                                               const Form& target) {
    const std::size_t n = forms.size();
    std::vector<std::vector<double>> system(n, std::vector<double>(n + 1, 0.0));
    for (std::size_t e = 0; e < n; ++e) {
        for (std::size_t t = 0; t < n; ++t) {
            system[e][t] = forms[t][e];
        }
        system[e][n] = target[e];
    }
    for (std::size_t column = 0; column < n; ++column) {
        std::size_t best = column;
        for (std::size_t row = column + 1; row < n; ++row) {
            if (std::abs(system[row][column]) > std::abs(system[best][column])) {
                best = row;
            }
        }
        if (std::abs(system[best][column]) < 1e-12) {
            return std::nullopt;
        }
        std::swap(system[best], system[column]);
        for (std::size_t row = 0; row < n; ++row) {
            if (row == column) {
                continue;
            }
            const double factor = system[row][column] / system[column][column];
            if (factor == 0.0) {
                continue;
            }
            for (std::size_t k = column; k <= n; ++k) {
                system[row][k] -= factor * system[column][k];
            }
        }
    }
    std::vector<double> m(n);
    for (std::size_t t = 0; t < n; ++t) {
        m[t] = system[t][n] / system[t][t];
    }
    return m;
}

// The gain of the overlap primitive that gives a row with no other content something to tune
// against: the product of it and the tuning gain is (1 - c) / c, spread over the two.
double overlap_gain(double c) {
    return std::clamp(std::sqrt(std::abs(1.0 - c) / std::abs(c)), 1.0, 8.0);
}

// The gain of `target` when it is one scaled element `element` and nothing else.
std::optional<double> single_gain(const Form& target, int element) {
    for (std::size_t e = 0; e < target.size(); ++e) {
        if (static_cast<int>(e) != element && std::abs(target[e]) > kChainNegligible) {
            return std::nullopt;
        }
    }
    const double gain = target[static_cast<std::size_t>(element)];
    return std::abs(gain) > kChainNegligible ? std::optional<double>(gain) : std::nullopt;
}

constexpr std::string_view kProblemLinear =
    "object_downmix: a bed row is a combination of rows that are already finished (the rows of "
    "the downmix must be independent; only a row that is exactly zero is allowed to depend on "
    "nothing)";
constexpr std::string_view kProblemRange =
    "object_downmix: the chain needs a coefficient of 32 or more (a gain this small or a downmix "
    "this ill-conditioned does not fit the matrices)";
constexpr std::string_view kProblemReach =
    "object_downmix: the chain does not reach the downmix to within 1e-3";

// One design of the chain: forms of the 16 rows, the rows that are finished (the bed rows already
// equal to their target, which no primitive may write again) and the primitives so far, in the order
// the encoder applies them.
struct ChainDesign {
    int n = 0;
    const std::vector<Form>* target = nullptr;  // the bed rows 0..7
    std::vector<Form> forms;
    std::vector<bool> finished;
    std::vector<ChainOp> ops;
    std::string_view problem;

    [[nodiscard]] int spare() const noexcept { return n - 1; }

    bool fail(std::string_view why) {
        if (problem.empty()) {
            problem = why;
        }
        return false;
    }

    bool emit(int out, const std::vector<std::pair<int, double>>& wanted) {
        auto op = quantise_op(out, wanted);
        if (!op) {
            return fail(kProblemRange);
        }
        apply_op(forms, *op);
        ops.push_back(std::move(*op));
        return true;
    }

    // The row to tune against: an unfinished row, not `row`, with the largest coordinate, and one
    // that gets the pivot to 1 with a coefficient under 30 (a row that shares only the rounding of
    // the chain with the target would do it with a coefficient in the thousands).
    [[nodiscard]] int tuning_row(const std::vector<double>& m, int row) const {
        const double smallest =
            std::max(1e-6, std::abs(m[static_cast<std::size_t>(row)] - 1.0) / 30.0);
        int best = -1;
        for (int t = 0; t < n; ++t) {
            if (t == row || finished[static_cast<std::size_t>(t)] ||
                std::abs(m[static_cast<std::size_t>(t)]) < smallest) {
                continue;
            }
            if (best < 0 || std::abs(m[static_cast<std::size_t>(t)]) >
                                std::abs(m[static_cast<std::size_t>(best)])) {
                best = t;
            }
        }
        return best;
    }

    // The main primitive of a row: its pivot coordinate is 1, so adding the other coordinates'
    // rows makes it the target.
    bool main_primitive(int row) {
        const Form& want = (*target)[static_cast<std::size_t>(row)];
        const auto m = coordinates(forms, want);
        if (!m) {
            return fail(kProblemRange);
        }
        std::vector<std::pair<int, double>> wanted;
        for (int t = 0; t < n; ++t) {
            if (t != row && std::abs((*m)[static_cast<std::size_t>(t)]) > kChainNegligible) {
                wanted.emplace_back(t, (*m)[static_cast<std::size_t>(t)]);
            }
        }
        if (!wanted.empty() && !emit(row, wanted)) {
            return false;
        }
        finished[static_cast<std::size_t>(row)] = true;
        const Form& got = forms[static_cast<std::size_t>(row)];
        for (std::size_t e = 0; e < got.size(); ++e) {
            if (std::abs(got[e] - want[e]) > kChainReach) {
                return fail(kProblemReach);
            }
        }
        return true;
    }

    // A row with its pivot made 1 (a tuning primitive, after an overlap one when no other
    // unfinished row shares its content), then its main primitive.
    bool finish_row(int row) {
        const Form& want = (*target)[static_cast<std::size_t>(row)];
        auto m = coordinates(forms, want);
        if (!m) {
            return fail(kProblemRange);
        }
        if (std::abs((*m)[static_cast<std::size_t>(row)] - 1.0) >= kChainPivotTolerance) {
            int u = tuning_row(*m, row);
            if (u < 0) {
                const double c = (*m)[static_cast<std::size_t>(row)];
                if (std::abs(c) < 1e-3) {
                    return fail(kProblemLinear);
                }
                if (!emit(row, {{spare(), overlap_gain(c)}})) {
                    return false;
                }
                m = coordinates(forms, want);
                if (!m) {
                    return fail(kProblemRange);
                }
                u = tuning_row(*m, row);
                if (u < 0) {
                    return fail(kProblemRange);
                }
            }
            const double gain = ((*m)[static_cast<std::size_t>(row)] - 1.0) /
                                (*m)[static_cast<std::size_t>(u)];
            if (!emit(u, {{row, gain}})) {
                return false;
            }
            m = coordinates(forms, want);
            if (!m || std::abs((*m)[static_cast<std::size_t>(row)] - 1.0) > kChainPivotLimit) {
                return fail(kProblemRange);
            }
        }
        return main_primitive(row);
    }
};

// The pivot of `row` once the shared primitive has added `x` times `row` and the earlier members'
// multiples to the spare and the earlier members have been finished (not quantised: a replay of
// the sequence for the affine search of TH spec 10.2.1d).
std::optional<double> replay_pivot(const std::vector<Form>& start, const std::vector<Form>& target,
                                   int spare, const std::vector<int>& group,
                                   const std::vector<double>& multiples, int row, double x) {
    std::vector<Form> forms = start;
    const auto add_to_spare = [&](int member, double multiple) {
        const Form source = forms[static_cast<std::size_t>(member)];
        for (std::size_t e = 0; e < source.size(); ++e) {
            forms[static_cast<std::size_t>(spare)][e] += multiple * source[e];
        }
    };
    for (std::size_t i = 0; i < group.size(); ++i) {
        add_to_spare(group[i], multiples[i]);
    }
    add_to_spare(row, x);
    for (const int member : group) {
        const auto m = coordinates(forms, target[static_cast<std::size_t>(member)]);
        if (!m) {
            return std::nullopt;
        }
        Form sum = forms[static_cast<std::size_t>(member)];
        for (std::size_t t = 0; t < forms.size(); ++t) {
            if (static_cast<int>(t) == member) {
                continue;
            }
            for (std::size_t e = 0; e < sum.size(); ++e) {
                sum[e] += (*m)[t] * forms[t][e];
            }
        }
        forms[static_cast<std::size_t>(member)] = sum;
    }
    const auto m = coordinates(forms, target[static_cast<std::size_t>(row)]);
    if (!m) {
        return std::nullopt;
    }
    return (*m)[static_cast<std::size_t>(row)];
}

struct ChainResult {
    std::vector<ChainOp> ops;
    int group = 0;
};

// The chain of TH spec 10.2.1d for the targets (the bed rows), starting from the forms of the
// positions (one element each). With `shared`, the single-element rows are tuned together.
std::expected<ChainResult, std::string_view> design_chain(const std::vector<Form>& target,
                                                          const std::vector<Form>& start,
                                                          const std::vector<bool>& finished,
                                                          const std::vector<int>& element_at,
                                                          bool shared) {
    ChainDesign design;
    design.n = static_cast<int>(start.size());
    design.target = &target;
    design.forms = start;
    design.finished = finished;
    ChainResult result;

    if (shared) {
        std::vector<int> singles;
        for (int r = 0; r < kBedChannels; ++r) {
            if (!design.finished[static_cast<std::size_t>(r)] &&
                single_gain(target[static_cast<std::size_t>(r)],
                            element_at[static_cast<std::size_t>(r)])) {
                singles.push_back(r);
            }
        }
        if (singles.size() >= 2) {
            for (const int r : singles) {
                const double c = *single_gain(target[static_cast<std::size_t>(r)],
                                              element_at[static_cast<std::size_t>(r)]);
                if (!design.emit(r, {{design.spare(), overlap_gain(c)}})) {
                    return std::unexpected(design.problem);
                }
            }
            const std::vector<Form> base = design.forms;
            const int spare = design.spare();
            // pass 1: which singles the shared primitive tunes, and by how much
            std::vector<int> group;
            std::vector<double> multiples;
            for (const int r : singles) {
                const auto p0 = replay_pivot(base, target, spare, group, multiples, r, 0.0);
                const auto p1 = replay_pivot(base, target, spare, group, multiples, r, 1.0);
                if (!p0 || !p1 || std::abs(*p1 - *p0) < kChainGroupSlope) {
                    continue;
                }
                group.push_back(r);
                multiples.push_back((1.0 - *p0) / (*p1 - *p0));
            }
            if (!group.empty()) {
                // pass 2: the multiples on the grid the shared primitive will have
                double biggest = 0.0;
                for (const double x : multiples) {
                    biggest = std::max(biggest, std::abs(x));
                }
                int shift = 0;
                while (shift < kChainMaxShift &&
                       std::round(biggest * std::ldexp(1.0, kChainFracBits - shift)) >
                           kChainMaxRaw) {
                    ++shift;
                }
                const double scale = std::ldexp(1.0, kChainFracBits - shift);
                std::vector<int> members;
                std::vector<double> quantised;
                for (const int r : group) {
                    const auto p0 = replay_pivot(base, target, spare, members, quantised, r, 0.0);
                    const auto p1 = replay_pivot(base, target, spare, members, quantised, r, 1.0);
                    if (!p0 || !p1 || std::abs(*p1 - *p0) < kChainGroupSlope) {
                        return std::unexpected(kProblemRange);
                    }
                    const double x = std::round((1.0 - *p0) / (*p1 - *p0) * scale) / scale;
                    if (std::abs(x * scale) > kChainMaxRaw) {
                        return std::unexpected(kProblemRange);
                    }
                    members.push_back(r);
                    quantised.push_back(x);
                }
                std::vector<std::pair<int, double>> wanted;
                for (std::size_t i = 0; i < members.size(); ++i) {
                    wanted.emplace_back(members[i], quantised[i]);
                }
                if (!design.emit(spare, wanted)) {
                    return std::unexpected(design.problem);
                }
                for (const int r : members) {
                    if (!design.main_primitive(r)) {
                        return std::unexpected(design.problem);
                    }
                }
                result.group = static_cast<int>(members.size());
            }
        }
    }
    for (int r = 0; r < kBedChannels; ++r) {
        if (!design.finished[static_cast<std::size_t>(r)] && !design.finish_row(r)) {
            return std::unexpected(design.problem);
        }
    }
    result.ops = std::move(design.ops);
    return result;
}

}  // namespace

std::expected<DownmixChain, std::string_view> build_downmix_chain(std::span<const double> downmix,
                                                                  int objects, int noise_shift) {
    if (objects <= kBedChannels || objects > kMaxChannels) {
        return std::unexpected("the object downmix needs 9 to 16 channels");
    }
    const auto n = static_cast<std::size_t>(objects);
    if (downmix.size() != static_cast<std::size_t>(kBedChannels) * n) {
        return std::unexpected("object_downmix is 8 rows (L R C LFE Ls Rs Lrs Rrs) of object_channels gains");
    }
    if (noise_shift < 0 || noise_shift > 15) {
        return std::unexpected("an object matrix parameter is out of range");
    }
    std::vector<Form> target(kBedChannels, Form(n, 0.0));
    for (std::size_t r = 0; r < static_cast<std::size_t>(kBedChannels); ++r) {
        for (std::size_t e = 0; e < n; ++e) {
            const double gain = downmix[r * n + e];
            if (!std::isfinite(gain) || std::abs(gain) > 4.0) {
                return std::unexpected("an object_downmix gain is not a number within +-4");
            }
            target[r][e] = gain;
        }
    }

    // Which element sits in which matrix channel (ch_assign): each bed row takes the element of
    // its largest gain, the rows with the largest gains choosing first; the rest fill in, zero
    // rows (which keep the element as it is) and the coded rows last.
    std::vector<int> element_at(n, -1);
    std::vector<bool> taken(n, false);
    std::vector<bool> zero(kBedChannels, false);
    std::vector<int> order;
    for (int r = 0; r < kBedChannels; ++r) {
        const Form& row = target[static_cast<std::size_t>(r)];
        const double largest = *std::ranges::max_element(row, {}, [](double g) { return std::abs(g); });
        zero[static_cast<std::size_t>(r)] = std::abs(largest) < kChainNegligible;
        if (!zero[static_cast<std::size_t>(r)]) {
            order.push_back(r);
        }
    }
    const auto largest_of = [&](int r) {
        double largest = 0.0;
        for (const double g : target[static_cast<std::size_t>(r)]) {
            largest = std::max(largest, std::abs(g));
        }
        return largest;
    };
    std::ranges::stable_sort(order, [&](int a, int b) { return largest_of(a) > largest_of(b); });
    for (const int r : order) {
        int best = -1;
        for (int e = 0; e < objects; ++e) {
            const double gain = std::abs(target[static_cast<std::size_t>(r)][static_cast<std::size_t>(e)]);
            if (taken[static_cast<std::size_t>(e)] || gain < kChainNegligible) {
                continue;
            }
            if (best < 0 ||
                gain > std::abs(target[static_cast<std::size_t>(r)][static_cast<std::size_t>(best)])) {
                best = e;
            }
        }
        if (best >= 0) {
            element_at[static_cast<std::size_t>(r)] = best;
            taken[static_cast<std::size_t>(best)] = true;
        }
    }
    int next_free = 0;
    for (std::size_t position = 0; position < n; ++position) {
        if (element_at[position] >= 0) {
            continue;
        }
        while (taken[static_cast<std::size_t>(next_free)]) {
            ++next_free;
        }
        element_at[position] = next_free;
        taken[static_cast<std::size_t>(next_free)] = true;
    }

    std::vector<Form> start(n, Form(n, 0.0));
    for (std::size_t position = 0; position < n; ++position) {
        start[position][static_cast<std::size_t>(element_at[position])] = 1.0;
    }
    // A bed row that is already its target needs no primitive: the zero rows (the lower
    // substreams zero them, the chain leaves the element in the row) and the unit rows.
    std::vector<bool> finished(n, false);
    std::vector<int> zero_rows;
    int singles = 0;
    for (int r = 0; r < kBedChannels; ++r) {
        const auto row = static_cast<std::size_t>(r);
        if (zero[row]) {
            finished[row] = true;
            zero_rows.push_back(r);
            continue;
        }
        bool unit = true;
        for (std::size_t e = 0; e < n; ++e) {
            unit = unit && std::abs(target[row][e] - start[row][e]) < kChainNegligible;
        }
        finished[row] = unit;
        if (!unit && single_gain(target[row], element_at[row])) {
            ++singles;
        }
    }

    auto individual = design_chain(target, start, finished, element_at, false);
    auto shared = design_chain(target, start, finished, element_at, true);
    const ChainResult* chosen = nullptr;
    if (individual && (!shared || individual->ops.size() <= shared->ops.size())) {
        chosen = &*individual;
    } else if (shared) {
        chosen = &*shared;
    }
    if (chosen == nullptr) {
        return std::unexpected(individual.error());
    }
    if (chosen->ops.size() > static_cast<std::size_t>(kMaxObjectMatrices)) {
        return std::unexpected(
            singles >= kBedChannels
                ? "object_downmix: eight single-element rows need 17 primitive matrices and "
                  "substream 3 carries at most 16"
                : "object_downmix: the chain needs more than the 16 primitive matrices substream 3 "
                  "carries");
    }

    DownmixChain chain;
    chain.zero_rows = std::move(zero_rows);
    chain.single_rows = singles;
    chain.group_rows = chosen->group;
    for (std::size_t position = 0; position < n; ++position) {
        chain.assign[position] = static_cast<std::uint8_t>(element_at[position]);
    }
    // the decoder's chain: the encoder's primitives last to first, each inverted
    for (std::size_t i = chosen->ops.size(); i-- > 0;) {
        const ChainOp& op = chosen->ops[i];
        MatrixSpec matrix;
        matrix.out_ch = static_cast<std::uint8_t>(op.out);
        matrix.frac_bits = kChainFracBits;
        matrix.f_shift = static_cast<std::int8_t>(op.f_shift);
        matrix.field_m4 = static_cast<std::uint8_t>(noise_shift);
        matrix.present[static_cast<std::size_t>(op.out)] = true;
        matrix.coeff[static_cast<std::size_t>(op.out)] = 1 << (kChainFracBits - op.f_shift);
        for (const auto& [row, raw] : op.terms) {
            matrix.present[static_cast<std::size_t>(row)] = true;
            matrix.coeff[static_cast<std::size_t>(row)] = -raw;
        }
        chain.matrices.push_back(matrix);
    }
    return chain;
}

ObjectTimeline fixed_chain_timeline(const std::vector<MatrixSpec>& chain,
                                    const std::vector<std::pair<std::size_t, int>>& blocks) {
    MatrixSet section;
    section.count = static_cast<std::uint8_t>(chain.size());
    for (std::size_t m = 0; m < chain.size(); ++m) {
        section.items[m] = chain[m];
    }
    section.extension = false;
    section.coeffs_sent = true;
    section.deltas_sent = false;
    const ObjectState state = apply_object_section(ObjectState{}, section);
    ObjectTimeline timeline;
    for (std::size_t j = 0; j < blocks.size(); ++j) {
        ObjectBlock& block = timeline.blocks.emplace_back();
        block.start = blocks[j].first;
        block.size = blocks[j].second;
        block.matrices = state.matrices;
        block.run = state.run;
        if (j == 0) {
            block.send = true;
            block.section = section;
        }
    }
    return timeline;
}

}  // namespace iclforge::truehd::detail
