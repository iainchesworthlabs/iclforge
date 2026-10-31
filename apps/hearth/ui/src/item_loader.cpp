#include "item_loader.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string_view>
#include <system_error>

#include "item_source.hpp"
#include "container_input.hpp"

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
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return std::unexpected("could not open the file");
        }
        // A bare TrueHD or MLP stream is told by its first bytes and is not read into memory:
        // the session indexes it and reads it as it plays.
        std::vector<std::byte> head(static_cast<std::size_t>(std::min<std::uintmax_t>(size, kSniffBytes)));
        if (!head.empty()) {
            file.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
            if (!file) {
                return std::unexpected("could not read the file");
            }
        }
        if (iclforge::apps::sniff_container(head) == iclforge::apps::ContainerKind::kUnknown &&
            iclforge::apps::looks_like_truehd(head)) {
            auto source = std::make_shared<FileByteSource>(path, size);
            if (!source->ok()) {
                return std::unexpected("could not open the file");
            }
            return LoadedItem{.source = std::move(source), .truehd = true};
        }
        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        std::copy(head.begin(), head.end(), bytes.begin());
        if (bytes.size() > head.size()) {
            file.read(reinterpret_cast<char*>(bytes.data() + head.size()),
                      static_cast<std::streamsize>(bytes.size() - head.size()));
            if (!file) {
                return std::unexpected("could not read the file");
            }
        }
        // A container is told by its first bytes, not its name (a rip is as
        // likely to be called "title00.mkv" when it is not one, and an MP4 may
        // be named ".ac3"), so a bare stream goes through whatever its
        // extension says, without a second copy of it.
        if (iclforge::apps::sniff_container(bytes) == iclforge::apps::ContainerKind::kUnknown) {
            return LoadedItem{.bytes = std::move(bytes)};
        }
        auto stream = iclforge::apps::elementary_stream_from_bytes(bytes, kPlayedCodecs);
        if (!stream.error.empty()) {
            return std::unexpected(std::move(stream.error));
        }
        // The edit list's priming and padding are decoded but not played
        // (Session applies skip_samples and play_samples); what the container
        // said about the track reaches the media information.
        return LoadedItem{.bytes = std::move(stream.bytes),
                          .truehd = stream.codec == iclforge::apps::TrackCodec::kTruehd,
                          .skip_samples = stream.trim.start,
                          .play_samples = stream.trim.length,
                          .note = std::move(stream.trim_note),
                          .container = std::move(stream.container)};
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
