#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "../support.hpp"

// `decode` and `probe` for TrueHD (0xF8726FBA) and MLP (0xF8726FBB) elementary streams, over
// iclforge::truehd. A stream is told by its first access unit's major sync, which follows the
// unit's four header bytes.

namespace forge_cli::commands {

// True when `stream` begins with an access unit that carries a major sync.
[[nodiscard]] bool is_truehd_stream(std::span<const std::byte> stream) noexcept;

// Decodes `stream` to a 24-bit PCM WAV at `out_path` ("-" is stdout), the channels in the order the
// stream carries them. Options: truehd-presentation= (best, 2ch, 6ch, 8ch, all) and truehd-check=
// (strict, the default, stops with kExitInput at the first failed check or damaged unit; lenient
// counts them and goes on).
int run_decode_truehd(std::span<const std::byte> stream, std::string_view in_path,
                      std::string_view out_path, const Options& meta);

// Describes the stream in `in_path` and checks every access unit's header, directory, parity and
// CRC bytes. Exit 1 when any of them fails.
int run_probe_truehd(std::string_view in_path, const Options& meta);

}  // namespace forge_cli::commands
