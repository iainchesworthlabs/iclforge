#include <fmt/base.h>
#include <fmt/format.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include "../exit_codes.hpp"
#include "../support.hpp"
#include "encode.hpp"
#include "iclforge/ac3/encoder/plan.hpp"
#include "iclforge/truehd/io/elementary.hpp"
#include "iclforge/truehd/io/major_sync.hpp"
#include "truehd.hpp"

// `transcode` of a TrueHD or MLP stream to AC-3 or E-AC-3: the decode of the presentation
// truehd-presentation= names (the best by default, as `decode` chooses it) re-encoded by the
// command that encodes a WAV for the target codec, so that the layout, the bitrate and the metadata
// options mean what they mean there. planning/truehd-parity-forge.md, question 5: no E-AC-3 to
// TrueHD (a lossless copy of a lossy decode), and none to AC-4 from TrueHD here.
//
// The stream's own dialogue norm is carried (stored_dialnorm): the 6 and 8 channel fields as the
// dialnorm, the 2 channel field less 6, as two DEE encodes of one programme settle them
// (libs/truehd/ERRATA.md, questions 51 to 55); dialnorm= overrides it and dialnorm=auto measures.

namespace forge_cli::commands {

namespace {

namespace fs = std::filesystem;

// A name in the system's temporary directory that this process has not used.
fs::path scratch_wav_path() {
    static std::atomic<unsigned> counter{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() /
           fmt::format("forge-transcode-{}-{}.wav", stamp, counter.fetch_add(1));
}

// The dialogue norm a TrueHD stream stores for the presentation a decode delivers, as an AC-3 or
// E-AC-3 dialnorm (1 to 31). The 6 and 8 channel fields (5 bits) hold the programme's dialogue norm
// n for -n dBFS, the same value as AC-3's; the 2 channel field (6 bits) holds it plus 6. Read from
// two DEE encodes of one programme at two settings, whose fields followed the dialnorm in both
// (libs/truehd/ERRATA.md, questions 51 to 55; one programme, not yet a second title). Nothing for a
// stream that stores none (a field of 0, as FFmpeg's streams have) or for MLP.
std::optional<int> stored_dialnorm(std::span<const std::byte> stream, std::string_view presentation) {
    namespace th = iclforge::truehd;
    const auto info = th::probe_stream(stream);
    if (!info || info->major_sync.kind != th::StreamKind::kTrueHd) {
        return std::nullopt;
    }
    const th::ChannelMeaning& meaning = info->major_sync.channel_meaning;
    const int available = std::min(info->num_substreams, 3);
    int which = available;  // 1: the 2 channel, 2: the 6 channel, 3: the 8 channel
    if (presentation == "2ch") {
        which = 1;
    } else if (presentation == "6ch") {
        which = 2;
    } else if (presentation == "8ch") {
        which = 3;
    }
    which = std::min(which, available);
    const int value = which == 1 ? static_cast<int>(meaning.twoch_dialogue_norm) - 6
                      : which == 2 ? static_cast<int>(meaning.sixch_dialogue_norm)
                                   : static_cast<int>(meaning.eightch_dialogue_norm);
    if (value < 1 || value > 31) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

int run_transcode_truehd(std::span<const std::byte> stream, std::string_view in_path,
                         std::string_view out_path, std::uint32_t bitrate,
                         std::string_view layout, const Options& meta,
                         iclforge::ac3::plan::Codec target) {
    namespace plan = iclforge::ac3::plan;
    if (target == plan::Codec::kAc4) {
        fmt::println(stderr,
                     "error: transcode writes AC-3 or E-AC-3 from {}, which is TrueHD or MLP; "
                     "there is no TrueHD to AC-4 here, and no E-AC-3 to TrueHD",
                     in_path);
        return kExitUsage;
    }
    if (const int refused = check_options_applied(
            meta, {"truehd-presentation=", "truehd-check=", "dialnorm=", "codec=", "drc="});
        refused != kExitOk) {
        return refused;
    }

    // The decode goes to a WAV that the encode command reads back; nothing of it is printed, since
    // the output may be stdout.
    const fs::path wav = scratch_wav_path();
    const bool was_quiet = quiet_mode();
    const bool was_verbose = verbose_mode();
    set_verbosity(true, false);
    DecodeLoss loss;
    const int decoded = run_decode_truehd(stream, in_path, wav.string(), meta, loss);
    set_verbosity(was_quiet, was_verbose);
    const auto remove_wav = [&wav] {
        std::error_code ec;
        fs::remove(wav, ec);
    };
    if (decoded != kExitOk) {
        remove_wav();
        return decoded;
    }
    // The dialogue norm the stream stores for the presentation decoded is carried across, as the
    // DD+ path carries its source's, unless dialnorm= says otherwise (dialnorm=auto measures).
    Options encode_meta = meta;
    if (!meta.dialnorm_given) {
        if (const auto stored = stored_dialnorm(stream, meta.truehd_presentation)) {
            encode_meta.p.dialnorm = static_cast<decltype(encode_meta.p.dialnorm)>(*stored);
            if (!quiet_mode()) {
                fmt::println(stderr,
                             "note: dialnorm {} carried from the dialogue norm {} stores for the "
                             "presentation decoded (one programme's encodes confirm the field; "
                             "libs/truehd/ERRATA.md, questions 51 to 55)",
                             *stored, in_path);
            }
        } else if (!quiet_mode()) {
            fmt::println(stderr,
                         "note: {} stores no dialogue norm (its field is 0, as FFmpeg's streams "
                         "have), so the encoder's default stands. Pass dialnorm=<1..31> or "
                         "dialnorm=auto",
                         in_path);
        }
    }
    const int encoded = target == plan::Codec::kAc3
                            ? run_encode(wav.string(), out_path, bitrate, false, layout, encode_meta)
                            : run_eac3_encode(wav.string(), out_path, bitrate, "none", layout, "off",
                                              encode_meta);
    remove_wav();
    return encoded;
}

}  // namespace forge_cli::commands
