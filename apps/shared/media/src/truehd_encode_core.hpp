#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/ac3/io/wav.hpp"
#include "iclforge/truehd/core/format.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"

// The encode that `forge truehd-encode` and the Forge window's TrueHD encode share: a WAV's PCM,
// coded losslessly by iclforge::truehd::Encoder into a bare stream (apps/forge/cli/src/commands/
// truehd_encode.cpp wraps it in Matroska, MP4 or a transport stream; the window writes the bare
// stream). Nothing here prints; a problem comes back as a sentence the caller says its own way.
//
// Compiled into each application that uses it, like the rest of apps/shared/media/src: it needs
// iclforge::truehd and iclforge::ac3 (the WAV reader), which both applications link.

namespace iclforge::apps::truehd_encode {

// How the WAV's channels go into the stream: the channel assignment (0 for the encoder's default
// for the count) and, for stream channel k, the WAV channel that carries it.
struct Layout {
    std::uint32_t assignment = 0;
    std::vector<std::size_t> wav_index;
    std::vector<std::string> warnings;  // said, not fatal
};

// A file with no channel mask is taken in the order `decode` writes (the default layout of its
// count). One with a mask names its speakers, which are put into the order of the assignment they
// make; a mask that names speakers TrueHD has no channel for, or that is not one assignment, is an
// error (the sentence). A back pair is the surround pair when the file has no side pair, and the
// rear surround pair when it has both: this program's reading (libs/truehd/ERRATA.md, open
// question 16).
[[nodiscard]] std::expected<Layout, std::string> layout_of(const iclforge::ac3::io::WavData& wav);

// ---- several sources, named places ---------------------------------------------------------------
//
// `map=` for truehd-encode, the grammar of the other encoders with TrueHD's channel names for the
// places (planning/truehd-parity-forge.md, question 16, option A): `<source>.<channel>:<place>`,
// comma separated, source and channel counted from 0, the place one of L R C LFE Ls Rs Lvh Rvh Lc Rc
// Lrs Rrs Cs Ts Lsd Rsd Lw Rw Cvh LFE2 (any case). The places named have to make one channel
// assignment (TH spec §13.1: the pairs and singles of `libs/truehd`'s table); the stream's channels
// are then in that assignment's order, whatever the order of the map.

struct Placement {
    std::size_t source = 0;   // 0 is the positional input, 1 the first src=, and so on
    std::size_t channel = 0;  // of that source, from 0
    iclforge::truehd::Channel place{};
};

// The channel a name stands for, in any case; none for anything else.
[[nodiscard]] std::optional<iclforge::truehd::Channel> channel_named(std::string_view name);

// A map= value. The error is a sentence for the user.
[[nodiscard]] std::expected<std::vector<Placement>, std::string> parse_map(std::string_view spec);

struct Assembled {
    iclforge::ac3::io::WavData wav;  // the placed channels, in the stream's order
    Layout layout;                   // its assignment, and the identity order of `wav`
};

// Puts the placed channels of `sources` into one WAV in the stream's order. A source starts
// `offset_samples[i]` samples in (silence ahead of it) and a shorter source is padded with silence
// to the longest. Refused, with a sentence: a place named twice, a source or channel that is not
// there, sources of more than one sample rate, a source none of whose channels is placed, and a
// set of places that is not a channel assignment (the sentence then names the nearest sets).
[[nodiscard]] std::expected<Assembled, std::string> assemble(
    std::span<const iclforge::ac3::io::WavData> sources, std::span<const std::size_t> offset_samples,
    std::span<const Placement> placements);

// `samples` samples of silence ahead of every channel.
void pad_leading(iclforge::ac3::io::WavData& wav, std::size_t samples);


struct Outcome {
    std::vector<std::byte> stream;
    std::uint64_t frames = 0;  // samples per channel
    std::uint64_t access_units = 0;
    int channels = 0;
    int substreams = 0;
    std::uint32_t sample_rate = 0;
    iclforge::truehd::EncoderStats stats{};
};

// Called after each chunk with the samples per channel coded so far and the total; false stops the
// encode (the result is then an error that says it was cancelled).
using Progress = std::function<bool(std::uint64_t done, std::uint64_t total)>;

struct Failure {
    iclforge::truehd::EncodeError error;
    bool cancelled = false;  // the progress callback said stop; `error` is then not a fault
};

// Codes the whole WAV. `config` is the caller's, with the sample rate, the channel count, the bit
// depth and the channel assignment filled in here from the WAV and `layout`.
[[nodiscard]] std::expected<Outcome, Failure> encode_wav(
    const iclforge::ac3::io::WavData& wav, const Layout& layout,
    iclforge::truehd::EncoderConfig config, const Progress& progress = {});

}  // namespace iclforge::apps::truehd_encode
