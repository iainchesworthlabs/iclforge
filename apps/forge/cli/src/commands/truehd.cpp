#include "truehd.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "../exit_codes.hpp"
#include "decode.hpp"
#include "encode.hpp"
#include "iclforge/mlp/decoder/decoder.hpp"
#include "iclforge/mlp/encoder/encoder.hpp"
#include "iclforge/mlp/io/elementary.hpp"

namespace forge_cli {

namespace {

namespace mlp = iclforge::mlp;

// Integer PCM, interleaved, at 24-bit scale (a 16-bit sample shifted left by 8): what the TrueHD
// encoder takes and the decoder gives.
struct Pcm {
    int channels = 0;
    std::uint32_t sample_rate = 0;
    int bits = 0;
    std::uint32_t channel_mask = 0;
    std::vector<std::int32_t> samples;
};

std::optional<std::vector<std::byte>> read_bytes(std::string_view path) {
    std::ifstream in(std::string{path}, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::vector<char> chars((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> out(chars.size());
    for (std::size_t i = 0; i < chars.size(); ++i) {
        out[i] = static_cast<std::byte>(chars[i]);
    }
    return out;
}

bool write_bytes(std::string_view path, std::span<const std::byte> bytes) {
    std::ofstream out(std::string{path}, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

// A PCM WAV's integer samples: WAVE_FORMAT_PCM, or WAVE_FORMAT_EXTENSIBLE with the PCM subformat,
// at 16, 24 or 32 bits (32 only when its low byte is zero throughout, a 24-bit source in 32-bit
// words). A floating-point WAV is refused: a lossless code of it would need its rounding first,
// which is the caller's to choose.
std::optional<Pcm> read_pcm(std::string_view path, std::string& error) {
    const auto bytes = read_bytes(path);
    if (!bytes || bytes->size() < 12) {
        error = "cannot read it";
        return std::nullopt;
    }
    const auto& d = *bytes;
    auto u16 = [&](std::size_t at) {
        return static_cast<std::uint32_t>(std::to_integer<unsigned>(d[at]) |
                                          std::to_integer<unsigned>(d[at + 1]) << 8U);
    };
    auto u32 = [&](std::size_t at) { return u16(at) | u16(at + 2) << 16U; };
    if (d[0] != std::byte{'R'} || d[8] != std::byte{'W'}) {
        error = "not a RIFF WAVE file";
        return std::nullopt;
    }
    Pcm pcm;
    std::uint32_t format = 0;
    std::size_t at = 12;
    while (at + 8 <= d.size()) {
        const std::uint32_t size = u32(at + 4);
        const std::size_t body = at + 8;
        const bool is_fmt =
            d[at] == std::byte{'f'} && d[at + 1] == std::byte{'m'} && d[at + 2] == std::byte{'t'};
        const bool is_data = d[at] == std::byte{'d'} && d[at + 1] == std::byte{'a'} &&
                             d[at + 2] == std::byte{'t'} && d[at + 3] == std::byte{'a'};
        if (is_fmt && body + 16 <= d.size()) {
            format = u16(body);
            pcm.channels = static_cast<int>(u16(body + 2));
            pcm.sample_rate = u32(body + 4);
            pcm.bits = static_cast<int>(u16(body + 14));
            if (format == 0xFFFE && size >= 40 && body + 40 <= d.size()) {
                pcm.channel_mask = u32(body + 20);
                format = u16(body + 24);  // the subformat GUID's first two bytes
            }
        } else if (is_data) {
            if (format != 1) {
                error = "not integer PCM (TrueHD codes integer samples)";
                return std::nullopt;
            }
            if (pcm.channels < 1 || (pcm.bits != 16 && pcm.bits != 24 && pcm.bits != 32)) {
                error = "not 16, 24 or 32-bit PCM";
                return std::nullopt;
            }
            const auto width = static_cast<std::size_t>(pcm.bits / 8);
            const std::size_t available = std::min<std::size_t>(size, d.size() - body);
            const std::size_t count = available / width / static_cast<std::size_t>(pcm.channels) *
                                      static_cast<std::size_t>(pcm.channels);
            pcm.samples.resize(count);
            for (std::size_t i = 0; i < count; ++i) {
                const std::size_t p = body + i * width;
                std::uint32_t v = 0;
                for (std::size_t b = 0; b < width; ++b) {
                    v |= std::to_integer<std::uint32_t>(d[p + b]) << (8U * b);
                }
                const auto s = static_cast<std::int32_t>(v << (32U - 8U * width));
                if (width == 4 && (s & 0xFF) != 0) {
                    error = "32-bit PCM with more than 24 significant bits";
                    return std::nullopt;
                }
                pcm.samples[i] = s >> 8;  // 24-bit scale
            }
            return pcm;
        }
        at = body + size + (size & 1U);
    }
    error = "no data chunk";
    return std::nullopt;
}

// A PCM WAV of 24-bit-scale samples at `bits` (16 drops the zero low byte), with
// WAVE_FORMAT_EXTENSIBLE and a channel mask when there are more than two channels.
bool write_pcm(std::string_view path, const Pcm& pcm, int bits) {
    const auto width = static_cast<std::uint32_t>(bits / 8);
    const auto channels = static_cast<std::uint32_t>(pcm.channels);
    const bool extensible = pcm.channels > 2;
    const std::uint32_t data_size = static_cast<std::uint32_t>(pcm.samples.size()) * width;
    const std::uint32_t fmt_size = extensible ? 40U : 16U;
    std::vector<std::byte> out;
    auto put = [&](std::uint32_t v, int n) {
        for (int b = 0; b < n; ++b) {
            out.push_back(static_cast<std::byte>((v >> (8 * b)) & 0xFFU));
        }
    };
    auto tag = [&](const char* t) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<std::byte>(t[i]));
        }
    };
    tag("RIFF");
    put(4 + 8 + fmt_size + 8 + data_size, 4);
    tag("WAVE");
    tag("fmt ");
    put(fmt_size, 4);
    put(extensible ? 0xFFFEU : 1U, 2);
    put(channels, 2);
    put(pcm.sample_rate, 4);
    put(pcm.sample_rate * channels * width, 4);
    put(channels * width, 2);
    put(static_cast<std::uint32_t>(bits), 2);
    if (extensible) {
        put(22, 2);
        put(static_cast<std::uint32_t>(bits), 2);
        put(pcm.channel_mask, 4);
        // KSDATAFORMAT_SUBTYPE_PCM: 00000001-0000-0010-8000-00aa00389b71
        static constexpr std::array<unsigned, 16> kPcmGuid{0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                           0x10, 0x00, 0x80, 0x00, 0x00, 0xAA,
                                                           0x00, 0x38, 0x9B, 0x71};
        for (const unsigned b : kPcmGuid) {
            out.push_back(static_cast<std::byte>(b));
        }
    }
    tag("data");
    put(data_size, 4);
    for (const std::int32_t s : pcm.samples) {
        const auto v = static_cast<std::uint32_t>(s) >> (bits == 16 ? 8U : 0U);
        put(v, static_cast<int>(width));
    }
    return write_bytes(path, out);
}

// A WAV carries its channels in the order of its SPEAKER_* mask bits (mmreg.h), which puts a
// 7.1 file's back pair before its side pair and a 6.1 file's back centre before the side pair;
// section 13.1 carries the side pair (Ls, Rs) first. order[w] is the 13.1 channel a WAV's channel
// w holds. FFmpeg's decoder gives the WAV order (planning/truehd.md, P38).
std::vector<int> wav_order(int channels) {
    switch (channels) {
        case 7:
            return {0, 1, 2, 3, 6, 4, 5};
        case 8:
            return {0, 1, 2, 3, 6, 7, 4, 5};
        default: {
            std::vector<int> order(static_cast<std::size_t>(channels));
            for (int c = 0; c < channels; ++c) {
                order[static_cast<std::size_t>(c)] = c;
            }
            return order;
        }
    }
}

// Interleaved samples from one channel order to the other: to 13.1's (`to_stream`) or back.
void reorder(std::vector<std::int32_t>& samples, int channels, bool to_stream) {
    const auto order = wav_order(channels);
    const auto n = static_cast<std::size_t>(channels);
    std::vector<std::int32_t> frame(n);
    for (std::size_t at = 0; at + n <= samples.size(); at += n) {
        for (std::size_t w = 0; w < n; ++w) {
            const auto s = static_cast<std::size_t>(order[w]);
            if (to_stream) {
                frame[s] = samples[at + w];
            } else {
                frame[w] = samples[at + s];
            }
        }
        std::copy(frame.begin(), frame.end(), samples.begin() + static_cast<std::ptrdiff_t>(at));
    }
}

// The SPEAKER_* mask of the WAV order above: L R C LFE, the side pair for 5.1, and for 7.1 the
// back pair and the side pair.
std::uint32_t wav_mask(int channels) {
    switch (channels) {
        case 3:
            return 0x7;
        case 4:
            return 0x603;
        case 5:
            return 0x607;
        case 6:
            return 0x60F;
        case 7:
            return 0x70F;
        case 8:
            return 0x63F;
        default:
            return 0;
    }
}

std::filesystem::path temp_wav_path() {
    static std::atomic<std::uint64_t> counter{0};
    std::random_device rd;
    const auto unique =
        (static_cast<std::uint64_t>(rd()) << 32) ^
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
        counter.fetch_add(1);
    return std::filesystem::temp_directory_path() /
           ("forge_truehd_" + std::to_string(unique) + ".wav");
}

mlp::EncoderConfig encoder_config(const Pcm& pcm, const Options& meta) {
    mlp::EncoderConfig config;
    config.sample_rate = pcm.sample_rate;
    config.channels = pcm.channels;
    const auto& th = meta.truehd;
    if (th.restart_interval) {
        config.restart_interval = *th.restart_interval;
    }
    if (th.peak_kbps) {
        config.peak_data_rate = std::uint64_t{*th.peak_kbps} * 1000U;
    }
    if (th.max_fir_order) {
        config.max_fir_order = *th.max_fir_order;
    }
    if (th.max_iir_order) {
        config.max_iir_order = *th.max_iir_order;
    }
    if (th.decorrelate) {
        config.decorrelate = *th.decorrelate;
    }
    if (th.downmix) {
        config.downmix = *th.downmix;
    }
    if (th.verify) {
        config.verify = *th.verify;
    }
    return config;
}

}  // namespace

bool is_truehd_stream(std::span<const std::byte> bytes) {
    if (bytes.size() < 8) {
        return false;
    }
    const auto word = std::to_integer<std::uint32_t>(bytes[4]) << 24U |
                      std::to_integer<std::uint32_t>(bytes[5]) << 16U |
                      std::to_integer<std::uint32_t>(bytes[6]) << 8U |
                      std::to_integer<std::uint32_t>(bytes[7]);
    return word == mlp::kMajorSyncTrueHd || word == mlp::kMajorSyncMlp;
}

bool is_truehd_path(std::string_view path) {
    for (const std::string_view suffix : {".thd", ".mlp", ".truehd"}) {
        if (path.size() > suffix.size() && path.ends_with(suffix)) {
            return true;
        }
    }
    return false;
}

int run_truehd_encode(std::string_view in_wav, std::string_view out_path, const Options& meta) {
    std::string error;
    const auto pcm = read_pcm(in_wav, error);
    if (!pcm) {
        fmt::println(stderr, "error: {}: {}", in_wav, error);
        return kExitInput;
    }
    std::vector<std::int32_t> samples = pcm->samples;
    reorder(samples, pcm->channels, true);
    mlp::EncoderStats stats;
    auto stream = mlp::encode_stream(samples, encoder_config(*pcm, meta), &stats);
    if (!stream) {
        fmt::println(stderr, "error: TrueHD encode of {} failed: {}", in_wav,
                     mlp::describe(stream.error()));
        return stream.error() == mlp::Error::kInvalidConfiguration ||
                       stream.error() == mlp::Error::kBadSampleRate
                   ? kExitUsage
                   : kExitRuntime;
    }
    if (!write_bytes(out_path, *stream)) {
        fmt::println(stderr, "error: cannot write {}", out_path);
        return kExitOutput;
    }
    fmt::println(stderr,
                 "{}: TrueHD, {} channels at {} Hz, {} access units, {} bytes, mean {} kbit/s, "
                 "peak {} kbit/s; filters: {} FIR, {} FIR+IIR, {} none; {} decorrelation "
                 "matrices{}",
                 out_path, pcm->channels, pcm->sample_rate, stats.access_units, stats.bytes,
                 stats.mean_bits_per_second / 1000U, stats.peak_bits_per_second / 1000U,
                 stats.fir_blocks, stats.iir_blocks, stats.unfiltered_blocks,
                 stats.decorrelation_matrices,
                 stats.limited_downmixes > 0
                     ? fmt::format("; the downmix limiter acted in {} restart intervals",
                                   stats.limited_downmixes)
                     : std::string{});
    return kExitOk;
}

int run_truehd_decode(std::span<const std::byte> stream, std::string_view out_wav,
                      const Options& meta) {
    mlp::DecoderConfig config;
    if (meta.truehd.presentation_channels) {
        config.presentation_channels = *meta.truehd.presentation_channels;
    }
    auto decoded = mlp::decode_stream(stream, config);
    if (!decoded) {
        fmt::println(stderr, "error: TrueHD decode failed: {}", mlp::describe(decoded.error()));
        return kExitInput;
    }
    Pcm pcm;
    pcm.channels = decoded->channels;
    pcm.sample_rate = decoded->sample_rate;
    pcm.channel_mask = wav_mask(decoded->channels);
    pcm.samples = std::move(decoded->pcm);
    reorder(pcm.samples, pcm.channels, false);
    // A 16-bit WAV when every sample's low byte is zero (a 16-bit source), unless truehd-bits=
    // says otherwise; a 24-bit one when not.
    int bits = 16;
    for (const std::int32_t s : pcm.samples) {
        if ((s & 0xFF) != 0) {
            bits = 24;
            break;
        }
    }
    if (meta.truehd.bits) {
        if (*meta.truehd.bits == 16 && bits == 24) {
            fmt::println(stderr,
                         "error: truehd-bits=16, but the stream holds 24-bit samples; a 16-bit "
                         "WAV would not be lossless");
            return kExitUsage;
        }
        bits = *meta.truehd.bits;
    }
    if (!write_pcm(out_wav, pcm, bits)) {
        fmt::println(stderr, "error: cannot write {}", out_wav);
        return kExitOutput;
    }
    std::size_t payloads = 0;
    for (const auto& unit : decoded->evolution) {
        payloads += unit.size();
    }
    fmt::println(stderr, "{}: {} channels at {} Hz, {} access units, {}-bit WAV{}", out_wav,
                 pcm.channels, pcm.sample_rate, decoded->access_units, bits,
                 payloads > 0 ? fmt::format(", {} evolution payloads", payloads) : std::string{});
    return kExitOk;
}

int run_truehd_probe(std::span<const std::byte> stream, std::string_view in_path,
                     const Options& meta) {
    auto summary = mlp::probe(stream);
    if (!summary) {
        fmt::println(stderr, "error: {}: {}", in_path, mlp::describe(summary.error()));
        return kExitInput;
    }
    const auto& ms = summary->major_sync;
    const auto layout = mlp::speakers(ms.format.eightch_assignment);
    std::string speakers;
    for (int i = 0; i < layout.count; ++i) {
        speakers += (i > 0 ? " " : "") + std::string{layout.labels[static_cast<std::size_t>(i)]};
    }
    std::string presentations;
    for (const int p : summary->presentations) {
        presentations += (presentations.empty() ? "" : ", ") + std::to_string(p);
    }
    if (meta.json) {
        fmt::print(
            "{{\"codec\":\"truehd\",\"stream_type\":\"{}\",\"sample_rate\":{},"
            "\"substreams\":{},\"presentations\":[{}],\"channel_assignment\":{},"
            "\"speakers\":\"{}\",\"access_units\":{},\"major_syncs\":{},\"samples\":{},"
            "\"duration_seconds\":{:.6f},\"bytes\":{},\"mean_bits_per_second\":{},"
            "\"peak_data_rate_bits_per_second\":{},\"peak_access_unit_bytes\":{},"
            "\"extra_data\":{},\"signature\":{},\"framing_error\":{}}}\n",
            ms.stream_type == mlp::StreamType::kTrueHd ? "truehd" : "mlp", ms.sample_rate(),
            ms.num_substreams, presentations, ms.format.eightch_assignment, speakers,
            summary->access_units, summary->major_syncs, summary->samples,
            summary->duration_seconds, summary->bytes, summary->mean_bits_per_second,
            ms.peak_bits_per_second(), summary->peak_access_unit_bytes,
            summary->has_extra_data ? "true" : "false", ms.signature,
            summary->stopped_at ? fmt::format("\"{}\"", mlp::describe(*summary->stopped_at))
                                : std::string{"null"});
        return kExitOk;
    }
    fmt::println("{}: {} stream", in_path,
                 ms.stream_type == mlp::StreamType::kTrueHd ? "TrueHD" : "MLP (DVD-Audio)");
    fmt::println("  sample rate        {} Hz ({} samples an access unit)", ms.sample_rate(),
                 ms.samples_per_access_unit());
    fmt::println("  substreams         {}", ms.num_substreams);
    fmt::println("  presentations      {} channels", presentations);
    fmt::println("  channels           {} (assignment 0x{:03X})", speakers,
                 ms.format.eightch_assignment);
    fmt::println("  access units       {} ({} with a major sync)", summary->access_units,
                 summary->major_syncs);
    fmt::println("  duration           {:.3f} s", summary->duration_seconds);
    fmt::println(
        "  rate               mean {} kbit/s; peak_data_rate {} kbit/s; largest access "
        "unit {} bytes",
        summary->mean_bits_per_second / 1000U, ms.peak_bits_per_second() / 1000U,
        summary->peak_access_unit_bytes);
    fmt::println("  extra data         {}", summary->has_extra_data ? "present" : "none");
    if (summary->stopped_at) {
        fmt::println("  framing error      {}", mlp::describe(*summary->stopped_at));
    }
    return kExitOk;
}

int run_truehd_transcode(std::string_view in_path, std::string_view out_path, std::uint32_t bitrate,
                         std::string_view layout, const Options& meta) {
    const auto bytes = read_bytes(in_path);
    if (!bytes) {
        fmt::println(stderr, "error: cannot read {}", in_path);
        return kExitInput;
    }
    const bool from_truehd = is_truehd_stream(*bytes);
    const bool to_truehd = is_truehd_path(out_path);
    // Through a WAV: TrueHD's side reads or writes it as integer PCM, the other codec's command
    // as it always does.
    const auto wav = temp_wav_path();
    const std::string wav_path = wav.string();
    int status = kExitOk;
    if (from_truehd) {
        status = run_truehd_decode(*bytes, wav_path, meta);
    } else {
        status = commands::run_decode(in_path, wav_path, meta, {}, {});
    }
    if (status == kExitOk) {
        if (to_truehd) {
            status = run_truehd_encode(wav_path, out_path, meta);
        } else if (out_path.ends_with(".ac4")) {
            status =
                commands::run_ac4_encode(wav_path, out_path, bitrate == 0 ? 192 : bitrate, meta);
        } else if (out_path.ends_with(".ec3") || out_path.ends_with(".eac3")) {
            status = commands::run_eac3_encode(wav_path, out_path, bitrate == 0 ? 448 : bitrate,
                                               "none", layout, "off", meta);
        } else {
            status = commands::run_encode(wav_path, out_path, bitrate == 0 ? 448 : bitrate, false,
                                          layout, meta);
        }
    }
    std::error_code ignored;
    std::filesystem::remove(wav, ignored);
    return status;
}

}  // namespace forge_cli
