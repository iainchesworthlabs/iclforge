#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include "iclforge/iab/ac3iab.hpp"
#include "iclforge/iab/export.hpp"
#include "iclforge/iab/model.hpp"

// AudioDataDLC decoding, SMPTE ST 2098-2:2022 §9.6 / §10.7 / Annex B: the sample-rate-scalable
// lossless coder IAB uses for its monaural audio essence. The reader (ac3iab.hpp) keeps each
// element's coded bytes; this header turns them into PCM.
//
// The decoder follows Annex B's normative steps in order: unpack the bitstream (§9.6 Table 10),
// convert each region's lattice coefficients to direct form (B.7), run the integer predictors
// (B.8), and for a 96 kHz element upsample and add the 48 kHz base layer to the extension layer
// (B.9, B.10). All arithmetic is the integer arithmetic Annex B specifies, so the output is bit
// exact.

namespace iclforge::iab {

// One decoded AudioDataDLC element.
struct DlcAudio {
    std::uint32_t sample_rate = 48000;  // §10.7.3 DLCSampleRate resolved to Hz; 48000 when a
                                        // 96 kHz element was decoded with base_layer_only set
    // §B.10: 32-bit samples, already shifted left by ShiftBits, so full scale is +/-2^31
    // whatever the source bit depth. A 24-bit sample is bits 31..8, a 16-bit one bits 31..16.
    std::vector<std::int32_t> samples;

    // The same samples as normalized floats in [-1, 1), the convention AudioDataPcm uses.
    [[nodiscard]] std::vector<float> normalized() const;
};

struct DlcDecodeOptions {
    // B.5: a 96 kHz element carries a 48 kHz base layer that decodes on its own. Set this to
    // skip the extension layer and return the base layer at 48 kHz.
    bool base_layer_only = false;
};

// Decodes one element. `frame_rate_code` is the parent IAFrame's FrameRate (§10.2.4); it fixes
// the sub block count and size (§10.7.9 Table 30), which the bitstream does not carry.
//
// Fails with kBadDlc for a reserved DLCSampleRate, a frame rate AudioDataDLC cannot be used with
// (the non-integer 24000/1001 rate, §10.7), predictor region lengths that do not add up to the
// sub block count, a residual outside the 32-bit range, or a Rice code that does not terminate;
// with kReservedFrameRate for FrameRate codes 0xA-0xF; and with kTruncated when the element ends
// early.
[[nodiscard]] ICLFORGE_IAB_EXPORT std::expected<DlcAudio, IabError> decode_dlc(
    const AudioDataDlc& element, std::uint8_t frame_rate_code, const DlcDecodeOptions& options = {});

// Returns every audio essence of `frame` as normalized PCM: the AudioDataPcm elements as parsed,
// followed by each AudioDataDLC element decoded and converted to the same representation, in
// bitstream order. A DLC element whose DLCSampleRate disagrees with the frame's SampleRate is
// refused with kBadDlc (§10.2.2: the two fields "shall be the same").
[[nodiscard]] ICLFORGE_IAB_EXPORT std::expected<std::vector<AudioDataPcm>, IabError> decode_audio(
    const IaFrame& frame);

}  // namespace iclforge::iab
