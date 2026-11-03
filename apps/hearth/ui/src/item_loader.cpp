#include "item_loader.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "item_source.hpp"
#include "container_input.hpp"
#include "container_stream.hpp"

namespace iclforge::hearth::ui {

namespace {

[[nodiscard]] std::string lowercase_extension(const std::string& path) {
    std::string extension = std::filesystem::path(path).extension().string();
    for (char& c : extension) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return extension;
}

// The files Hearth reads, which is also what list_folder_items() lists:
// the elementary streams (AC-3 and E-AC-3, which io::scan() splits, AC-4,
// whose sync frames Session::open() splits - planning/ac4.md, I2 - and TrueHD and
// MLP, which Session::open() indexes) and the three containers
// apps/shared/media/src/container_input.hpp demuxes.
constexpr std::array<std::string_view, 8> kMediaExtensions{".ac3", ".ec3", ".ac4", ".thd",
                                                           ".mlp", ".mp4", ".mkv", ".ts"};

[[nodiscard]] bool is_media_extension(const std::string& extension) {
    return std::find(kMediaExtensions.begin(), kMediaExtensions.end(), extension) !=
           kMediaExtensions.end();
}

// The codecs a file's track may be in for Hearth to play it: all four. A file with TrueHD first
// and AC-3 or E-AC-3 after it (a Blu-ray remux) plays the TrueHD, decoded here; Hearth cannot send
// it on as it is yet, so a receiver that would have been sent the AC-3 track is not sent it.
constexpr iclforge::apps::CodecSet kPlayedCodecs{
    .ac3 = true, .eac3 = true, .ac4 = true, .truehd = true};

// How much of a file the loader looks at to tell a bare TrueHD stream from the rest, before it
// decides whether to read the file at all.
constexpr std::size_t kSniffBytes = 64 * 1024;

// A TrueHD or MLP file read as the player plays it (iclforge::hearth::ByteSource): by offset, a
// window at a time, so that a film's lossless track is not held in memory.
class FileByteSource final : public iclforge::hearth::ByteSource {
public:
    FileByteSource(const std::string& path, std::uint64_t size)
        : file_(path, std::ios::binary), size_(size) {}

    [[nodiscard]] bool ok() const { return static_cast<bool>(file_); }

    [[nodiscard]] std::uint64_t size() const override { return size_; }

    [[nodiscard]] std::size_t read(std::uint64_t offset, std::span<std::byte> out) override {
        if (offset >= size_ || out.empty()) {
            return 0;
        }
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(offset));
        file_.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
        return static_cast<std::size_t>(file_.gcount());
    }

private:
    std::ifstream file_;
    std::uint64_t size_;
};

}  // namespace

namespace {

// The codecs of a file's tracks that Hearth can send to an output as they are.
constexpr iclforge::apps::CodecSet kCompressedCodecs{
    .ac3 = true, .eac3 = true, .ac4 = true, .truehd = false};

// A transport stream is read whole: its reader rebuilds a PES packet from the payloads of many
// transport packets, so a frame is not one run of the file (container_stream.hpp). This is the
// most of one the loader holds; a longer one is refused, by name, instead of exhausting memory.
constexpr std::uint64_t kTransportStreamLimit = std::uint64_t{1} << 30;

// Whether a container says it holds an AC-3, E-AC-3 or AC-4 audio track besides the one read.
[[nodiscard]] bool lists_compressed_track(const iclforge::apps::ContainerFacts& container) {
    return std::ranges::any_of(container.other_tracks, [](const iclforge::apps::OtherTrack& other) {
        return other.kind == "audio" &&
               (other.codec_id == "A_AC3" || other.codec_id == "A_EAC3" || other.codec_id == "ac-3" ||
                other.codec_id == "ec-3" || other.codec_id == "ac-4");
    });
}

// The item for a track found in a container file. A TrueHD track is read where it lies, a window
// at a time (iclforge::apps::ExtentSource); any other is small, and is read whole.
[[nodiscard]] std::expected<LoadedItem, std::string> track_item(
    const std::shared_ptr<FileByteSource>& file, iclforge::apps::StreamedTrack track,
    bool offer_compressed) {
    if (!track.error.empty()) {
        return std::unexpected(std::move(track.error));
    }
    LoadedItem item{.skip_samples = track.trim.start,
                    .play_samples = track.trim.length,
                    .note = std::move(track.trim_note),
                    .container = std::move(track.container)};
    if (track.codec == iclforge::apps::TrackCodec::kTruehd) {
        item.truehd = true;
        if (offer_compressed && lists_compressed_track(item.container)) {
            item.compressed_track = [file]() {
                return track_item(
                    file, iclforge::apps::index_container_track(*file, kCompressedCodecs), false);
            };
        }
        item.source = std::make_shared<iclforge::apps::ExtentSource>(file, std::move(track.extents));
    } else {
        item.bytes = iclforge::apps::read_track_bytes(*file, track);
    }
    return item;
}

// A transport stream's item, from the whole file as the batch demuxer reads it.
[[nodiscard]] std::expected<LoadedItem, std::string> transport_stream_item(
    std::vector<std::byte> bytes) {
    auto stream = iclforge::apps::elementary_stream_from_bytes(bytes, kPlayedCodecs);
    if (!stream.error.empty()) {
        return std::unexpected(std::move(stream.error));
    }
    LoadedItem item{.bytes = std::move(stream.bytes),
                    .truehd = stream.codec == iclforge::apps::TrackCodec::kTruehd,
                    .skip_samples = stream.trim.start,
                    .play_samples = stream.trim.length,
                    .note = std::move(stream.trim_note),
                    .container = std::move(stream.container)};
    if (item.truehd) {
        // The file is in memory already; the compressed track comes out of the same bytes.
        auto whole = std::make_shared<const std::vector<std::byte>>(std::move(bytes));
        item.compressed_track = [whole]() -> std::expected<LoadedItem, std::string> {
            auto other = iclforge::apps::elementary_stream_from_bytes(*whole, kCompressedCodecs);
            if (!other.error.empty()) {
                return std::unexpected(std::move(other.error));
            }
            return LoadedItem{.bytes = std::move(other.bytes),
                              .skip_samples = other.trim.start,
                              .play_samples = other.trim.length,
                              .note = std::move(other.trim_note),
                              .container = std::move(other.container)};
        };
    }
    return item;
}

}  // namespace

iclforge::hearth::ItemLoader make_file_item_loader() {
    return [](const std::string& path) -> std::expected<LoadedItem, std::string> {
        if (!is_media_extension(lowercase_extension(path))) {
            return std::unexpected(
                "not a file hearth reads: it opens .ac3, .ec3, .ac4, .thd, .mlp, .mp4, .mkv and .ts files");
        }
        std::error_code sized;
        const std::uintmax_t size = std::filesystem::file_size(path, sized);
        if (sized) {
            return std::unexpected(sized.message());
        }
        auto file = std::make_shared<FileByteSource>(path, size);
        if (!file->ok()) {
            return std::unexpected("could not open the file");
        }
        // The file is told by its first bytes, not its name (a rip is as likely to be called
        // "title00.mkv" when it is not one, and an MP4 may be named ".ac3").
        std::vector<std::byte> head(
            static_cast<std::size_t>(std::min<std::uintmax_t>(size, kSniffBytes)));
        if (file->read(0, head) != head.size()) {
            return std::unexpected("could not read the file");
        }
        const iclforge::apps::ContainerKind kind = iclforge::apps::sniff_container(head);
        if (kind == iclforge::apps::ContainerKind::kUnknown) {
            // A bare TrueHD or MLP stream is not read into memory: the session indexes it and
            // reads it as it plays.
            if (iclforge::apps::looks_like_truehd(head)) {
                return LoadedItem{.source = std::move(file), .truehd = true};
            }
            std::vector<std::byte> bytes(static_cast<std::size_t>(size));
            if (file->read(0, bytes) != bytes.size()) {
                return std::unexpected("could not read the file");
            }
            return LoadedItem{.bytes = std::move(bytes)};
        }
        if (kind == iclforge::apps::ContainerKind::kMpegTs) {
            if (size > kTransportStreamLimit) {
                return std::unexpected(
                    "a Transport Stream over 1 GiB is not read yet: Hearth holds one in memory");
            }
            std::vector<std::byte> bytes(static_cast<std::size_t>(size));
            if (file->read(0, bytes) != bytes.size()) {
                return std::unexpected("could not read the file");
            }
            return transport_stream_item(std::move(bytes));
        }
        // Matroska and MP4: the track is found where it lies and read from there, so a film's
        // lossless track is never held beside the file. The edit list's priming and padding are
        // decoded but not played (Session applies skip_samples and play_samples); what the
        // container said about the track reaches the media information.
        return track_item(file, iclforge::apps::index_container_track(*file, kPlayedCodecs), true);
    };
}

std::vector<std::string> list_folder_items(const std::string& folder) {
    std::vector<std::string> paths;
    std::error_code walk_error;
    std::filesystem::recursive_directory_iterator it(
        folder, std::filesystem::directory_options::skip_permission_denied, walk_error);
    const std::filesystem::recursive_directory_iterator end;
    while (!walk_error && it != end) {
        std::error_code type_error;
        if (it->is_regular_file(type_error) && !type_error &&
            is_media_extension(lowercase_extension(it->path().string()))) {
            // generic_string(), not string(): every other path QueueItem::path ever holds
            // (addFiles' selectedFiles, a drop) arrives through a QUrl/QDir, which is always
            // "/"-separated even on Windows (Qt's own convention) - std::filesystem::path's
            // native string() would instead give this one caller "\\"-separated paths, so a
            // file added both ways would show up under two different-looking queue entries.
            paths.push_back(it->path().generic_string());
        }
        it.increment(walk_error);
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

}  // namespace iclforge::hearth::ui
