#include "truehd_wrap.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <utility>

#include "iclforge/containers/matroska/matroska.hpp"
#include "iclforge/containers/mp4/mp4.hpp"
#include "iclforge/containers/mpegts/mpegts.hpp"
#include "iclforge/truehd/core/error.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/elementary.hpp"
#include "iclforge/truehd/io/major_sync.hpp"

namespace iclforge::apps::truehd_wrap {

namespace th = iclforge::truehd;
namespace mkv = iclforge::containers::matroska;
namespace mp4 = iclforge::containers::mp4;
namespace ts = iclforge::containers::mpegts;

namespace {

std::string error_text(const th::Error& error) {
    std::string text{th::to_string(error.kind)};
    if (!error.detail.empty()) {
        text += ": ";
        text += error.detail;
    }
    return text;
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

}  // namespace

const char* codec_label(const th::MajorSync& major_sync) {
    return major_sync.kind == th::StreamKind::kTrueHd ? "TrueHD" : "MLP";
}

std::expected<Scan, std::string> scan_stream(std::span<const std::byte> stream,
                                             std::string_view name) {
    const auto info = th::probe_stream(stream);
    if (!info) {
        return std::unexpected(std::string{name} + ": " + error_text(info.error()));
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
    if (reader.leading_bytes() != 0) {
        scan.warnings.push_back(std::string{name} + ": " + std::to_string(reader.leading_bytes()) +
                                " byte(s) ahead of the first access unit are not part of the "
                                "stream and are left out");
    }
    if (reader.skipped_bytes() != reader.leading_bytes()) {
        scan.warnings.push_back(std::string{name} + ": " +
                                std::to_string(reader.skipped_bytes() - reader.leading_bytes()) +
                                " byte(s) between access units do not read as one and are left out");
    }
    if (scan.units.empty() || scan.channels == 0) {
        return std::unexpected(std::string{name} + ": no access unit decoded");
    }
    return scan;
}

std::expected<std::vector<std::byte>, std::string> mux(Container container, const Scan& scan) {
    switch (container) {
        case Container::kMatroska: {
            mkv::MuxOptions options;
            options.key_frames = scan.major;
            auto file = mkv::mux(mkv::AudioTrack{.codec_id = std::string{mkv::kCodecTruehd},
                                                 .sample_rate = scan.info.sample_rate,
                                                 .channels = scan.channels,
                                                 .samples_per_frame = scan.samples_per_unit},
                                 scan.units, options);
            if (!file) {
                return std::unexpected(std::string{mkv::describe(file.error())});
            }
            return std::move(*file);
        }
        case Container::kMp4: {
            // An edit list drops the samples the last access unit's end-of-stream marker counts as
            // padding (libs/truehd ERRATA 14), as FFmpeg's decoder does, so a decode of the MP4 is as
            // long as the source.
            const std::uint64_t total =
                static_cast<std::uint64_t>(scan.units.size()) * scan.samples_per_unit;
            mp4::MuxOptions options;
            options.sync_samples = scan.major;
            options.edit = mp4::MuxOptions::Edit{
                .start_samples = 0,
                .duration_samples = total - static_cast<std::uint64_t>(scan.padding_samples)};
            auto file = mp4::mux(mp4_track(scan), scan.units, options);
            if (!file) {
                return std::unexpected(std::string{mp4::describe(file.error())});
            }
            return std::move(*file);
        }
        case Container::kTransport: {
            // TH spec §21.2 gives TrueHD stream_type 0x83 under Blu-ray.
            auto file = ts::mux(ts::AudioTrack{.codec = ts::AudioCodec::kTruehd,
                                               .sample_rate = scan.info.sample_rate,
                                               .channels = scan.channels,
                                               .samples_per_frame = scan.samples_per_unit},
                                scan.units, ts::MuxOptions{.profile = ts::BroadcastProfile::kBluray});
            if (!file) {
                return std::unexpected(std::string{ts::describe(file.error())});
            }
            return std::move(*file);
        }
    }
    return std::unexpected(std::string{"unknown container"});
}

std::optional<Container> container_of(std::string_view path) {
    std::string ext = std::filesystem::path{std::string{path}}.extension().string();
    std::ranges::transform(ext, ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".mkv" || ext == ".mka" || ext == ".webm") {
        return Container::kMatroska;
    }
    if (ext == ".mp4" || ext == ".m4a" || ext == ".mov") {
        return Container::kMp4;
    }
    if (ext == ".ts" || ext == ".m2ts") {
        return Container::kTransport;
    }
    return std::nullopt;
}

}  // namespace iclforge::apps::truehd_wrap
