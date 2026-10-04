#include "iclforge/iab/dlc.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <utility>
#include <vector>

#include "bitreader.hpp"
#include "dlc_detail.hpp"

// SMPTE ST 2098-2:2022 §9.6 Table 10 (syntax), §10.7 (fields) and Annex B.6-B.10 (decoding).
// Every step is named after the clause it transcribes. Annex B's "all scalar values shall be
// implemented as 32-bit integers" is kept literally; the signed sums are accumulated in unsigned
// types so that a corrupt stream wraps instead of hitting signed overflow.

namespace iclforge::iab {

namespace detail {

std::vector<std::int32_t> upsample_base_layer(std::span<const std::int32_t> pcm48,
                                              std::size_t count96) {
    // B.9.
    std::array<std::int32_t, 64> buffer{};
    unsigned index1 = 0;
    std::vector<std::int32_t> out(count96, 0);
    std::size_t k = 0;
    for (std::size_t n = 0; n < count96; n += 2, ++k) {
        buffer[index1] = k < pcm48.size() ? pcm48[k] : 0;

        unsigned index2 = (index1 - 8U) & 63U;
        out[n] = buffer[index2];

        index2 = index1;
        std::uint64_t accum = 0;
        for (unsigned i = 1; i < 33; i += 2) {
            accum += static_cast<std::uint64_t>(static_cast<std::int64_t>(buffer[index2]) *
                                                static_cast<std::int64_t>(kInterp[i]));
            index2 = (index2 - 1U) & 63U;
        }
        if (n + 1 < count96) {
            out[n + 1] = static_cast<std::int32_t>(static_cast<std::int64_t>(accum) >> 15);
        }
        index1 = (index1 + 1U) & 63U;
    }
    return out;
}

// B.7: lattice (reflection) coefficients to direct form.
void convert_lattice_to_direct(PredictorRegion& region) {
    constexpr std::int32_t kOne = 1048576;  // 1.0 in Q20
    std::array<std::int32_t, kMaxOrder + 1> temp{};
    region.k_coeff[0] = kOne;
    region.a_coeff[0] = kOne;
    temp[0] = kOne;
    for (unsigned j = 1; j <= region.order; ++j) {
        region.k_coeff[j] = (region.k_coeff[j] - 512) * 2048;  // "-= 512; <<= 11"
        region.a_coeff[j] = 0;
        for (unsigned k = 1; k <= j; ++k) {
            const std::int64_t accum = static_cast<std::int64_t>(region.k_coeff[j]) *
                                       static_cast<std::int64_t>(region.a_coeff[j - k]);
            temp[k] = wrap_add(region.a_coeff[k], static_cast<std::int32_t>(accum >> 20));
        }
        for (unsigned k = 1; k <= j; ++k) {
            region.a_coeff[k] = temp[k];
        }
    }
}

}  // namespace detail

namespace {

using detail::BitReader;
using detail::kMaxOrder;
using detail::PredictorRegion;
using detail::wrap_add;

// §9.6 Table 10, "Predictor information": NumPredRegions (2 bits), then per region RegionLength
// (4), Order (5) and Order KCoeff values (10 each). §10.7.6: the RegionLengths sum to
// NumDLCSubBlocks unless the predictor is disabled.
[[nodiscard]] std::expected<std::vector<PredictorRegion>, IabError> read_predictor_info(
    BitReader& br, unsigned num_sub_blocks) {
    auto num_regions = br.read_bits(2);
    if (!num_regions.has_value()) {
        return std::unexpected(num_regions.error());
    }
    std::vector<PredictorRegion> regions(static_cast<std::size_t>(*num_regions));
    unsigned total_length = 0;
    for (auto& region : regions) {
        auto length = br.read_bits(4);
        auto order = br.read_bits(5);
        if (!length.has_value() || !order.has_value()) {
            return std::unexpected(IabError::kTruncated);
        }
        region.length = static_cast<unsigned>(*length);
        region.order = static_cast<unsigned>(*order);
        if (region.length == 0) {
            return std::unexpected(IabError::kBadDlc);  // §10.7.6: "greater than 0"
        }
        total_length += region.length;
        for (unsigned m = 1; m <= region.order; ++m) {
            auto k = br.read_bits(10);
            if (!k.has_value()) {
                return std::unexpected(k.error());
            }
            region.k_coeff[m] = static_cast<std::int32_t>(*k);
        }
    }
    if (!regions.empty() && total_length != num_sub_blocks) {
        return std::unexpected(IabError::kBadDlc);
    }
    return regions;
}

// §9.6 Table 10, "Coded residual", for one sub block: CodeType, then BitDepth or RiceRemBits and
// `count` residuals, each a magnitude followed by a sign bit when non-zero.
//
// Table 10's 96 kHz Rice branch is indented differently from the 48 kHz one and would, read
// literally, drop the quotient and the sign when RiceRemBits is 0. The two branches are the same
// syntax; this reads both as the 48 kHz one. See src/iab/ERRATA.md.
[[nodiscard]] std::expected<void, IabError> read_residual_sub_block(BitReader& br, unsigned count,
                                                                    std::int32_t* out) {
    auto code_type = br.read_bits(1);
    if (!code_type.has_value()) {
        return std::unexpected(code_type.error());
    }

    if (*code_type == 0) {
        auto bit_depth = br.read_bits(5);
        if (!bit_depth.has_value()) {
            return std::unexpected(bit_depth.error());
        }
        for (unsigned i = 0; i < count; ++i) {
            std::uint64_t magnitude = 0;
            if (*bit_depth != 0) {
                auto value = br.read_bits(static_cast<unsigned>(*bit_depth));
                if (!value.has_value()) {
                    return std::unexpected(value.error());
                }
                magnitude = *value;
            }
            bool negative = false;
            if (magnitude != 0) {
                auto sign = br.read_bits(1);
                if (!sign.has_value()) {
                    return std::unexpected(sign.error());
                }
                negative = *sign == 1;
            }
            if (magnitude > 0x7FFFFFFFULL) {
                return std::unexpected(IabError::kBadDlc);
            }
            const auto signed_value = static_cast<std::int32_t>(magnitude);
            out[i] = negative ? -signed_value : signed_value;
        }
        return {};
    }

    auto rice_bits = br.read_bits(5);
    if (!rice_bits.has_value()) {
        return std::unexpected(rice_bits.error());
    }
    for (unsigned i = 0; i < count; ++i) {
        std::uint64_t quotient = 0;
        while (true) {
            auto unary = br.read_bits(1);
            if (!unary.has_value()) {
                return std::unexpected(unary.error());
            }
            if (*unary == 0) {
                break;
            }
            ++quotient;
        }
        std::uint64_t magnitude = 0;
        if (*rice_bits != 0) {
            auto remainder = br.read_bits(static_cast<unsigned>(*rice_bits));
            if (!remainder.has_value()) {
                return std::unexpected(remainder.error());
            }
            magnitude = *remainder;
        }
        magnitude += quotient << *rice_bits;
        bool negative = false;
        if (magnitude != 0) {
            auto sign = br.read_bits(1);
            if (!sign.has_value()) {
                return std::unexpected(sign.error());
            }
            negative = *sign == 1;
        }
        if (magnitude > 0x7FFFFFFFULL) {
            return std::unexpected(IabError::kBadDlc);
        }
        const auto signed_value = static_cast<std::int32_t>(magnitude);
        out[i] = negative ? -signed_value : signed_value;
    }
    return {};
}

// B.8: applies the direct form predictors to the residual, region by region. The 64 entry
// history is cleared once at the start of the layer and carries across regions.
[[nodiscard]] std::vector<std::int32_t> apply_predictors(std::span<const std::int32_t> residual,
                                                         const std::vector<PredictorRegion>& regions,
                                                         unsigned sub_block_size) {
    if (regions.empty()) {
        return std::vector<std::int32_t>(residual.begin(), residual.end());
    }

    std::vector<std::int32_t> pcm(residual.size(), 0);
    std::array<std::int32_t, 64> buffer{};
    unsigned index1 = 0;
    std::size_t n = 0;
    for (const auto& region : regions) {
        for (unsigned j = 0; j < region.length; ++j) {
            for (unsigned k = 0; k < sub_block_size; ++k) {
                unsigned index2 = index1;
                std::uint64_t accum = 0;
                for (unsigned p = 1; p <= region.order; ++p) {
                    accum -= static_cast<std::uint64_t>(static_cast<std::int64_t>(buffer[index2]) *
                                                        static_cast<std::int64_t>(region.a_coeff[p]));
                    index2 = (index2 - 1U) & 63U;
                }
                const auto predicted = static_cast<std::int32_t>(static_cast<std::int64_t>(accum) >> 20);
                const std::int32_t output = wrap_add(residual[n], predicted);
                index1 = (index1 + 1U) & 63U;
                buffer[index1] = output;
                pcm[n] = output;
                ++n;
            }
        }
    }
    return pcm;
}

// One layer: predictor information, then NumDLCSubBlocks sub blocks of residual, then B.7 and
// B.8. The 48 kHz base layer and the 96 kHz extension layer have the same syntax.
[[nodiscard]] std::expected<std::vector<std::int32_t>, IabError> decode_layer(BitReader& br,
                                                                              unsigned num_sub_blocks,
                                                                              unsigned sub_block_size) {
    auto regions = read_predictor_info(br, num_sub_blocks);
    if (!regions.has_value()) {
        return std::unexpected(regions.error());
    }

    std::vector<std::int32_t> residual(static_cast<std::size_t>(num_sub_blocks) * sub_block_size, 0);
    for (unsigned n = 0; n < num_sub_blocks; ++n) {
        auto status = read_residual_sub_block(
            br, sub_block_size, residual.data() + static_cast<std::size_t>(n) * sub_block_size);
        if (!status.has_value()) {
            return std::unexpected(status.error());
        }
    }

    for (auto& region : *regions) {
        convert_lattice_to_direct(region);
    }
    return apply_predictors(residual, *regions, sub_block_size);
}

[[nodiscard]] std::int32_t shift_left(std::int32_t value, unsigned bits) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(value) << bits);
}

}  // namespace

std::vector<float> DlcAudio::normalized() const {
    std::vector<float> out;
    out.reserve(samples.size());
    for (const std::int32_t sample : samples) {
        out.push_back(static_cast<float>(static_cast<double>(sample) / 2147483648.0));
    }
    return out;
}

std::expected<DlcAudio, IabError> decode_dlc(const AudioDataDlc& element, std::uint8_t frame_rate_code,
                                              const DlcDecodeOptions& options) {
    if (frame_rate_code > 0x9) {
        return std::unexpected(IabError::kReservedFrameRate);
    }
    const auto layout = detail::dlc_layout(frame_rate_code);
    if (!layout.has_value()) {
        return std::unexpected(IabError::kBadDlc);
    }

    BitReader br(element.coded);
    auto sample_rate_code = br.read_bits(2);  // §10.7.3
    auto shift_bits = br.read_bits(5);         // §10.7.4
    if (!sample_rate_code.has_value() || !shift_bits.has_value()) {
        return std::unexpected(IabError::kTruncated);
    }
    if (*sample_rate_code > 1) {
        return std::unexpected(IabError::kBadDlc);
    }
    const bool is_96k = *sample_rate_code == 1;
    const auto shift = static_cast<unsigned>(*shift_bits);

    auto base = decode_layer(br, layout->num_sub_blocks, layout->sub_block_size_48);
    if (!base.has_value()) {
        return std::unexpected(base.error());
    }

    DlcAudio audio;
    if (!is_96k || options.base_layer_only) {
        // B.10: the 48 kHz output is the base layer shifted left by ShiftBits.
        audio.sample_rate = 48000;
        audio.samples = std::move(*base);
        for (auto& sample : audio.samples) {
            sample = shift_left(sample, shift);
        }
        return audio;
    }

    auto extension = decode_layer(br, layout->num_sub_blocks, layout->sub_block_size_96());
    if (!extension.has_value()) {
        return std::unexpected(extension.error());
    }

    // B.9 and B.10: add the upsampled base layer to the extension layer, then shift.
    const std::vector<std::int32_t> upsampled =
        detail::upsample_base_layer(*base, extension->size());
    audio.sample_rate = 96000;
    audio.samples = std::move(*extension);
    for (std::size_t n = 0; n < audio.samples.size(); ++n) {
        audio.samples[n] = shift_left(wrap_add(audio.samples[n], upsampled[n]), shift);
    }
    return audio;
}

std::expected<std::vector<AudioDataPcm>, IabError> decode_audio(const IaFrame& frame) {
    std::vector<AudioDataPcm> out = frame.audio_pcm;
    for (const auto& element : frame.audio_dlc) {
        auto audio = decode_dlc(element, frame.frame_rate_code);
        if (!audio.has_value()) {
            return std::unexpected(audio.error());
        }
        if (audio->sample_rate != frame.sample_rate) {
            return std::unexpected(IabError::kBadDlc);
        }
        out.push_back(AudioDataPcm{.audio_data_id = element.audio_data_id, .samples = audio->normalized()});
    }
    return out;
}

}  // namespace iclforge::iab
