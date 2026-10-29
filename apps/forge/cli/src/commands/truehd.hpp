#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "../support.hpp"

// TrueHD (MLP) in forge (libs/mlp, planning/truehd.md): 'truehd-encode', and 'decode', 'probe'
// and 'transcode' when the stream is TrueHD or the output is. TrueHD is lossless, so these read
// and write integer PCM WAV directly, never through the floating-point paths the lossy codecs
// take: a 16-bit or 24-bit WAV encoded and decoded again comes back sample for sample.

namespace forge_cli {

// Whether `bytes` is a TrueHD or MLP elementary stream: a major sync word (0xF8726FBA or
// 0xF8726FBB) four bytes in, after the first access unit's header.
[[nodiscard]] bool is_truehd_stream(std::span<const std::byte> bytes);

// Whether an output name asks for TrueHD: .thd, .mlp or .truehd.
[[nodiscard]] bool is_truehd_path(std::string_view path);

int run_truehd_encode(std::string_view in_wav, std::string_view out_path, const Options& meta);
int run_truehd_decode(std::span<const std::byte> stream, std::string_view out_wav,
                      const Options& meta);
int run_truehd_probe(std::span<const std::byte> stream, std::string_view in_path,
                     const Options& meta);
// Atmos (iclforge/mlp/encoder/atmos.hpp): a 7.1 bed WAV and an objects WAV (1 to 8 channels, one
// object each) to a 16-channel TrueHD stream with object audio metadata. The objects move on
// fixed paths, object o circling the room at (1 + o / 2) turns a second, every fourth access unit
// a new position; the library takes any metadata, this command is for exercising it.
int run_truehd_atmos_encode(std::string_view bed_wav, std::string_view objects_wav,
                            std::string_view out_path, const Options& meta);
// The bed, the objects and the metadata (as JSON) of an Atmos TrueHD stream.
int run_truehd_atmos_decode(std::string_view in_path, std::string_view bed_wav,
                            std::string_view objects_wav, std::string_view metadata_json,
                            const Options& meta);

// A transcode with TrueHD on either side: through integer PCM when both sides are TrueHD, and
// through a WAV the other codec's own command reads or writes otherwise.
int run_truehd_transcode(std::string_view in_path, std::string_view out_path, std::uint32_t bitrate,
                         std::string_view layout, const Options& meta);

}  // namespace forge_cli
