#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "core/huffman.hpp"
#include "iclforge/base/bitreader.hpp"
#include "iclforge/truehd/core/crc.hpp"
#include "iclforge/truehd/core/format.hpp"

// The decoder's inner loops, one function each, as the scalar reference. Decoder::decode_block calls
// them in the order below; libs/truehd's tests and tests/performance/kernel_bench.cpp call them on
// their own. Every one is exact integer arithmetic: the products are 32 bit by 32 bit into 64 bits
// (the filters' coefficients are at most 16 bits wide and their history 24, the matrices'
// coefficients are scaled to 2^-15), the sums are 64 bit, and the shifts are by amounts the stream
// sends, so a kernel that gives other bits than these is wrong rather than approximate. A variant
// of a kernel for a wider vector unit has to give the same samples for every input, which
// libs/truehd/tests/decoder/test_kernels.cpp holds it to.
//
// The block's samples are `rows`, one row per sample time and `stride` entries to a row (a row
// holds the matrix channels and then the two noise channels, TH spec §2.3), and a channel is a
// column of them.

namespace iclforge::truehd::kernels {

// How far back a filter looks, and so how many entries of history a channel carries in front of the
// first sample of its block (TH spec §9.3).
inline constexpr int kHistoryPad = kMaxFirOrder;

// The matrices' coefficients are scaled to 2^-15 (ERRATA.md entry 60): a sum is shifted down by this
// much. frac_bits runs to 15 in substream 3, so 2^-14 cannot hold every coefficient; a coefficient
// of frac_bits 14 or less, scaled to 2^-15 instead, gives the same samples (the sum is twice as
// large and shifted one more).
inline constexpr int kMatrixShift = 15;
// The matrix noise of TH spec §17.3 was defined against a sum at 2^-14.
inline constexpr int kNoiseScaleShift = 7 + (kMatrixShift - 14);

// What one channel's residuals need, worked out once per block (TH spec §11.3, ERRATA.md).
struct ResidualPlan {
    const detail::DecodeTable* table = nullptr;
    int raw_bits = 0;
    int quant = 0;
    std::int64_t bias = 0;
};

// TH spec §11: the residuals of one block, sample by sample and channel by channel, from the bit
// reader: for each sample first the bits that put back the LSBs a matrix dropped (`bypass_count`
// of them, one per matrix with bypass, kept in `bypass`), then for each channel its Huffman code
// (if the channel has a codebook) and its raw bits, shifted up by the quantiser step. False for a
// Huffman code no codebook has.
[[nodiscard]] inline bool read_residuals(BitReader& reader, int block, int min_chan, int max_chan,
                                         const ResidualPlan* plans, int bypass_count,
                                         const std::uint8_t* bypass_matrix,
                                         const std::uint8_t* bypass_bits, std::int32_t* rows,
                                         std::ptrdiff_t stride, std::uint64_t* bypass) {
    for (int i = 0; i < block; ++i) {
        std::int32_t* const row = rows + static_cast<std::ptrdiff_t>(i) * stride;
        std::uint64_t bits = 0;
        for (int b = 0; b < bypass_count; ++b) {
            // `bypass_bits` bits a sample for the matrix (1 for the standard section, the
            // object matrix's field_2b otherwise), kept in the matrix's nibble.
            bits |= std::uint64_t{reader.read(bypass_bits[static_cast<std::size_t>(b)])}
                    << (4U * bypass_matrix[static_cast<std::size_t>(b)]);
        }
        bypass[static_cast<std::size_t>(i)] = bits;
        for (int ch = min_chan; ch <= max_chan; ++ch) {
            const ResidualPlan& plan = plans[static_cast<std::size_t>(ch)];
            std::int64_t value = plan.bias;
            if (plan.table != nullptr) {
                const detail::DecodeEntry entry = plan.table->entries[reader.peek(kHuffmanMaxBits)];
                if (entry.bits == 0) {
                    return false;
                }
                reader.consume(entry.bits);
                value +=
                    static_cast<std::int64_t>(entry.symbol) * (std::int64_t{1} << plan.raw_bits);
            }
            if (plan.raw_bits > 0) {
                value += reader.read(plan.raw_bits);
            }
            row[ch] = static_cast<std::int32_t>(static_cast<std::uint32_t>(value) << plan.quant);
        }
    }
    return true;
}

// TH spec §9.4, §9.5: the inverse prediction filters of one channel over one block. `fir_history`
// and `iir_history` point at the entry for block position 0, with kHistoryPad older entries before
// it; the newest is at index i - 1 for the sample at position i. The prediction is the FIR sum
// over the last outputs plus the IIR sum over the last residuals, shifted right by `shift`; the
// output is the prediction plus the residual with the quantiser step's low bits cleared (`keep`),
// and the residual the next IIR sum sees is the output less the prediction. The samples are
// replaced in place. A recurrence: each output is in the next sample's sum.
inline void inverse_filter(int block, int fir_order, const std::int32_t* fir_coeff, int iir_order,
                           const std::int32_t* iir_coeff, int shift, std::uint32_t keep,
                           std::int32_t* fir_history, std::int32_t* iir_history,
                           std::int32_t* samples, std::ptrdiff_t stride) {
    for (int i = 0; i < block; ++i) {
        std::int64_t prediction = 0;
        for (int lag = 0; lag < fir_order; ++lag) {
            prediction += std::int64_t{fir_history[i - 1 - lag]} * fir_coeff[lag];
        }
        for (int lag = 0; lag < iir_order; ++lag) {
            prediction += std::int64_t{iir_history[i - 1 - lag]} * iir_coeff[lag];
        }
        prediction >>= shift;
        std::int32_t& sample = samples[static_cast<std::ptrdiff_t>(i) * stride];
        const auto output =
            static_cast<std::int32_t>(static_cast<std::uint32_t>(prediction + sample) & keep);
        fir_history[i] = output;
        iir_history[i] = static_cast<std::int32_t>(static_cast<std::uint32_t>(output) -
                                                   static_cast<std::uint32_t>(prediction));
        sample = output;
    }
}

// TH spec §17.2: generator 0's two noise channels, one step of the LFSR a sample whether or not a
// matrix draws on them, so that the sequence runs on across blocks and units. `shr7` is the 16
// bits of the seed from bit 7 up: FFmpeg's decoder reads it so, and the 32-bit reading the text
// shows gives a different sequence (ERRATA.md entry 23, open question 5; provisional). `rows`
// points at the first row and `column` at the first noise channel in it.
inline void generate_noise(int block, std::uint32_t& seed_state, int noise_shift,
                           std::int32_t* rows, std::ptrdiff_t stride, int column) {
    std::uint32_t seed = seed_state;
    for (int i = 0; i < block; ++i) {
        std::int32_t* const row = rows + static_cast<std::ptrdiff_t>(i) * stride;
        const std::uint32_t shr7 = (seed >> 7U) & 0xFFFFU;
        row[column] =
            static_cast<std::int32_t>(static_cast<std::int8_t>(seed >> 15U)) * (1 << noise_shift);
        row[column + 1] =
            static_cast<std::int32_t>(static_cast<std::int8_t>(shr7)) * (1 << noise_shift);
        seed = (seed << 16U) ^ shr7 ^ (shr7 << 5U);
    }
    seed_state = seed;
}

// TH spec §17.3: what a matrix draws from generator 1's noise buffer. The matrix with a non-zero
// `shift` adds buffer[index] * 2^(shift + 7) to its sum at 2^-14 (twice that at 2^-15), for the
// sample at position i of the block the index (first + i * stride) & mask (the stride and the first index are those of the
// matrix: ERRATA.md, noise generator 1). `shift` 0 draws nothing.
struct NoiseDraw {
    const std::int8_t* buffer = nullptr;
    int shift = 0;
    unsigned first = 0;
    unsigned stride = 1;
    unsigned mask = 0;
    int tie_mode = 0;  // lab: how a sum exactly between two outputs rounds (0 down)
    int extra_shift = 0;  // lab: the draw is shifted by this much more (negative: down)
};

// TH spec §10: one primitive matrix over one block. Row[out] becomes the sum over the `count`
// channels it reads of channel times coefficient (the coefficients are scaled to 2^-15), plus the
// matrix's noise (TH spec §17.3) if it draws any, shifted down by 14, with the quantiser step's low
// bits cleared (`keep`); with LSB bypass the bit the stream sent for this matrix (`bypass`, bit
// `matrix_index`) is added back.
// The extension of an object matrix (ERRATA.md entry 69): `delta[k]` is the finer part of coefficient
// k, in units of 2^-`bits` of the coefficient's own last place and already scaled to 2^-15.
struct FineTerms {
    const std::int32_t* delta = nullptr;
    int bits = 0;
    int ramp = 0;  // lab: 0 the same for the block; 1 grows by (i + 1) / block; 2 by i / block
};

inline void apply_matrix(int block, int count, const std::uint8_t* channel,
                         const std::int32_t* coeff, int out, std::uint32_t keep, bool lsb_bypass,
                         int matrix_index, const std::uint64_t* bypass, std::int32_t* rows,
                         std::ptrdiff_t stride, const NoiseDraw& noise = {},
                         const FineTerms& fine = {}) {
    const std::int64_t noise_scale = std::int64_t{1} << (noise.shift + kNoiseScaleShift);
    for (int i = 0; i < block; ++i) {
        std::int32_t* const row = rows + static_cast<std::ptrdiff_t>(i) * stride;
        std::int64_t sum = 0;
        for (int k = 0; k < count; ++k) {
            sum += std::int64_t{row[channel[k]]} * coeff[k];
        }
        if (noise.shift != 0) {
            std::int64_t draw = std::int64_t{noise.buffer[(noise.first + noise.stride * static_cast<unsigned>(i)) &
                                                          noise.mask]} *
                                noise_scale;
            if (noise.extra_shift < 0) {
                draw >>= -noise.extra_shift;
            } else if (noise.extra_shift > 0) {
                draw <<= noise.extra_shift;
            }
            sum += draw;
        }
        if (noise.tie_mode != 0) {
            const std::int64_t low = sum & ((std::int64_t{1} << kMatrixShift) - 1);
            const std::int64_t half = std::int64_t{1} << (kMatrixShift - 1);
            if (noise.tie_mode == 3 || (low == half && (noise.tie_mode == 1 ||
                                                         (noise.tie_mode == 2 && ((sum >> kMatrixShift) & 1) != 0)))) {
                sum += half;
            }
        }
        int shift = kMatrixShift;
        if (fine.bits != 0) {
            // Rows of 32 bits and deltas of 32 bits can leave 64 bits in a sum of products: the
            // sum is kept as unsigned, which wraps.
            std::uint64_t finer = 0;
            for (int k = 0; k < count; ++k) {
                finer += static_cast<std::uint64_t>(std::int64_t{row[channel[k]]} * fine.delta[k]);
            }
            if (fine.ramp != 0) {
                const std::int64_t numerator = fine.ramp == 1 ? i + 1 : i;
                finer = static_cast<std::uint64_t>(static_cast<std::int64_t>(finer) * numerator /
                                                   block);
            }
            sum = static_cast<std::int64_t>((static_cast<std::uint64_t>(sum) << fine.bits) + finer);
            shift += fine.bits;
        }
        std::uint32_t value = static_cast<std::uint32_t>(sum >> shift) & keep;
        if (lsb_bypass) {
            const std::uint64_t sent = bypass[static_cast<std::size_t>(i)];
            value += static_cast<std::uint32_t>((sent >> (4 * matrix_index)) & 0xFU);
        }
        row[out] = static_cast<std::int32_t>(value);
    }
}

[[nodiscard]] inline std::int32_t sign_extend_24(std::int32_t value) noexcept {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(value) << 8U) >> 8;
}

// The output of a block: each output channel from the matrix channel `source_channel` names for it
// (the decoder builds that from ch_assign, which assigns matrix channels to output channels:
// ERRATA.md entry 42), shifted by the output shift of that matrix channel (`output_shift` is indexed
// by matrix channel, like the lossless check's fold: ERRATA.md entry 43), folded into the lossless
// check (TH spec §7.3)
// and, when `pcm` is not null, written as 24 bit samples, `out_channels` to a sample time. Returns
// the new fold.
[[nodiscard]] inline std::uint32_t assign_output(int block, int out_channels,
                                                 const std::uint8_t* source_channel,
                                                 const std::int8_t* output_shift,
                                                 const std::int32_t* rows, std::ptrdiff_t stride,
                                                 std::int32_t* pcm, std::uint32_t fold) {
    for (int i = 0; i < block; ++i) {
        const std::int32_t* const row = rows + static_cast<std::ptrdiff_t>(i) * stride;
        for (int ch = 0; ch < out_channels; ++ch) {
            const std::int32_t source = row[source_channel[ch]];
            const int shift = output_shift[source_channel[ch]];
            const std::int32_t output =
                shift >= 0 ? static_cast<std::int32_t>(static_cast<std::uint32_t>(source) << shift)
                           : source >> -shift;
            fold = lossless_fold_step(fold, static_cast<std::uint32_t>(output), source_channel[ch]);
            if (pcm != nullptr) {
                pcm[static_cast<std::ptrdiff_t>(i) * out_channels + ch] = sign_extend_24(output);
            }
        }
    }
    return fold;
}

}  // namespace iclforge::truehd::kernels
