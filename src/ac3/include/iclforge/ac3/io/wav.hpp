#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "iclforge/ac3/core/types.hpp"
#include "iclforge/ac3/export.hpp"
#include "iclforge/base/wav.hpp"

// WAV reading and writing are iclforge::base's (iclforge/base/wav.hpp, since
// planning/consolidation.md's C6), and their names are kept here for a release.
// What is AC-3's is how a WAV's channels map onto A/52's coded order.

namespace iclforge::ac3::io {

using base::describe;
using base::read_wav;
using base::WavData;
using base::WavError;
using base::WavPcm16StreamWriter;
using base::WavStreamReader;
using base::WavStreamWriter;
using base::write_wav_f32;
using base::write_wav_pcm16_raw;

// A WAV file's channel order (the WAVE_FORMAT_EXTENSIBLE convention: FL, FR,
// FC, LFE, BL, BR) is not A/52 Table 5.8's (L, C, R, SL, SR, LFE), so the two
// have to be reconciled before any multichannel file reaches the encoder.
struct Ac3Layout {
    Acmod acmod = Acmod::k2_0;
    bool lfe = false;
    // wav_index[k] is the position in a WAV frame of AC-3 channel k.
    std::vector<std::size_t> wav_index;
};

// The AC-3 layout that carries a WAV of this width, or nothing when no legal
// acmod does (7 channels and up, or none at all).
[[nodiscard]] ICLFORGE_AC3_EXPORT std::optional<Ac3Layout> ac3_layout_for(std::size_t wav_channels);

// The inverse permutation, in the form write_wav_f32 takes: entry i names the
// AC-3 channel that belongs at WAV position i.
//
// Every acmod is placed by WAVE_FORMAT_EXTENSIBLE speaker position, not by
// A/52 coded order - including the two mono-surround modes, which sit on
// SPEAKER_BACK_CENTER. Two things move relative to the bitstream: C swaps
// with R (WAV is FL FR FC, A/52 is L C R), and the LFE moves from last to
// fourth. So 3/1 goes out L R C S and 3/1+LFE goes out L R C LFE S, which is
// what FFmpeg and every other WAV consumer expect. The one exception is 1+1,
// which carries two independent programmes rather than a soundfield and so
// has no speaker positions to sort; it goes out in coded order.
[[nodiscard]] ICLFORGE_AC3_EXPORT std::vector<std::size_t> wav_channel_order(Acmod acmod, bool lfe);

}  // namespace iclforge::ac3::io
