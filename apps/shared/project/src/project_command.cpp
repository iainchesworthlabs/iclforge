#include "project_command.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <system_error>

#include "iclforge/ac3/core/eac3_tables.hpp"
#include "iclforge/ac3/core/types.hpp"
#include "iclforge/ac3/encoder/plan.hpp"

namespace iclforge::apps::project {

namespace fs = std::filesystem;
namespace plan = iclforge::ac3::plan;

namespace {

std::string utf8(const fs::path& path) {
    const auto text = path.generic_u8string();
    return std::string{reinterpret_cast<const char*>(text.data()), text.size()};
}

fs::path path_of(std::string_view text) {
    return fs::path{std::u8string{reinterpret_cast<const char8_t*>(text.data()), text.size()}};
}

// The bed ids, in Acmod's own order (EncoderController's kBeds is the same table).
constexpr std::array<std::string_view, 8> kBeds{"1+1", "1/0", "2/0", "3/0",
                                                "2/1", "3/1", "2/2", "3/2"};

struct Extra {
    std::string_view id;
    std::uint16_t bit;
};
constexpr std::array<Extra, 5> kExtras{{
    {"wide", ac3::eac3::chanmap::kLwRwBit},
    {"rear", ac3::eac3::chanmap::kLrsRrsBit},
    {"topf", ac3::eac3::chanmap::kVhlVhrBit},
    {"topr", ac3::eac3::chanmap::kLtsRtsBit},
    {"lfe2", ac3::eac3::chanmap::kLfe2Bit},
}};

// A number as a person would type it: a whole number as one (-3.0 is -3), and a fraction in the
// shortest text that reads back as it was.
std::string number_text(const Json& value) {
    if (const auto whole = int_of(value)) {
        return std::to_string(*whole);
    }
    return value.dump();
}

// An option's value as the command line takes it: `key=value`, or the bare `key` of a flag.
// Nothing for a value a command line has no spelling for.
std::optional<std::string> option_token(const std::string& key, const Json& value) {
    if (value.is_boolean()) {
        return value.get<bool>() ? std::optional<std::string>{key} : std::nullopt;
    }
    if (value.is_string()) {
        return key + "=" + value.get<std::string>();
    }
    if (value.is_number()) {
        return key + "=" + number_text(value);
    }
    return std::nullopt;
}

const char* container_suffix(std::string_view container) {
    if (container == "matroska") {
        return ".mkv";
    }
    if (container == "spdif") {
        return ".wav";
    }
    if (container == "mp4") {
        return ".mp4";
    }
    if (container == "mpegts") {
        return ".ts";
    }
    return "";  // fmp4 is a folder, and the stream has the codec's own
}

}  // namespace

std::expected<CommandPlan, Message> command_plan(const Json& project, const Request& request) {
    CommandPlan out;
    const fs::path folder = request.project_file.parent_path();
    const std::string stem = utf8(request.project_file.stem());

    // ---- the sources -------------------------------------------------------------------------
    std::vector<std::string> sources;
    std::vector<double> offsets;
    if (const Json* given = member(project, "sources"); given != nullptr && given->is_array()) {
        for (const auto& source : *given) {
            const std::string stored = string_of(source, "path");
            if (stored.empty()) {
                continue;  // read_project warned of it
            }
            const fs::path absolute = resolved_path(folder, stored);
            std::error_code ignored;
            if (!fs::exists(absolute, ignored)) {
                return std::unexpected(Message{"source-missing", {stored}});
            }
            sources.push_back(utf8(absolute));
            offsets.push_back(number_of(source, "offsetSeconds", 0.0));
        }
    }
    if (sources.empty()) {
        return std::unexpected(Message{"no-sources", {}});
    }

    // ---- the encode --------------------------------------------------------------------------
    const Json empty = Json::object();
    const Json* encode_ptr = member(project, "encode");
    const Json& encode = encode_ptr != nullptr && encode_ptr->is_object() ? *encode_ptr : empty;
    const std::string codec = string_of(encode, "codec", "ac3");
    const std::string container = string_of(encode, "container", "elementary");
    const bool atmos = bool_of(project, "atmos");

    std::string stream_suffix;
    if (atmos) {
        stream_suffix = codec == "ac4" ? ".ac4" : ".ec3";
    } else if (codec == "ac3") {
        stream_suffix = ".ac3";
    } else if (codec == "eac3") {
        stream_suffix = ".ec3";
    } else if (codec == "ac4") {
        stream_suffix = ".ac4";
    } else {
        return std::unexpected(Message{"unknown-codec", {codec}});
    }
    static constexpr std::array<std::string_view, 6> kContainers{"elementary", "matroska", "spdif",
                                                                 "mp4",        "fmp4",     "mpegts"};
    bool known = false;
    for (const auto id : kContainers) {
        known = known || id == container;
    }
    if (!known) {
        return std::unexpected(Message{"unknown-container", {container}});
    }
    if (codec == "ac4" && atmos) {
        return std::unexpected(Message{"unsupported", {"AC-4 objects"}});
    }
    if (atmos && sources.size() > 1) {
        return std::unexpected(Message{"unsupported", {"objects from several sources"}});
    }

    // Where the result goes, and where the stream is written on the way to it.
    if (request.output) {
        out.output = *request.output;
    } else if (container == "fmp4") {
        out.output = folder / path_of(stem);
    } else {
        out.output = folder / path_of(stem + (container == "elementary"
                                                  ? stream_suffix
                                                  : container_suffix(container)));
    }
    fs::path stream = out.output;
    if (container != "elementary") {
        stream = request.scratch / path_of(stem + stream_suffix);
        out.scratch_files.push_back(stream);
    }

    // ---- the rate, the layout and the routing -------------------------------------------------
    std::string rate = "192";
    if (const Json* given = member(encode, "bitrateKbps"); given != nullptr) {
        if (const auto n = int_of(*given)) {
            rate = std::to_string(*n);
        }
    }

    std::string layout;
    if (const std::string bed = string_of(project, "bed"); !bed.empty()) {
        std::size_t at = kBeds.size();
        for (std::size_t i = 0; i < kBeds.size(); ++i) {
            if (kBeds[i] == bed) {
                at = i;
            }
        }
        if (at == kBeds.size()) {
            return std::unexpected(Message{"unknown-bed", {bed}});
        }
        if (at == 0) {
            layout = "1+1";
        } else {
            std::uint16_t mask = ac3::eac3::chanmap::acmod_map(static_cast<ac3::Acmod>(at),
                                                               bool_of(project, "lfe"));
            if (const Json* extras = member(project, "extras");
                extras != nullptr && extras->is_array()) {
                for (const auto& extra : *extras) {
                    for (const auto& known_extra : kExtras) {
                        if (extra.is_string() && extra.get<std::string>() == known_extra.id) {
                            mask = static_cast<std::uint16_t>(mask | known_extra.bit);
                        }
                    }
                }
            }
            layout = plan::format_channels(mask);
        }
    }

    // What follows the positionals, in the order the command bar writes them: the other sources,
    // the assignment, the offsets, the options.
    std::vector<std::string> trailing;
    for (std::size_t i = 1; i < sources.size(); ++i) {
        trailing.push_back("src=" + sources[i]);
    }
    if (const Json* assignments = member(project, "assignments");
        assignments != nullptr && assignments->is_array() && assignments->size() > 0) {
        std::string map;
        for (std::size_t i = 0; i < assignments->size(); ++i) {
            const Json& entry = (*assignments)[i];
            const auto source = member(entry, "source") ? int_of(*member(entry, "source"))
                                                       : std::nullopt;
            const auto channel = member(entry, "channel") ? int_of(*member(entry, "channel"))
                                                         : std::nullopt;
            const std::string to = string_of(entry, "to");
            if (!source || !channel || to.empty() || *source < 0 || *channel < 0 ||
                static_cast<std::size_t>(*source) >= sources.size()) {
                return std::unexpected(Message{"bad-assignment", {std::to_string(i + 1)}});
            }
            if (!map.empty()) {
                map += ",";
            }
            map += std::to_string(*source) + "." + std::to_string(*channel) + ":" + to;
        }
        trailing.push_back("map=" + map);
    }
    // Several sources are routed by name or not at all: the commands refuse a second source with
    // no map=, since a channel nobody placed is a mistake and not a default.
    const bool has_map = !trailing.empty() && trailing.back().rfind("map=", 0) == 0;
    if (sources.size() > 1 && !atmos && !has_map) {
        return std::unexpected(Message{"no-assignments", {std::to_string(sources.size())}});
    }
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        if (offsets[i] > 0.0) {
            trailing.push_back("offset=" + std::to_string(i) + ":" +
                               number_text(Json(offsets[i])));
        }
    }

    std::vector<std::string> options;
    if (const Json* given = member(encode, "options"); given != nullptr && given->is_object()) {
        for (auto it = given->begin(); it != given->end(); ++it) {
            if (const auto token = option_token(it.key(), it.value())) {
                options.push_back(*token);
            }
        }
    }

    // ---- the stream's command ----------------------------------------------------------------
    Step first;
    if (atmos) {
        first.argv = {"atmos-encode", sources.front(), utf8(stream), rate};
        if (const Json* objects = member(project, "objects"); objects != nullptr) {
            const std::int64_t count =
                member(*objects, "count") ? int_of(*member(*objects, "count")).value_or(0) : 0;
            if (count > 0) {
                first.argv.push_back(std::to_string(count));
                if (const Json* scene = member(*objects, "scene"); scene != nullptr) {
                    // The scene file the command bar's line names, written where the steps can
                    // read it.
                    const fs::path file = request.scratch / path_of(stem + "-objects.json");
                    out.files.emplace_back(file, scene->dump(2) + "\n");
                    out.scratch_files.push_back(file);
                    first.argv.push_back(utf8(file));
                }
            }
        }
        // Object mode takes the loudness and nothing else of the metadata group.
        for (const auto& token : options) {
            if (token.rfind("dialnorm", 0) == 0) {
                first.argv.push_back(token);
            }
        }
        for (const auto& token : trailing) {
            if (token.rfind("offset=", 0) == 0) {
                first.argv.push_back(token);
            }
        }
    } else if (codec == "ac3") {
        first.argv = {"encode", sources.front(), utf8(stream), rate, layout};
        bool couple = false;
        if (const std::string tools = string_of(encode, "tools"); !tools.empty()) {
            plan::Tools parsed;
            couple = plan::parse_tools(tools, parsed) && parsed.coupling;
        }
        if (couple) {
            first.argv.push_back("couple");
        }
        first.argv.insert(first.argv.end(), trailing.begin(), trailing.end());
        first.argv.insert(first.argv.end(), options.begin(), options.end());
    } else if (codec == "eac3") {
        const std::string tools = string_of(encode, "tools", "none");
        const std::string vbr = string_of(encode, "vbr", "off");
        first.argv = {"eac3-encode", sources.front(), utf8(stream), rate,
                      tools.empty() ? "none" : tools, layout, vbr};
        first.argv.insert(first.argv.end(), trailing.begin(), trailing.end());
        first.argv.insert(first.argv.end(), options.begin(), options.end());
    } else {
        first.argv = {"ac4-encode", sources.front(), utf8(stream), rate};
        first.argv.insert(first.argv.end(), options.begin(), options.end());
    }
    out.steps.push_back(std::move(first));

    // ---- the container -----------------------------------------------------------------------
    if (container != "elementary") {
        static constexpr std::array<std::pair<std::string_view, std::string_view>, 5> kCommands{{
            {"matroska", "mkv"},
            {"spdif", "spdif"},
            {"mp4", "mp4"},
            {"fmp4", "fmp4"},
            {"mpegts", "ts"},
        }};
        for (const auto& [id, command] : kCommands) {
            if (id == container) {
                out.steps.push_back(Step{{std::string{command}, utf8(stream), utf8(out.output)}});
            }
        }
    }
    return out;
}

std::string command_text(const Step& step) {
    std::string out = "forge";
    for (const auto& argument : step.argv) {
        out += ' ';
        const bool quote = argument.empty() ||
                           argument.find_first_of(" \t\"'") != std::string::npos;
        if (!quote) {
            out += argument;
            continue;
        }
        out += '"';
        for (const char c : argument) {
            if (c == '"') {
                out += '\\';
            }
            out += c;
        }
        out += '"';
    }
    return out;
}

}  // namespace iclforge::apps::project
