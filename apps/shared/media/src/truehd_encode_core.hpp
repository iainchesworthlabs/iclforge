#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string>
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
