#include <fmt/base.h>
#include <fmt/format.h>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include "../exit_codes.hpp"
#include "../support.hpp"
#include "encode.hpp"
#include "iclforge/ac3/encoder/plan.hpp"
#include "truehd.hpp"

// `transcode` of a TrueHD or MLP stream to AC-3 or E-AC-3: the decode of the presentation
// truehd-presentation= names (the best by default, as `decode` chooses it) re-encoded by the
// command that encodes a WAV for the target codec, so that the layout, the bitrate and the metadata
// options mean what they mean there. planning/truehd-parity-forge.md, question 5: no E-AC-3 to
// TrueHD (a lossless copy of a lossy decode), and none to AC-4 from TrueHD here.
//
// The stream's own dialogue norm is not carried: its meaning is provisional (libs/truehd/ERRATA.md,
// questions 51 to 55), so the encoder's default stands unless dialnorm= says otherwise, and the
// report says so.

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
    if (!meta.dialnorm_given && !quiet_mode()) {
        fmt::println(stderr,
                     "note: the dialogue norm {} carries is not carried across (its meaning is "
                     "provisional); the encoder's default stands. Pass dialnorm=<1..31> or "
                     "dialnorm=auto",
                     in_path);
    }
    const int encoded = target == plan::Codec::kAc3
                            ? run_encode(wav.string(), out_path, bitrate, false, layout, meta)
                            : run_eac3_encode(wav.string(), out_path, bitrate, "none", layout, "off",
                                              meta);
    remove_wav();
    return encoded;
}

}  // namespace forge_cli::commands
