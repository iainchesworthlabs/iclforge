#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "iclforge/ac3/core/tables.hpp"
#include "iclforge/ac3/decoder/transient_prenoise.hpp"
#include "iclforge/ac3/detail/decode_scalar.hpp"

// The decoder's own way into iclforge::ac3::apply_transient_prenoise: the same
// correction, with the synthesis buffer supplied by the caller instead of
// allocated per call, so a stream whose transients the decoder corrects a few
// times a second does not allocate for each of them.

namespace iclforge::ac3::internal {

// The most §3.7.2's synthesis buffer holds: 2*TC1 + pnlen, pnlen at most two
// blocks less transprocloc's four-sample step.
inline constexpr int kTransientPrenoiseMaxSynthesis =
    2 * kTransientPrenoiseTC1 + 2 * kSamplesPerBlock - 4;  // 1020

// §3.7.2's two cross-fades, a raised cosine each (the "standard Hanning windows" the
// section names): fade_in runs 0 -> 1 over the region, fade_out is its complement.
// Computed in double and narrowed once to the decode scalar, as output.cpp narrows
// its taps - the scalar the cross-fades' products run in, so the fixed-point tier's
// PCM cannot differ between two machines through a float expression.
//
// A value the CALLER holds and not a table of this file's own. They were function-
// local statics, 3,072 bytes of .bss at Q7.24 or float that every image linking the
// decoder carried whether or not a stream ever used transient pre-noise processing -
// and on an ESP32-C6 whose internal RAM is the player's and the network's, almost
// none of the streams do (FFmpeg's encoder never sets transproce). The decoder
// keeps one in the identity's hold-back (eac3_decoder.cpp's TransientHold), which is
// allocated at the first frame that signals a correction and let go with the
// decoder's state, so a stream that never uses the tool pays for it nowhere.
struct TransientPrenoiseWindows {
    using Scalar = decode_scalar_t;
    static constexpr auto kTc1 = static_cast<std::size_t>(kTransientPrenoiseTC1);
    static constexpr auto kTc2 = static_cast<std::size_t>(kTransientPrenoiseTC2);

    std::array<Scalar, kTc1> tc1_in{};
    std::array<Scalar, kTc1> tc1_out{};
    std::array<Scalar, kTc2> tc2_in{};
    std::array<Scalar, kTc2> tc2_out{};

    TransientPrenoiseWindows();
};

// iclforge::ac3::apply_transient_prenoise, same contract, with `synthesis` holding at
// least kTransientPrenoiseMaxSynthesis samples of scratch and `windows` the
// cross-fades (above) the caller keeps.
void apply_transient_prenoise(std::span<float> pcm, int transloc, int translen,
                              std::span<float> synthesis, const TransientPrenoiseWindows& windows);

}  // namespace iclforge::ac3::internal
