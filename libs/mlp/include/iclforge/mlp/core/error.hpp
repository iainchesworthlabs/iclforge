#pragma once

#include <cstdint>
#include <string_view>

#include "iclforge/mlp/export.hpp"

// The failures of the TrueHD (MLP) library, one value per conformance check point of the
// specification's Appendix E and per malformed-input condition the syntax can meet. Every one is
// returned through std::expected; none is an assertion, since each describes input a caller can be
// handed (CONTRIBUTING.md, "No exceptions for stream-level failure").

namespace iclforge::mlp {

enum class Error : std::uint8_t {
    kTruncated,              // the data ends inside a structure
    kBadAccessUnitLength,    // access_unit_length is 0, or runs past the data (Appendix E)
    kCheckNibble,            // the access unit header's check nibble does not give 0xF (3.2)
    kNoMajorSync,            // decoding needs a major sync and none has been seen (20.3)
    kMajorSyncCrc,           // major_sync_info_crc (4.1, Appendix E)
    kMajorSyncSignature,     // signature is neither 0xB752 nor 0xB753 (4.3)
    kUnsupportedStreamType,  // 0xBB (DVD-Audio MLP, FBB): not decoded (planning/truehd.md)
    kBadSampleRate,          // a reserved sample rate code (1.4)
    kBadSubstreamCount,      // num_substreams is 0 or above 4 (4.1)
    kSubstreamEndPointer,    // substream_end_ptr goes backwards or past the access unit (6.3)
    kRestartInMajorSync,     // restart_nonexistent set in an access unit with a major sync (6.5)
    kNoRestartHeader,        // a substream's data before its first restart header (20.3)
    kRestartSync,            // restart_sync_word not one of 0x31EA, 0x31EB, 0x31EC (7.2)
    kRestartHeaderCrc,       // restart_header_crc (7.1, Appendix E)
    kChannelRange,           // min_chan > max_chan, or max_chan past the substream's limit (7.4)
    kMatrixChannel,          // max_matrix_chan past the stream type's maximum (10.3)
    kChannelAssign,          // ch_assign[ch] past max_matrix_chan (7.4)
    kBlockSize,              // block_size outside 8 to 160 (8.4)
    kTooManySamples,         // the blocks of a segment exceed the access unit's samples
    kMatrixCount,            // num_primitive_matrices past the substream's maximum (10.3)
    kMatrixOutputChannel,    // matrix_out_ch past max_matrix_chan (10.2)
    kMatrixFracBits,         // frac_bits above 14 (10.2)
    kNoiseChannel,           // a matrix takes a noise channel, whose generator is unspecified (17)
    kOutputShift,            // output_shift past max_shift (9.3)
    kQuantStepSize,          // quant_step_size above huff_lsbs, or above 15 (11.1)
    kFilterOrder,            // FIR order above 8, IIR above 4, or their sum above 8 (9.3)
    kFilterCoeffBits,        // coeff_bits outside 1 to 16, or coeff_bits + coeff_shift > 16 (9.3)
    kFilterCoeffQ,           // coeff_q outside 8 to 15, or different for filters A and B (9.3)
    kFirState,               // filter A carries state data (9.2)
    kIirStateBits,           // state_bits outside 1 to 15 (9.3)
    kHuffLsbs,               // huff_lsbs above 24 (above 31 in a 0x31EC substream) (8.3)
    kHuffmanCode,            // a codeword not in the codebook (11.4, Appendix C)
    kSampleRange,            // a decoded sample past the 24-bit range (11.4, Appendix E)
    kFilterOverload,         // a prediction past the substream's range (9.3, Appendix E)
    kBlockDataBits,          // block_data_bits above 16,000 or not what was read (8.1)
    kBlockDataCrc,           // block_data_crc (8.1)
    kSegmentOverrun,         // a segment's syntax runs past its substream_end_ptr
    kSubstreamParity,        // substream_parity (6.2, Appendix E)
    kSubstreamCrc,           // substream_crc (6.2, Appendix E)
    kLosslessCheck,          // lossless_check (7.3, Appendix E)
    kExtraDataLength,        // extra_data_length past the access unit (16.1, Appendix E)
    kExtraDataParity,        // extra_data_parity (16.1, Appendix E)
    kInvalidConfiguration,   // an encoder or decoder configuration outside what is supported
    kRateExceeded,           // the encoder could not meet peak_data_rate (18.2)
    kVerificationFailed,     // the encoder's self-check decode differed from its input (19.2)
};

// A sentence naming the check that failed, for logs and the command line.
[[nodiscard]] ICLFORGE_MLP_EXPORT std::string_view describe(Error error) noexcept;

}  // namespace iclforge::mlp
