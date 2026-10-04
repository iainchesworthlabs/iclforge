#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/iab/ac3iab.hpp"
#include "iclforge/iab/export.hpp"
#include "iclforge/iab/model.hpp"

// The IAB writer: turns the element graph in model.hpp back into SMPTE ST 2098-2:2022's
// bitstream. write_iaframe() and write_iabitstream() are the inverses of parse_iaframe() and
// parse_iabitstream(); encode_dlc() produces the AudioDataDLC elements (Annex B) a frame carries
// as lossless audio.
//
// The model holds resolved values (linear gains, positions on the unit cube, spreads), so the
// writer quantizes them to the nearest code the format has, with the exact inverse of the §5.4
// and §5.5 formulas. A value the reader produced from a code is written back as that code.
// Gains above unity cannot be expressed (§5.5's range is at most 1) and are written as unity.
//
// The output is the elementary IABitstream of §7, which parse_iabitstream() reads. The ST 2067-201
// MXF track-file wrapping is write_mxf_iab() in mxf.hpp.

namespace iclforge::iab {

enum class WriteError : std::uint8_t {
    kCannotOpen,           // the output path could not be opened
    kBadSampleRate,        // IaFrame::sample_rate is not 48000 or 96000 (§10.2.2)
    kBadBitDepth,          // IaFrame::bit_depth is not 16 or 24 (§10.2.3)
    kReservedFrameRate,    // frame_rate_code is 0xA-0xF (§10.2.4)
    kSubBlockCount,        // a sub block list is not NumPanSubBlocks long (§10.5.3 Table 23)
    kSampleCount,          // an AudioDataPCM or encode_dlc() input is not SampleCount samples long
                           // (§10.2.4 Table 18)
    kBadElement,           // a field is outside what its syntax can carry: a ChannelCount or sub
                           // element count beyond Plex's range, a remap matrix whose rows are not
                           // SourceChannels wide, a description text holding a NUL byte
    kDlcTooLarge,          // DLCSize does not fit its 16-bit field (§10.7.2)
    kDlcFrameRate,         // AudioDataDLC cannot be used at this frame rate (§10.7, Table 30)
};

[[nodiscard]] ICLFORGE_IAB_EXPORT std::string_view describe(WriteError error);

// Writes one IAFrame element's payload (§9.1 Table 5): no ElementID or ElementSize, the same
// span parse_iaframe() takes.
[[nodiscard]] ICLFORGE_IAB_EXPORT std::expected<std::vector<std::byte>, WriteError> write_iaframe(
    const IaFrame& frame);

// Writes a whole IABitstream (§7): for each frame the Preamble segment, then the IAFrame segment
// holding the frame as an IAElement(IA_FRAME).
[[nodiscard]] ICLFORGE_IAB_EXPORT std::expected<std::vector<std::byte>, WriteError> write_iabitstream(
    std::span<const IABitstreamFrame> frames);

[[nodiscard]] ICLFORGE_IAB_EXPORT std::expected<void, WriteError> write_iabitstream(
    const std::string& path, std::span<const IABitstreamFrame> frames);

struct DlcEncodeOptions {
    std::uint32_t sample_rate = 48000;  // 48000 or 96000; the IAFrame's SampleRate
    std::uint32_t bit_depth = 24;       // 16 or 24; the IAFrame's BitDepth
    // Highest linear prediction order to try, 0 to 31. 0 disables prediction (the minimal encoder
    // of Annex B.11). The encoder keeps whichever of the predicted and unpredicted layers codes
    // smaller.
    unsigned max_prediction_order = 8;
};

// Encodes one frame of one monaural waveform as an AudioDataDLC element (Annex B). `samples` is
// SampleCount long for the frame rate and sample rate (Table 18) and is quantized to
// `bit_depth` bits the way the AudioDataPCM path does, so decoding the element gives back exactly
// those integers (decode_dlc, normalized()). A 96 kHz frame is coded as a 48 kHz base layer plus
// the extension layer of B.3; the base layer is a low-passed, decimated copy of the input.
[[nodiscard]] ICLFORGE_IAB_EXPORT std::expected<AudioDataDlc, WriteError> encode_dlc(
    std::uint32_t audio_data_id, std::span<const float> samples, std::uint8_t frame_rate_code,
    const DlcEncodeOptions& options = {});

}  // namespace iclforge::iab
