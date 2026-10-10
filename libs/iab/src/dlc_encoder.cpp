#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "bitwriter.hpp"
#include "dlc_detail.hpp"
#include "iclforge/iab/writer.hpp"

// An AudioDataDLC encoder, SMPTE ST 2098-2:2022 Annex B.2, B.3 and B.11. The decoding process of
// Annex B is normative and the encoder is not, so this makes its own choices inside what the
// syntax allows: one predictor region per layer, found by Levinson-Durbin on the layer's
// autocorrelation, and per sub block the smaller of direct PCM and Rice/Golomb residual coding.
// Whatever it chooses, decode_dlc() returns the input integers exactly, because the residual is
// computed with the same integer arithmetic B.7 and B.8 give the decoder.

namespace iclforge::iab {

namespace {

using detail::BitWriter;
using detail::PredictorRegion;

// One layer's coded form, ready to write.
struct LayerPlan {
    std::vector<unsigned> k_codes;  // empty when the predictor is disabled
    std::vector<std::int32_t> residual;
    std::uint64_t bits = 0;
};

struct SubBlockCoding {
    bool rice = false;
    unsigned parameter = 0;  // BitDepth for direct PCM, RiceRemBits for Rice
    std::uint64_t bits = 0;
};

// §9.6 Table 10: the cheaper of direct PCM and Rice/Golomb for one sub block of residual.
[[nodiscard]] SubBlockCoding choose_sub_block_coding(std::span<const std::int32_t> residual) {
    std::uint64_t max_abs = 0;
    std::uint64_t non_zero = 0;
    for (const std::int32_t r : residual) {
        const std::uint64_t magnitude = r < 0 ? static_cast<std::uint64_t>(-static_cast<std::int64_t>(r))
                                              : static_cast<std::uint64_t>(r);
        max_abs = std::max(max_abs, magnitude);
        non_zero += magnitude != 0 ? 1 : 0;
    }

    unsigned bit_depth = 0;
    while ((std::uint64_t{1} << bit_depth) <= max_abs) {
        ++bit_depth;
    }
    SubBlockCoding best;
    best.rice = false;
    best.parameter = bit_depth;
    best.bits = 1 + 5 + residual.size() * bit_depth + non_zero;

    for (unsigned k = 0; k <= 24; ++k) {
        std::uint64_t bits = 1 + 5;
        for (const std::int32_t r : residual) {
            const std::uint64_t magnitude = r < 0 ? static_cast<std::uint64_t>(-static_cast<std::int64_t>(r))
                                                  : static_cast<std::uint64_t>(r);
            bits += 1 + (magnitude >> k) + k + (magnitude != 0 ? 1 : 0);
            if (bits >= best.bits) {
                break;
            }
        }
        if (bits < best.bits) {
            best.rice = true;
            best.parameter = k;
            best.bits = bits;
        }
    }
    return best;
}

[[nodiscard]] std::uint64_t residual_bits(std::span<const std::int32_t> residual, unsigned sub_block_size) {
    std::uint64_t bits = 0;
    for (std::size_t start = 0; start < residual.size(); start += sub_block_size) {
        bits += choose_sub_block_coding(residual.subspan(start, sub_block_size)).bits;
    }
    return bits;
}

// B.7 and B.8 forward: the residual a decoder must be given to reproduce `values` through the
// predictor the codes describe. Fails (nullopt) if a residual would not fit the 31-bit magnitude
// the syntax carries.
[[nodiscard]] std::optional<std::vector<std::int32_t>> predict_residual(std::span<const std::int32_t> values,
                                                                        const std::vector<unsigned>& k_codes) {
    PredictorRegion region;
    region.order = static_cast<unsigned>(k_codes.size());
    for (std::size_t m = 0; m < k_codes.size(); ++m) {
        region.k_coeff[m + 1] = static_cast<std::int32_t>(k_codes[m]);
    }
    detail::convert_lattice_to_direct(region);

    std::vector<std::int32_t> residual(values.size());
    for (std::size_t n = 0; n < values.size(); ++n) {
        std::uint64_t accum = 0;
        for (unsigned p = 1; p <= region.order && p <= n; ++p) {
            accum -= static_cast<std::uint64_t>(static_cast<std::int64_t>(values[n - p]) *
                                                static_cast<std::int64_t>(region.a_coeff[p]));
        }
        const auto predicted = static_cast<std::int32_t>(static_cast<std::int64_t>(accum) >> 20);
        const std::int64_t r = static_cast<std::int64_t>(values[n]) - predicted;
        if (r > std::numeric_limits<std::int32_t>::max() || r < -std::numeric_limits<std::int32_t>::max()) {
            return std::nullopt;
        }
        residual[n] = static_cast<std::int32_t>(r);
    }
    return residual;
}

// Reflection coefficients of order up to `max_order` by Levinson-Durbin on the autocorrelation
// of `values`, as 10-bit lattice codes (§10.7.8: code = 512 + k * 512). The recursion is the
// standard one whose step-up B.7 implements.
[[nodiscard]] std::vector<unsigned> lattice_codes(std::span<const std::int32_t> values, unsigned max_order) {
    std::vector<double> r(max_order + 1, 0.0);
    for (unsigned lag = 0; lag <= max_order; ++lag) {
        for (std::size_t n = lag; n < values.size(); ++n) {
            r[lag] += static_cast<double>(values[n]) * static_cast<double>(values[n - lag]);
        }
    }
    std::vector<unsigned> codes;
    if (r[0] <= 0.0) {
        return codes;
    }

    std::vector<double> a(max_order + 1, 0.0);
    std::vector<double> next(max_order + 1, 0.0);
    double error = r[0];
    for (unsigned j = 1; j <= max_order; ++j) {
        double acc = r[j];
        for (unsigned i = 1; i < j; ++i) {
            acc += a[i] * r[j - i];
        }
        const double k = std::clamp(-acc / error, -1.0, 1.0);
        for (unsigned i = 1; i < j; ++i) {
            next[i] = a[i] + k * a[j - i];
        }
        next[j] = k;
        for (unsigned i = 1; i <= j; ++i) {
            a[i] = next[i];
        }
        error *= 1.0 - k * k;
        const double code = std::clamp(std::round(512.0 + k * 512.0), 0.0, 1023.0);
        codes.push_back(static_cast<unsigned>(code));
        if (error <= 0.0) {
            break;
        }
    }
    return codes;
}

// Plans one layer: the cheaper of no predictor and the best of a few predictor orders.
[[nodiscard]] std::expected<LayerPlan, WriteError> plan_layer(std::span<const std::int32_t> values,
                                                              unsigned sub_block_size, unsigned max_order) {
    LayerPlan best;
    best.residual.assign(values.begin(), values.end());
    best.bits = 2 + residual_bits(best.residual, sub_block_size);  // NumPredRegions (2 bits)

    if (max_order == 0) {
        return best;
    }
    const std::vector<unsigned> all_codes = lattice_codes(values, std::min(max_order, detail::kMaxOrder));
    std::vector<std::size_t> orders;
    for (std::size_t order = 1; order < all_codes.size(); order *= 2) {
        orders.push_back(order);
    }
    if (!all_codes.empty()) {
        orders.push_back(all_codes.size());
    }

    for (const std::size_t order : orders) {
        const std::vector<unsigned> codes(all_codes.begin(),
                                          all_codes.begin() + static_cast<std::ptrdiff_t>(order));
        auto residual = predict_residual(values, codes);
        if (!residual.has_value()) {
            continue;
        }
        // NumPredRegions, RegionLength (4), Order (5) and one 10-bit code per coefficient.
        const std::uint64_t bits = 2 + 4 + 5 + 10ULL * order + residual_bits(*residual, sub_block_size);
        if (bits < best.bits) {
            best.k_codes = codes;
            best.residual = std::move(*residual);
            best.bits = bits;
        }
    }
    return best;
}

// §9.6 Table 10 for one layer: predictor information, then the coded residual.
void put_layer(BitWriter& bw, const LayerPlan& plan, unsigned num_sub_blocks, unsigned sub_block_size) {
    if (plan.k_codes.empty()) {
        bw.put_bits(0, 2);  // NumPredRegions
    } else {
        bw.put_bits(1, 2);
        bw.put_bits(num_sub_blocks, 4);  // RegionLength: the whole layer
        bw.put_bits(plan.k_codes.size(), 5);
        for (const unsigned code : plan.k_codes) {
            bw.put_bits(code, 10);
        }
    }

    for (unsigned n = 0; n < num_sub_blocks; ++n) {
        const std::span<const std::int32_t> block =
            std::span(plan.residual).subspan(static_cast<std::size_t>(n) * sub_block_size, sub_block_size);
        const SubBlockCoding coding = choose_sub_block_coding(block);
        bw.put_bits(coding.rice ? 1U : 0U, 1);
        bw.put_bits(coding.parameter, 5);
        // The field above is five bits wide and predict_residual keeps every magnitude under
        // 2^31, so the parameter is at most 31 and the mask changes nothing; it is what makes
        // every shift below provably less than 64.
        const unsigned parameter = coding.parameter & 31U;
        for (const std::int32_t r : block) {
            const std::uint64_t magnitude = r < 0 ? static_cast<std::uint64_t>(-static_cast<std::int64_t>(r))
                                                  : static_cast<std::uint64_t>(r);
            if (!coding.rice) {
                if (parameter != 0) {
                    bw.put_bits(magnitude, parameter);
                }
            } else {
                for (std::uint64_t q = 0; q < (magnitude >> parameter); ++q) {
                    bw.put_bits(1, 1);
                }
                bw.put_bits(0, 1);
                if (parameter != 0) {
                    bw.put_bits(magnitude & ((std::uint64_t{1} << parameter) - 1), parameter);
                }
            }
            if (magnitude != 0) {
                bw.put_bits(r < 0 ? 1U : 0U, 1);
            }
        }
    }
}

// B.3 Figure B.2: a 48 kHz base layer from 96 kHz input. The half band filter of Table B.1,
// scaled to unity DC gain and centred on input[2k + 16], so that the decoder's 8 sample delay in
// B.9 lines the base layer up with the input.
[[nodiscard]] std::vector<std::int32_t> decimate(std::span<const std::int32_t> input96) {
    const std::size_t count = input96.size() / 2;
    std::vector<std::int32_t> base(count, 0);
    for (std::size_t k = 0; k < count; ++k) {
        std::int64_t accum = 1 << 15;  // round to nearest
        for (std::size_t i = 0; i < detail::kInterp.size(); ++i) {
            const std::size_t index = 2 * k + i;
            if (index < input96.size()) {
                accum += static_cast<std::int64_t>(detail::kInterp[i]) * input96[index];
            }
        }
        base[k] = static_cast<std::int32_t>(std::clamp<std::int64_t>(
            accum >> 16, std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()));
    }
    return base;
}

}  // namespace

std::expected<AudioDataDlc, WriteError> encode_dlc(std::uint32_t audio_data_id, std::span<const float> samples,
                                                    std::uint8_t frame_rate_code, const DlcEncodeOptions& options) {
    if (options.sample_rate != 48000 && options.sample_rate != 96000) {
        return std::unexpected(WriteError::kBadSampleRate);
    }
    if (options.bit_depth != 16 && options.bit_depth != 24) {
        return std::unexpected(WriteError::kBadBitDepth);
    }
    if (frame_rate_code > 0x9) {
        return std::unexpected(WriteError::kReservedFrameRate);
    }
    const auto layout = detail::dlc_layout(frame_rate_code);
    if (!layout.has_value()) {
        return std::unexpected(WriteError::kDlcFrameRate);
    }
    const bool is_96k = options.sample_rate == 96000;
    const unsigned expected_samples = is_96k ? layout->sample_count_96() : layout->sample_count_48();
    if (samples.size() != expected_samples) {
        return std::unexpected(WriteError::kSampleCount);
    }

    // Quantize the way AudioDataPCM does, so a DLC frame and a PCM frame of the same input hold
    // the same integers.
    const auto full_scale = static_cast<double>(std::uint32_t{1} << (options.bit_depth - 1));
    std::vector<std::int32_t> quantized(samples.size());
    for (std::size_t n = 0; n < samples.size(); ++n) {
        const double scaled = std::round(static_cast<double>(samples[n]) * full_scale);
        quantized[n] = static_cast<std::int32_t>(std::clamp(scaled, -full_scale, full_scale - 1.0));
    }
    const unsigned shift_bits = 32 - options.bit_depth;  // B.11: ShiftBits = 32 - BitDepth
    const unsigned max_order = std::min(options.max_prediction_order, detail::kMaxOrder);

    BitWriter bw;
    bw.put_bits(is_96k ? 1U : 0U, 2);  // DLCSampleRate
    bw.put_bits(shift_bits, 5);

    if (!is_96k) {
        auto plan = plan_layer(quantized, layout->sub_block_size_48, max_order);
        if (!plan.has_value()) {
            return std::unexpected(plan.error());
        }
        put_layer(bw, *plan, layout->num_sub_blocks, layout->sub_block_size_48);
    } else {
        const std::vector<std::int32_t> base = decimate(quantized);
        const std::vector<std::int32_t> upsampled = detail::upsample_base_layer(base, quantized.size());
        std::vector<std::int32_t> extension(quantized.size());
        for (std::size_t n = 0; n < quantized.size(); ++n) {
            extension[n] = detail::wrap_add(quantized[n], static_cast<std::int32_t>(-static_cast<std::int64_t>(upsampled[n])));
        }

        auto base_plan = plan_layer(base, layout->sub_block_size_48, max_order);
        auto extension_plan = plan_layer(extension, layout->sub_block_size_96(), max_order);
        if (!base_plan.has_value()) {
            return std::unexpected(base_plan.error());
        }
        if (!extension_plan.has_value()) {
            return std::unexpected(extension_plan.error());
        }
        put_layer(bw, *base_plan, layout->num_sub_blocks, layout->sub_block_size_48);
        put_layer(bw, *extension_plan, layout->num_sub_blocks, layout->sub_block_size_96());
    }

    AudioDataDlc element;
    element.audio_data_id = audio_data_id;
    element.coded = bw.take();
    if (element.coded.size() > 0xFFFFU) {
        return std::unexpected(WriteError::kDlcTooLarge);
    }
    return element;
}

}  // namespace iclforge::iab
