#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "platform/process.hpp"

// `forge project` (commands/project.cpp over apps/shared/project): a Forge project file run as the
// commands it stands for. The real binary, as a subprocess, on the karaoke example's own project
// (examples/karaoke/auld-lang-syne/): it writes the example's committed stream byte for byte, which
// is also what the Forge GUI's encode of it writes (tst_projects.qml holds that side).
//
// ICLFORGE_CLI_EXE and ICLFORGE_KARAOKE_EXAMPLE_DIR come from apps/forge/cli/tests/CMakeLists.txt;
// run_cli is a trimmed copy of test_cli_probe.cpp's helper of the same name.

namespace fs = std::filesystem;

namespace {

fs::path scratch_dir() {
    auto dir = fs::path{ICLFORGE_TEST_SCRATCH_DIR} /
               ("cli_project_" + iclforge::test::platform::process_id());
    fs::create_directories(dir);
    return dir;
}

int run_cli(const std::string& args, const fs::path& log) {
    const std::string command =
        "\"" + std::string(ICLFORGE_CLI_EXE) + "\" " + args + " > \"" + log.string() + "\" 2>&1";
    return iclforge::test::platform::run_shell(command);
}

std::string read_bytes(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

fs::path example_project() {
    return fs::path{ICLFORGE_KARAOKE_EXAMPLE_DIR} / "auld-lang-syne.forgeproj";
}

std::string quoted(const fs::path& path) {
    return "\"" + path.generic_string() + "\"";
}

}  // namespace

TEST_CASE("forge project writes the example's stream byte for byte", "[cli][project]") {
    const auto dir = scratch_dir();
    const auto out = dir / "from-project.ac3";
    fs::remove(out);
    const auto log = dir / "project.log";
    REQUIRE(run_cli("project " + quoted(example_project()) + " " + quoted(out), log) == 0);
    INFO(read_bytes(log));
    const std::string made = read_bytes(out);
    REQUIRE_FALSE(made.empty());
    CHECK(made == read_bytes(fs::path{ICLFORGE_KARAOKE_EXAMPLE_DIR} / "auld-lang-syne.ac3"));
}

TEST_CASE("forge project print says the commands and runs none", "[cli][project]") {
    const auto dir = scratch_dir();
    const auto log = dir / "print.log";
    const auto out = dir / "printed.ac3";
    fs::remove(out);
    REQUIRE(run_cli("project " + quoted(example_project()) + " print " + quoted(out), log) == 0);
    const std::string text = read_bytes(log);
    INFO(text);
    CHECK(text.rfind("forge encode ", 0) == 0);
    CHECK(text.find("384 L,C,R,Ls,Rs") != std::string::npos);
    CHECK(text.find("map=0.0:L,0.1:R,1.0:C,2.0:Ls,3.0:Rs") != std::string::npos);
    CHECK(text.find("bsmod=voiceover") != std::string::npos);
    CHECK_FALSE(fs::exists(out));
}

TEST_CASE("forge project wraps the stream in the container the project names",
          "[cli][project]") {
    const auto dir = scratch_dir();
    const auto project = dir / "wrapped.forgeproj";
    // The example's own parts, so nothing is generated here: the example's project with its
    // container changed and its paths made absolute.
    const fs::path parts = fs::path{ICLFORGE_KARAOKE_EXAMPLE_DIR} / "parts";
    {
        std::ofstream out{project, std::ios::binary | std::ios::trunc};
        out << R"({
  "format": "iclforge-project",
  "version": 1,
  "sources": [
    {"path": ")" << (parts / "backing.wav").generic_string() << R"("},
    {"path": ")" << (parts / "melody-guide.wav").generic_string() << R"("}
  ],
  "bed": "3/0",
  "assignments": [
    {"source": 0, "channel": 0, "to": "L"},
    {"source": 0, "channel": 1, "to": "R"},
    {"source": 1, "channel": 0, "to": "C"}
  ],
  "encode": {"codec": "ac3", "container": "matroska", "bitrateKbps": 192}
})";
    }
    const auto result = dir / "wrapped.mkv";
    fs::remove(result);
    const auto log = dir / "wrapped.log";
    REQUIRE(run_cli("project " + quoted(project), log) == 0);
    INFO(read_bytes(log));
    const std::string bytes = read_bytes(result);
    REQUIRE(bytes.size() > 4);
    // The EBML header every Matroska file starts with.
    CHECK(static_cast<unsigned char>(bytes[0]) == 0x1A);
    CHECK(static_cast<unsigned char>(bytes[1]) == 0x45);
    CHECK(static_cast<unsigned char>(bytes[2]) == 0xDF);
    CHECK(static_cast<unsigned char>(bytes[3]) == 0xA3);
    // And no stream is left beside it: the one the container was made from was a scratch file.
    CHECK_FALSE(fs::exists(dir / "wrapped.ac3"));
}

TEST_CASE("forge project says what is wrong with a file that is not a project",
          "[cli][project]") {
    const auto dir = scratch_dir();
    const auto log = dir / "bad.log";
    const auto write = [&dir](const std::string& name, const std::string& text) {
        const auto file = dir / name;
        std::ofstream out{file, std::ios::binary | std::ios::trunc};
        out << text;
        return file;
    };
    // The input's fault, exit 2: not there, not a project, a newer version.
    CHECK(run_cli("project " + quoted(dir / "nowhere.forgeproj"), log) == 2);
    CHECK(read_bytes(log).find("could not be opened") != std::string::npos);
    CHECK(run_cli("project " + quoted(write("prose.forgeproj", "not json")), log) == 2);
    CHECK(read_bytes(log).find("not a JSON document") != std::string::npos);
    CHECK(run_cli("project " + quoted(write("other.forgeproj", R"({"a": 1})")), log) == 2);
    CHECK(read_bytes(log).find("does not say it is one") != std::string::npos);
    const auto newer =
        write("newer.forgeproj", R"({"format": "iclforge-project", "version": 99})");
    CHECK(run_cli("project " + quoted(newer), log) == 2);
    CHECK(read_bytes(log).find("newer Forge") != std::string::npos);
    // A project with nothing to encode is a mistake in the command, exit 1.
    const auto empty =
        write("empty.forgeproj", R"({"format": "iclforge-project", "version": 1})");
    CHECK(run_cli("project " + quoted(empty), log) == 1);
    CHECK(read_bytes(log).find("no sources") != std::string::npos);
    // A source that is not there is the input's.
    const auto gone = write("gone.forgeproj",
                            R"({"format": "iclforge-project", "version": 1,
                                "sources": [{"path": "missing.wav"}]})");
    CHECK(run_cli("project " + quoted(gone), log) == 2);
    CHECK(read_bytes(log).find("was not found") != std::string::npos);
}
