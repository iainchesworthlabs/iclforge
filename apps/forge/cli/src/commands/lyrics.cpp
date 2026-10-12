#include "lyrics.hpp"

#include <filesystem>
#include <fmt/base.h>
#include <string>
#include <string_view>

#include "../exit_codes.hpp"
#include "iclforge/lyrics/lyrics.hpp"

namespace forge_cli::commands {

int check_lyrics_option(const Options& meta, std::string_view out_path) {
    if (meta.lyrics_path.empty()) {
        return kExitOk;
    }
    if (out_path.empty() || is_stdio_path(out_path)) {
        fmt::println(stderr,
                     "error: lyrics= puts a file beside the stream, and '-' (standard output) has "
                     "no name to put one beside");
        return kExitUsage;
    }
    // Read here, and not only when it is written, so that a file that is not lyrics is said so
    // before the encode and not after it.
    const auto read = iclforge::lyrics::read_lyrics(std::filesystem::path{meta.lyrics_path});
    if (!read) {
        fmt::println(stderr, "error: lyrics={}: {}", meta.lyrics_path, read.error());
        return kExitInput;
    }
    return kExitOk;
}

int place_lyrics_option(const Options& meta, std::string_view out_path) {
    if (meta.lyrics_path.empty()) {
        return kExitOk;
    }
    FILE* status = status_stream(out_path);
    const auto placed = iclforge::lyrics::place_lyrics_beside(
        std::filesystem::path{meta.lyrics_path}, std::filesystem::path{std::string{out_path}});
    switch (placed.status) {
        case iclforge::lyrics::PlacementStatus::kWritten:
            status_println(status, "wrote {} (lyrics from {}, UTF-8 LRC)", placed.path.string(),
                           meta.lyrics_path);
            return kExitOk;
        case iclforge::lyrics::PlacementStatus::kExists:
            status_println(status, "left {} as it was (a lyrics file of that name is already there)",
                           placed.path.string());
            return kExitOk;
        case iclforge::lyrics::PlacementStatus::kUnreadable:
            fmt::println(stderr, "error: lyrics={}: {}", meta.lyrics_path, placed.error);
            return kExitInput;
        case iclforge::lyrics::PlacementStatus::kFailed:
            break;
    }
    fmt::println(stderr, "error: lyrics={}: {}", placed.path.string(), placed.error);
    return kExitOutput;
}

}  // namespace forge_cli::commands
