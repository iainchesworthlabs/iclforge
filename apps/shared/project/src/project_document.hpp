#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

// The project file's document: what is read, checked and written of a `.forgeproj`, shared by
// the Forge GUI and `forge` so that the two cannot disagree about a file. docs/forge/gui/
// projects.md is the format's own page; this is the part of it that is code.
//
// A project is a JSON object:
//
//   { "format": "iclforge-project", "version": 1, "title": "...", "flow": "karaoke",
//     "sources": [ { "path": "parts/backing.wav", "part": "backing" }, ... ],
//     "encode": { "codec": "ac3", "bitrateKbps": 384, "options": { "bsmod": "voiceover" } },
//     "karaoke": { "listening": "aware", "vocals": "both" } }
//
// and is built to be extended:
//
//   - `version` is the format's revision. A reader opens any version up to the one it knows and
//     refuses a newer one by saying so, rather than reading half of it. A change that adds a key
//     or a value does not raise it; one that changes what an existing key means does.
//   - A key this reader does not know is not an error: the document is kept whole, in the order it
//     was written (nlohmann's ordered_json), and written back with the keys it did change, so
//     opening and saving a project made by a newer version loses nothing this one did not
//     understand.
//   - A source's `path` is relative to the project file's folder when it can be, so a folder of
//     parts and its project move together.
//
// Every message is a code and its arguments, not a sentence: the window words it, and so
// translates it, and `forge` words it for a terminal.

namespace iclforge::apps::project {

// An object that keeps its keys in the order they were written in. Numbers are read and written
// without consulting the process's locale, which matters here: Qt sets the C locale from the
// environment, and strtod would read "0.5" as 0 under one that writes a comma.
using Json = nlohmann::ordered_json;

inline constexpr std::string_view kFormat = "iclforge-project";
inline constexpr int kVersion = 1;
// "forgeproj", without the dot.
inline constexpr std::string_view kSuffix = "forgeproj";

struct Message {
    // cannot-open (name), not-json (name), not-a-project (name), no-version (name),
    // newer-version (name, the file's version, the one read here), cannot-write (name),
    // source-no-path (the source's number from 1).
    std::string code;
    std::vector<std::string> args;
};

struct Opened {
    Json project;
    // What was set aside rather than refused: a source with no path.
    std::vector<Message> warnings;
};

// The keys of a project in the order they are written, format and version first, so that a person
// (or a file(1)) sees what it is.
[[nodiscard]] const std::vector<std::string>& key_order();

// What a member of a project holds, or `fallback` where it is missing or holds another kind of
// value: a reader that is told a string where it wanted a number is told no more than that, and
// goes on. None of them throws.
[[nodiscard]] std::string string_of(const Json& object, std::string_view key,
                                    std::string_view fallback = {});
[[nodiscard]] bool bool_of(const Json& object, std::string_view key, bool fallback = false);
// A whole number (an integer, or a float with no fraction), else `fallback`.
[[nodiscard]] std::optional<std::int64_t> int_of(const Json& value);
[[nodiscard]] double number_of(const Json& object, std::string_view key, double fallback = 0.0);
// An object's member, or nullptr.
[[nodiscard]] const Json* member(const Json& object, std::string_view key);

// Reads `file`. Refused, with the reason, when it cannot be opened, is not a JSON object, does not
// say it is a project, gives no version or gives a newer one than this reads.
[[nodiscard]] std::expected<Opened, Message> read_project(const std::filesystem::path& file);

// The document as a file's text: `project` with `format` and `version` set on it, in key_order().
[[nodiscard]] std::string project_text(Json project);

// Writes the document to `file`. The file is replaced only once whole.
[[nodiscard]] std::expected<void, Message> write_project(const std::filesystem::path& file,
                                                         Json project);

// `file` as a path to keep in a project that lives in `folder`: relative to it, with `/` as the
// separator, where there is a relative form (another drive has none), absolute otherwise.
[[nodiscard]] std::string stored_path(const std::filesystem::path& folder,
                                      const std::filesystem::path& file);

// A path as a project keeps it, made absolute from `folder` and cleaned.
[[nodiscard]] std::filesystem::path resolved_path(const std::filesystem::path& folder,
                                                  std::string_view stored);

}  // namespace iclforge::apps::project
