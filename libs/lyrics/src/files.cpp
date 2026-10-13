#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "iclforge/lyrics/lyrics.hpp"
#include "iclforge/lyrics/timing.hpp"

// The part of the lyric reader that touches the file system: a file read, and the file beside an
// item. A translation unit of its own so that a program that only parses text (the sink firmware,
// which is handed the document by a server) does not link <fstream>, <filesystem> and the locale
// machinery they pull in: an S3 with Wi-Fi up has a few hundred bytes of internal RAM to spare for
// its decode task's stack (planning/karaoke-phase2.md, W7d).

namespace iclforge::lyrics {

namespace {

constexpr std::uintmax_t kMaxFileBytes = 4U * 1024U * 1024U;

// ASCII only, whatever the locale (lyrics.cpp's is_space says why the locale is not asked).
[[nodiscard]] std::string lowercase(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

}  // namespace

std::expected<Lyrics, std::string> read_lyrics(const std::filesystem::path& file,
                                               std::optional<std::int64_t> duration_ms,
                                               const TextEncoding& encoding) {
    std::error_code sized;
    const std::uintmax_t size = std::filesystem::file_size(file, sized);
    if (sized) {
        return std::unexpected(std::string{"The lyrics file could not be read: "} + sized.message());
    }
    if (size > kMaxFileBytes) {
        return std::unexpected(std::string{"The lyrics file is too large to be lyrics."});
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::unexpected(std::string{"The lyrics file could not be opened."});
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (!bytes.empty() && !in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
        return std::unexpected(std::string{"The lyrics file could not be read."});
    }
    return parse_timed_text(bytes, duration_ms, encoding);
}

namespace {

// `lyrics_file` read and written as UTF-8 LRC at `target`, through `target`.part and a rename, so
// that a write that stops part way never leaves a short file under the name a player looks for.
[[nodiscard]] Placement write_converted(const std::filesystem::path& lyrics_file,
                                        const std::filesystem::path& target,
                                        std::optional<std::int64_t> duration_ms,
                                        const TextEncoding& encoding, bool replace) {
    Placement out;
    out.path = target;
    std::error_code error;
    const bool there = std::filesystem::exists(target, error);
    if (there && std::filesystem::equivalent(lyrics_file, target, error)) {
        out.status = PlacementStatus::kFailed;
        out.error = "The output is the file being read: write the converted lyrics under another name.";
        return out;
    }
    if (there && !replace) {
        out.status = PlacementStatus::kExists;
        return out;
    }
    const auto read = read_lyrics(lyrics_file, duration_ms, encoding);
    if (!read) {
        out.status = PlacementStatus::kUnreadable;
        out.error = read.error();
        return out;
    }
    const std::string text = write_lrc(*read, LrcWriteOptions{.mark_gaps = true});
    std::filesystem::path partial = target;
    partial += ".part";
    {
        std::ofstream file(partial, std::ios::binary);
        if (!file || !file.write(text.data(), static_cast<std::streamsize>(text.size()))) {
            out.status = PlacementStatus::kFailed;
            out.error = "The lyrics file could not be written.";
            std::filesystem::remove(partial, error);
            return out;
        }
    }
    std::filesystem::rename(partial, target, error);
    if (error) {
        out.status = PlacementStatus::kFailed;
        out.error = "The lyrics file could not be written: " + error.message();
        std::filesystem::remove(partial, error);
        return out;
    }
    out.status = PlacementStatus::kWritten;
    return out;
}

}  // namespace

Placement place_lyrics_beside(const std::filesystem::path& lyrics_file,
                              const std::filesystem::path& stream_path,
                              std::optional<std::int64_t> duration_ms, const TextEncoding& encoding) {
    std::filesystem::path target = stream_path;
    target.replace_extension(".lrc");
    // Beside a stream an existing lyrics file is never replaced, and is not an error to find there.
    std::error_code error;
    if (std::filesystem::exists(target, error)) {
        Placement out;
        out.path = target;
        out.status = PlacementStatus::kExists;
        return out;
    }
    return write_converted(lyrics_file, target, duration_ms, encoding, false);
}

Placement convert_lyrics_file(const std::filesystem::path& lyrics_file,
                              const std::filesystem::path& out_path,
                              std::optional<std::int64_t> duration_ms, const TextEncoding& encoding,
                              bool replace) {
    return write_converted(lyrics_file, out_path, duration_ms, encoding, replace);
}

std::optional<std::filesystem::path> find_lyrics_file(const std::string& item_path) {
    const std::filesystem::path media{item_path};
    const std::filesystem::path folder = media.parent_path();
    const std::string stem = media.stem().string();
    if (stem.empty()) {
        return std::nullopt;
    }
    std::error_code error;
    // `.lrc` first and `.srt` where there is none: the usual spelling of each, which is also
    // the only one on a case-insensitive file system, then any case.
    for (const std::string_view extension :
         {std::string_view{".lrc"}, std::string_view{".srt"}, std::string_view{".ass"},
          std::string_view{".kar"}}) {
        const std::filesystem::path usual = folder / (stem + std::string{extension});
        if (std::filesystem::is_regular_file(usual, error) && !error) {
            return usual;
        }
        error.clear();
        std::filesystem::directory_iterator it(folder.empty() ? std::filesystem::path{"."} : folder,
                                               std::filesystem::directory_options::skip_permission_denied,
                                               error);
        const std::filesystem::directory_iterator end;
        while (!error && it != end) {
            std::error_code entry_error;
            if (it->is_regular_file(entry_error) && !entry_error &&
                it->path().stem().string() == stem &&
                lowercase(it->path().extension().string()) == extension) {
                return it->path();
            }
            it.increment(error);
        }
        error.clear();
    }
    return std::nullopt;
}

}  // namespace iclforge::lyrics
