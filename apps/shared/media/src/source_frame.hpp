#pragma once

#include <cstddef>
#include <span>

// What a source supplies to an encoder, one frame at a time.
//
// Every file encode reads its channels a frame (1,536 samples of AC-3) at a
// time, and the last frame of a source that is not whole frames has to be
// filled with something past the end of the audio. The library's encoders pad
// nothing: encode_frame() takes a whole frame and the caller decides.
//
// The answer is the channel's last real sample, held, and not silence. A hard
// drop to zero at the end of a file is itself a transient: the encoder's own
// §8.2.2 detector flags it and spends a block switch on a discontinuity that
// exists only because the buffer ends mid-frame, and the side information
// starves that frame's mantissa budget (commit e89f5f6c7: every channel's
// last-frame SNR was 23 to 28 dB on the 5.1 reference file, and 67 to 88 dB
// with the last sample held).
//
// `forge encode` has always held it, in four loops of its own; the Forge GUI
// wrote zero in four of its own, so one plan gave two different last frames
// (planning/karaoke-phase2.md, W1). This is the one place that says which, for
// both: compiled straight into forge, forge-gui and iclforge-app-media-tests,
// as sink_wait.hpp beside it is, so a test can hold the policy and a window's
// bytes can be compared with the command line's.

namespace iclforge::apps {

// Samples [start, start + out.size()) of `channel`, a source whose first
// sample is `offset` samples into the programme: silence ahead of it (an
// offset is real silence, not padding), the source's own samples, then its
// last sample held. An empty channel supplies silence throughout.
inline void fill_source_frame(std::span<const float> channel, std::size_t offset, std::size_t start,
                              std::span<float> out) noexcept {
    const float hold = channel.empty() ? 0.0F : channel.back();
    for (std::size_t i = 0; i < out.size(); ++i) {
        const std::size_t at = start + i;
        if (at < offset) {
            out[i] = 0.0F;
            continue;
        }
        const std::size_t shifted = at - offset;
        out[i] = shifted < channel.size() ? channel[shifted] : hold;
    }
}

}  // namespace iclforge::apps
