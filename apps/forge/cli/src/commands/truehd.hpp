#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <initializer_list>
#include <span>
#include <string_view>
#include <vector>

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

// A TrueHD stream with the frames of its AC-3 compatibility core taken out. A Blu-ray's TrueHD
// track is said to carry the core in the same stream as the access units (libs/containers/ERRATA.md,
// C9, which says what is and is not known of the layout); a decoder that meets an AC-3 frame
// between two access units does not read it as one.
struct CoreStrip {
    std::vector<std::byte> bytes;  // the access units, back to back
    std::size_t frames = 0;        // AC-3 syncframes set aside
    std::size_t frame_bytes = 0;
    std::size_t leading_bytes = 0;  // bytes dropped before the first access unit with a major sync
};

// Walks `stream` from its start taking, at each boundary, an AC-3 syncframe (a valid header, bsid
// at most 10, the whole frame in the data) or a TrueHD access unit that reads, and nothing else:
// the first byte that begins neither, once the first major sync has been seen, ends the walk and
// the rest of the stream is copied as it is for the elementary reader to resynchronise in. Before
// the first major sync, bytes that begin neither are dropped (a capture starts anywhere).
// std::nullopt when no AC-3 frame was found, so that a stream without a core is not copied.
[[nodiscard]] std::optional<CoreStrip> strip_ac3_core(std::span<const std::byte> stream);

// True when `stream` opens with one or more AC-3 syncframes and then an access unit with a major
// sync: a TrueHD file saved with its core, whose first bytes are the core's.
[[nodiscard]] bool starts_with_ac3_core_then_truehd(std::span<const std::byte> stream);

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
// document's "container" member (key and value) and `container_text` prints what the container said of the
// track; both empty for a bare stream.
int run_probe_truehd_stream(std::span<const std::byte> stream, std::string_view in_path,
                            const Options& meta,
                            const std::function<void(iclforge::apps::JsonSink&)>& container_json,
                            const std::function<void()>& container_text,
                            std::size_t ac3_core_frames = 0);

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

// --- options a TrueHD run does not apply -----------------------------------------------------------
// PROVISIONAL, until the owner answers question 7 of planning/truehd-parity-forge.md (and the entry
// in libs/truehd/ERRATA.md): an option that a run on TrueHD or MLP does not apply is refused by name
// with a usage error, as E-AC-3 refuses verify-content, and is never accepted and dropped. A
// verification flag (verify-content, check-content, verify-objects, syntax-trace= and the like) must
// never succeed without verifying. The only options warned about instead are the choices between
// ways of computing a transform (fast-mdct, fast-imdct, mode=), which cannot change the output or
// any verdict of a codec that has no transform.

// `applied` is what the command does with TrueHD or MLP, spelled as the help spells it
// ("truehd-presentation=", "verify"); quiet and verbose are always applied. Returns kExitOk, or
// names every other option the line carries (Options::given) on stderr and returns kExitUsage.
[[nodiscard]] int check_options_applied(const Options& meta,
                                        std::initializer_list<std::string_view> applied);

// The warning a command that also reads other streams gives when truehd-presentation= or
// truehd-check= is given and `stream_path` is not TrueHD or MLP: neither can change anything there.
void warn_truehd_options_unused(const Options& meta, std::string_view stream_path);

}  // namespace forge_cli::commands
