#include "encoder/object_bed.hpp"

#include <array>
#include <cmath>

#include "core/noise.hpp"
#include "decoder/kernels.hpp"

namespace iclforge::truehd::detail {

namespace {

constexpr int kMatrixShift = kernels::kMatrixShift;
constexpr int kFirstCodedRow = kBedChannels;
constexpr int kMixFracBits = 12;
constexpr double kMixGain = 0.7071067811865476;  // 1/sqrt(2), -3 dB

// The bed row (L R C LFE Ls Rs Lrs Rrs) each object above the bed is mixed into: object 8 into L, 9
// into R, 10 into C, and so on round the surrounds, the LFE last. A placeholder for a renderer
// (ERRATA.md entry 70): the encoder makes a valid bed, not Dolby's.
constexpr std::array<int, kBedChannels> kBedRowOfObject{0, 1, 2, 4, 5, 6, 7, 3};

// A coefficient as the decoder scales it, to 2^-kMatrixShift: raw / 2^(frac_bits - f_shift).
[[nodiscard]] std::int64_t scaled_coefficient(const MatrixSpec& matrix, int channel) noexcept {
    const int scale = kMatrixShift - matrix.frac_bits + matrix.f_shift;
    return std::int64_t{matrix.coeff[static_cast<std::size_t>(channel)]} *
           (std::int64_t{1} << scale);
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
    [[nodiscard]] std::int64_t term(int index, int count, int shift, std::size_t t) const noexcept {
        if (shift == 0) {
            return 0;
        }
        const auto left = static_cast<std::size_t>(count - index);
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

// The sum the decoder forms for a matrix at sample `t`: the channels it reads times their
// coefficients, without the matrix's own channel when `skip_own`, plus its noise.
[[nodiscard]] std::int64_t matrix_sum(const MatrixSpec& matrix,
                                      const std::vector<ObjectPlane>& rows, std::size_t t,
                                      bool skip_own, std::int64_t noise) noexcept {
    std::int64_t sum = noise;
    for (std::size_t c = 0; c < rows.size(); ++c) {
        if (!matrix.present[c] || matrix.coeff[c] == 0 || (skip_own && c == matrix.out_ch)) {
            continue;
        }
        sum += std::int64_t{rows[c][t]} * scaled_coefficient(matrix, static_cast<int>(c));
    }
    return sum;
}

// The matrix is primitive when its own coefficient is +1.0 or -1.0 (or 2.0 with the bypass bit that
// gives back the low bit the doubling loses), the one thing that makes it invertible in integers.
// Returns the own coefficient scaled to 2^-kMatrixShift, or 0 if the matrix is not primitive.
[[nodiscard]] std::int64_t own_coefficient(const MatrixSpec& matrix, std::size_t rows) noexcept {
    if (matrix.out_ch >= rows || !matrix.present[matrix.out_ch] ||
        matrix.frac_bits - matrix.f_shift > kMatrixShift || matrix.frac_bits < matrix.f_shift) {
        return 0;
    }
    const std::int64_t own = scaled_coefficient(matrix, matrix.out_ch);
    const std::int64_t unit = std::int64_t{1} << kMatrixShift;
    if (own == unit || own == -unit || (matrix.lsb_bypass && own == 2 * unit)) {
        return own;
    }
    return 0;
}

}  // namespace

std::vector<MatrixSpec> make_object_bed_chain(int objects, int mix_step, int noise_shift,
                                              bool lsb_bypass) {
    std::vector<MatrixSpec> chain;
    MatrixSpec flip;
    flip.out_ch = kFirstCodedRow;
    flip.frac_bits = 0;
    flip.f_shift = -1;
    flip.present[kFirstCodedRow] = true;
    flip.coeff[kFirstCodedRow] = -2;  // -2 / 2^(0 + 1): -1.0 in two bits
    chain.push_back(flip);

    if (lsb_bypass) {
        MatrixSpec doubled;
        doubled.out_ch = static_cast<std::uint8_t>(objects - 1);
        doubled.frac_bits = kMixFracBits;
        doubled.f_shift = 1;
        doubled.lsb_bypass = true;
        doubled.field_m4 = static_cast<std::uint8_t>(noise_shift);
        doubled.present[doubled.out_ch] = true;
        doubled.coeff[doubled.out_ch] = 1 << kMixFracBits;  // 2^12 / 2^(12 - 1): 2.0
        chain.push_back(doubled);
    }

    const double gain = mix_step < 3 ? kMixGain / static_cast<double>(1 << mix_step) : 0.0;
    const long raw = std::lround(-gain * (1 << kMixFracBits));
    if (raw != 0) {
        for (int k = 0; k + kFirstCodedRow < objects; ++k) {
            MatrixSpec mix;
            mix.out_ch = static_cast<std::uint8_t>(kBedRowOfObject[static_cast<std::size_t>(k)]);
            mix.frac_bits = kMixFracBits;
            mix.field_m4 = static_cast<std::uint8_t>(noise_shift);
            mix.present[mix.out_ch] = true;
            mix.coeff[mix.out_ch] = 1 << kMixFracBits;
            mix.present[static_cast<std::size_t>(kFirstCodedRow + k)] = true;
            mix.coeff[static_cast<std::size_t>(kFirstCodedRow + k)] =
                static_cast<std::int32_t>(raw);
            chain.push_back(mix);
        }
    }
    return chain;
}

bool invert_object_chain(const std::vector<MatrixSpec>& chain, const ObjectChainShape& shape,
                         std::vector<ObjectPlane>& rows,
                         std::vector<std::vector<std::uint8_t>>& bypass) {
    const IntervalNoise noise(shape);
    const auto count = static_cast<int>(chain.size());
    const std::int64_t keep = ~((std::int64_t{1} << shape.quant) - 1);
    std::size_t bypass_matrices = 0;
    for (const MatrixSpec& matrix : chain) {
        bypass_matrices += matrix.lsb_bypass ? 1U : 0U;
    }
    const std::size_t samples = rows.empty() ? 0 : rows.front().size();
    bypass.assign(bypass_matrices, std::vector<std::uint8_t>(samples, 0));
    std::size_t next_bypass = bypass_matrices;
    for (int m = count - 1; m >= 0; --m) {
        const MatrixSpec& matrix = chain[static_cast<std::size_t>(m)];
        const std::int64_t own = own_coefficient(matrix, rows.size());
        if (own == 0 || (matrix.lsb_bypass && shape.quant != 0)) {
            return false;
        }
        std::vector<std::uint8_t>* bits = nullptr;
        if (matrix.lsb_bypass) {
            bits = &bypass[--next_bypass];
        }
        ObjectPlane& out = rows[matrix.out_ch];
        for (std::size_t t = 0; t < samples; ++t) {
            const std::int64_t sum =
                matrix_sum(matrix, rows, t, true, noise.term(m, count, matrix.field_m4, t));
            const std::int64_t base = sum >> kMatrixShift;
            const std::int64_t after = out[t];
            std::int64_t before = 0;
            if (bits != nullptr) {
                // after = 2 * before + base + bit: the bit is the low bit of what is left
                const std::int64_t rest = after - base;
                before = rest >> 1;
                (*bits)[t] = static_cast<std::uint8_t>(rest & 1);
            } else if (own > 0) {
                before = after - (base & keep);
            } else {
                before = (base & keep) - after;
            }
            out[t] = static_cast<std::int32_t>(before);
        }
    }
    return true;
}

void apply_object_chain(const std::vector<MatrixSpec>& chain, const ObjectChainShape& shape,
                        const std::vector<std::vector<std::uint8_t>>& bypass,
                        std::vector<ObjectPlane>& rows) {
    const IntervalNoise noise(shape);
    const auto count = static_cast<int>(chain.size());
    const std::int64_t keep = ~((std::int64_t{1} << shape.quant) - 1);
    const std::size_t samples = rows.empty() ? 0 : rows.front().size();
    std::size_t next_bypass = 0;
    for (int m = 0; m < count; ++m) {
        const MatrixSpec& matrix = chain[static_cast<std::size_t>(m)];
        const std::vector<std::uint8_t>* bits =
            matrix.lsb_bypass ? &bypass[next_bypass++] : nullptr;
        ObjectPlane& out = rows[matrix.out_ch];
        for (std::size_t t = 0; t < samples; ++t) {
            const std::int64_t sum =
                matrix_sum(matrix, rows, t, false, noise.term(m, count, matrix.field_m4, t));
            std::int64_t value = (sum >> kMatrixShift) & keep;
            if (bits != nullptr) {
                value += (*bits)[t];
            }
            out[t] = static_cast<std::int32_t>(value);
        }
    }
}

}  // namespace iclforge::truehd::detail
