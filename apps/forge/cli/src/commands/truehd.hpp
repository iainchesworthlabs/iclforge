#pragma once

#include <cstddef>
#include <functional>
#include <span>
#include <string_view>

#include "../support.hpp"

namespace iclforge::apps {
class JsonSink;
}  // namespace iclforge::apps

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

// The same over a stream already read, which a container gave up. `container_json` writes the
// document's "container" member's value and `container_text` prints what the container said of the
// track; both empty for a bare stream.
int run_probe_truehd_stream(std::span<const std::byte> stream, std::string_view in_path,
                            const Options& meta,
                            const std::function<void(iclforge::apps::JsonSink&)>& container_json,
                            const std::function<void()>& container_text);

// --- containers and transports (truehd_containers.cpp) ---------------------------------------
// Each takes the elementary stream (a TrueHD or MLP stream, container already removed) and writes
// the file. TH spec §21 names the transports and gives no layout; libs/containers/ERRATA.md records
// what each writes and what the one oracle, FFmpeg's muxer, shows.

int run_mkv_truehd(std::span<const std::byte> stream, std::string_view in_path,
                   std::string_view out_path);
int run_mp4_truehd(std::span<const std::byte> stream, std::string_view in_path,
                   std::string_view out_path);
// `profile_name` is empty or "bluray"; any other profile is refused (Blu-ray's stream_type 0x83 is
// the only TrueHD carriage in TH spec §21.2).
int run_ts_truehd(std::span<const std::byte> stream, std::string_view in_path,
                  std::string_view out_path, std::string_view profile_name);
// MAT 1.0 bursts (TH spec §21.3): an eight-channel PCM16 WAV when `out_path` ends in .wav, else the
// raw bursts.
int run_spdif_truehd(std::span<const std::byte> stream, std::string_view in_path,
                     std::string_view out_path);
// True when the carrier (a WAV or raw bytes) opens with an IEC 61937 burst of data type 22.
[[nodiscard]] bool is_mat_carrier(std::span<const std::byte> carrier) noexcept;
int run_unspdif_mat(std::span<const std::byte> carrier, std::string_view in_path,
                    std::string_view out_path, bool keep_partial);

// `levels` and `loudness` over a decode of the stream (BS.1770-5 Annex 3 weights from the channel
// names; truehd-presentation= and truehd-check= apply).
int run_levels_truehd(std::span<const std::byte> stream, std::string_view in_path,
                      const Options& meta);
int run_loudness_truehd(std::span<const std::byte> stream, std::string_view in_path,
                        const Options& meta);

// `truehd-encode`: the PCM in a WAV, coded losslessly as TrueHD (iclforge::truehd::Encoder) into a
// bare .thd, or wrapped as .mkv, .mp4 or a Blu-ray .ts. The WAV's channel mask names its speakers
// (libs/truehd/ERRATA.md, open question 16); a WAV with none is taken in the order `decode` writes
// (L R C LFE Ls Rs Lrs Rrs). "-" as the output is a bare stream on stdout.
int run_truehd_encode(std::string_view in_path, std::string_view out_path, const Options& meta);

// Says that `command` does not read TrueHD or MLP and returns kExitUsage.
int refuse_truehd(std::string_view command, std::string_view in_path);

}  // namespace forge_cli::commands
