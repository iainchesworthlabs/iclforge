// `truehd-encode`: PCM in a WAV to a TrueHD stream, over iclforge::truehd::Encoder
// (libs/truehd/include/iclforge/truehd/encoder/encoder.hpp). The stream is a bare .thd, or is
// wrapped as Matroska, MP4 or a Blu-ray transport stream by the same writers `mkv`, `mp4` and `ts`
// use.

#include <fmt/base.h>
#include <fmt/format.h>
#include <algorithm>
#include <array>
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
#include <utility>
#include <vector>

#include "../exit_codes.hpp"
#include "../multi_source.hpp"
#include "../platform/stdio_binary.hpp"
#include "../support.hpp"
#include "iclforge/ac3/io/wav.hpp"
#include "iclforge/base/wav.hpp"
#include "iclforge/truehd/core/format.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"
#include "iclforge/truehd/encoder/ratectl.hpp"
#include "iclforge/truehd/io/metadata_edit.hpp"
#include "truehd.hpp"
#include "truehd_encode.hpp"
#include "truehd_encode_core.hpp"
#include "truehd_meaning.hpp"
#include "truehd_rate.hpp"
#include "truehd_wrap.hpp"

namespace forge_cli::commands {

namespace th = iclforge::truehd;
namespace wrap = iclforge::apps::truehd_wrap;

namespace {

constexpr double kFullScale = 8388608.0;  // 2^23: a 24 bit sample against the WAV reader's [-1, 1)

}  // namespace

std::string lower_extension(std::string_view path) {
    std::string ext = std::filesystem::path{std::string{path}}.extension().string();
    std::ranges::transform(ext, ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

EncodeOutput output_kind(std::string_view out_path) {
    if (is_stdio_path(out_path)) {
        return EncodeOutput::kRaw;
    }
    const std::string ext = lower_extension(out_path);
    if (ext == ".thd") {
        return EncodeOutput::kRaw;
    }
    if (const auto container = wrap::container_of(out_path)) {
        switch (*container) {
            case wrap::Container::kMatroska:
                return EncodeOutput::kMatroska;
            case wrap::Container::kMp4:
                return EncodeOutput::kMp4;
            case wrap::Container::kTransport:
                return EncodeOutput::kTransport;
        }
    }
    return EncodeOutput::kUnknown;
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

void report_encode_error(std::string_view command, const th::EncodeError& error) {
    fmt::println(stderr, "error: {}: {}{}{}", command, th::to_string(error.kind),
                 error.detail.empty() ? "" : ": ", error.detail);
    if (error.kind == th::EncodeErrorKind::kRateExceeded) {
        fmt::println(stderr,
                     "  the stream cannot be sent within the peak rate and TH spec §18's 75 ms: "
                     "raise truehd-peak-rate= or write it anyway with truehd-rate-check=lenient "
                     "(truehd-atmos can also let the objects give up low bits with "
                     "truehd-rate-mode=shape)");
    }
}

// The WAV reader's [-1, 1) floats back to the integers they were read from: a 16 or 24 bit PCM
// sample survives the trip exactly (a float32 holds 24 bits), and a wider one or a float is rounded
// to 24 bits. Stream channel k comes from WAV channel `wav_index[k]`.
void interleave_wav(const iclforge::ac3::io::WavData& wav, std::span<const std::size_t> wav_index,
                    std::size_t begin, std::size_t end, std::vector<std::int32_t>& out) {
    const std::size_t channels = wav.channels.size();
    out.resize((end - begin) * channels);
    for (std::size_t i = begin; i < end; ++i) {
        for (std::size_t c = 0; c < channels; ++c) {
            const double scaled =
                std::nearbyint(static_cast<double>(wav.channels[wav_index[c]][i]) * kFullScale);
            out[(i - begin) * channels + c] =
                static_cast<std::int32_t>(std::clamp(scaled, -kFullScale, kFullScale - 1.0));
        }
    }
}

bool write_raw_stream(std::string_view out_path, std::span<const std::byte> bytes) {
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

int run_truehd_encode(std::string_view in_path, std::string_view out_path, const Options& meta) {
    // The metadata, layout and multi-source options of the other encoders are not applied here:
    // refused by name, so that a second source or a dialnorm is not dropped from a run that exits 0.
    if (const int refused = check_options_applied(
            meta,
            {"truehd-downmix=", "truehd-downmix-gains=", "truehd-restart=", "truehd-peak-rate=",
             "truehd-rate-check=", "truehd-rate-mode=", "truehd-bitrate=",
             "truehd-major-sync=", "truehd-fir-order=", "truehd-matrix=", "truehd-stereo-mode=",
             "truehd-dialnorm-2ch=", "truehd-dialnorm-6ch=", "truehd-dialnorm-8ch=",
             "truehd-mix-level-2ch=", "truehd-mix-level-6ch=", "truehd-mix-level-8ch=",
             "truehd-source-format-6ch=", "truehd-source-format-8ch=", "truehd-control-2ch=",
             "truehd-control-6ch=", "truehd-control-8ch=", "truehd-heavy-drc-start-up-gain=",
             "truehd-drc-start-up-gain=", "verify", "src=", "map=", "offset="});
        refused != kExitOk) {
        return refused;
    }
    const Options::TruehdEncode& opts = meta.thdenc;
    const EncodeOutput kind = output_kind(out_path);
    if (kind == EncodeOutput::kUnknown) {
        fmt::println(
            stderr,
            "error: truehd-encode writes a bare .thd, or Matroska (.mkv), MP4 (.mp4) or a "
            "Blu-ray transport stream (.ts) - not '{}'{}",
            lower_extension(out_path),
            lower_extension(out_path) == ".mlp" ? " (the encoder writes TrueHD, not MLP)" : "");
        return kExitUsage;
    }

    // src= and offset= are about where each channel of each source goes, and a channel nobody
    // placed is a mistake, not a default (the other encoders say the same): src= needs map=.
    if (!meta.sources.empty() && !meta.map_spec) {
        fmt::println(stderr,
                     "error: truehd-encode: src= needs map= to say where each channel of each source "
                     "goes, as in map=0.0:L,0.1:R,1.0:C,2.0:LFE,2.1:Ls,2.2:Rs");
        return kExitUsage;
    }
    for (const auto& [index, seconds] : meta.offsets) {
        if (index > meta.sources.size()) {
            fmt::println(stderr,
                         "error: truehd-encode: offset={}:{} names source {}, but there {} {} (the "
                         "positional input is source 0, each src= the next)",
                         index, seconds, index, meta.sources.size() + 1 == 1 ? "is" : "are",
                         meta.sources.size() + 1);
            return kExitUsage;
        }
    }

    auto primary = read_wav_arg(in_path);
    if (!primary) {
        fmt::println(stderr, "error: {}: {}", in_path, iclforge::ac3::io::describe(primary.error()));
        return kExitInput;
    }
    std::optional<iclforge::ac3::io::WavData> wav;
    std::optional<iclforge::apps::truehd_encode::Layout> layout;
    std::string placed;  // map=: where each channel of the stream came from
    if (meta.map_spec) {
        // The places are TrueHD's channel names; the channels of every source go into one WAV in
        // the order of the assignment they make (truehd_encode_core.hpp).
        const auto placements = iclforge::apps::truehd_encode::parse_map(*meta.map_spec);
        if (!placements) {
            fmt::println(stderr, "error: truehd-encode: {}", placements.error());
            return kExitUsage;
        }
        std::vector<iclforge::ac3::io::WavData> sources;
        sources.push_back(std::move(*primary));
        for (const std::string& path : meta.sources) {
            auto more = iclforge::ac3::io::read_wav(path);
            if (!more) {
                fmt::println(stderr, "error: {}: {}", path, iclforge::ac3::io::describe(more.error()));
                return kExitInput;
            }
            sources.push_back(std::move(*more));
        }
        std::vector<std::size_t> offset_samples(sources.size());
        for (std::size_t i = 0; i < sources.size(); ++i) {
            offset_samples[i] = offset_samples_for(meta.offsets, i, sources.front().sample_rate);
        }
        auto joined =
            iclforge::apps::truehd_encode::assemble(sources, offset_samples, *placements);
        if (!joined) {
            fmt::println(stderr, "error: truehd-encode: {}", joined.error());
            return kExitUsage;
        }
        for (const iclforge::apps::truehd_encode::Placement& p : *placements) {
            placed += fmt::format("{}{} <- {}.{}", placed.empty() ? "" : ", ",
                                  th::channel_name(p.place), p.source, p.channel);
        }
        wav = std::move(joined->wav);
        layout = std::move(joined->layout);
    } else {
        wav = std::move(*primary);
        if (const std::size_t lead = offset_samples_for(meta.offsets, 0, wav->sample_rate);
            lead != 0) {
            iclforge::apps::truehd_encode::pad_leading(*wav, lead);
        }
        const auto named = iclforge::apps::truehd_encode::layout_of(*wav);
        if (!named) {
            fmt::println(stderr, "error: {}", named.error());
            return kExitInput;
        }
        layout = *named;
    }
    if (wav->frame_count() == 0) {
        fmt::println(stderr, "error: {}: no samples to encode", in_path);
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
    // The settings the window's Encode TrueHD options share (truehd_encode_core.hpp), the channel
    // meaning (TH spec §14.1) among them: the fields the line names, the rest zero as FFmpeg's
    // streams have them. Provisional, no oracle until the DEE set (ERRATA.md, questions 51 to 55).
    if (const auto applied = iclforge::apps::truehd_encode::apply(
            iclforge::apps::truehd_encode::Settings{.standard_downmix = opts.downmix == "standard",
                                                    .downmix_gains = opts.downmix_gains,
                                                    .restart_units = opts.restart_units,
                                                    .peak_rate_kbps = opts.peak_rate_kbps,
                                                    .rate_lenient = opts.rate_lenient,
                                                    .bitrate_kbps = opts.bitrate_kbps,
                                                    .major_sync_units = opts.major_sync_units,
                                                    .fir_order = opts.fir_order,
                                                    .matrix = opts.matrix,
                                                    .stereo_mode = opts.stereo_mode,
                                                    .verify = meta.verify,
                                                    .meaning = meta.thd_meaning},
            config);
        !applied) {
        fmt::println(stderr, "error: truehd-encode: {}", applied.error());
        return kExitUsage;
    }

    auto outcome = iclforge::apps::truehd_encode::encode_wav(*wav, *layout, config);
    if (!outcome) {
        report_encode_error("truehd-encode", outcome.error().error);
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
        if (!placed.empty()) {
            status_println(status, "placed {} (from {} source(s))", placed, meta.sources.size() + 1);
        }
        if (stats.rate_violations != 0) {
            status_println(
                status,
                "warning: {} access unit(s) exceed the peak rate (truehd-rate-check=lenient)",
                stats.rate_violations);
        }
        if (opts.bitrate_kbps) {
            status_println(status, "  rate: held to {} kbit/s over {} ms; least headroom {} ms",
                           *opts.bitrate_kbps, th::kRateLatencyMs,
                           format_headroom_ms(stats.min_headroom_us));
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

    if (kind == EncodeOutput::kRaw) {
        if (!write_raw_stream(out_path, stream)) {
            return kExitOutput;
        }
        describe();
        status_println(status, "wrote {} ({} bytes)", is_stdio_path(out_path) ? "stdout" : out_path,
                       stream.size());
        return kExitOk;
    }
    describe();
    if (kind == EncodeOutput::kMatroska) {
        return run_mkv_truehd(stream, in_path, out_path);
    }
    if (kind == EncodeOutput::kMp4) {
        return run_mp4_truehd(stream, in_path, out_path);
    }
    return run_ts_truehd(stream, in_path, out_path, "");
}

}  // namespace forge_cli::commands
