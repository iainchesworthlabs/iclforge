#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "platform/process.hpp"

#include "project_command.hpp"

// apps/shared/project/src/project_command.cpp: the commands a project stands for. They are what
// `forge project` runs and what the Forge GUI's command bar shows for the same draft, so the cases
// are about the line: each codec's, the container's second step, the words a number is written in,
// and what a project that cannot be run says.

namespace {

namespace fs = std::filesystem;
namespace project = iclforge::apps::project;
using Json = project::Json;

class Folder {
public:
    Folder()
        : path_(fs::path{ICLFORGE_TEST_SCRATCH_DIR} /
                ("project_command_" + iclforge::test::platform::process_id())) {
        fs::remove_all(path_);
        fs::create_directories(path_);
    }
    ~Folder() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    Folder(const Folder&) = delete;
    Folder& operator=(const Folder&) = delete;

    fs::path touch(const std::string& name) const {
        const fs::path file = path_ / name;
        fs::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::binary);
        out << "x";
        return file;
    }
    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

project::Request request(const Folder& folder, std::optional<fs::path> out = std::nullopt) {
    project::Request r;
    r.project_file = folder.path() / "song.forgeproj";
    r.output = std::move(out);
    r.scratch = folder.path() / "scratch";
    return r;
}

std::string g(const fs::path& path) {
    return path.generic_string();
}

}  // namespace

TEST_CASE("project command: the karaoke example is the line its README gives",
          "[project][command]") {
    const fs::path example{ICLFORGE_KARAOKE_EXAMPLE_DIR};
    const auto opened = project::read_project(example / "auld-lang-syne.forgeproj");
    REQUIRE(opened.has_value());
    project::Request r;
    r.project_file = example / "auld-lang-syne.forgeproj";
    r.output = fs::path{"out.ac3"};
    r.scratch = fs::temp_directory_path();
    const auto plan = project::command_plan(opened->project, r);
    REQUIRE(plan.has_value());
    REQUIRE(plan->steps.size() == 1);
    const auto& argv = plan->steps[0].argv;
    const std::string parts = g((example / "parts").lexically_normal());
    const std::vector<std::string> expected{
        "encode",
        parts + "/backing.wav",
        "out.ac3",
        "384",
        "L,C,R,Ls,Rs",
        "src=" + parts + "/melody-guide.wav",
        "src=" + parts + "/vocal-1-lead.wav",
        "src=" + parts + "/vocal-2-harmony.wav",
        "map=0.0:L,0.1:R,1.0:C,2.0:Ls,3.0:Rs",
        "bsmod=voiceover",
    };
    CHECK(argv == expected);
    CHECK(plan->output == fs::path{"out.ac3"});
    CHECK(plan->scratch_files.empty());
}

TEST_CASE("project command: the result is named for the project, beside it, by default",
          "[project][command]") {
    const Folder folder;
    folder.touch("a.wav");
    const auto doc = Json::parse(R"({"sources": [{"path": "a.wav"}]})");
    const auto plan = project::command_plan(doc, request(folder));
    REQUIRE(plan.has_value());
    CHECK(plan->output == folder.path() / "song.ac3");
    CHECK(plan->steps[0].argv[0] == "encode");
    CHECK(plan->steps[0].argv[3] == "192");
    // No layout stated: the command takes it from the source, as it does for a typed line.
    CHECK(plan->steps[0].argv[4].empty());
}

TEST_CASE("project command: E-AC-3 takes its tools, layout and VBR, and a container wraps it",
          "[project][command]") {
    const Folder folder;
    folder.touch("a.wav");
    const auto doc = Json::parse(R"({
        "sources": [{"path": "a.wav"}],
        "bed": "3/2", "lfe": true, "extras": ["wide", "topf"],
        "encode": { "codec": "eac3", "container": "matroska", "bitrateKbps": 448,
                    "tools": "cpl+spx", "vbr": "q:0.8,max:640",
                    "options": { "dialnorm": 27, "drc": "film-standard", "heavy": true,
                                 "cmixlev": -3.0, "surmixlev": "off", "copyright": false } }
    })");
    const auto plan = project::command_plan(doc, request(folder));
    REQUIRE(plan.has_value());
    REQUIRE(plan->steps.size() == 2);
    const auto& first = plan->steps[0].argv;
    CHECK(first[0] == "eac3-encode");
    CHECK(first[3] == "448");
    CHECK(first[4] == "cpl+spx");
    CHECK(first[5] == "L,C,R,Ls,Rs,Lw,Rw,Vhl,Vhr,LFE");
    CHECK(first[6] == "q:0.8,max:640");
    // The options as the command line spells them: a whole number without its point, a flag as
    // its bare word, and a flag that is off not at all.
    const std::vector<std::string> options(first.begin() + 7, first.end());
    CHECK(options == std::vector<std::string>{"dialnorm=27", "drc=film-standard", "heavy",
                                              "cmixlev=-3", "surmixlev=off"});
    // The stream goes beside the container's own file, under the scratch folder.
    CHECK(first[2] == g(folder.path() / "scratch" / "song.ec3"));
    CHECK(plan->steps[1].argv ==
          std::vector<std::string>{"mkv", first[2], g(folder.path() / "song.mkv")});
    CHECK(plan->output == folder.path() / "song.mkv");
    CHECK(plan->scratch_files.size() == 1);
}

TEST_CASE("project command: AC-3 takes coupling as the bare word it is", "[project][command]") {
    const Folder folder;
    folder.touch("a.wav");
    const auto doc = Json::parse(R"({"sources": [{"path": "a.wav"}],
        "encode": {"codec": "ac3", "tools": "cpl", "bitrateKbps": 384}})");
    const auto plan = project::command_plan(doc, request(folder));
    REQUIRE(plan.has_value());
    const auto& argv = plan->steps[0].argv;
    CHECK(argv[0] == "encode");
    CHECK(std::find(argv.begin(), argv.end(), "couple") != argv.end());
}

TEST_CASE("project command: an offset, and a map written as the command line writes it",
          "[project][command]") {
    const Folder folder;
    folder.touch("a.wav");
    folder.touch("b.wav");
    const auto doc = Json::parse(R"({
        "sources": [{"path": "a.wav"}, {"path": "b.wav", "offsetSeconds": 0.25}],
        "bed": "3/0",
        "assignments": [
          {"source": 0, "channel": 0, "to": "L"}, {"source": 0, "channel": 1, "to": "R"},
          {"source": 1, "channel": 0, "to": "C@-3.5"} ]
    })");
    const auto plan = project::command_plan(doc, request(folder));
    REQUIRE(plan.has_value());
    const auto& argv = plan->steps[0].argv;
    const std::vector<std::string> trailing(argv.begin() + 5, argv.end());
    CHECK(trailing == std::vector<std::string>{"src=" + g(folder.path() / "b.wav"),
                                               "map=0.0:L,0.1:R,1.0:C@-3.5", "offset=1:0.25"});
    CHECK(argv[4] == "L,C,R");
}

TEST_CASE("project command: a session of objects is atmos-encode with its scene beside it",
          "[project][command]") {
    const Folder folder;
    folder.touch("a.wav");
    const auto doc = Json::parse(R"({
        "sources": [{"path": "a.wav"}], "atmos": true,
        "objects": {"count": 2, "scene": {"objects": [{"name": "o"}]}},
        "encode": {"bitrateKbps": 448,
                   "options": {"dialnorm": "auto", "drc": "film-standard"}}
    })");
    const auto plan = project::command_plan(doc, request(folder));
    REQUIRE(plan.has_value());
    const auto& argv = plan->steps[0].argv;
    CHECK(argv[0] == "atmos-encode");
    CHECK(argv[3] == "448");
    CHECK(argv[4] == "2");
    REQUIRE(plan->files.size() == 1);
    CHECK(argv[5] == g(plan->files[0].first));
    // Object mode takes the loudness and nothing else of the metadata.
    CHECK(argv.back() == "dialnorm=auto");
    CHECK(argv.size() == 7);
    CHECK(plan->files[0].second.find("\"objects\"") != std::string::npos);
    CHECK(argv[2].find(".ec3") != std::string::npos);
}

TEST_CASE("project command: what cannot be run says why", "[project][command]") {
    const Folder folder;
    folder.touch("a.wav");
    folder.touch("b.wav");
    const auto fails = [&](const char* text) {
        const auto plan = project::command_plan(Json::parse(text), request(folder));
        REQUIRE_FALSE(plan.has_value());
        return plan.error();
    };
    CHECK(fails(R"({})").code == "no-sources");
    CHECK(fails(R"({"sources": [{"path": "gone.wav"}]})").code == "source-missing");
    CHECK(fails(R"({"sources": [{"path": "a.wav"}], "encode": {"codec": "mp3"}})").code ==
          "unknown-codec");
    CHECK(fails(R"({"sources": [{"path": "a.wav"}], "encode": {"container": "avi"}})").code ==
          "unknown-container");
    CHECK(fails(R"({"sources": [{"path": "a.wav"}], "bed": "9/9"})").code == "unknown-bed");
    CHECK(fails(R"({"sources": [{"path": "a.wav"}], "assignments": [{"source": 0}]})").code ==
          "bad-assignment");
    const auto several = fails(R"({"sources": [{"path": "a.wav"}, {"path": "b.wav"}]})");
    CHECK(several.code == "no-assignments");
    CHECK(several.args.at(0) == "2");
    CHECK(fails(R"({"sources": [{"path": "a.wav"}], "atmos": true,
                    "encode": {"codec": "ac4"}})")
              .code == "unsupported");
    CHECK(fails(R"({"sources": [{"path": "a.wav"}, {"path": "b.wav"}], "atmos": true})").code ==
          "unsupported");
}

TEST_CASE("project command: a line is quoted where a person would have to", "[project][command]") {
    project::Step step;
    step.argv = {"encode", "my song.wav", "out.ac3", "", "src=a b.wav", "plain", "say \"hi\""};
    CHECK(project::command_text(step) ==
          "forge encode \"my song.wav\" out.ac3 \"\" \"src=a b.wav\" plain \"say \\\"hi\\\"\"");
}
