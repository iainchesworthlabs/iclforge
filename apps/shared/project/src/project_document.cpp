#include "project_document.hpp"

#include <cmath>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace iclforge::apps::project {

namespace fs = std::filesystem;

namespace {

// Paths are UTF-8 in the document and whatever the platform keeps them as in std::filesystem.
fs::path from_utf8(std::string_view text) {
    return fs::path{std::u8string{reinterpret_cast<const char8_t*>(text.data()), text.size()}};
}

std::string to_utf8(const fs::path& path) {
    const auto text = path.generic_u8string();
    return std::string{reinterpret_cast<const char*>(text.data()), text.size()};
}

std::string name_of(const fs::path& file) {
    return to_utf8(file.filename());
}

}  // namespace

const std::vector<std::string>& key_order() {
    static const std::vector<std::string> order{
        "format", "version", "title",   "flow",        "sources", "bed",     "lfe",
        "extras", "atmos",   "objects", "assignments", "encode",  "karaoke",
    };
    return order;
}

const Json* member(const Json& object, std::string_view key) {
    if (!object.is_object()) {
        return nullptr;
    }
    const auto found = object.find(std::string{key});
    return found == object.end() ? nullptr : &*found;
}

std::string string_of(const Json& object, std::string_view key, std::string_view fallback) {
    const Json* value = member(object, key);
    if (value == nullptr || !value->is_string()) {
        return std::string{fallback};
    }
    return value->get<std::string>();
}

bool bool_of(const Json& object, std::string_view key, bool fallback) {
    const Json* value = member(object, key);
    return value != nullptr && value->is_boolean() ? value->get<bool>() : fallback;
}

std::optional<std::int64_t> int_of(const Json& value) {
    if (value.is_number_integer()) {
        return value.get<std::int64_t>();
    }
    if (value.is_number_unsigned()) {
        const auto big = value.get<std::uint64_t>();
        if (big <= static_cast<std::uint64_t>(INT64_MAX)) {
            return static_cast<std::int64_t>(big);
        }
        return std::nullopt;
    }
    if (value.is_number_float()) {
        const double number = value.get<double>();
        if (std::floor(number) == number && std::fabs(number) < 9007199254740992.0) {
            return static_cast<std::int64_t>(number);
        }
    }
    return std::nullopt;
}

double number_of(const Json& object, std::string_view key, double fallback) {
    const Json* value = member(object, key);
    return value != nullptr && value->is_number() ? value->get<double>() : fallback;
}

std::expected<Opened, Message> read_project(const fs::path& file) {
    std::ifstream in{file, std::ios::binary};
    if (!in) {
        return std::unexpected(Message{"cannot-open", {name_of(file)}});
    }
    const std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    // No exceptions: a file that is not JSON comes back discarded. A byte-order mark in front is
    // skipped by the parser.
    Json parsed = Json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return std::unexpected(Message{"not-json", {name_of(file)}});
    }
    Opened out;
    out.project = std::move(parsed);
    if (string_of(out.project, "format") != kFormat) {
        return std::unexpected(Message{"not-a-project", {name_of(file)}});
    }
    const Json* version_value = member(out.project, "version");
    const auto version = version_value != nullptr ? int_of(*version_value) : std::nullopt;
    if (!version || *version < 1) {
        return std::unexpected(Message{"no-version", {name_of(file)}});
    }
    if (*version > kVersion) {
        return std::unexpected(Message{
            "newer-version", {name_of(file), std::to_string(*version), std::to_string(kVersion)}});
    }
    if (const Json* sources = member(out.project, "sources");
        sources != nullptr && sources->is_array()) {
        for (std::size_t i = 0; i < sources->size(); ++i) {
            if (string_of((*sources)[i], "path").empty()) {
                out.warnings.push_back(Message{"source-no-path", {std::to_string(i + 1)}});
            }
        }
    }
    return out;
}

std::string project_text(Json project) {
    if (!project.is_object()) {
        project = Json::object();
    }
    project["format"] = std::string{kFormat};
    project["version"] = kVersion;
    // format and version first and the rest in reading order; what this version has no place for
    // goes after, in the order it came.
    Json ordered = Json::object();
    for (const auto& key : key_order()) {
        if (const auto found = project.find(key); found != project.end()) {
            ordered[key] = std::move(*found);
        }
    }
    for (auto it = project.begin(); it != project.end(); ++it) {
        if (!ordered.contains(it.key())) {
            ordered[it.key()] = std::move(*it);
        }
    }
    // Indented two spaces, with non-ASCII text left as it is.
    return ordered.dump(2, ' ', /*ensure_ascii=*/false) + "\n";
}

std::expected<void, Message> write_project(const fs::path& file, Json project) {
    const std::string text = project_text(std::move(project));
    // Whole or not at all: written beside the file and moved over it.
    fs::path scratch = file;
    scratch += ".tmp";
    {
        std::ofstream out{scratch, std::ios::binary | std::ios::trunc};
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) {
            out.close();
            std::error_code ignored;
            fs::remove(scratch, ignored);
            return std::unexpected(Message{"cannot-write", {name_of(file)}});
        }
    }
    std::error_code error;
    fs::rename(scratch, file, error);
    if (error) {
        std::error_code ignored;
        fs::remove(scratch, ignored);
        return std::unexpected(Message{"cannot-write", {name_of(file)}});
    }
    return {};
}

std::string stored_path(const fs::path& folder, const fs::path& file) {
    std::error_code error;
    const fs::path base = fs::absolute(folder, error).lexically_normal();
    const fs::path target = fs::absolute(file, error).lexically_normal();
    const fs::path relative = target.lexically_relative(base);
    // Another drive has no relative form: lexically_relative answers with an empty path.
    if (relative.empty()) {
        return to_utf8(target);
    }
    return to_utf8(relative);
}

fs::path resolved_path(const fs::path& folder, std::string_view stored) {
    const fs::path path = from_utf8(stored);
    std::error_code error;
    if (path.is_absolute()) {
        return path.lexically_normal();
    }
    return fs::absolute(folder / path, error).lexically_normal();
}

}  // namespace iclforge::apps::project
