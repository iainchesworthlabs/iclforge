#include "project.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fmt/base.h>
#include <fmt/format.h>
#include <fstream>
#include <ios>
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
        return fmt::format("codec '{}' is not one of ac3, eac3, ac4", arg(0));
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
            "the project has {} sources and says nothing of where their channels go: "
            "save it from the Forge GUI, or give it a 'bed' and 'assignments'",
            arg(0));
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
    // A folder of its own in the temporary one, named for this run so that two do not share it.
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path scratch =
        fs::temp_directory_path(error) / ("forge-project-" + std::to_string(stamp));
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

    fs::create_directories(scratch, error);
    if (error) {
        fmt::println(stderr, "error: cannot make a folder for the intermediate files in {}",
                     scratch.string());
        return kExitOutput;
    }
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
