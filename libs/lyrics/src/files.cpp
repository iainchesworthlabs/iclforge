#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

#include "iclforge/lyrics/lyrics.hpp"

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
                                               std::optional<std::int64_t> duration_ms) {
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
    return parse_lyrics(bytes, duration_ms);
}

std::optional<std::filesystem::path> find_lyrics_file(const std::string& item_path) {
    const std::filesystem::path media{item_path};
    const std::filesystem::path folder = media.parent_path();
    const std::string stem = media.stem().string();
    if (stem.empty()) {
        return std::nullopt;
    }
    std::error_code error;
    // The usual spelling first, which is also the only one on a case-
    // insensitive file system.
    const std::filesystem::path usual = folder / (stem + ".lrc");
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
            lowercase(it->path().extension().string()) == ".lrc") {
            return it->path();
        }
        it.increment(error);
    }
    return std::nullopt;
}

}  // namespace iclforge::lyrics
