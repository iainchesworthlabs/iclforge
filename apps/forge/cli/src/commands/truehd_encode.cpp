// `truehd-encode`: PCM in a WAV to a TrueHD stream, over iclforge::truehd::Encoder
// (libs/truehd/include/iclforge/truehd/encoder/encoder.hpp). The stream is a bare .thd, or is
// wrapped as Matroska, MP4 or a Blu-ray transport stream by the same writers `mkv`, `mp4` and `ts`
// use.

#include <fmt/base.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../exit_codes.hpp"
#include "../platform/stdio_binary.hpp"
#include "../support.hpp"
#include "iclforge/base/wav.hpp"
#include "iclforge/truehd/core/format.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"
#include "iclforge/truehd/io/metadata_edit.hpp"
#include "truehd.hpp"
#include "truehd_encode_core.hpp"
#include "truehd_meaning.hpp"

namespace forge_cli::commands {

namespace {

namespace th = iclforge::truehd;


enum class Output : std::uint8_t { kRaw, kMatroska, kMp4, kTransport, kUnknown };

std::string lower_extension(std::string_view path) {
    std::string ext = std::filesystem::path{std::string{path}}.extension().string();
    std::ranges::transform(ext, ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

Output output_kind(std::string_view out_path) {
    if (is_stdio_path(out_path)) {
        return Output::kRaw;
    }
    const std::string ext = lower_extension(out_path);
    if (ext == ".thd") {
        return Output::kRaw;
    }
    if (ext == ".mkv" || ext == ".mka" || ext == ".webm") {
        return Output::kMatroska;
    }
    if (ext == ".mp4" || ext == ".m4a" || ext == ".mov") {
        return Output::kMp4;
    }
    if (ext == ".ts" || ext == ".m2ts") {
        return Output::kTransport;
    }
    return Output::kUnknown;
}


int exit_code_for(th::EncodeErrorKind kind) {
    switch (kind) {
        case th::EncodeErrorKind::kInvalidConfig:
        case th::EncodeErrorKind::kUnsupported:
        case th::EncodeErrorKind::kRateExceeded:
            return kExitUsage;
        case th::EncodeErrorKind::kBadInput:
            return kExitInput;
        case th::EncodeErrorKind::kVerifyFailed:
            return kExitRuntime;
        case th::EncodeErrorKind::kInternal:
            return kExitInternal;
    }
    return kExitInternal;
}

void report(const th::EncodeError& error) {
    fmt::println(stderr, "error: truehd-encode: {}{}{}", th::to_string(error.kind),
                 error.detail.empty() ? "" : ": ", error.detail);
    if (error.kind == th::EncodeErrorKind::kRateExceeded) {
        fmt::println(stderr,
                     "  the stream cannot be sent within the peak rate and TH spec §18's 75 ms: "
                     "raise truehd-peak-rate= or write it anyway with truehd-rate-check=lenient");
    }
}

bool write_raw(std::string_view out_path, std::span<const std::byte> bytes) {
    if (is_stdio_path(out_path)) {
        iclforge::cli::platform::set_stdio_binary();
        return std::fwrite(bytes.data(), 1, bytes.size(), stdout) == bytes.size() &&
               std::fflush(stdout) == 0;
    }
    std::ofstream out{std::string{out_path}, std::ios::binary};
    if (!out) {
        fmt::println(stderr, "error: cannot write {}", out_path);
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    // A file that fits the stream's buffer reaches the kernel only here; a full disk shows now.
    out.flush();
    if (!out) {
        fmt::println(stderr, "error: write failed for {}", out_path);
        return false;
    }
    return true;
}

}  // namespace

int run_truehd_encode(std::string_view in_path, std::string_view out_path, const Options& meta) {
    // The metadata, layout and multi-source options of the other encoders are not applied here:
    // refused by name, so that a second source or a dialnorm is not dropped from a run that exits 0.
    if (const int refused = check_options_applied(
            meta,
            {"truehd-downmix=", "truehd-downmix-gains=", "truehd-restart=", "truehd-peak-rate=",
             "truehd-rate-check=", "truehd-fir-order=", "truehd-matrix=", "truehd-stereo-mode=",
             "truehd-dialnorm-2ch=", "truehd-dialnorm-6ch=", "truehd-dialnorm-8ch=",
             "truehd-mix-level-2ch=", "truehd-mix-level-6ch=", "truehd-mix-level-8ch=",
             "truehd-source-format-6ch=", "truehd-source-format-8ch=", "truehd-control-2ch=",
             "truehd-control-6ch=", "truehd-control-8ch=", "truehd-heavy-drc-start-up-gain=",
             "truehd-drc-start-up-gain=", "verify"});
        refused != kExitOk) {
        return refused;
    }
    const Options::TruehdEncode& opts = meta.thdenc;
    const Output kind = output_kind(out_path);
    if (kind == Output::kUnknown) {
        fmt::println(
            stderr,
            "error: truehd-encode writes a bare .thd, or Matroska (.mkv), MP4 (.mp4) or a "
            "Blu-ray transport stream (.ts) - not '{}'{}",
            lower_extension(out_path),
            lower_extension(out_path) == ".mlp" ? " (the encoder writes TrueHD, not MLP)" : "");
        return kExitUsage;
    }

    auto wav = read_wav_arg(in_path);
    if (!wav) {
        fmt::println(stderr, "error: {}: {}", in_path, iclforge::ac3::io::describe(wav.error()));
        return kExitInput;
    }
    if (wav->frame_count() == 0) {
        fmt::println(stderr, "error: {}: no samples to encode", in_path);
        return kExitInput;
    }

    const auto layout = iclforge::apps::truehd_encode::layout_of(*wav);
    if (!layout) {
        fmt::println(stderr, "error: {}", layout.error());
        return kExitInput;
    }
    for (const std::string& warning : layout->warnings) {
        fmt::println(stderr, "warning: {}", warning);
    }

    th::EncoderConfig config;
    config.channel_assignment = layout->assignment;
    config.sample_rate = wav->sample_rate;
    config.channels = static_cast<int>(wav->channels.size());
    config.bits = 24;
    config.downmix =
        opts.downmix == "standard" ? th::DownmixMode::kStandard : th::DownmixMode::kNone;
    if (opts.downmix_gains) {
        const std::array<double, 4>& g = *opts.downmix_gains;
        config.downmix_gains = th::DownmixGains{
            .centre = g[0], .surround = g[1], .lfe = g[2], .rear_to_surround = g[3]};
    }
    if (opts.restart_units) {
        config.restart_interval_units = *opts.restart_units;
    }
    if (opts.peak_rate_kbps) {
        config.peak_data_rate_bps = *opts.peak_rate_kbps * 1000U;
    }
    config.allow_rate_violations = opts.rate_lenient;
    if (opts.fir_order) {
        config.max_fir_order = *opts.fir_order;
    }
    config.matrix_search = opts.matrix;
    config.stereo_mode = opts.stereo_mode;
    // The channel meaning (TH spec §14.1): the fields the line names, the rest zero as FFmpeg's
    // streams have them. Provisional, no oracle until the DEE set (ERRATA.md, questions 51 to 55).
    if (const std::string_view field = th::apply_edit(meta.thd_meaning, config.channel_meaning);
        !field.empty()) {
        fmt::println(stderr, "error: truehd-encode: {}", field);
        return kExitUsage;
    }
    config.verify = meta.verify;

    auto outcome = iclforge::apps::truehd_encode::encode_wav(*wav, *layout, config);
    if (!outcome) {
        report(outcome.error().error);
        return exit_code_for(outcome.error().error.kind);
    }
    const std::vector<std::byte>& stream = outcome->stream;
    const std::uint64_t access_units = outcome->access_units;
    const std::size_t frames = wav->frame_count();

    const th::EncoderStats& stats = outcome->stats;
    const double pcm_bytes =
        static_cast<double>(frames) * static_cast<double>(config.channels) * 3.0;
    FILE* status = status_stream(out_path);
    const auto describe = [&] {
        status_println(status,
                       "encoded {} samples of {} channel(s) at {} Hz: {} access units, {} restart "
                       "intervals, {} bytes ({:.1f}% of the 24-bit PCM), {} substream(s)",
                       frames, config.channels, config.sample_rate, access_units,
                       stats.restart_intervals, stream.size(),
                       100.0 * static_cast<double>(stream.size()) / pcm_bytes,
                       outcome->substreams);
        if (stats.rate_violations != 0) {
            status_println(
                status,
                "warning: {} access unit(s) exceed the peak rate (truehd-rate-check=lenient)",
                stats.rate_violations);
        }
        if (stats.downmix_reduced_intervals != 0) {
            status_println(
                status,
                "{} restart interval(s) had the downmix gains lowered to keep it within 24 bits",
                stats.downmix_reduced_intervals);
        }
        if (config.verify) {
            status_println(status, "verified: {} access units decode to the input",
                           stats.verified_units);
        }
        if (!meta.thd_meaning.empty()) {
            // Written into every major sync; no decoder here applies it, and what the values
            // mean is not settled (ERRATA.md, questions 51 to 55).
            status_println(status, "channel meaning written (provisional, no oracle until the DEE set):");
            for (const MeaningPresentation presentation :
                 {MeaningPresentation::k2ch, MeaningPresentation::k6ch,
                  MeaningPresentation::k8ch}) {
                status_println(status, "  {}  {}", presentation_name(presentation),
                               describe_presentation(config.channel_meaning, presentation));
            }
            status_println(status, "  drc start-up  {}",
                           describe_start_up_gains(config.channel_meaning));
        }
    };

    if (kind == Output::kRaw) {
        if (!write_raw(out_path, stream)) {
            return kExitOutput;
        }
        describe();
        status_println(status, "wrote {} ({} bytes)", is_stdio_path(out_path) ? "stdout" : out_path,
                       stream.size());
        return kExitOk;
    }
    describe();
    if (kind == Output::kMatroska) {
        return run_mkv_truehd(stream, in_path, out_path);
    }
    if (kind == Output::kMp4) {
        return run_mp4_truehd(stream, in_path, out_path);
    }
    return run_ts_truehd(stream, in_path, out_path, "");
}

}  // namespace forge_cli::commands
