#include "project_command.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <system_error>

#include "iclforge/ac3/core/eac3_tables.hpp"
#include "iclforge/ac3/core/types.hpp"
#include "iclforge/ac3/encoder/plan.hpp"
#include "karaoke_parts.hpp"

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
    // What each source says it is (`part`), empty where it does not say.
    std::vector<std::string> parts;
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
            parts.push_back(string_of(source, "part"));
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
    } else if (codec == "truehd") {
        stream_suffix = ".thd";
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
    const bool ac4_objects = atmos && codec == "ac4";
    // AC-4 is written into an MP4 file by ac4-encode and atmos-encode themselves, as the window's
    // line says (`... out.mp4`), and not wrapped from a stream: the sample has no CRC.
    const bool direct_mp4 = codec == "ac4" && container == "mp4";

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
    if (container != "elementary" && !direct_mp4) {
        stream = request.scratch / path_of(stem + stream_suffix);
        out.scratch_files.push_back(stream);
    }

    // ---- TrueHD: one source, one command ---------------------------------------------------------
    // `forge truehd-encode <source> <stream>` [truehd-downmix=standard] [verify], and then the
    // container command like any other stream's. A lossless encode has no bit rate, tools, layout
    // or metadata of the AC-3 family, so a project that names them for it is refused, not run with
    // them dropped; the bit rate the window always writes is the one thing left out.
    if (codec == "truehd") {
        if (atmos) {
            return std::unexpected(Message{"truehd-no-atmos", {}});
        }
        if (container == "fmp4") {
            return std::unexpected(Message{"truehd-no-fmp4", {}});
        }
        if (sources.size() != 1) {
            return std::unexpected(Message{"truehd-one-source", {std::to_string(sources.size())}});
        }
        if (const Json* given = member(project, "assignments");
            given != nullptr && given->is_array() && !given->empty()) {
            return std::unexpected(Message{"truehd-no-assignments", {}});
        }
        if (const Json* given = member(encode, "options");
            given != nullptr && given->is_object() && !given->empty()) {
            return std::unexpected(Message{"truehd-no-options", {}});
        }
        Step first;
        first.argv = {"truehd-encode", sources.front(), utf8(stream)};
        if (const Json* thd = member(encode, "truehd"); thd != nullptr && thd->is_object()) {
            if (string_of(*thd, "downmix", "none") == "standard") {
                first.argv.emplace_back("truehd-downmix=standard");
            }
            if (bool_of(*thd, "verify")) {
                first.argv.emplace_back("verify");
            }
        }
        out.steps.push_back(std::move(first));
        if (container != "elementary") {
            static constexpr std::array<std::pair<std::string_view, std::string_view>, 4> kThd{{
                {"matroska", "mkv"},
                {"spdif", "spdif"},
                {"mp4", "mp4"},
                {"mpegts", "ts"},
            }};
            for (const auto& [id, command] : kThd) {
                if (id == container) {
                    out.steps.push_back(
                        Step{{std::string{command}, utf8(stream), utf8(out.output)}});
                }
            }
        }
        return out;
    }

    // ---- the rate, the layout and the routing -------------------------------------------------
    std::string rate = "192";
    if (const Json* given = member(encode, "bitrateKbps"); given != nullptr) {
        if (const auto n = int_of(*given)) {
            rate = std::to_string(*n);
        }
    }

    // Where each channel goes: the project's own `bed` and `assignments`, or, for a karaoke
    // project that names its sources' parts and nothing else, where Annex C puts them.
    std::string bed = string_of(project, "bed");
    std::string map;
    const Json* assignments = member(project, "assignments");
    const bool has_assignments =
        assignments != nullptr && assignments->is_array() && assignments->size() > 0;
    bool karaoke_layout = false;
    const bool all_named = !parts.empty() &&
                           std::all_of(parts.begin(), parts.end(),
                                       [](const std::string& part) { return !part.empty(); });
    if (!has_assignments && bed.empty() && sources.size() > 1 && all_named && !atmos) {
        std::array<std::optional<std::size_t>, 4> at;  // backing, melody, vocal 1, vocal 2
        for (std::size_t i = 0; i < parts.size(); ++i) {
            const std::array<std::string_view, 4> names{kPartBacking, kPartMelody, kPartVocal1,
                                                        kPartVocal2};
            std::size_t slot = names.size();
            for (std::size_t n = 0; n < names.size(); ++n) {
                if (names[n] == parts[i]) {
                    slot = n;
                }
            }
            if (slot == names.size()) {
                return std::unexpected(Message{"karaoke-unknown-part", {parts[i]}});
            }
            if (at[slot]) {
                return std::unexpected(Message{"karaoke-duplicate-part", {parts[i]}});
            }
            at[slot] = i;
        }
        if (!at[0]) {
            return std::unexpected(Message{"karaoke-no-backing", {}});
        }
        if (at[3] && !at[2]) {
            return std::unexpected(Message{"karaoke-vocal2-needs-vocal1", {}});
        }
        const KaraokeMode* mode =
            karaoke_mode(at[1].has_value(), (at[2] ? 1 : 0) + (at[3] ? 1 : 0));
        if (mode == nullptr) {
            return std::unexpected(Message{"karaoke-nothing-to-sing", {}});
        }
        bed = std::string{mode->bed};
        // Written in the order the window writes a map= in: by source, then by channel.
        struct Placed {
            std::size_t source;
            std::size_t channel;
            std::string_view position;
        };
        std::vector<Placed> placed;
        for (std::size_t i = 0; i < mode->count; ++i) {
            const std::string_view part = mode->slots[i].part;
            if (part == "L" || part == "R") {
                placed.push_back({*at[0], part == "L" ? std::size_t{0} : std::size_t{1},
                                  mode->slots[i].position});
            } else {
                const std::size_t source = part == "M" ? *at[1] : (part == "V1" ? *at[2] : *at[3]);
                placed.push_back({source, 0, mode->slots[i].position});
            }
        }
        std::sort(placed.begin(), placed.end(), [](const Placed& a, const Placed& b) {
            return a.source != b.source ? a.source < b.source : a.channel < b.channel;
        });
        for (const Placed& entry : placed) {
            if (!map.empty()) {
                map += ",";
            }
            map += std::to_string(entry.source) + "." + std::to_string(entry.channel) + ":" +
                   std::string{entry.position};
        }
        karaoke_layout = true;
    }

    std::string layout;
    if (!bed.empty()) {
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
    if (has_assignments) {
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
    }
    if (!map.empty()) {
        trailing.push_back("map=" + map);
    }
    // Several sources are routed by name or not at all: the commands refuse a second source with
    // no map=, since a channel nobody placed is a mistake and not a default. (The window says the
    // same of them: "unassigned".) A karaoke project's parts are named, so they are placed above.
    if (sources.size() > 1 && !atmos && map.empty()) {
        return std::unexpected(Message{"no-assignments", {std::to_string(sources.size())}});
    }
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        if (offsets[i] > 0.0) {
            trailing.push_back("offset=" + std::to_string(i) + ":" +
                               number_text(Json(offsets[i])));
        }
    }

    std::vector<std::string> options;
    bool names_service = false;
    if (const Json* given = member(encode, "options"); given != nullptr && given->is_object()) {
        for (auto it = given->begin(); it != given->end(); ++it) {
            names_service = names_service || it.key() == "bsmod";
            if (const auto token = option_token(it.key(), it.value())) {
                options.push_back(*token);
            }
        }
    }
    // A karaoke stream is a main service with bsmod 7 (Annex C): laying the parts out says so,
    // as the Karaoke flow does, unless the project has a service of its own to name.
    if (karaoke_layout && !names_service && (codec == "ac3" || codec == "eac3")) {
        options.emplace_back("bsmod=voiceover");
    }

    // ---- the scene of an object session -------------------------------------------------------
    // The file the command bar's line names, written where the steps can read it.
    std::string count_text;
    std::string scene_path;
    if (atmos) {
        if (const Json* objects = member(project, "objects"); objects != nullptr) {
            const std::int64_t count =
                member(*objects, "count") ? int_of(*member(*objects, "count")).value_or(0) : 0;
            if (count > 0) {
                count_text = std::to_string(count);
            }
            if (const Json* scene = member(*objects, "scene"); scene != nullptr && count > 0) {
                const fs::path file = request.scratch / path_of(stem + "-objects.json");
                out.files.emplace_back(file, scene->dump(2) + "\n");
                out.scratch_files.push_back(file);
                scene_path = utf8(file);
            }
        }
    }

    // ---- the stream's command ----------------------------------------------------------------
    Step first;
    if (ac4_objects) {
        // atmos-encode with codec=ac4 takes the sources, the assignment and the offsets the way
        // the window's line writes them, and the AC-4 tab's words after codec=ac4. Several sources
        // or a map= state their own channels, so [objects] is 0 then.
        first.argv = {"atmos-encode", sources.front(), utf8(stream), rate};
        const bool routed = sources.size() > 1 || !map.empty();
        if (routed || !count_text.empty() || !scene_path.empty()) {
            first.argv.push_back(routed || count_text.empty() ? "0" : count_text);
            if (!scene_path.empty()) {
                first.argv.push_back(scene_path);
            }
        }
        first.argv.insert(first.argv.end(), trailing.begin(), trailing.end());
        first.argv.emplace_back("codec=ac4");
        first.argv.insert(first.argv.end(), options.begin(), options.end());
    } else if (atmos) {
        first.argv = {"atmos-encode", sources.front(), utf8(stream), rate};
        // Several sources or a map= state their own channels, and [objects] is then 0 (the
        // command refuses both).
        const bool routed = sources.size() > 1 || !map.empty();
        if (routed || !count_text.empty()) {
            first.argv.push_back(routed || count_text.empty() ? "0" : count_text);
            if (!scene_path.empty()) {
                first.argv.push_back(scene_path);
            }
        }
        // Object mode takes the loudness and nothing else of the metadata group.
        for (const auto& token : options) {
            if (token.rfind("dialnorm", 0) == 0) {
                first.argv.push_back(token);
            }
        }
        if (routed) {
            first.argv.insert(first.argv.end(), trailing.begin(), trailing.end());
        } else {
            for (const auto& token : trailing) {
                if (token.rfind("offset=", 0) == 0) {
                    first.argv.push_back(token);
                }
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
    if (container != "elementary" && !direct_mp4) {
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
