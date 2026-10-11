#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "platform/process.hpp"

// The karaoke duet example, examples/karaoke/leave-her-johnny/, held to the real binary: `forge
// probe` says its committed stream is an Annex C karaoke stream at 3/2, `forge encode` of its
// committed parts with the line its README gives writes that stream again byte for byte, and so
// does `forge project` of its project, which says which file is which part and nothing of where the
// channels go. The parts, the words and the project are held to their generator by
// tools/checks/test_gen_karaoke_duet_example.py, which needs no build.
//
// The stream is a pin on the encoder, like testdata/bitstream-hashes.json: a change that moves
// the bytes the encoder writes for this input fails here until the stream is made again
//
//     python tools/generators/gen_karaoke_duet_example.py --forge <forge>
//
// and committed.
//
// ICLFORGE_CLI_EXE and ICLFORGE_KARAOKE_DUET_EXAMPLE_DIR come from
// apps/forge/cli/tests/CMakeLists.txt; run_cli is a trimmed copy of test_cli_probe.cpp's helper of
// the same name.

namespace fs = std::filesystem;

namespace {

fs::path scratch_dir() {
    auto dir = fs::path{ICLFORGE_TEST_SCRATCH_DIR} /
               ("cli_karaoke_duet_example_" + iclforge::test::platform::process_id());
    fs::create_directories(dir);
    return dir;
}

int run_cli(const std::string& args, const fs::path& log) {
    const std::string command =
        "\"" + std::string(ICLFORGE_CLI_EXE) + "\" " + args + " > \"" + log.string() + "\" 2>&1";
    return iclforge::test::platform::run_shell(command);
}

std::string read_text(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

fs::path example_dir() {
    return fs::path{ICLFORGE_KARAOKE_DUET_EXAMPLE_DIR};
}

std::string quoted(const fs::path& path) {
    return "\"" + path.generic_string() + "\"";
}

}  // namespace

TEST_CASE("the committed duet example probes as an Annex C karaoke stream at 3/2",
          "[cli][karaoke][example]") {
    const auto dir = scratch_dir();
    const auto log = dir / "probe.log";
    const auto stream = example_dir() / "leave-her-johnny.ac3";
    REQUIRE(fs::exists(stream));

    REQUIRE(run_cli("probe " + quoted(stream), log) == 0);
    const std::string report = read_text(log);
    INFO(report);
    CHECK(report.find("AC-3 (bsid 8)") != std::string::npos);
    CHECK(report.find("48000 Hz") != std::string::npos);
    CHECK(report.find("7 (karaoke)") != std::string::npos);
    CHECK(report.find("3/2 (acmod 7, lfeon 0)") != std::string::npos);
    CHECK(report.find("5 channel(s): L C R Ls Rs") != std::string::npos);
    CHECK(report.find("600 syncframe(s)") != std::string::npos);
    CHECK(report.find("600 of 600 syncframe(s) valid") != std::string::npos);
}

TEST_CASE("encoding the committed duet parts writes the committed stream byte for byte",
          "[cli][karaoke][example]") {
    const auto dir = scratch_dir();
    const auto log = dir / "encode.log";
    const auto out = dir / "leave-her-johnny.ac3";
    fs::remove(out);

    const auto parts = example_dir() / "parts";
    // The README's line: the backing is the primary source, the guide melody and the two
    // vocals follow, and each is mapped to the place Annex C's Table C.2.1 gives its part.
    const std::string args = "encode " + quoted(parts / "backing.wav") + " " + quoted(out) +
                             " 384 L,C,R,Ls,Rs src=" + quoted(parts / "melody-guide.wav") +
                             " src=" + quoted(parts / "vocal-1-shantyman.wav") +
                             " src=" + quoted(parts / "vocal-2-crew.wav") +
                             " map=0.0:L,0.1:R,1.0:C,2.0:Ls,3.0:Rs bsmod=voiceover";
    REQUIRE(run_cli(args, log) == 0);
    INFO(read_text(log));

    const std::string made = read_text(out);
    const std::string committed = read_text(example_dir() / "leave-her-johnny.ac3");
    REQUIRE_FALSE(made.empty());
    CHECK(made.size() == committed.size());
    CHECK(made == committed);
}

TEST_CASE("forge project of the duet's parts alone writes the committed stream",
          "[cli][karaoke][example][project]") {
    const auto dir = scratch_dir();
    const auto log = dir / "project.log";
    const auto out = dir / "from-project.ac3";
    fs::remove(out);

    // The project names the four parts and says nothing of the layout or the service: forge
    // lays them out where Annex C puts them and sets bsmod 7, which is the line above.
    REQUIRE(run_cli("project " + quoted(example_dir() / "leave-her-johnny.forgeproj") + " " +
                        quoted(out),
                    log) == 0);
    INFO(read_text(log));

    const std::string made = read_text(out);
    REQUIRE_FALSE(made.empty());
    CHECK(made == read_text(example_dir() / "leave-her-johnny.ac3"));
}
