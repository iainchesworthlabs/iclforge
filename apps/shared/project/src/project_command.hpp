#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "project_document.hpp"

// What a project stands for as `forge` commands: `forge project <file>` runs these, and
// `forge project <file> print` shows them. They are the line the Forge GUI's command bar shows
// for the same draft (Main.qml's cliLine), so a project is run by the commands that already exist
// and nothing else: the same parsing, the same refusals, the same bytes. tst_projects.qml holds the
// two to each other.
//
// What a project does not say, the commands' own defaults supply, as they do for a person typing
// the line.
//
//   codec ac3        forge encode <source> <stream> <rate> [layout] [couple] [src=...] [map=...]
//                         [offset=...] <options>
//   codec eac3       forge eac3-encode <source> <stream> <rate> <tools> <layout> <vbr> [src=...]
//                         [map=...] [offset=...] <options>
//   codec ac4        forge ac4-encode <source> <stream> <rate> <options>
//   atmos: true      forge atmos-encode <source> <stream> <rate> [objects] [scene] <dialnorm>
//   atmos, codec ac4 forge atmos-encode <source> <stream> <rate> <objects> [scene] [src=...]
//                         [map=...] [offset=...] codec=ac4 <options>
//
// and then, for a container other than the bare stream, `forge mkv|spdif|mp4|fmp4|ts <stream>
// <out>`. AC-4 in MP4 is one command, not two: ac4-encode and atmos-encode write the MP4 file
// themselves when the output is named .mp4, as the window's line has them do.
//
// A project of several sources that names each one's `part` (backing, melody, vocal1, vocal2) and
// gives no `bed` and no `assignments` is laid out where Annex C's Table C.2.1 puts the parts
// (karaoke_parts.hpp), with the service set to bsmod 7 unless the options name one; that is what the
// Karaoke flow writes for the same parts, so the flow's project and the bare parts are one stream.

namespace iclforge::apps::project {

struct Step {
    // What follows `forge`: the command and its arguments, one to an element, no quoting.
    std::vector<std::string> argv;
};

struct Request {
    // The project's own file: its folder is what a source's relative path is relative to, and
    // its name is what the output is called when none is given.
    std::filesystem::path project_file;
    // Where the result goes. Beside the project, named for it, when it is not given.
    std::optional<std::filesystem::path> output;
    // A folder for what the steps make on the way (the stream a container is wrapped from, the
    // objects' scene file). It must exist.
    std::filesystem::path scratch;
};

struct CommandPlan {
    std::vector<Step> steps;
    // The files to write before the first step runs: a path and its text.
    std::vector<std::pair<std::filesystem::path, std::string>> files;
    // What was made for the steps and is the caller's to remove after them: the files above and
    // the stream a container is wrapped from.
    std::vector<std::filesystem::path> scratch_files;
    // What the steps write.
    std::filesystem::path output;
    // What the project holds that is not used here, said once so that nothing is dropped
    // silently. Empty for now: every key a project has is used, or is another version's.
    std::vector<Message> notes;
};

// The commands of a project, or why it has none. The codes: no-sources, source-missing (path),
// unknown-codec (value), unknown-container (value), unknown-bed (value), bad-assignment (the
// entry's number), no-assignments (how many sources: several are routed by name or not at all),
// karaoke-unknown-part (the part), karaoke-duplicate-part (the part), karaoke-no-backing,
// karaoke-vocal2-needs-vocal1, karaoke-nothing-to-sing, unsupported (what).
[[nodiscard]] std::expected<CommandPlan, Message> command_plan(const Json& project,
                                                               const Request& request);

// A step as a line a person could type: `forge` first, an argument quoted where it has a space.
[[nodiscard]] std::string command_text(const Step& step);

}  // namespace iclforge::apps::project
