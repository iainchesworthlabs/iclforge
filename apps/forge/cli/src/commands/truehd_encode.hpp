#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/base/wav.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"

// What `truehd-encode` and `truehd-atmos` share: where the stream goes, how a WAV's samples become
// the encoder's integers, and how an encoder error is said.

namespace forge_cli::commands {

enum class EncodeOutput : std::uint8_t { kRaw, kMatroska, kMp4, kTransport, kUnknown };

[[nodiscard]] std::string lower_extension(std::string_view path);

// A bare .thd (or "-"), Matroska, MP4, a Blu-ray transport stream, or kUnknown.
[[nodiscard]] EncodeOutput output_kind(std::string_view out_path);

// The WAV reader's [-1, 1) floats back to the 24 bit integers they were read from, interleaved:
// frames `begin` to `end`, channel k of the stream from WAV channel `wav_index[k]`.
void interleave_wav(const iclforge::ac3::io::WavData& wav, std::span<const std::size_t> wav_index,
                    std::size_t begin, std::size_t end, std::vector<std::int32_t>& out);

[[nodiscard]] int exit_code_for(iclforge::truehd::EncodeErrorKind kind);

// Says an encoder error on stderr, for `command`.
void report_encode_error(std::string_view command, const iclforge::truehd::EncodeError& error);

// Writes the whole stream to a file or, for "-", stdout.
[[nodiscard]] bool write_raw_stream(std::string_view out_path, std::span<const std::byte> bytes);

}  // namespace forge_cli::commands
