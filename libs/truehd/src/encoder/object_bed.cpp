#include "encoder/object_bed.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <iterator>
#include <ranges>

#include "core/noise.hpp"
#include "decoder/kernels.hpp"

namespace iclforge::truehd::detail {

namespace {

constexpr int kMatrixShift = kernels::kMatrixShift;
constexpr int kFirstCodedRow = kBedChannels;
constexpr int kMixFracBits = 12;
constexpr std::int64_t kMixPlaces = std::int64_t{1} << kMixFracBits;  // coefficient 1.0 as sent
constexpr std::int64_t kGainOne = std::int64_t{1} << kObjectGainBits;
constexpr std::int32_t kMaxGain = 130000;  // +-1.98: the coefficient is 14 bits, signed, at 2^-12
constexpr std::int32_t kMaxDelta = (1 << kMaxDeltaWidth) - 1;
constexpr std::int32_t kMinDelta = -(1 << kMaxDeltaWidth);

// A coefficient as the decoder scales it, to 2^-kMatrixShift: raw / 2^(frac_bits - f_shift).
[[nodiscard]] int coefficient_scale(const MatrixSpec& matrix) noexcept {
    return kMatrixShift - matrix.frac_bits + matrix.f_shift;
}

[[nodiscard]] std::int64_t scaled_coefficient(const MatrixSpec& matrix,
                                              std::size_t channel) noexcept {
    return std::int64_t{matrix.coeff[channel]} * (std::int64_t{1} << coefficient_scale(matrix));
}

// The number of bypass bits a sample of a matrix (0 when it has none).
[[nodiscard]] int bypass_bits_of(const MatrixSpec& matrix) noexcept {
    return matrix.lsb_bypass ? std::max<int>(matrix.field_2b, 1) : 0;
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

    // What the matrix at `index` of `count` adds to its sum (at 2^-kMatrixShift) for sample `t` of
    // the interval: the decoder starts the access unit at the index of the matrices left (this one
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
               (std::int64_t{1} << (shift + kernels::kNoiseScaleShift));
    }

   private:
    std::size_t size_;
    std::size_t nominal_;
    std::vector<std::int8_t> buffer_;
};

// What the decoder's kernels::apply_matrix makes of one matrix at sample `t`, before the
// quantiser step's mask and the bypass bits: the channels it reads times their coefficients, less
// the matrix's own channel when `skip_own`, plus its noise. With the ramp and a width the deltas'
// part is scaled by the sample's place in the block and added at kRampScaleBits finer
// (SUBSTREAM3.md section 6, provisional): kEnd weighs sample `phase` of `size` by (phase + 1) /
// size, kStart by phase / size, the product divided toward zero as the kernel does.
[[nodiscard]] std::int64_t matrix_value(const MatrixSpec& matrix,
                                        const std::vector<ObjectPlane>& rows, std::size_t t,
                                        int phase, int size, ObjectRamp ramp, bool skip_own,
                                        std::int64_t noise) noexcept {
    const bool fine = ramp != ObjectRamp::kNone && matrix.ext_width != 0;
    const std::int64_t place = std::int64_t{1} << coefficient_scale(matrix);
    std::int64_t sum = noise;
    std::int64_t finer = 0;
    for (std::size_t c = 0; c < rows.size(); ++c) {
        if (!matrix.present[c] || (skip_own && c == matrix.out_ch)) {
            continue;
        }
        const std::int64_t row = rows[c][t];
        sum += row * matrix.coeff[c] * place;
        if (fine) {
            finer += row * matrix.ext_value[c] * place;
        }
    }
    if (!fine) {
        return sum >> kMatrixShift;
    }
    const std::int64_t numerator = ramp == ObjectRamp::kEnd ? phase + 1 : phase;
    finer = finer * numerator / size;
    return ((sum << kRampScaleBits) + finer) >> (kMatrixShift + kRampScaleBits);
}

// The matrix is primitive when its own coefficient is +1.0 or -1.0 (or 2^n with the n bits of LSB
// bypass that give back the low bits the multiplication loses) and carries no delta, the one thing
// that makes it invertible in integers. Returns the own coefficient scaled to 2^-kMatrixShift, or 0
// if the matrix is not primitive.
[[nodiscard]] std::int64_t own_coefficient(const MatrixSpec& matrix, std::size_t rows) noexcept {
    if (matrix.out_ch >= rows || !matrix.present[matrix.out_ch] ||
        matrix.frac_bits - matrix.f_shift > kMatrixShift || matrix.frac_bits < matrix.f_shift ||
        matrix.ext_value[matrix.out_ch] != 0) {
        return 0;
    }
    const std::int64_t own = scaled_coefficient(matrix, matrix.out_ch);
    const std::int64_t unit = std::int64_t{1} << kMatrixShift;
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

// A matrix as the decoder holds it: the deltas of a matrix without a width are not used.
[[nodiscard]] MatrixSpec held(MatrixSpec matrix) noexcept {
    if (matrix.ext_width == 0) {
        matrix.ext_value = {};
    }
    return matrix;
}

[[nodiscard]] bool same_set(const std::vector<MatrixSpec>& a, const std::vector<MatrixSpec>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t m = 0; m < a.size(); ++m) {
        if (held(a[m]) != held(b[m])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool has_delta(const std::vector<MatrixSpec>& set) noexcept {
    return std::ranges::any_of(set, [](const MatrixSpec& matrix) {
        return matrix.ext_width != 0 &&
               std::ranges::any_of(matrix.ext_value, [](std::int32_t v) { return v != 0; });
    });
}

// --- a path's coefficients ----------------------------------------------------------------------

struct Coefficient {
    std::int32_t raw = 0;
    std::int32_t delta = 0;
};

// The coefficient on an object channel of the mixing matrix of a bed row for a gain that is
// `start` at the block's first anchor and `end` at its second: raw is the nearest whole number to
// minus the gain at 2^-kMixFracBits, and with a ramp the delta is eight times how far the end of
// the block is from raw (the nearest whole number), so that raw + delta / 8 is the end. The gains
// are scaled by 2^-mix_step; the last step mixes nothing.
[[nodiscard]] Coefficient trajectory_coefficient(const GainFraction& start, const GainFraction& end,
                                                 int mix_step, bool ramp) noexcept {
    Coefficient out;
    if (mix_step >= kObjectMixSteps - 1) {
        return out;
    }
    const std::int64_t halves = std::int64_t{1} << mix_step;
    out.raw = static_cast<std::int32_t>(
        round_div(-start.numerator * kMixPlaces, start.denominator * kGainOne * halves));
    if (ramp) {
        const std::int64_t denominator = end.denominator * kGainOne * halves;
        const std::int64_t eighths = std::int64_t{1} << kRampScaleBits;
        const std::int64_t delta = round_div(
            -eighths * end.numerator * kMixPlaces - eighths * out.raw * denominator, denominator);
        out.delta =
            static_cast<std::int32_t>(std::clamp<std::int64_t>(delta, kMinDelta, kMaxDelta));
    }
    return out;
}

// The smallest width whose deltas (width + 1 bits, signed) hold every one of `deltas`; 0 when
// they are all 0.
[[nodiscard]] int width_of(const std::vector<std::int32_t>& deltas) noexcept {
    int width = 0;
    for (const std::int32_t d : deltas) {
        const auto magnitude = static_cast<std::uint32_t>(d >= 0 ? d : -(d + 1));
        width = std::max(width, d == 0 ? 0 : std::max(1, std::bit_width(magnitude)));
    }
    return width;
}

struct MixRow {
    int row = 0;               // the bed row the matrix writes
    std::vector<int> objects;  // the objects (from 0 = channel 8) with a gain into it
    int width = 0;             // the extension width over the interval
};

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

std::vector<MatrixSpec> apply_object_section(const std::vector<MatrixSpec>& before,
                                             const MatrixSet& section) {
    const ObjectSectionForm& form = section.form;
    std::vector<MatrixSpec> state = before;
    if (section.count == 0 && form.change) {
        return state;
    }
    const std::size_t count = section.count;
    if (form.change) {
        std::vector<MatrixSpec> fresh;
        for (std::size_t m = 0; m < count; ++m) {
            MatrixSpec matrix;
            if (form.headers) {
                matrix = section.items[m];
                matrix.coeff = {};
                matrix.ext_value = {};
                matrix.ext_width = m < before.size() ? before[m].ext_width : 0;
                matrix.ext_field2 = m < before.size() ? before[m].ext_field2 : 0;
            } else {
                matrix = before.at(m);
                matrix.coeff = {};
                matrix.ext_value = {};
            }
            for (std::size_t c = 0; c < matrix.present.size(); ++c) {
                if (matrix.present[c]) {
                    matrix.coeff[c] = section.items[m].coeff[c];
                }
            }
            fresh.push_back(matrix);
        }
        state = std::move(fresh);
    }
    if (form.deltas) {
        for (std::size_t m = 0; m < count; ++m) {
            if (form.widths) {
                state[m].ext_width = section.items[m].ext_width;
                state[m].ext_field2 = section.items[m].ext_field2;
            }
        }
        for (std::size_t m = 0; m < count; ++m) {
            if (state[m].ext_width == 0) {
                continue;
            }
            for (std::size_t c = 0; c < state[m].present.size(); ++c) {
                if (state[m].present[c]) {
                    state[m].ext_value[c] = section.items[m].ext_value[c];
                }
            }
        }
    }
    return state;
}

std::pair<bool, MatrixSet> plan_object_section(const std::vector<MatrixSpec>& before,
                                               const std::vector<MatrixSpec>& after) {
    MatrixSet set;
    if (same_set(before, after)) {
        return {false, set};
    }
    set.count = static_cast<std::uint8_t>(after.size());
    for (std::size_t m = 0; m < after.size(); ++m) {
        set.items[m] = after[m];
    }
    const bool headers_same =
        !before.empty() && before.size() == after.size() &&
        std::ranges::all_of(std::views::iota(std::size_t{0}, after.size()),
                            [&](std::size_t m) { return same_headers(before[m], after[m]); });
    ObjectSectionForm& form = set.form;
    const auto widths_differ = [&](const std::vector<MatrixSpec>& reference) {
        for (std::size_t m = 0; m < after.size(); ++m) {
            const int held_width = m < reference.size() ? reference[m].ext_width : 0;
            if (held_width != after[m].ext_width) {
                return true;
            }
        }
        return false;
    };
    bool coefficients_same = headers_same;
    for (std::size_t m = 0; coefficients_same && m < after.size(); ++m) {
        coefficients_same = before[m].coeff == after[m].coeff;
    }
    if (coefficients_same) {
        // A clear: the set stands, the widths and deltas are what changed.
        form.change = false;
        form.deltas = true;
        form.widths = widths_differ(before);
    } else if (headers_same) {
        // B clear: the headers stand; new coefficients clear the deltas.
        form.headers = false;
        form.widths = widths_differ(before);
        form.deltas = form.widths || has_delta(after);
    } else {
        // New headers keep the widths of the matrix at the same place.
        form.widths = widths_differ(before);
        form.deltas = form.widths || has_delta(after);
    }
    return {true, set};
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
    const bool ramp = request.ramp != ObjectRamp::kNone;
    if (ramp && request.update_units != 1) {
        return std::unexpected("the ramp needs an update at every block");
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

    // The coefficients of every block: [block][mix][object of the mix].
    const std::size_t block_count = request.blocks.size();
    std::vector<std::vector<std::vector<Coefficient>>> coefficients(block_count);
    for (std::size_t j = 0; j < block_count; ++j) {
        const std::size_t start = request.blocks[j].first;
        const int size = request.blocks[j].second;
        // The updates fall on the stream's access units, not the interval's, so that where a path
        // steps does not depend on where the restarts are.
        const auto first = request.first_sample + static_cast<std::int64_t>(start);
        const bool update =
            ramp || j == 0 ||
            (first % request.nominal == 0 && (first / request.nominal) % request.update_units == 0);
        if (!update) {
            coefficients[j] = coefficients[j - 1];
            continue;
        }
        const std::int64_t anchor_start = request.ramp == ObjectRamp::kEnd ? first - 1 : first;
        const std::int64_t anchor_end =
            request.ramp == ObjectRamp::kEnd ? first + size - 1 : first + size;
        coefficients[j].resize(mixes.size());
        for (std::size_t r = 0; r < mixes.size(); ++r) {
            for (const int k : mixes[r].objects) {
                const ObjectPath& path = paths[static_cast<std::size_t>(k)];
                coefficients[j][r].push_back(trajectory_coefficient(
                    path_gain(path, mixes[r].row, anchor_start),
                    path_gain(path, mixes[r].row, anchor_end), request.mix_step, ramp));
            }
        }
    }
    for (std::size_t r = 0; r < mixes.size() && ramp; ++r) {
        std::vector<std::int32_t> deltas;
        for (std::size_t j = 0; j < block_count; ++j) {
            for (const Coefficient& c : coefficients[j][r]) {
                deltas.push_back(c.delta);
            }
        }
        mixes[r].width = width_of(deltas);
    }

    // The matrices in force in each block, then the section that gets the decoder there.
    ObjectTimeline timeline;
    timeline.ramp = request.ramp;
    for (std::size_t j = 0; j < block_count; ++j) {
        ObjectBlock block;
        block.start = request.blocks[j].first;
        block.size = request.blocks[j].second;

        MatrixSpec flip;
        flip.out_ch = kFirstCodedRow;
        flip.frac_bits = 0;
        flip.f_shift = -1;
        flip.present[kFirstCodedRow] = true;
        flip.coeff[kFirstCodedRow] = -2;  // -2 / 2^(0 + 1): -1.0 in two bits
        block.matrices.push_back(flip);

        if (request.bypass_bits > 0) {
            MatrixSpec doubled;
            doubled.out_ch = static_cast<std::uint8_t>(request.objects - 1);
            doubled.frac_bits = kMixFracBits;
            doubled.f_shift = static_cast<std::int8_t>(request.bypass_bits);
            doubled.lsb_bypass = true;
            doubled.field_2b = static_cast<std::uint8_t>(request.bypass_bits);
            doubled.field_m4 = static_cast<std::uint8_t>(request.noise_shift);
            doubled.present[doubled.out_ch] = true;
            doubled.coeff[doubled.out_ch] = 1 << kMixFracBits;  // 2^12 / 2^(12 - n): 2^n
            block.matrices.push_back(doubled);
        }

        for (std::size_t r = 0; r < mixes.size(); ++r) {
            MatrixSpec mix;
            mix.out_ch = static_cast<std::uint8_t>(mixes[r].row);
            mix.frac_bits = kMixFracBits;
            mix.field_m4 = static_cast<std::uint8_t>(request.noise_shift);
            mix.ext_width = static_cast<std::uint8_t>(mixes[r].width);
            mix.present[mix.out_ch] = true;
            mix.coeff[mix.out_ch] = 1 << kMixFracBits;
            bool idle = true;
            for (std::size_t o = 0; o < mixes[r].objects.size(); ++o) {
                const auto channel = static_cast<std::size_t>(kFirstCodedRow + mixes[r].objects[o]);
                const Coefficient& c = coefficients[j][r][o];
                mix.present[channel] = true;
                mix.coeff[channel] = c.raw;
                mix.ext_value[channel] = mix.ext_width != 0 ? c.delta : 0;
                idle = idle && c.raw == 0 && c.delta == 0;
            }
            if (!(idle && request.drop_idle)) {
                block.matrices.push_back(mix);
            }
        }
        if (block.matrices.size() > static_cast<std::size_t>(kMaxObjectMatrices)) {
            return std::unexpected("more than 16 object matrices");
        }

        const std::vector<MatrixSpec> none;
        const std::vector<MatrixSpec>& before = j == 0 ? none : timeline.blocks[j - 1].matrices;
        auto [send, section] = plan_object_section(before, block.matrices);
        if (send && !same_set(apply_object_section(before, section), block.matrices)) {
            return std::unexpected("an object matrix section does not give the matrices planned");
        }
        block.send = send;
        block.section = section;
        timeline.blocks.push_back(std::move(block));
    }
    return timeline;
}

bool invert_object_timeline(const ObjectTimeline& timeline, const ObjectChainShape& shape,
                            std::vector<ObjectPlane>& rows, ObjectBypass& bypass) {
    const IntervalNoise noise(shape);
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
            own[m] = own_coefficient(block.matrices[m], rows.size());
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
                    matrix_value(matrix, rows, t, i, block.size, timeline.ramp, true,
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
                std::int64_t value = matrix_value(matrix, rows, t, i, block.size, timeline.ramp,
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

}  // namespace iclforge::truehd::detail
