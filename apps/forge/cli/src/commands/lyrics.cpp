#include "lyrics.hpp"

#include <filesystem>
#include <fmt/base.h>
#include <optional>
#include <string>
#include <string_view>

#include "../exit_codes.hpp"
#include "code_pages.hpp"
#include "iclforge/lyrics/lyrics.hpp"

namespace forge_cli::commands {

namespace {

// What to read a lyrics file in a code page with: the page lyrics-encoding= named (checked when the
// key was read, so that it is a page this system has), or nothing, so that such a file is refused.
iclforge::lyrics::TextEncoding encoding_of(const Options& meta) {
    if (meta.lyrics_encoding.empty()) {
        return {};
    }
    const auto* page = iclforge::apps::find_code_page(meta.lyrics_encoding);
    return page != nullptr ? iclforge::apps::lyrics_encoding(*page) : iclforge::lyrics::TextEncoding{};
}

// The library says a file is not UTF-8 or UTF-16 and cannot say what to do about it, which is a
// thing only a command line has: name the page.
std::string with_hint(const Options& meta, std::string error) {
    if (meta.lyrics_encoding.empty() && iclforge::lyrics::is_code_page_refusal(error)) {
        error += " If it is in a code page, name it with lyrics-encoding= (for example windows-1252, "
                 "windows-1251 or shift-jis).";
    }
    return error;
}

}  // namespace

int check_lyrics_option(const Options& meta, std::string_view out_path) {
    if (meta.lyrics_path.empty()) {
        if (!meta.lyrics_encoding.empty()) {
            fmt::println(stderr,
                         "error: lyrics-encoding= is the code page of the file lyrics= names, and "
                         "there is no lyrics=");
            return kExitUsage;
        }
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
    const auto read =
        iclforge::lyrics::read_lyrics(std::filesystem::path{meta.lyrics_path}, std::nullopt, encoding_of(meta));
    if (!read) {
        fmt::println(stderr, "error: lyrics={}: {}", meta.lyrics_path, with_hint(meta, read.error()));
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
        std::filesystem::path{meta.lyrics_path}, std::filesystem::path{std::string{out_path}},
        std::nullopt, encoding_of(meta));
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
            fmt::println(stderr, "error: lyrics={}: {}", meta.lyrics_path, with_hint(meta, placed.error));
            return kExitInput;
        case iclforge::lyrics::PlacementStatus::kFailed:
            break;
    }
    fmt::println(stderr, "error: lyrics={}: {}", placed.path.string(), placed.error);
    return kExitOutput;
}

int run_lyrics_convert(std::string_view in_path, std::string_view out_path, const Options& meta) {
    if (is_stdio_path(in_path) || is_stdio_path(out_path)) {
        fmt::println(stderr,
                     "error: lyrics-to-lrc reads and writes named files, not standard input or "
                     "output");
        return kExitUsage;
    }
    const auto placed =
        iclforge::lyrics::convert_lyrics_file(std::filesystem::path{std::string{in_path}},
                                              std::filesystem::path{std::string{out_path}},
                                              std::nullopt, encoding_of(meta), meta.lyrics_overwrite);
    switch (placed.status) {
        case iclforge::lyrics::PlacementStatus::kWritten:
            status_println(status_stream(out_path), "wrote {} (UTF-8 LRC from {})",
                           placed.path.string(), in_path);
            return kExitOk;
        case iclforge::lyrics::PlacementStatus::kExists:
            fmt::println(stderr, "error: {} is already there; overwrite=1 replaces it",
                         placed.path.string());
            return kExitOutput;
        case iclforge::lyrics::PlacementStatus::kUnreadable:
            fmt::println(stderr, "error: {}: {}", in_path, with_hint(meta, placed.error));
            return kExitInput;
        case iclforge::lyrics::PlacementStatus::kFailed:
            break;
    }
    fmt::println(stderr, "error: {}: {}", placed.path.string(), placed.error);
    return kExitOutput;
}

}  // namespace forge_cli::commands
