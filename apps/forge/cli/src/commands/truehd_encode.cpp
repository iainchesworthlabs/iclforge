// `truehd-encode`: PCM in a WAV to a TrueHD stream, over iclforge::truehd::Encoder
// (libs/truehd/include/iclforge/truehd/encoder/encoder.hpp). The stream is a bare .thd, or is
// wrapped as Matroska, MP4 or a Blu-ray transport stream by the same writers `mkv`, `mp4` and `ts`
// use.

#include <fmt/base.h>
#include <fmt/format.h>
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
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../exit_codes.hpp"
#include "../platform/stdio_binary.hpp"
#include "../support.hpp"
#include "iclforge/truehd/core/format.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"
#include "truehd.hpp"

namespace forge_cli::commands {

namespace {

namespace th = iclforge::truehd;

constexpr std::size_t kChunkFrames = 65536;
constexpr double kFullScale = 8388608.0;  // 2^23: a 24-bit sample against the WAV reader's [-1, 1)

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

// The TrueHD channel a WAVE_FORMAT_EXTENSIBLE speaker bit (0-based) stands for: the inverse of what
// `decode` writes in its channel mask. A back pair is the surround pair when the file has no side
// pair (the usual 5.1 mask is front, LFE and back), and the rear surround pair when it has both
// (the usual 7.1 mask). This is this program's reading, not the specification's (libs/truehd
// ERRATA.md, open question 16).
std::optional<th::Channel> channel_of_speaker_bit(unsigned bit, bool has_side_pair) {
    switch (bit) {
        case 0:
            return th::Channel::kL;
        case 1:
            return th::Channel::kR;
        case 2:
            return th::Channel::kC;
        case 3:
            return th::Channel::kLfe;
        case 4:
            return has_side_pair ? th::Channel::kLrs : th::Channel::kLs;
        case 5:
            return has_side_pair ? th::Channel::kRrs : th::Channel::kRs;
        case 6:
            return th::Channel::kLc;
        case 7:
            return th::Channel::kRc;
        case 8:
            return th::Channel::kCs;
        case 9:
            return th::Channel::kLs;
        case 10:
            return th::Channel::kRs;
        case 11:
            return th::Channel::kTs;
        case 12:
            return th::Channel::kLvh;
        case 13:
            return th::Channel::kCvh;
        case 14:
            return th::Channel::kRvh;
        default:
            return std::nullopt;
    }
}

// How the WAV's channels go into the stream: its channel assignment (0 for the encoder's default
// for the count) and, for stream channel k, the WAV channel that carries it.
struct Layout {
    std::uint32_t assignment = 0;
    std::vector<std::size_t> wav_index;
};

// A file with no channel mask is taken in the order `decode` writes (the default layout of its
// count). One with a mask names its speakers, which are put into the order of the assignment they
// make; a mask that names speakers TrueHD has no channel for, or that is not one assignment, is an
// error.
std::optional<Layout> layout_of(const iclforge::ac3::io::WavData& wav) {
    const std::size_t count = wav.channels.size();
    Layout layout;
    layout.wav_index.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        layout.wav_index[i] = i;
    }
    if (wav.channel_mask == 0) {
        return layout;
    }
    if (static_cast<std::size_t>(std::popcount(wav.channel_mask)) != count) {
        fmt::println(
            stderr,
            "warning: the WAV's channel mask names {} speakers for {} channels; the channels "
            "are taken in the order decode writes them",
            std::popcount(wav.channel_mask), count);
        return layout;
    }
    const bool has_side_pair = (wav.channel_mask & 0x600U) != 0;
    std::vector<th::Channel> speakers;
    for (unsigned bit = 0; bit < 32; ++bit) {
        if ((wav.channel_mask & (1U << bit)) == 0) {
            continue;
        }
        const auto channel = channel_of_speaker_bit(bit, has_side_pair);
        if (!channel) {
            fmt::println(
                stderr,
                "error: the WAV's channel mask has a speaker (bit {}) that TrueHD has no channel "
                "for",
                bit);
            return std::nullopt;
        }
        speakers.push_back(*channel);
    }
    const auto has = [&](th::Channel channel) {
        return std::ranges::find(speakers, channel) != speakers.end();
    };
    std::size_t covered = 0;
    for (unsigned bit = 0; bit < th::kChannelAssignmentBits; ++bit) {
        const std::vector<th::Channel> named = th::channels_of_assignment(1U << bit);
        if (!named.empty() && std::ranges::all_of(named, has)) {
            layout.assignment |= 1U << bit;
            covered += named.size();
        }
    }
    if (covered != speakers.size()) {
        fmt::println(
            stderr,
            "error: the WAV's speakers are not a TrueHD channel assignment (L R, C, LFE, Ls "
            "Rs, Lvh Rvh, Lc Rc, Lrs Rrs, Cs, Ts, Lsd Rsd, Lw Rw, Cvh, LFE2 as pairs and "
            "singles)");
        return std::nullopt;
    }
    const std::vector<th::Channel> order = th::channels_of_assignment(layout.assignment);
    for (std::size_t k = 0; k < order.size() && k < count; ++k) {
        layout.wav_index[k] =
            static_cast<std::size_t>(std::ranges::find(speakers, order[k]) - speakers.begin());
    }
    return layout;
}

// The WAV reader's [-1, 1) floats back to the integers they were read from: a 16 or 24 bit PCM
// sample survives the trip exactly (a float32 holds 24 bits), and a wider one or a float is rounded
// to 24 bits. Stream channel k comes from WAV channel `wav_index[k]`.
void interleave(const iclforge::ac3::io::WavData& wav, std::span<const std::size_t> wav_index,
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
             "verify"});
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

    const auto layout = layout_of(*wav);
    if (!layout) {
        return kExitInput;
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
    config.verify = meta.verify;

    auto encoder = th::Encoder::create(config);
    if (!encoder) {
        report(encoder.error());
        return exit_code_for(encoder.error().kind);
    }

    std::vector<std::byte> stream;
    std::uint64_t access_units = 0;
    const auto take = [&](const std::vector<th::EncodedAccessUnit>& units) {
        for (const th::EncodedAccessUnit& unit : units) {
            stream.insert(stream.end(), unit.bytes.begin(), unit.bytes.end());
            ++access_units;
        }
    };
    std::vector<std::int32_t> pcm;
    const std::size_t frames = wav->frame_count();
    for (std::size_t at = 0; at < frames; at += kChunkFrames) {
        interleave(*wav, layout->wav_index, at, std::min(frames, at + kChunkFrames), pcm);
        const auto units = encoder->encode(pcm);
        if (!units) {
            report(units.error());
            return exit_code_for(units.error().kind);
        }
        take(*units);
    }
    const auto last = encoder->flush();
    if (!last) {
        report(last.error());
        return exit_code_for(last.error().kind);
    }
    take(*last);

    const th::EncoderStats& stats = encoder->stats();
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
                       encoder->substreams());
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
