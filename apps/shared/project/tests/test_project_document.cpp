#include <catch2/catch_test_macros.hpp>

#include <clocale>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "platform/process.hpp"

#include "project_document.hpp"

// apps/shared/project/src/project_document.cpp: reading a project, the refusals and what each
// says, writing it back with what it did not know, and a path kept relative.

namespace {

namespace fs = std::filesystem;
namespace project = iclforge::apps::project;

class Folder {
public:
    Folder()
        : path_(fs::path{ICLFORGE_TEST_SCRATCH_DIR} /
                ("project_document_" + iclforge::test::platform::process_id())) {
        fs::remove_all(path_);
        fs::create_directories(path_);
    }
    ~Folder() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    Folder(const Folder&) = delete;
    Folder& operator=(const Folder&) = delete;

    fs::path write(const std::string& name, const std::string& text) const {
        const fs::path file = path_ / name;
        std::ofstream out(file, std::ios::binary);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        REQUIRE(out.good());
        return file;
    }
    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

std::string read_all(const fs::path& file) {
    std::ifstream in{file, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

}  // namespace

TEST_CASE("project: a project is read whole, with what this version does not know",
          "[project][document]") {
    const Folder folder;
    const auto file = folder.write("a.forgeproj", R"({
        "format": "iclforge-project", "version": 1, "title": "mine",
        "sources": [ { "path": "x.wav", "part": "backing", "note": "kept" } ],
        "encode": { "codec": "ac3", "tomorrow": 7 },
        "future": { "x": 1 }
    })");
    const auto opened = project::read_project(file);
    REQUIRE(opened.has_value());
    CHECK(opened->warnings.empty());
    const auto& doc = opened->project;
    CHECK(doc["title"] == "mine");
    CHECK(doc["encode"]["tomorrow"] == 7);
    CHECK(doc["future"]["x"] == 1);
    CHECK(doc["sources"][0]["note"] == "kept");
    // In the order the file wrote them.
    std::vector<std::string> keys;
    for (auto it = doc.begin(); it != doc.end(); ++it) {
        keys.push_back(it.key());
    }
    CHECK(keys == std::vector<std::string>{"format", "version", "title", "sources", "encode",
                                           "future"});
}

TEST_CASE("project: a file that is not a project is refused with a reason", "[project][document]") {
    const Folder folder;
    struct Case {
        const char* text;
        const char* code;
    };
    const Case cases[] = {
        {"not json at all", "not-json"},
        {"[1, 2]", "not-json"},
        {R"({"hello": 1})", "not-a-project"},
        {R"({"format": "something-else", "version": 1})", "not-a-project"},
        {R"({"format": "iclforge-project"})", "no-version"},
        {R"({"format": "iclforge-project", "version": "1"})", "no-version"},
        {R"({"format": "iclforge-project", "version": 0})", "no-version"},
        {R"({"format": "iclforge-project", "version": 1.5})", "no-version"},
        {R"({"format": "iclforge-project", "version": 2})", "newer-version"},
    };
    for (const auto& c : cases) {
        INFO(c.text);
        const auto file = folder.write("case.forgeproj", c.text);
        const auto opened = project::read_project(file);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error().code == c.code);
        CHECK(opened.error().args.at(0) == "case.forgeproj");
    }
    const auto newer = project::read_project(
        folder.write("n.forgeproj", R"({"format": "iclforge-project", "version": 7})"));
    REQUIRE_FALSE(newer.has_value());
    CHECK(newer.error().args.size() == 3);
    CHECK(newer.error().args[1] == "7");
    CHECK(newer.error().args[2] == "1");
}

TEST_CASE("project: a file that is not there is named", "[project][document]") {
    const Folder folder;
    const auto opened = project::read_project(folder.path() / "gone.forgeproj");
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().code == "cannot-open");
    CHECK(opened.error().args.at(0) == "gone.forgeproj");
}

TEST_CASE("project: a source with no path is set aside and the rest is read",
          "[project][document]") {
    const Folder folder;
    const auto file = folder.write("s.forgeproj", R"({
        "format": "iclforge-project", "version": 1,
        "sources": [ { "path": "a.wav" }, { "part": "melody" }, { "path": "" } ]
    })");
    const auto opened = project::read_project(file);
    REQUIRE(opened.has_value());
    REQUIRE(opened->warnings.size() == 2);
    CHECK(opened->warnings[0].code == "source-no-path");
    CHECK(opened->warnings[0].args.at(0) == "2");
    CHECK(opened->warnings[1].args.at(0) == "3");
}

TEST_CASE("project: writing stamps the format and puts it first, and reads back the same",
          "[project][document]") {
    const Folder folder;
    project::Json doc = project::Json::object();
    doc["encode"]["codec"] = "ac3";
    doc["title"] = "t";
    doc["zzz"] = 1;
    const auto file = folder.path() / "out.forgeproj";
    REQUIRE(project::write_project(file, doc).has_value());
    const std::string text = read_all(file);
    CHECK(text.rfind("{\n  \"format\": \"iclforge-project\",\n  \"version\": 1,\n  \"title\"", 0) ==
          0);
    CHECK(text.find("\"zzz\"") > text.find("\"encode\""));
    CHECK_FALSE(fs::exists(fs::path{file} += ".tmp"));
    const auto back = project::read_project(file);
    REQUIRE(back.has_value());
    CHECK(back->project["encode"]["codec"] == "ac3");
}

TEST_CASE("project: writing over a project replaces it whole", "[project][document]") {
    const Folder folder;
    const auto file = folder.write("p.forgeproj", "old contents");
    project::Json doc = project::Json::object();
    doc["title"] = "new";
    REQUIRE(project::write_project(file, doc).has_value());
    CHECK(read_all(file).find("old contents") == std::string::npos);
    CHECK(project::read_project(file).has_value());
}

TEST_CASE("project: a project that cannot be written says which", "[project][document]") {
    const Folder folder;
    const auto result = project::write_project(folder.path() / "no" / "such" / "p.forgeproj",
                                               project::Json::object());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == "cannot-write");
    CHECK(result.error().args.at(0) == "p.forgeproj");
}

TEST_CASE("project: a path is kept relative to the project's folder, with a slash",
          "[project][document]") {
    const Folder folder;
    const auto parts = folder.path() / "parts";
    CHECK(project::stored_path(folder.path(), parts / "backing.wav") == "parts/backing.wav");
    CHECK(project::stored_path(folder.path(), folder.path() / "a.wav") == "a.wav");
    CHECK(project::stored_path(parts, folder.path() / "a.wav") == "../a.wav");
}

TEST_CASE("project: a stored path is made absolute from the folder and cleaned",
          "[project][document]") {
    const Folder folder;
    const auto absolute = project::resolved_path(folder.path(), "parts/../parts/backing.wav");
    CHECK(absolute.is_absolute());
    CHECK(absolute.filename().string() == "backing.wav");
    CHECK(absolute.parent_path().filename().string() == "parts");
    const auto given = project::resolved_path(folder.path(), "../x.wav");
    CHECK(given.filename().string() == "x.wav");
    // An absolute path stays itself.
    const auto here = (folder.path() / "y.wav").lexically_normal();
    CHECK(project::resolved_path(folder.path() / "elsewhere", here.generic_string()) == here);
}

TEST_CASE("project: a document read and written back is the same document", "[project][document]") {
    const Folder folder;
    // Two-space indentation, format and version first, an object's members in the order they were
    // written (zeta before alpha, y before x), and text that is not ASCII as it was.
    const std::string text = std::string{R"({
  "format": "iclforge-project",
  "version": 1,
  "title": "caf)"} + "\xC3\xA9" + R"( \"q\"",
  "sources": [
    {
      "path": "a.wav",
      "offsetSeconds": 0.25,
      "zeta": true,
      "alpha": null
    }
  ],
  "encode": {
    "bitrateKbps": 384,
    "options": {}
  },
  "zzz": [],
  "aaa": {
    "y": 2,
    "x": 1
  }
}
)";
    const auto file = folder.write("rt.forgeproj", text);
    const auto opened = project::read_project(file);
    REQUIRE(opened.has_value());
    CHECK(project::project_text(opened->project) == text);
}

TEST_CASE("project: the top-level keys are in reading order and the rest keep theirs",
          "[project][document]") {
    project::Json doc = project::Json::object();
    doc["karaoke"] = project::Json::object();
    doc["zzz"] = 1;
    doc["encode"] = project::Json::object();
    doc["aaa"] = 2;
    doc["title"] = "t";
    const std::string text = project::project_text(doc);
    const auto at = [&text](const char* key) { return text.find(std::string{"\""} + key + "\""); };
    CHECK(at("format") < at("version"));
    CHECK(at("version") < at("title"));
    CHECK(at("title") < at("encode"));
    CHECK(at("encode") < at("karaoke"));
    CHECK(at("karaoke") < at("zzz"));
    CHECK(at("zzz") < at("aaa"));
}

TEST_CASE("project: a number does not depend on the process's locale", "[project][document]") {
    // A locale whose decimal point is a comma, where one is installed: strtod would read 0.5 as 0
    // there. Where none is, the case is the same one with the C locale, which still holds.
    const std::string before = std::setlocale(LC_NUMERIC, nullptr);
    for (const char* name : {"de_DE.UTF-8", "de_DE.utf8", "German_Germany.1252", "fr_FR.UTF-8"}) {
        if (std::setlocale(LC_NUMERIC, name) != nullptr) {
            break;
        }
    }
    const Folder folder;
    const auto file = folder.write(
        "n.forgeproj",
        R"({"format": "iclforge-project", "version": 1, "sources": [{"path": "a.wav", "offsetSeconds": 0.5}]})");
    const auto opened = project::read_project(file);
    project::Json doc = project::Json::object();
    doc["offset"] = 0.25;
    const std::string text = project::project_text(doc);
    std::setlocale(LC_NUMERIC, before.c_str());
    REQUIRE(opened.has_value());
    CHECK(opened->project["sources"][0]["offsetSeconds"].get<double>() == 0.5);
    CHECK(text.find("\"offset\": 0.25") != std::string::npos);
}

TEST_CASE("project: a member is read as what it is or not at all", "[project][document]") {
    const auto doc = project::Json::parse(
        R"({"s": "text", "n": 384, "f": 0.5, "w": 4.0, "b": true, "nothing": null})");
    CHECK(project::string_of(doc, "s") == "text");
    CHECK(project::string_of(doc, "n", "fallback") == "fallback");
    CHECK(project::string_of(doc, "missing", "fallback") == "fallback");
    CHECK(project::bool_of(doc, "b"));
    CHECK_FALSE(project::bool_of(doc, "s", false));
    CHECK(project::int_of(doc["n"]) == 384);
    CHECK(project::int_of(doc["w"]) == 4);
    CHECK_FALSE(project::int_of(doc["f"]).has_value());
    CHECK_FALSE(project::int_of(doc["s"]).has_value());
    CHECK(project::number_of(doc, "f") == 0.5);
    CHECK(project::number_of(doc, "s", -1.0) == -1.0);
    CHECK(project::member(doc, "n") != nullptr);
    CHECK(project::member(doc, "nope") == nullptr);
    CHECK(project::member(doc["s"], "x") == nullptr);
}
