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
}

TEST_CASE("project command: E-AC-3 objects from several sources are routed by name",
          "[project][command]") {
    const Folder folder;
    folder.touch("a.wav");
    folder.touch("b.wav");
    const auto doc = Json::parse(R"({
        "sources": [{"path": "a.wav"}, {"path": "b.wav", "offsetSeconds": 0.25}], "atmos": true,
        "assignments": [ {"source": 0, "channel": 0, "to": "obj"},
                         {"source": 1, "channel": 0, "to": "objm"} ],
        "objects": {"count": 2, "scene": {"objects": [{"name": "o"}]}},
        "encode": {"bitrateKbps": 448, "options": {"dialnorm": 26, "drc": "film-standard"}}
    })");
    const auto plan = project::command_plan(doc, request(folder));
    REQUIRE(plan.has_value());
    REQUIRE(plan->files.size() == 1);
    // [objects] is 0 where the map states the channels, and the loudness is all the metadata.
    CHECK(plan->steps[0].argv ==
          std::vector<std::string>{"atmos-encode", g(folder.path() / "a.wav"),
                                   g(folder.path() / "song.ec3"), "448", "0",
                                   g(plan->files[0].first), "dialnorm=26",
                                   "src=" + g(folder.path() / "b.wav"), "map=0.0:obj,1.0:objm",
                                   "offset=1:0.25"});
}

TEST_CASE("project command: AC-4 is ac4-encode with its own words, and MP4 is written by it",
          "[project][command]") {
    const Folder folder;
    folder.touch("a.wav");
    const auto doc = Json::parse(R"({
        "sources": [{"path": "a.wav"}],
        "encode": {"codec": "ac4", "container": "mp4", "bitrateKbps": 256,
                   "options": {"frame-rate": 24, "codec-mode": "simple", "dialnorm": 27.5,
                               "cmixlev": "+3", "dialogue-channels": "l,c",
                               "drc": "speech", "iframe-interval": 12} }
    })");
    const auto plan = project::command_plan(doc, request(folder));
    REQUIRE(plan.has_value());
    // One command that writes the MP4 file itself: there is no stream to wrap.
    REQUIRE(plan->steps.size() == 1);
    CHECK(plan->scratch_files.empty());
    CHECK(plan->output == folder.path() / "song.mp4");
    const auto& argv = plan->steps[0].argv;
    CHECK(argv[0] == "ac4-encode");
    CHECK(argv[2] == g(folder.path() / "song.mp4"));
    CHECK(argv[3] == "256");
    CHECK(std::vector<std::string>(argv.begin() + 4, argv.end()) ==
          std::vector<std::string>{"frame-rate=24", "codec-mode=simple", "dialnorm=27.5",
                                   "cmixlev=+3", "dialogue-channels=l,c", "drc=speech",
                                   "iframe-interval=12"});
}

TEST_CASE("project command: AC-4 objects are atmos-encode with codec=ac4, as the window's line is",
          "[project][command]") {
    const Folder folder;
    folder.touch("a.wav");
    folder.touch("b.wav");
    // One file: its channels are the objects, counted by [objects].
    const auto one = Json::parse(R"({
        "sources": [{"path": "a.wav"}], "atmos": true,
        "objects": {"count": 3, "scene": {"objects": [{"name": "o"}]}},
        "encode": {"codec": "ac4", "bitrateKbps": 384,
                   "options": {"coding": "direct", "dialnorm": 27, "crc": "off"}}
    })");
    auto plan = project::command_plan(one, request(folder));
    REQUIRE(plan.has_value());
    REQUIRE(plan->steps.size() == 1);
    REQUIRE(plan->files.size() == 1);
    CHECK(plan->steps[0].argv ==
          std::vector<std::string>{"atmos-encode", g(folder.path() / "a.wav"),
                                   g(folder.path() / "song.ac4"), "384", "3",
                                   g(plan->files[0].first), "codec=ac4", "coding=direct",
                                   "dialnorm=27", "crc=off"});

    // Several files, each channel placed by name: [objects] is 0, and src=, map= and the offsets
    // follow in the order the line writes them.
    const auto several = Json::parse(R"({
        "sources": [{"path": "a.wav"}, {"path": "b.wav", "offsetSeconds": 0.5}],
        "atmos": true, "bed": "3/2", "lfe": true,
        "assignments": [ {"source": 0, "channel": 0, "to": "obj"},
                         {"source": 1, "channel": 0, "to": "objm"} ],
        "objects": {"count": 2, "scene": {"objects": [{"name": "o"}]}},
        "encode": {"codec": "ac4", "container": "mp4", "bitrateKbps": 448}
    })");
    plan = project::command_plan(several, request(folder));
    REQUIRE(plan.has_value());
    REQUIRE(plan->steps.size() == 1);
    CHECK(plan->output == folder.path() / "song.mp4");
    CHECK(plan->steps[0].argv ==
          std::vector<std::string>{"atmos-encode", g(folder.path() / "a.wav"),
                                   g(folder.path() / "song.mp4"), "448", "0",
                                   g(plan->files[0].first), "src=" + g(folder.path() / "b.wav"),
                                   "map=0.0:obj,1.0:objm", "offset=1:0.5", "codec=ac4"});
}

TEST_CASE("project command: parts that are named are laid out where Annex C puts them",
          "[project][command]") {
    const Folder folder;
    for (const char* name : {"backing.wav", "melody.wav", "lead.wav", "harmony.wav"}) {
        folder.touch(name);
    }
    const auto run = [&](const char* sources, const char* encode = "{}") {
        const auto doc = Json::parse(std::string{R"({"sources": )"} + sources +
                                     R"(, "encode": )" + encode + "}");
        return project::command_plan(doc, request(folder));
    };
    const std::string base = g(folder.path());
    const auto tail = [](const std::vector<std::string>& argv, std::size_t from) {
        return std::vector<std::string>(argv.begin() + static_cast<std::ptrdiff_t>(from),
                                        argv.end());
    };

    // The whole set: 3/2, the melody in the centre and the vocals where the surrounds would be.
    auto plan = run(R"([{"path": "backing.wav", "part": "backing"},
                        {"path": "harmony.wav", "part": "vocal2"},
                        {"path": "melody.wav", "part": "melody"},
                        {"path": "lead.wav", "part": "vocal1"}])");
    REQUIRE(plan.has_value());
    auto argv = plan->steps[0].argv;
    CHECK(argv[4] == "L,C,R,Ls,Rs");
    // The files are numbered in the order the project lists them, whatever their parts.
    CHECK(tail(argv, 5) == std::vector<std::string>{"src=" + base + "/harmony.wav",
                                                    "src=" + base + "/melody.wav",
                                                    "src=" + base + "/lead.wav",
                                                    "map=0.0:L,0.1:R,1.0:Rs,2.0:C,3.0:Ls",
                                                    "bsmod=voiceover"});

    // One vocal and no melody guide: 2/1, the vocal in the one surround.
    plan = run(R"([{"path": "backing.wav", "part": "backing"},
                   {"path": "lead.wav", "part": "vocal1"}])");
    REQUIRE(plan.has_value());
    argv = plan->steps[0].argv;
    CHECK(argv[4] == "L,R,Cs");
    CHECK(argv.back() == "bsmod=voiceover");
    CHECK(argv[argv.size() - 2] == "map=0.0:L,0.1:R,1.0:Cs");

    // The backing and a melody guide alone: 3/0, for singing along.
    plan = run(R"([{"path": "backing.wav", "part": "backing"},
                   {"path": "melody.wav", "part": "melody"}])");
    REQUIRE(plan.has_value());
    CHECK(plan->steps[0].argv[4] == "L,C,R");

    // A service the project names is not overwritten.
    plan = run(R"([{"path": "backing.wav", "part": "backing"},
                   {"path": "melody.wav", "part": "melody"}])",
               R"({"options": {"bsmod": "commentary"}})");
    REQUIRE(plan.has_value());
    argv = plan->steps[0].argv;
    CHECK(std::count(argv.begin(), argv.end(), "bsmod=commentary") == 1);
    CHECK(std::count(argv.begin(), argv.end(), "bsmod=voiceover") == 0);

    // A project that says where the channels go is not laid out again.
    const auto stated = Json::parse(R"({
        "sources": [{"path": "backing.wav", "part": "backing"},
                    {"path": "melody.wav", "part": "melody"}],
        "bed": "3/0",
        "assignments": [ {"source": 0, "channel": 1, "to": "L"},
                         {"source": 0, "channel": 0, "to": "R"},
                         {"source": 1, "channel": 0, "to": "C"} ]
    })");
    plan = project::command_plan(stated, request(folder));
    REQUIRE(plan.has_value());
    argv = plan->steps[0].argv;
    CHECK(std::count(argv.begin(), argv.end(), "map=0.1:L,0.0:R,1.0:C") == 1);
    CHECK(std::count(argv.begin(), argv.end(), "bsmod=voiceover") == 0);
}

TEST_CASE("project command: parts that cannot make a karaoke stream say which", "[project][command]") {
    const Folder folder;
    for (const char* name : {"backing.wav", "melody.wav", "lead.wav", "harmony.wav"}) {
        folder.touch(name);
    }
    const auto fails = [&](const char* sources) {
        const auto plan = project::command_plan(
            Json::parse(std::string{R"({"sources": )"} + sources + "}"), request(folder));
        REQUIRE_FALSE(plan.has_value());
        return plan.error();
    };
    CHECK(fails(R"([{"path": "melody.wav", "part": "melody"},
                    {"path": "lead.wav", "part": "vocal1"}])")
              .code == "karaoke-no-backing");
    CHECK(fails(R"([{"path": "backing.wav", "part": "backing"},
                    {"path": "harmony.wav", "part": "vocal2"}])")
              .code == "karaoke-vocal2-needs-vocal1");
    CHECK(fails(R"([{"path": "backing.wav", "part": "backing"},
                    {"path": "melody.wav", "part": "backing"}])")
              .code == "karaoke-duplicate-part");
    CHECK(fails(R"([{"path": "backing.wav", "part": "backing"},
                    {"path": "melody.wav", "part": "bass"}])")
              .args.at(0) == "bass");
    // Only some of the sources naming a part is not a karaoke project: the routing is asked for.
    CHECK(fails(R"([{"path": "backing.wav", "part": "backing"},
                    {"path": "melody.wav"}])")
              .code == "no-assignments");
}

TEST_CASE("project command: a line is quoted where a person would have to", "[project][command]") {
    project::Step step;
    step.argv = {"encode", "my song.wav", "out.ac3", "", "src=a b.wav", "plain", "say \"hi\""};
    CHECK(project::command_text(step) ==
          "forge encode \"my song.wav\" out.ac3 \"\" \"src=a b.wav\" plain \"say \\\"hi\\\"\"");
}

TEST_CASE("project command: TrueHD is truehd-encode with its own two options, and a container wraps it",
          "[project][command][truehd]") {
    const Folder folder;
    folder.touch("a.wav");
    SECTION("the bare stream: one command, the window's bit rate left out") {
        const auto plan = project::command_plan(
            Json::parse(R"({"sources": [{"path": "a.wav"}],
                            "encode": {"codec": "truehd", "bitrateKbps": 448}})"),
            request(folder));
        REQUIRE(plan.has_value());
        REQUIRE(plan->steps.size() == 1);
        CHECK(plan->scratch_files.empty());
        CHECK(plan->output == folder.path() / "song.thd");
        CHECK(plan->steps[0].argv ==
              std::vector<std::string>{"truehd-encode", g(folder.path() / "a.wav"),
                                       g(folder.path() / "song.thd")});
    }
    SECTION("verify and the folded-down presentations are the command's words") {
        const auto plan = project::command_plan(
            Json::parse(R"({"sources": [{"path": "a.wav"}],
                            "encode": {"codec": "truehd",
                                       "truehd": {"verify": true, "downmix": "standard"}}})"),
            request(folder));
        REQUIRE(plan.has_value());
        const auto& argv = plan->steps[0].argv;
        CHECK(std::vector<std::string>(argv.begin() + 3, argv.end()) ==
              std::vector<std::string>{"truehd-downmix=standard", "verify"});
        // downmix "none" is the command's own default and is not written.
        const auto plain = project::command_plan(
            Json::parse(R"({"sources": [{"path": "a.wav"}],
                            "encode": {"codec": "truehd", "truehd": {"downmix": "none"}}})"),
            request(folder));
        REQUIRE(plain.has_value());
        CHECK(plain->steps[0].argv.size() == 3);
    }
    SECTION("a container is wrapped from the stream by its own command, as for any codec") {
        for (const auto& [container, command, suffix] :
             {std::tuple{"matroska", "mkv", ".mkv"}, std::tuple{"mp4", "mp4", ".mp4"},
              std::tuple{"mpegts", "ts", ".ts"}, std::tuple{"spdif", "spdif", ".wav"}}) {
            DYNAMIC_SECTION(container) {
                const auto plan = project::command_plan(
                    Json::parse(std::string(R"({"sources": [{"path": "a.wav"}],
                                    "encode": {"codec": "truehd", "container": ")") +
                                container + R"("}})"),
                    request(folder));
                REQUIRE(plan.has_value());
                REQUIRE(plan->steps.size() == 2);
                CHECK(plan->steps[0].argv[0] == "truehd-encode");
                CHECK(plan->steps[1].argv[0] == command);
                CHECK(plan->steps[1].argv[1] == plan->steps[0].argv[2]);
                CHECK(plan->output.extension() == suffix);
                CHECK(plan->scratch_files.size() == 1);
            }
        }
    }
    SECTION("what a lossless encode does not have is refused, not dropped") {
        const auto fails = [&](const char* text) {
            const auto plan = project::command_plan(Json::parse(text), request(folder));
            REQUIRE_FALSE(plan.has_value());
            return plan.error().code;
        };
        folder.touch("b.wav");
        CHECK(fails(R"({"sources": [{"path": "a.wav"}], "encode": {"codec": "truehd",
                         "container": "fmp4"}})") == "truehd-no-fmp4");
        CHECK(fails(R"({"sources": [{"path": "a.wav"}, {"path": "b.wav"}],
                         "encode": {"codec": "truehd"}})") == "truehd-one-source");
        CHECK(fails(R"({"sources": [{"path": "a.wav"}], "atmos": true,
                         "encode": {"codec": "truehd"}})") == "truehd-no-atmos");
        CHECK(fails(R"({"sources": [{"path": "a.wav"}],
                         "assignments": [{"source": 0, "channel": 0, "to": "L"}],
                         "encode": {"codec": "truehd"}})") == "truehd-no-assignments");
        CHECK(fails(R"({"sources": [{"path": "a.wav"}],
                         "encode": {"codec": "truehd", "options": {"dialnorm": 27}}})") ==
              "truehd-no-options");
    }
}
