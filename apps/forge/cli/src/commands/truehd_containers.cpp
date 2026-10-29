#include <fmt/base.h>
#include <fmt/format.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../exit_codes.hpp"
#include "../support.hpp"
#include "iclforge/base/loudness.hpp"
#include "iclforge/base/speaker.hpp"
#include "iclforge/containers/iec61937/mat.hpp"
#include "iclforge/containers/matroska/matroska.hpp"
#include "iclforge/containers/mp4/mp4.hpp"
#include "iclforge/containers/mpegts/mpegts.hpp"
#include "iclforge/truehd/core/format.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/elementary.hpp"
#include "truehd.hpp"

// TrueHD and MLP in the container, transport and analysis commands (planning/truehd.md, phase
// T2c). The containers library knows no codec, so the facts a track description needs (the
// access units, which of them open with a major sync, the channel count, the end-of-stream padding)
// are read here, with iclforge::truehd, and handed to it as plain values.
//
// What each container writes has no layout in the TH spec (§21 names the transports and nothing
// more). libs/containers/ERRATA.md records each layout and its single oracle, FFmpeg's muxers.

namespace forge_cli::commands {

namespace {

namespace th = iclforge::truehd;
namespace mat = iclforge::containers::iec61937;
namespace mkv = iclforge::containers::matroska;
namespace mp4 = iclforge::containers::mp4;
namespace ts = iclforge::containers::mpegts;

// A stream scanned for what a container needs.
struct Scan {
    std::vector<std::span<const std::byte>> units;  // whole access units, views into the stream
    std::vector<bool> major;                        // per unit: it opens with a major sync
    th::StreamInfo info;
    int channels = 0;         // of the best presentation the stream has
    int padding_samples = 0;  // the end-of-stream marker's count (libs/truehd ERRATA 14)
    std::uint32_t samples_per_unit = 0;
};

std::string error_text(const th::Error& error) {
    return error.detail.empty() ? std::string{th::to_string(error.kind)}
                                : fmt::format("{}: {}", th::to_string(error.kind), error.detail);
}

const char* codec_label(const th::MajorSync& major_sync) {
    return major_sync.kind == th::StreamKind::kTrueHd ? "TrueHD" : "MLP";
}

// The units of `stream`, which decode lenient so that a container can still be written from a
// stream with a fault in it: `probe` and `decode` are where a fault is reported.
std::optional<Scan> scan_stream(std::span<const std::byte> stream, std::string_view in_path) {
    const auto info = th::probe_stream(stream);
    if (!info) {
        fmt::println(stderr, "error: {}: {}", in_path, error_text(info.error()));
        return std::nullopt;
    }
    Scan scan;
    scan.info = *info;
    scan.samples_per_unit = static_cast<std::uint32_t>(info->samples_per_access_unit);
    th::ElementaryReader reader(stream);
    th::Decoder decoder(th::DecoderConfig{.strictness = th::Strictness::kLenient});
    while (const auto unit = reader.next()) {
        scan.units.push_back(*unit);
        scan.major.push_back(th::starts_with_major_sync(unit->subspan(4)));
        const auto decoded = decoder.decode(*unit);
        if (decoded && *decoded) {
            scan.channels = (*decoded)->channels;
            scan.padding_samples += (*decoded)->padding_samples;
        }
    }
    if (reader.skipped_bytes() != 0) {
        fmt::println(
            stderr,
            "warning: {}: {} byte(s) between access units do not read as one and are left out",
            in_path, reader.skipped_bytes());
    }
    if (scan.units.empty() || scan.channels == 0) {
        fmt::println(stderr, "error: {}: no access unit decoded", in_path);
        return std::nullopt;
    }
    return scan;
}

bool write_file(std::string_view out_path, std::span<const std::byte> bytes) {
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

mp4::AudioTrack mp4_track(const Scan& scan) {
    return mp4::AudioTrack{
        .codec_id = std::string{mp4::kCodecTruehd},
        .sample_rate = scan.info.sample_rate,
        .channels = scan.channels,
        .samples_per_frame = scan.samples_per_unit,
        .codec_config =
            mp4::build_dmlp_payload({.format_info = scan.info.major_sync.format_info,
                                     .peak_data_rate = scan.info.major_sync.peak_data_rate}),
    };
}

// libs/containers/ERRATA.md open question 7: the level a carrier of MAT bursts runs at. A burst is
// 61,440 bytes of 16-bit words: eight channels is 3,840 frames, which is 24 access units of
// samples_per_access_unit samples at the content rate, so the carrier is 160 times the content
// rate over the unit length: 192 kHz for the 48 kHz family and 176.4 kHz for 44.1 kHz. Worked out
// from the burst size and the 24 units of TH spec §21.3, with eight channels assumed; no stream or
// receiver has confirmed it (the same open question).
std::uint32_t mat_carrier_rate(const Scan& scan) {
    const std::uint32_t family = scan.info.sample_rate % 44100U == 0 ? 44100U : 48000U;
    return family * 4U;
}

bool is_wav_path(std::string_view path) {
    const std::filesystem::path p{std::string{path}};
    const auto ext = p.extension().string();
    return ext == ".wav" || ext == ".WAV";
}

}  // namespace

int run_mkv_truehd(std::span<const std::byte> stream, std::string_view in_path,
                   std::string_view out_path) {
    const auto scan = scan_stream(stream, in_path);
    if (!scan) {
        return kExitInput;
    }
    mkv::MuxOptions options;
    options.key_frames = scan->major;
    const auto file = mkv::mux(mkv::AudioTrack{.codec_id = std::string{mkv::kCodecTruehd},
                                               .sample_rate = scan->info.sample_rate,
                                               .channels = scan->channels,
                                               .samples_per_frame = scan->samples_per_unit},
                               scan->units, options);
    if (!file) {
        fmt::println(stderr, "error: {}", mkv::describe(file.error()));
        return kExitInput;
    }
    if (!write_file(out_path, *file)) {
        return kExitOutput;
    }
    status_println(status_stream(), "wrote {} {} access units ({} Hz, {} channels, {} bytes) to {}",
                   scan->units.size(), codec_label(scan->info.major_sync), scan->info.sample_rate,
                   scan->channels, file->size(), out_path);
    return kExitOk;
}

int run_mp4_truehd(std::span<const std::byte> stream, std::string_view in_path,
                   std::string_view out_path) {
    const auto scan = scan_stream(stream, in_path);
    if (!scan) {
        return kExitInput;
    }
    // An edit list drops the samples the last access unit's end-of-stream marker counts as padding
    // (libs/truehd ERRATA 14), as FFmpeg's decoder does, so a decode of the MP4 is as long as the
    // source.
    const std::uint64_t total =
        static_cast<std::uint64_t>(scan->units.size()) * scan->samples_per_unit;
    mp4::MuxOptions options;
    options.sync_samples = scan->major;
    options.edit = mp4::MuxOptions::Edit{
        .start_samples = 0,
        .duration_samples = total - static_cast<std::uint64_t>(scan->padding_samples)};
    const auto file = mp4::mux(mp4_track(*scan), scan->units, options);
    if (!file) {
        fmt::println(stderr, "error: {}", mp4::describe(file.error()));
        return kExitInput;
    }
    if (!write_file(out_path, *file)) {
        return kExitOutput;
    }
    status_println(status_stream(),
                   "wrote {} {} access units ({} Hz, {} channels, {} bytes, 'mlpa'/'dmlp') to {}",
                   scan->units.size(), codec_label(scan->info.major_sync), scan->info.sample_rate,
                   scan->channels, file->size(), out_path);
    return kExitOk;
}

int run_ts_truehd(std::span<const std::byte> stream, std::string_view in_path,
                  std::string_view out_path, std::string_view profile_name) {
    // TH spec §21.2 gives TrueHD stream_type 0x83 under Blu-ray; DVB and ATSC define no TrueHD
    // carriage, so naming either with a TrueHD input is a contradiction worth stopping at.
    if (!profile_name.empty() && profile_name != "bluray") {
        fmt::println(stderr,
                     "error: TrueHD in a transport stream is Blu-ray's (stream_type 0x83); the {} "
                     "profile defines no TrueHD carriage - leave the profile out or use bluray",
                     profile_name);
        return kExitUsage;
    }
    const auto scan = scan_stream(stream, in_path);
    if (!scan) {
        return kExitInput;
    }
    const auto file =
        ts::mux(ts::AudioTrack{.codec = ts::AudioCodec::kTruehd,
                               .sample_rate = scan->info.sample_rate,
                               .channels = scan->channels,
                               .samples_per_frame = scan->samples_per_unit},
                scan->units, ts::MuxOptions{.profile = ts::BroadcastProfile::kBluray});
    if (!file) {
        fmt::println(stderr, "error: {}", ts::describe(file.error()));
        return kExitInput;
    }
    if (!write_file(out_path, *file)) {
        return kExitOutput;
    }
    status_println(
        status_stream(),
        "wrote {} {} access units ({} Hz, {} channels, {} bytes) to {} (Blu-ray profile, "
        "188-byte packets)",
        scan->units.size(), codec_label(scan->info.major_sync), scan->info.sample_rate,
        scan->channels, file->size(), out_path);
    return kExitOk;
}

int run_spdif_truehd(std::span<const std::byte> stream, std::string_view in_path,
                     std::string_view out_path) {
    const auto scan = scan_stream(stream, in_path);
    if (!scan) {
        return kExitInput;
    }
    const auto bursts = mat::wrap_mat_stream(scan->units);
    if (!bursts) {
        fmt::println(stderr, "error: {}: {}", in_path, mat::describe(bursts.error()));
        return kExitInput;
    }
    const std::uint32_t rate = mat_carrier_rate(*scan);
    if (is_wav_path(out_path)) {
        Pcm16RawWavSink sink;
        if (!sink.open(out_path, rate, 8) || !sink.push(*bursts) || !sink.close()) {
            sink.abort();
            return kExitOutput;
        }
    } else if (!write_file(out_path, *bursts)) {
        return kExitOutput;
    }
    const auto status = status_stream();
    status_println(status, "wrapped {} {} access units into {} MAT burst(s) -> {}",
                   scan->units.size(), codec_label(scan->info.major_sync),
                   bursts->size() / mat::kMatBurstBytes, out_path);
    if (is_wav_path(out_path)) {
        status_println(status,
                       "carrier: eight channels of 16-bit PCM at {} Hz (worked out from the burst "
                       "size; no receiver has been tried); 'unspdif' reads the units back",
                       rate);
    } else {
        status_println(status,
                       "raw MAT bursts, as FFmpeg's spdif muxer writes them; 'unspdif' reads "
                       "the units back");
    }
    return kExitOk;
}

bool is_mat_carrier(std::span<const std::byte> carrier) noexcept {
    // Pa 0xF872, Pb 0x4E1F, in either word order, then Pc: data type 22 (TH spec §21.1). A WAV
    // carrier opens with a RIFF header, which is skipped to the data chunk.
    std::size_t at = 0;
    if (carrier.size() >= 12 && std::to_integer<char>(carrier[0]) == 'R' &&
        std::to_integer<char>(carrier[1]) == 'I') {
        at = 12;
        while (at + 8 <= carrier.size()) {
            const std::uint32_t size = std::to_integer<std::uint32_t>(carrier[at + 4]) |
                                       (std::to_integer<std::uint32_t>(carrier[at + 5]) << 8U) |
                                       (std::to_integer<std::uint32_t>(carrier[at + 6]) << 16U) |
                                       (std::to_integer<std::uint32_t>(carrier[at + 7]) << 24U);
            if (std::to_integer<char>(carrier[at]) == 'd' &&
                std::to_integer<char>(carrier[at + 1]) == 'a') {
                at += 8;
                break;
            }
            at += 8U + size + (size & 1U);
        }
    }
    if (at + 8 > carrier.size()) {
        return false;
    }
    const auto b = [&](std::size_t i) { return std::to_integer<std::uint8_t>(carrier[at + i]); };
    const bool little = b(0) == 0x72 && b(1) == 0xF8 && b(2) == 0x1F && b(3) == 0x4E;
    const bool big = b(0) == 0xF8 && b(1) == 0x72 && b(2) == 0x4E && b(3) == 0x1F;
    if (!little && !big) {
        return false;
    }
    const unsigned pc = little ? (b(4) | (b(5) << 8U)) : ((b(4) << 8U) | b(5));
    return (pc & 0x1FU) == mat::kMatPc;
}

int run_unspdif_mat(std::span<const std::byte> carrier, std::string_view in_path,
                    std::string_view out_path, bool keep_partial) {
    // The whole carrier is held: a MAT burst is 61 kB and the unit table is small, but this path
    // reads the file once instead of through the streaming reader the other codecs use.
    std::size_t at = 0;
    if (carrier.size() >= 12 && std::to_integer<char>(carrier[0]) == 'R') {
        at = 12;
        while (at + 8 <= carrier.size()) {
            const std::uint32_t size = std::to_integer<std::uint32_t>(carrier[at + 4]) |
                                       (std::to_integer<std::uint32_t>(carrier[at + 5]) << 8U) |
                                       (std::to_integer<std::uint32_t>(carrier[at + 6]) << 16U) |
                                       (std::to_integer<std::uint32_t>(carrier[at + 7]) << 24U);
            if (std::to_integer<char>(carrier[at]) == 'd' &&
                std::to_integer<char>(carrier[at + 1]) == 'a') {
                at += 8;
                break;
            }
            at += 8U + size + (size & 1U);
        }
    }
    const auto units = mat::unwrap_mat_stream(carrier.subspan(std::min(at, carrier.size())));
    if (!units) {
        fmt::println(stderr, "error: {}: {}", in_path, mat::describe(units.error()));
        return kExitInput;
    }
    EncodedStreamSink sink;
    if (!sink.open(out_path, keep_partial)) {
        return kExitOutput;
    }
    std::uint64_t bytes = 0;
    for (const auto& unit : *units) {
        bytes += unit.size();
        if (!sink.push(std::span<const std::byte>{unit})) {
            sink.abort();
            return kExitOutput;
        }
    }
    if (!sink.close()) {
        return kExitOutput;
    }
    status_println(status_stream(out_path),
                   "unwrapped {} TrueHD access unit(s) from MAT -> {} ({} bytes)", units->size(),
                   out_path, bytes);
    return kExitOk;
}

namespace {

// libs/truehd's channel names, as the speakers the BS.1770 weights are keyed on. The surround pair
// Ls/Rs is the side surround of BS.2051 (weight 1.41); Lrs/Rrs are the backs. The mapping of the
// less common pairs (Lc/Rc to the screen pair, Lsd/Rsd to the sides, Cvh to the top front centre)
// is the nearest speaker and is this program's reading, not the specification's.
std::optional<iclforge::base::Speaker> speaker_of(th::Channel channel) {
    using S = iclforge::base::Speaker;
    switch (channel) {
        case th::Channel::kL:
            return S::kLeft;
        case th::Channel::kR:
            return S::kRight;
        case th::Channel::kC:
            return S::kCentre;
        case th::Channel::kLfe:
            return S::kLfe;
        case th::Channel::kLs:
            return S::kLeftSurround;
        case th::Channel::kRs:
            return S::kRightSurround;
        case th::Channel::kLvh:
            return S::kTopFrontLeft;
        case th::Channel::kRvh:
            return S::kTopFrontRight;
        case th::Channel::kLc:
            return S::kLeftScreen;
        case th::Channel::kRc:
            return S::kRightScreen;
        case th::Channel::kLrs:
            return S::kLeftBack;
        case th::Channel::kRrs:
            return S::kRightBack;
        case th::Channel::kCs:
            return S::kCentreBack;
        case th::Channel::kTs:
            return S::kTopCentre;
        case th::Channel::kLsd:
            return S::kLeftSurround;
        case th::Channel::kRsd:
            return S::kRightSurround;
        case th::Channel::kLw:
            return S::kLeftWide;
        case th::Channel::kRw:
            return S::kRightWide;
        case th::Channel::kCvh:
            return S::kTopFrontCentre;
        case th::Channel::kLfe2:
            return S::kLfe2;
    }
    return std::nullopt;
}

constexpr double kFullScale = 8388608.0;  // 2^23: the decoder's samples are 24-bit

// Decodes `stream` and hands each access unit to `visit` as planar floats in -1..1. Returns false
// after saying why when the stream does not decode.
template <typename Visit>
bool for_each_unit(std::span<const std::byte> stream, std::string_view in_path, const Options& meta,
                   Visit&& visit) {
    th::DecoderConfig config;
    config.strictness = meta.truehd_lenient ? th::Strictness::kLenient : th::Strictness::kStrict;
    if (meta.truehd_presentation == "2ch") {
        config.presentation = th::Presentation::k2ch;
    } else if (meta.truehd_presentation == "6ch") {
        config.presentation = th::Presentation::k6ch;
    } else if (meta.truehd_presentation == "8ch") {
        config.presentation = th::Presentation::k8ch;
    } else if (meta.truehd_presentation == "all") {
        config.presentation = th::Presentation::kAll;
    } else if (!meta.truehd_presentation.empty() && meta.truehd_presentation != "best") {
        fmt::println(stderr, "error: truehd-presentation is best, 2ch, 6ch, 8ch or all (got '{}')",
                     meta.truehd_presentation);
        return false;
    }
    th::Decoder decoder(config);
    th::ElementaryReader reader(stream);
    std::vector<std::vector<float>> planes;
    std::size_t index = 0;
    while (const auto unit = reader.next()) {
        const auto result = decoder.decode(*unit);
        if (!result) {
            fmt::println(stderr, "error: {}: access unit {}: {}", in_path, index,
                         error_text(result.error()));
            if (!meta.truehd_lenient) {
                return false;
            }
        } else if (*result) {
            const th::DecodedAccessUnit& decoded = **result;
            const auto channels = static_cast<std::size_t>(decoded.channels);
            const auto samples = static_cast<std::size_t>(decoded.samples);
            planes.resize(channels);
            for (std::size_t c = 0; c < channels; ++c) {
                planes[c].resize(samples);
                for (std::size_t s = 0; s < samples; ++s) {
                    planes[c][s] = static_cast<float>(
                        static_cast<double>(decoded.pcm[(s * channels) + c]) / kFullScale);
                }
            }
            visit(decoded, planes);
        }
        ++index;
    }
    return index != 0;
}

double to_db(double linear) {
    return linear > 0.0 ? 20.0 * std::log10(linear) : -std::numeric_limits<double>::infinity();
}

}  // namespace

int run_levels_truehd(std::span<const std::byte> stream, std::string_view in_path,
                      const Options& meta) {
    struct Channel {
        std::string name;
        double peak = 0.0;
        double sum_squares = 0.0;
    };
    std::vector<Channel> channels;
    std::uint64_t samples = 0;
    std::uint32_t rate = 0;
    const bool ok = for_each_unit(
        stream, in_path, meta,
        [&](const th::DecodedAccessUnit& decoded, const std::vector<std::vector<float>>& planes) {
            if (channels.empty()) {
                channels.resize(planes.size());
                rate = decoded.sample_rate;
                for (std::size_t c = 0; c < planes.size(); ++c) {
                    channels[c].name = c < decoded.layout.size()
                                           ? std::string{th::channel_name(decoded.layout[c])}
                                           : fmt::format("ch{}", c + 1);
                }
            }
            for (std::size_t c = 0; c < planes.size() && c < channels.size(); ++c) {
                for (const float value : planes[c]) {
                    const double v = static_cast<double>(value);
                    channels[c].peak = std::max(channels[c].peak, std::abs(v));
                    channels[c].sum_squares += v * v;
                }
            }
            samples += static_cast<std::uint64_t>(decoded.samples);
        });
    if (!ok || channels.empty()) {
        return kExitInput;
    }
    fmt::println("{}: TrueHD/MLP, {} channel(s), {} Hz, {:.2f} s", in_path, channels.size(), rate,
                 static_cast<double>(samples) / static_cast<double>(rate));
    fmt::println("  {:<6} {:>10} {:>10}", "ch", "peak dBFS", "rms dBFS");
    for (const Channel& channel : channels) {
        const double rms =
            samples == 0 ? 0.0 : std::sqrt(channel.sum_squares / static_cast<double>(samples));
        fmt::println("  {:<6} {:>10.2f} {:>10.2f}", channel.name, to_db(channel.peak), to_db(rms));
    }
    return kExitOk;
}

int run_loudness_truehd(std::span<const std::byte> stream, std::string_view in_path,
                        const Options& meta) {
    std::optional<iclforge::base::LoudnessMeter> meter;
    std::vector<std::span<const float>> views;
    std::uint32_t rate = 0;
    std::string layout_text;
    bool unnamed = false;
    const bool ok = for_each_unit(
        stream, in_path, meta,
        [&](const th::DecodedAccessUnit& decoded, const std::vector<std::vector<float>>& planes) {
            if (!meter && !unnamed) {
                std::vector<iclforge::base::Speaker> speakers;
                for (const th::Channel channel : decoded.layout) {
                    const auto speaker = speaker_of(channel);
                    if (speaker) {
                        speakers.push_back(*speaker);
                    }
                    layout_text += layout_text.empty() ? "" : " ";
                    layout_text += th::channel_name(channel);
                }
                if (speakers.size() != planes.size() || decoded.layout.empty()) {
                    unnamed = true;
                    return;
                }
                rate = decoded.sample_rate;
                meter.emplace(rate, std::span<const iclforge::base::Speaker>{speakers});
            }
            if (meter) {
                views.clear();
                for (const auto& plane : planes) {
                    views.emplace_back(plane);
                }
                meter->push(views);
            }
        });
    if (unnamed) {
        fmt::println(stderr,
                     "error: {}: the stream does not name its channels' speakers for this channel "
                     "count, so no BS.1770 weights can be chosen",
                     in_path);
        return kExitInput;
    }
    if (!ok || !meter) {
        return kExitInput;
    }
    fmt::println("{}: TrueHD/MLP, {} ({} Hz)", in_path, layout_text, rate);
    const auto lkfs = meter->integrated_lkfs();
    if (!lkfs) {
        fmt::println("no audio above the -70 LKFS absolute gate: loudness undefined");
        return kExitRuntime;
    }
    fmt::println("  integrated loudness {:.1f} LKFS (BS.1770-5 Annex 3 weights)", *lkfs);
    if (const auto peak = meter->true_peak_dbtp()) {
        fmt::println("  true peak {:.1f} dBTP", *peak);
    }
    return kExitOk;
}

int refuse_truehd(std::string_view command, std::string_view in_path) {
    fmt::println(stderr,
                 "error: {} is TrueHD or MLP, which '{}' does not read. decode, probe, levels, "
                 "loudness, mkv, mp4, ts, spdif, unspdif and demux do",
                 in_path, command);
    return kExitUsage;
}

}  // namespace forge_cli::commands
