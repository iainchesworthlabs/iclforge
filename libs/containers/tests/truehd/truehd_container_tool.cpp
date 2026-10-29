// A small command-line tool over the TrueHD container code, for
// tools/checks/verify_truehd_containers.py to drive against FFmpeg without the forge CLI
// (planning/truehd.md, phase T2c: the CLI is a later phase).
//
//   iclforge-truehd-container-tool mux   <mp4|mp4frag|mkv|ts|mat> <in.thd> <out>
//   iclforge-truehd-container-tool demux <mp4|mkv|ts|mat>          <in>     <out.thd>
//   iclforge-truehd-container-tool info  <mp4|mkv|ts|mat>          <in>
//
// `demux` writes the access units back to back, which is a raw .thd stream. `info` prints one
// `key=value` pair a line, for the script to read.

#include <fmt/format.h>

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "iclforge/containers/iec61937/mat.hpp"
#include "iclforge/containers/matroska/matroska.hpp"
#include "iclforge/containers/matroska/reader.hpp"
#include "iclforge/containers/mp4/mp4.hpp"
#include "iclforge/containers/mp4/reader.hpp"
#include "iclforge/containers/mpegts/mpegts.hpp"
#include "iclforge/containers/mpegts/reader.hpp"
#include "truehd/truehd_stream.hpp"

namespace {

namespace mat = iclforge::containers::iec61937;
namespace mkv = iclforge::containers::matroska;
namespace mp4 = iclforge::containers::mp4;
namespace ts = iclforge::containers::mpegts;
using iclforge::containers::testing::TruehdStream;

constexpr int kExitUsage = 2;
constexpr int kExitFailure = 1;

int fail(std::string_view what) {
    fmt::println(stderr, "error: {}", what);
    return kExitFailure;
}

[[nodiscard]] std::vector<std::byte> concatenate(
    std::span<const std::span<const std::byte>> parts) {
    std::vector<std::byte> out;
    for (const auto& part : parts) {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

mp4::AudioTrack mp4_track(const TruehdStream& stream) {
    return mp4::AudioTrack{
        .codec_id = std::string{mp4::kCodecTruehd},
        .sample_rate = stream.info.sample_rate,
        .channels = stream.channels,
        .samples_per_frame = stream.samples_per_unit,
        .codec_config =
            mp4::build_dmlp_payload({.format_info = stream.info.major_sync.format_info,
                                     .peak_data_rate = stream.info.major_sync.peak_data_rate}),
    };
}

int mux(std::string_view format, const std::string& in, const std::string& out) {
    const auto stream = iclforge::containers::testing::load_truehd(in);
    if (!stream) {
        return fail("not a TrueHD stream: " + in);
    }
    std::vector<std::byte> bytes;
    if (format == "mp4" || format == "mp4frag") {
        // An edit list drops the padding samples the last access unit's end-of-stream marker
        // counts.
        const std::uint64_t total =
            static_cast<std::uint64_t>(stream->units.size()) * stream->samples_per_unit;
        if (format == "mp4") {
            mp4::MuxOptions options;
            options.sync_samples = stream->major;
            options.edit = mp4::MuxOptions::Edit{
                .start_samples = 0,
                .duration_samples = total - static_cast<std::uint64_t>(stream->padding_samples)};
            const auto muxed = mp4::mux(mp4_track(*stream), stream->units, options);
            if (!muxed) {
                return fail(mp4::describe(muxed.error()));
            }
            bytes = *muxed;
        } else {
            mp4::FragmentOptions options;
            options.frames_per_fragment = 48;
            options.sync_samples = stream->major;
            const auto fragmented = mp4::fragment(mp4_track(*stream), stream->units, options);
            if (!fragmented) {
                return fail(mp4::describe(fragmented.error()));
            }
            bytes = fragmented->init_segment;
            for (const auto& segment : fragmented->media_segments) {
                bytes.insert(bytes.end(), segment.bytes.begin(), segment.bytes.end());
            }
        }
    } else if (format == "mkv") {
        mkv::MuxOptions options;
        options.key_frames = stream->major;
        const auto muxed = mkv::mux(mkv::AudioTrack{.codec_id = std::string{mkv::kCodecTruehd},
                                                    .sample_rate = stream->info.sample_rate,
                                                    .channels = stream->channels,
                                                    .samples_per_frame = stream->samples_per_unit},
                                    stream->units, options);
        if (!muxed) {
            return fail(mkv::describe(muxed.error()));
        }
        bytes = *muxed;
    } else if (format == "ts") {
        ts::MuxOptions options;
        options.profile = ts::BroadcastProfile::kBluray;
        const auto muxed = ts::mux(ts::AudioTrack{.codec = ts::AudioCodec::kTruehd,
                                                  .sample_rate = stream->info.sample_rate,
                                                  .channels = stream->channels,
                                                  .samples_per_frame = stream->samples_per_unit},
                                   stream->units, options);
        if (!muxed) {
            return fail(ts::describe(muxed.error()));
        }
        bytes = *muxed;
    } else if (format == "mat") {
        const auto wrapped = mat::wrap_mat_stream(stream->units);
        if (!wrapped) {
            return fail(mat::describe(wrapped.error()));
        }
        bytes = *wrapped;
    } else {
        return kExitUsage;
    }
    return iclforge::containers::testing::write_file(out, bytes) ? 0 : fail("cannot write " + out);
}

// The elementary stream a container holds, and the facts about its track worth printing.
struct Demuxed {
    std::vector<std::byte> stream;
    std::vector<std::pair<std::string, std::string>> facts;
};

std::expected<Demuxed, std::string> demux_file(std::string_view format,
                                               const std::vector<std::byte>& file) {
    Demuxed out;
    const auto add = [&](std::string key, auto value) {
        out.facts.emplace_back(std::move(key), fmt::to_string(value));
    };
    if (format == "mp4") {
        const auto demuxed = mp4::demux(file);
        if (!demuxed) {
            return std::unexpected(std::string{mp4::describe(demuxed.error())});
        }
        out.stream = concatenate(demuxed->samples);
        const mp4::ReadTrack& track = demuxed->track;
        add("codec", track.codec_id);
        add("channels", track.channels);
        add("sample_rate", track.sample_rate);
        add("timescale", track.timescale);
        add("samples", demuxed->samples.size());
        add("format_info", fmt::format("0x{:08x}", track.codec_config.mlp_format_info));
        add("peak_data_rate", track.codec_config.mlp_peak_data_rate);
        for (const mp4::EditListEntry& edit : track.edits) {
            add("edit_duration", edit.segment_duration);
            add("edit_media_time", edit.media_time);
        }
    } else if (format == "mkv") {
        const auto demuxed = mkv::demux(file);
        if (!demuxed) {
            return std::unexpected(std::string{mkv::describe(demuxed.error())});
        }
        out.stream = concatenate(demuxed->frames);
        add("codec", demuxed->track.codec_id);
        add("channels", demuxed->track.channels);
        add("sample_rate", demuxed->track.sample_rate);
        add("frames", demuxed->frames.size());
    } else if (format == "ts") {
        const auto demuxed = ts::demux(file);
        if (!demuxed) {
            return std::unexpected(std::string{ts::describe(demuxed.error())});
        }
        out.stream = concatenate(demuxed->payloads);
        add("stream_type", fmt::format("0x{:02x}", demuxed->stream.stream_type));
        add("truehd", demuxed->stream.truehd);
        add("signalling", static_cast<int>(demuxed->stream.signalling));
        add("packet_size", demuxed->stream.packet_size);
        add("payloads", demuxed->payloads.size());
    } else if (format == "mat") {
        const auto units = mat::unwrap_mat_stream(file);
        if (!units) {
            return std::unexpected(std::string{mat::describe(units.error())});
        }
        for (const auto& unit : *units) {
            out.stream.insert(out.stream.end(), unit.begin(), unit.end());
        }
        add("access_units", units->size());
    } else {
        return std::unexpected(std::string{"unknown format"});
    }
    return out;
}

int demux(std::string_view format, const std::string& in, const std::string& out) {
    const auto file = iclforge::containers::testing::read_file(in);
    const auto demuxed = demux_file(format, file);
    if (!demuxed) {
        return fail(demuxed.error());
    }
    return iclforge::containers::testing::write_file(out, demuxed->stream)
               ? 0
               : fail("cannot write " + out);
}

int info(std::string_view format, const std::string& in) {
    const auto file = iclforge::containers::testing::read_file(in);
    const auto demuxed = demux_file(format, file);
    if (!demuxed) {
        return fail(demuxed.error());
    }
    for (const auto& [key, value] : demuxed->facts) {
        fmt::println("{}={}", key, value);
    }
    fmt::println("elementary_bytes={}", demuxed->stream.size());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.size() == 4 && args[0] == "mux") {
        return mux(args[1], args[2], args[3]);
    }
    if (args.size() == 4 && args[0] == "demux") {
        return demux(args[1], args[2], args[3]);
    }
    if (args.size() == 3 && args[0] == "info") {
        return info(args[1], args[2]);
    }
    fmt::println(
        stderr,
        "usage: iclforge-truehd-container-tool mux <mp4|mp4frag|mkv|ts|mat> <in.thd> <out>");
    fmt::println(stderr,
                 "       iclforge-truehd-container-tool demux <mp4|mkv|ts|mat> <in> <out.thd>");
    fmt::println(stderr, "       iclforge-truehd-container-tool info <mp4|mkv|ts|mat> <in>");
    return kExitUsage;
}
