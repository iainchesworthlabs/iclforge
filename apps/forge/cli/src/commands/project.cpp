#include "project.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fmt/base.h>
#include <fmt/format.h>
#include <fstream>
#include <ios>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "../exit_codes.hpp"
#include "project_command.hpp"
#include "project_document.hpp"

namespace forge_cli::commands {

namespace fs = std::filesystem;
namespace project = iclforge::apps::project;

namespace {

// A message of the document or the commands, in a sentence for a terminal. The window words the
// same codes for itself (Main.qml's projectMessage).
std::string sentence(const project::Message& m) {
    const auto arg = [&m](std::size_t i) { return i < m.args.size() ? m.args[i] : std::string{}; };
    if (m.code == "cannot-open") {
        return fmt::format("{} could not be opened", arg(0));
    }
    if (m.code == "not-json") {
        return fmt::format("{} is not a Forge project: it is not a JSON document", arg(0));
    }
    if (m.code == "not-a-project") {
        return fmt::format("{} is not a Forge project: it does not say it is one", arg(0));
    }
    if (m.code == "no-version") {
        return fmt::format("{} does not say which version of the project format it is", arg(0));
    }
    if (m.code == "newer-version") {
        return fmt::format(
            "{} was saved by a newer Forge (project version {}); this one reads up to version {}",
            arg(0), arg(1), arg(2));
    }
    if (m.code == "source-no-path") {
        return fmt::format("source {} has no path and was left out", arg(0));
    }
    if (m.code == "no-sources") {
        return "the project has no sources";
    }
    if (m.code == "source-missing") {
        return fmt::format("source '{}' was not found", arg(0));
    }
    if (m.code == "unknown-codec") {
        return fmt::format("codec '{}' is not one of ac3, eac3, ac4, truehd", arg(0));
    }
    if (m.code == "truehd-no-atmos") {
        return "a TrueHD project cannot be an Atmos one: TrueHD Atmos is not written";
    }
    if (m.code == "truehd-no-fmp4") {
        return "TrueHD has no fragmented MP4, HLS or DASH signalling here; use elementary, "
               "matroska, spdif, mp4 or mpegts";
    }
    if (m.code == "truehd-one-source") {
        return fmt::format("a TrueHD project codes one WAV, and this one has {}", arg(0));
    }
    if (m.code == "truehd-no-assignments") {
        return "a TrueHD project routes no channels: the WAV's channel mask names its speakers";
    }
    if (m.code == "truehd-no-options") {
        return "the AC-3 family's metadata options do not apply to a TrueHD encode; the project's "
               "'options' must be empty (the TrueHD options are in encode.truehd)";
    }
    if (m.code == "unknown-container") {
        return fmt::format(
            "container '{}' is not one of elementary, matroska, spdif, mp4, fmp4, mpegts",
            arg(0));
    }
    if (m.code == "unknown-bed") {
        return fmt::format("the channel layout '{}' is not one this forge knows", arg(0));
    }
    if (m.code == "bad-assignment") {
        return fmt::format("assignment {} needs a source, a channel and a destination", arg(0));
    }
    if (m.code == "no-assignments") {
        return fmt::format(
            "the project has {} sources and says nothing of where their channels go: give it "
            "'assignments' (and a 'bed'), name each source's 'part' (backing, melody, vocal1, "
            "vocal2) for a karaoke stream, or save it from the Forge GUI",
            arg(0));
    }
    if (m.code == "karaoke-unknown-part") {
        return fmt::format("a source's part '{}' is not one of backing, melody, vocal1, vocal2",
                           arg(0));
    }
    if (m.code == "karaoke-duplicate-part") {
        return fmt::format("two sources are the part '{}': a part is one place in the stream",
                           arg(0));
    }
    if (m.code == "karaoke-no-backing") {
        return "a karaoke stream needs a backing: name one source's part 'backing' (the music on "
               "the left and the right)";
    }
    if (m.code == "karaoke-vocal2-needs-vocal1") {
        return "a stream has no mode that carries vocal2 without vocal1: name it 'vocal1'";
    }
    if (m.code == "karaoke-nothing-to-sing") {
        return "a backing alone is stereo, with nothing to sing along to: add a melody guide or "
               "a vocal";
    }
    if (m.code == "unsupported") {
        return fmt::format("{} cannot be run from a project yet", arg(0));
    }
    return m.code;
}

}  // namespace

int run_project(std::string_view project_path, std::string_view out, bool print,
                const RunCommand& run) {
    const fs::path file{std::u8string{reinterpret_cast<const char8_t*>(project_path.data()),
                                      project_path.size()}};
    const auto opened = project::read_project(file);
    if (!opened) {
        fmt::println(stderr, "error: {}", sentence(opened.error()));
        // A file that is not there or not a project is the input's fault.
        return kExitInput;
    }
    for (const auto& warning : opened->warnings) {
        fmt::println(stderr, "warning: {}", sentence(warning));
    }

    project::Request request;
    request.project_file = file;
    if (!out.empty()) {
        request.output = fs::path{std::u8string{reinterpret_cast<const char8_t*>(out.data()),
                                                out.size()}};
    }
    std::error_code error;
    // A folder of its own in the temporary one, named for this run so that two do not share it, and
    // by 64 bits from the system's entropy, so that no other user of a shared temporary folder can
    // have made it first (it is made below with create_directory, which says when it was there).
    std::random_device entropy;
    const std::uint64_t stamp = (static_cast<std::uint64_t>(entropy()) << 32U) | entropy();
    const fs::path scratch =
        fs::temp_directory_path(error) / fmt::format("forge-project-{:016x}", stamp);
    request.scratch = scratch;

    const auto plan = project::command_plan(opened->project, request);
    if (!plan) {
        fmt::println(stderr, "error: {}", sentence(plan.error()));
        return plan.error().code == "source-missing" ? kExitInput : kExitUsage;
    }
    for (const auto& note : plan->notes) {
        fmt::println(stderr, "note: {}", sentence(note));
    }

    if (print) {
        std::string line;
        for (const auto& step : plan->steps) {
            if (!line.empty()) {
                line += " && ";
            }
            line += project::command_text(step);
        }
        fmt::println("{}", line);
        return kExitOk;
    }

    // A folder that was there already is not ours to write into: false from create_directory.
    const bool made = fs::create_directory(scratch, error);
    if (error || !made) {
        fmt::println(stderr, "error: cannot make a folder for the intermediate files in {}",
                     scratch.string());
        return kExitOutput;
    }
    // Only its owner reads what is in it (best effort: a file system with no such thing says no).
    fs::permissions(scratch, fs::perms::owner_all, fs::perm_options::replace, error);
    int result = kExitOk;
    for (const auto& [path, text] : plan->files) {
        std::ofstream stream{path, std::ios::binary | std::ios::trunc};
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!stream) {
            fmt::println(stderr, "error: cannot write {}", path.string());
            result = kExitOutput;
            break;
        }
    }
    if (result == kExitOk) {
        for (const auto& step : plan->steps) {
            result = run(step.argv);
            if (result != kExitOk) {
                break;
            }
        }
    }
    // What was made for the steps is not the result: the stream a container was wrapped from
    // and the objects' scene file, with the folder they were in.
    fs::remove_all(scratch, error);
    return result;
}

}  // namespace forge_cli::commands
