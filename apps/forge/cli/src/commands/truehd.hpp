#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../support.hpp"
#include "iclforge/ac3/encoder/plan.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/major_sync.hpp"
#include "probe_truehd.hpp"
#include "truehd_meaning.hpp"

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
// counts them and goes on). `loss` takes the units a lenient decode lost after the first one
// decoded: bytes the reader skipped (the decoder is told, lost_data()), units it could not read,
// and units that waited for a restart header (`decode strict` makes them an input fault).
int run_decode_truehd(std::span<const std::byte> stream, std::string_view in_path,
                      std::string_view out_path, const Options& meta, DecodeLoss& loss);

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

// --- measuring a stream (truehd_containers.cpp) ---
// What `normalize` and `qc` read of a TrueHD stream in one pass: the loudness of each presentation
// they ask for (BS.1770-5 Annex 3 weights from the speakers the major sync names, as `loudness`
// weights them), the checks of every substream, and the data rate against TH spec §18's model.

struct PresentationLoudness {
    MeaningPresentation presentation = MeaningPresentation::k2ch;
    int channels = 0;
    std::uint32_t sample_rate = 0;
    std::string layout;    // the channel names in decode order
    bool unnamed = false;  // the major sync does not name the speakers for this count: no weights
    std::uint64_t samples = 0;  // per channel
    std::optional<double> integrated_lkfs = std::nullopt;
    std::optional<double> loudness_range_lu = std::nullopt;
    std::optional<double> true_peak_dbtp = std::nullopt;
};

// TH spec §18's data rate model and its report: apps/shared/media/src/probe_truehd.hpp.
using RateReport = iclforge::apps::truehd_probe::RateReport;

struct TruehdMeasurement {
    std::vector<PresentationLoudness> presentations;        // as asked, in the order asked
    std::optional<iclforge::truehd::MajorSync> major_sync;  // the first
    int substreams = 0;
    std::uint64_t units = 0;          // access units the reader handed out
    std::size_t skipped_bytes = 0;    // bytes that did not read as an access unit
    std::uint64_t decode_errors = 0;  // units a decoder could not read (lenient runs only)
    iclforge::truehd::DecoderCounters counters{};  // of the decoder that read every substream
    RateReport rate{};
};

// Decodes `stream` once for each presentation in `presentations`, metering it, and (with
// `integrity`) once more through every substream for the counters of the checks. With `lenient`
// a failed check is counted and the run goes on; otherwise the first failure is reported on
// stderr and nothing is returned. Also nothing when no access unit reads.
[[nodiscard]] std::optional<TruehdMeasurement> measure_truehd(
    std::span<const std::byte> stream, std::string_view in_path,
    std::span<const MeaningPresentation> presentations, bool integrity, bool lenient);

// `metadata` of a TrueHD stream (truehd_tools.cpp): the channel meaning of TH spec section 14.1 set in
// every major sync of the file's content `bytes`, re-stamping each one's CRC, and written to `out_path`.
// E-AC-3's own options are refused by name. Provisional, no oracle until the DEE set (libs/truehd/
// ERRATA.md, questions 51 to 55).
int run_metadata_truehd(std::vector<std::byte>&& bytes, std::string_view in_path,
                        std::string_view out_path, const Options& meta);

// `normalize` of a TrueHD stream (truehd_tools.cpp): the integrated loudness of each presentation
// the stream has (BS.1770-5 Annex 3 weights, as `loudness` weights them) mapped to a dialogue norm
// the way E-AC-3's normalize maps it (ATSC A/85 section 8: round(-LKFS), 1 to 31), and written into
// that presentation's field in every major sync. No gain is applied, since the stream is lossless.
int run_normalize_truehd(std::vector<std::byte>&& bytes, std::string_view in_path,
                         std::string_view out_path, const Options& meta);

// `qc` of a TrueHD stream (truehd_tools.cpp): an integrity gate (the lossless check of every
// restart interval, the check nibbles, major sync, restart header, parity and CRC of every unit,
// and TH spec section 18's data-rate model against the peak rate the stream declares) and the
// loudness of each presentation, with the dialogue norm it carries beside the measurement and the
// preset= gates applied to the presentation truehd-presentation= names (the largest by default).
// Exit 6 when the integrity gate or a preset gate fails. The dialogue norm's meaning is provisional,
// no oracle until the DEE set (libs/truehd/ERRATA.md, questions 51 to 56), and so is the rate model,
// which is the encoder's own (no player has been asked).
int run_qc_truehd(std::span<const std::byte> stream, std::string_view in_path, const Options& meta);

// `truehd-encode`: the PCM in a WAV, coded losslessly as TrueHD (iclforge::truehd::Encoder) into a
// bare .thd, or wrapped as .mkv, .mp4 or a Blu-ray .ts. The WAV's channel mask names its speakers
// (libs/truehd/ERRATA.md, open question 16); a WAV with none is taken in the order `decode` writes
// (L R C LFE Ls Rs Lrs Rrs). "-" as the output is a bare stream on stdout.
int run_truehd_encode(std::string_view in_path, std::string_view out_path, const Options& meta);

// `cut` and `cat` of TrueHD or MLP elementary streams (truehd_cut.cpp). A cut starts at the first access
// unit that carries a major sync at or after the requested time, says which time it used, and keeps
// whole units; a join refuses an input that does not open with a major sync or whose shape differs.
// Bytes are never changed (libs/truehd/ERRATA.md, entry 80; provisional until owner question 14).
int run_cut_truehd(std::span<const std::byte> stream, std::string_view in_path,
                   std::string_view out_path, std::string_view start_seconds,
                   std::string_view duration_seconds, const Options& meta);
int run_cat_truehd(std::string_view out_path, std::span<const std::string_view> in_paths,
                   const Options& meta);

// `transcode` of a TrueHD or MLP stream to AC-3 or E-AC-3 (truehd_transcode.cpp): the decode of
// the presentation truehd-presentation= names, encoded as the target codec's WAV encode does, so
// that layout, bitrate and metadata options mean what they mean there. Refused for AC-4. The stream's
// dialogue norm is not carried (provisional, libs/truehd/ERRATA.md questions 51 to 55).
int run_transcode_truehd(std::span<const std::byte> stream, std::string_view in_path,
                         std::string_view out_path, std::uint32_t bitrate,
                         std::string_view layout, const Options& meta,
                         iclforge::ac3::plan::Codec target);

// The dialogue norm a TrueHD stream stores for the presentation `presentation` names ("" or "best"
// is the largest the stream has), as an AC-3 or E-AC-3 dialnorm (1 to 31): the 6 and 8 channel
// fields as they are, the 2 channel field less 6 (libs/truehd/ERRATA.md, questions 51 to 55).
// Nothing for a stream that stores none (a field of 0, as FFmpeg's streams have) or for MLP.
[[nodiscard]] std::optional<int> stored_dialnorm(std::span<const std::byte> stream,
                                                 std::string_view presentation);

// `silence` and `sine` with a .thd output (truehd_synth.cpp): a generated signal coded by the
// lossless encoder. `layout` is mono, stereo, 51 or 71 (stereo when empty); the AC-3 bit rate and
// metadata options are refused.
int run_truehd_synth(std::string_view out_path, std::uint32_t seconds, bool sine,
                     std::uint32_t freq_hz, std::uint32_t amplitude_pct, std::string_view layout,
                     const Options& meta);

// Says that `command` does not read TrueHD or MLP, and why when `reason` is given, and returns
// kExitUsage.
int refuse_truehd(std::string_view command, std::string_view in_path,
                  std::string_view reason = {});

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
                                        std::span<const std::string_view> applied);
[[nodiscard]] inline int check_options_applied(const Options& meta,
                                               std::initializer_list<std::string_view> applied) {
    return check_options_applied(meta, std::span<const std::string_view>(applied.begin(), applied.size()));
}

// The warning a command that also reads other streams gives when truehd-presentation= or
// truehd-check= is given and `stream_path` is not TrueHD or MLP: neither can change anything there.
void warn_truehd_options_unused(const Options& meta, std::string_view stream_path);

}  // namespace forge_cli::commands
