#include "iclforge/mlp/core/stream_info.hpp"

#include "iclforge/mlp/core/error.hpp"

namespace iclforge::mlp {

std::optional<std::uint32_t> sample_rate_from_code(std::uint8_t code) noexcept {
    // Section 1.4: 0 to 2 are the 48 kHz family, 8 to 10 the 44.1 kHz family.
    switch (code) {
        case 0x0:
            return 48000U;
        case 0x1:
            return 96000U;
        case 0x2:
            return 192000U;
        case 0x8:
            return 44100U;
        case 0x9:
            return 88200U;
        case 0xA:
            return 176400U;
        default:
            return std::nullopt;
    }
}

std::optional<std::uint8_t> sample_rate_code(std::uint32_t hz) noexcept {
    for (std::uint8_t code = 0; code < 16; ++code) {
        if (sample_rate_from_code(code) == hz) {
            return code;
        }
    }
    return std::nullopt;
}

namespace {

// Section 13.1, bit by bit: the speakers each bit names, in carriage order.
struct AssignmentBit {
    int count;
    std::string_view first;
    std::string_view second;
};
constexpr std::array<AssignmentBit, 13> kAssignmentBits{{
    {2, "L", "R"},
    {1, "C", ""},
    {1, "LFE", ""},
    {2, "Ls", "Rs"},
    {2, "Lvh", "Rvh"},
    {2, "Lc", "Rc"},
    {2, "Lrs", "Rrs"},
    {1, "Cs", ""},
    {1, "Ts", ""},
    {2, "Lsd", "Rsd"},
    {2, "Lw", "Rw"},
    {1, "Cvh", ""},
    {1, "LFE2", ""},
}};

}  // namespace

int channel_count(std::uint16_t assignment) noexcept {
    int n = 0;
    for (std::size_t bit = 0; bit < kAssignmentBits.size(); ++bit) {
        if (((assignment >> bit) & 1U) != 0) {
            n += kAssignmentBits[bit].count;
        }
    }
    return n;
}

SpeakerList speakers(std::uint16_t assignment) noexcept {
    SpeakerList list;
    for (std::size_t bit = 0; bit < kAssignmentBits.size(); ++bit) {
        if (((assignment >> bit) & 1U) == 0) {
            continue;
        }
        const auto& b = kAssignmentBits[bit];
        if (list.count < kMaxChannels) {
            list.labels[static_cast<std::size_t>(list.count++)] = b.first;
        }
        if (b.count == 2 && list.count < kMaxChannels) {
            list.labels[static_cast<std::size_t>(list.count++)] = b.second;
        }
    }
    return list;
}

std::uint32_t FormatInfo::pack() const noexcept {
    return (std::uint32_t{sample_rate_code} & 0xFU) << 28U |
           (std::uint32_t{reserved} & 0xFU) << 24U | (std::uint32_t{twoch_modifier} & 0x3U) << 22U |
           (std::uint32_t{sixch_modifier} & 0x3U) << 20U |
           (std::uint32_t{sixch_assignment} & 0x1FU) << 15U |
           (std::uint32_t{eightch_modifier} & 0x3U) << 13U |
           (std::uint32_t{eightch_assignment} & 0x1FFFU);
}

FormatInfo FormatInfo::unpack(std::uint32_t word) noexcept {
    FormatInfo f;
    f.sample_rate_code = static_cast<std::uint8_t>(word >> 28U);
    f.reserved = static_cast<std::uint8_t>((word >> 24U) & 0xFU);
    f.twoch_modifier = static_cast<std::uint8_t>((word >> 22U) & 0x3U);
    f.sixch_modifier = static_cast<std::uint8_t>((word >> 20U) & 0x3U);
    f.sixch_assignment = static_cast<std::uint8_t>((word >> 15U) & 0x1FU);
    f.eightch_modifier = static_cast<std::uint8_t>((word >> 13U) & 0x3U);
    f.eightch_assignment = static_cast<std::uint16_t>(word & 0x1FFFU);
    return f;
}

std::uint32_t MajorSync::sample_rate() const noexcept {
    return sample_rate_from_code(format.sample_rate_code).value_or(0U);
}

int MajorSync::samples_per_access_unit() const noexcept {
    return mlp::samples_per_access_unit(sample_rate());
}

std::uint64_t MajorSync::peak_bits_per_second() const noexcept {
    // P2: peak_data_rate is in units of fs / 16 bits per second.
    return std::uint64_t{peak_data_rate} * sample_rate() / 16U;
}

std::string_view describe(Error error) noexcept {
    switch (error) {
        case Error::kTruncated:
            return "the data ends inside a structure";
        case Error::kBadAccessUnitLength:
            return "access_unit_length is too small for an access unit";
        case Error::kCheckNibble:
            return "the access unit header's check nibble does not verify (3.2)";
        case Error::kNoMajorSync:
            return "no major sync has been seen (20.3)";
        case Error::kMajorSyncCrc:
            return "major_sync_info_crc does not verify (4.1)";
        case Error::kMajorSyncSignature:
            return "the major sync signature is neither 0xB752 nor 0xB753 (4.3)";
        case Error::kUnsupportedStreamType:
            return "a DVD-Audio MLP (0xF8726FBB) stream, which is not decoded";
        case Error::kBadSampleRate:
            return "a reserved sample rate code (1.4)";
        case Error::kBadSubstreamCount:
            return "num_substreams is outside 1 to 4 (4.1)";
        case Error::kSubstreamEndPointer:
            return "a substream_end_ptr goes backwards or past the access unit (6.3)";
        case Error::kRestartInMajorSync:
            return "restart_nonexistent is set in an access unit with a major sync (6.5)";
        case Error::kNoRestartHeader:
            return "a substream has data before its first restart header (20.3)";
        case Error::kRestartSync:
            return "restart_sync_word is not 0x31EA, 0x31EB or 0x31EC (7.2)";
        case Error::kRestartHeaderCrc:
            return "restart_header_crc does not verify (7.1)";
        case Error::kChannelRange:
            return "min_chan or max_chan is out of range for the substream (7.4)";
        case Error::kMatrixChannel:
            return "max_matrix_chan is out of range (10.3)";
        case Error::kChannelAssign:
            return "a ch_assign value exceeds max_matrix_chan (7.4)";
        case Error::kBlockSize:
            return "block_size is outside 8 to 160 (8.4)";
        case Error::kTooManySamples:
            return "a segment's blocks hold more samples than an access unit";
        case Error::kMatrixCount:
            return "num_primitive_matrices exceeds the substream's maximum (10.3)";
        case Error::kMatrixOutputChannel:
            return "matrix_out_ch exceeds max_matrix_chan (10.2)";
        case Error::kMatrixFracBits:
            return "frac_bits exceeds 14 (10.2)";
        case Error::kNoiseChannel:
            return "a primitive matrix takes a noise channel, whose generator is not specified "
                   "(17)";
        case Error::kOutputShift:
            return "output_shift exceeds max_shift (9.3)";
        case Error::kQuantStepSize:
            return "quant_step_size exceeds the channel's huff_lsbs (11.1)";
        case Error::kFilterOrder:
            return "a prediction filter's order is out of range (9.3)";
        case Error::kFilterCoeffBits:
            return "coeff_bits or coeff_shift is out of range (9.3)";
        case Error::kFilterCoeffQ:
            return "coeff_q is out of range or differs between filters A and B (9.3)";
        case Error::kFirState:
            return "filter A carries state data (9.2)";
        case Error::kIirStateBits:
            return "state_bits is zero (9.3)";
        case Error::kHuffLsbs:
            return "huff_lsbs is out of range (8.3)";
        case Error::kHuffmanCode:
            return "a Huffman codeword is not in the codebook (11.4)";
        case Error::kSampleRange:
            return "a decoded sample exceeds the 24-bit range (11.4)";
        case Error::kFilterOverload:
            return "a prediction filter's output exceeds its range (9.3)";
        case Error::kBlockDataBits:
            return "block_data_bits does not match the bits read (8.1)";
        case Error::kBlockDataCrc:
            return "block_data_crc does not verify (8.1)";
        case Error::kSegmentOverrun:
            return "a substream segment's syntax does not end at its end pointer (6.2)";
        case Error::kSubstreamParity:
            return "substream_parity does not verify (6.2)";
        case Error::kSubstreamCrc:
            return "substream_crc does not verify (6.2)";
        case Error::kLosslessCheck:
            return "lossless_check does not verify (7.3)";
        case Error::kExtraDataLength:
            return "extra_data_length exceeds the access unit (16.1)";
        case Error::kExtraDataParity:
            return "extra_data_parity does not verify (16.1)";
        case Error::kInvalidConfiguration:
            return "the configuration is outside what is supported";
        case Error::kRateExceeded:
            return "the stream cannot be coded within peak_data_rate (18.2)";
        case Error::kVerificationFailed:
            return "the encoder's self-check decode differs from its input (19.2)";
    }
    return "unknown error";
}

}  // namespace iclforge::mlp
