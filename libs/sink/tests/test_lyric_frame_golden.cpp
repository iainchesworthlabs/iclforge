#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/lyrics/lyrics.hpp"
#include "iclforge/lyrics/stage.hpp"
#include "iclforge/sink/lyric_frame.hpp"

// The conformance fixtures of planning/hearth-sink-screen.md, framed at fixed positions and held to
// the checked-in frames (lyric-frames/golden.txt): the karaoke examples, a file timed as lines, a
// plain one, lines of 18, 36, 44, 54 and 55 characters, a gap of 3.9 s and of 4.0 s, Hebrew and
// Yiddish in UTF-8 and UTF-16, Arabic, Japanese, Cyrillic and Greek. The Android app's instrumented
// test reads the same files and compares the frames its own build of this library gives, so what a
// renderer is tested with and what the library says are one thing.

namespace {

namespace fs = std::filesystem;
namespace lyr = iclforge::lyrics;

std::optional<fs::path> directory_of(const char* variable, const char* compiled) {
    if (const char* from_environment = std::getenv(variable); from_environment != nullptr && *from_environment != '\0') {
        return fs::path(from_environment);
    }
    if (compiled != nullptr && fs::exists(compiled)) {
        return fs::path(compiled);
    }
    return std::nullopt;
}

struct Case {
    std::string name;
    std::string document;
    std::int64_t delay_ms = 0;
    std::vector<std::int64_t> positions;
};

std::string trimmed(std::string text) {
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

std::vector<Case> read_manifest(const fs::path& path) {
    std::ifstream in(path);
    std::vector<Case> cases;
    std::string line;
    while (std::getline(in, line)) {
        if (trimmed(line).empty() || trimmed(line).front() == '#') {
            continue;
        }
        std::vector<std::string> fields;
        std::stringstream parts(line);
        std::string field;
        while (std::getline(parts, field, '|')) {
            fields.push_back(trimmed(field));
        }
        REQUIRE(fields.size() == 4);
        Case entry;
        entry.name = fields[0];
        entry.document = fields[1];
        entry.delay_ms = std::stoll(fields[2]);
        std::stringstream positions(fields[3]);
        while (std::getline(positions, field, ',')) {
            entry.positions.push_back(std::stoll(trimmed(field)));
        }
        cases.push_back(std::move(entry));
    }
    return cases;
}

std::string read_all(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// The frame as the app asks for it: item 1, a stream playing, the screen's rows.
std::string frame_json(const std::string& document, std::int64_t position_ms, std::int64_t delay_ms) {
    auto parsed = lyr::parse_lyrics(document);
    REQUIRE(parsed.has_value());
    const lyr::Stage stage =
        lyr::make_stage(&*parsed, position_ms, delay_ms, iclforge::sink::kScreenStageOptions);
    return iclforge::sink::lyric_frame_json(
        iclforge::sink::make_lyric_frame(std::int64_t{1}, &*parsed, stage, true));
}

}  // namespace

TEST_CASE("lyric frames: the conformance fixtures at fixed positions are the checked-in frames",
          "[sink][lyrics][golden]") {
    const std::optional<fs::path> frames = directory_of("ICLFORGE_LYRIC_FRAMES_DIR", ICLFORGE_LYRIC_FRAMES_DIR);
    const std::optional<fs::path> root = directory_of("ICLFORGE_REPO_ROOT", ICLFORGE_REPO_ROOT);
    if (!frames || !root) {
        SKIP("the fixture directories are not here (set ICLFORGE_LYRIC_FRAMES_DIR and ICLFORGE_REPO_ROOT)");
    }

    std::vector<std::string> lines;
    for (const Case& entry : read_manifest(*frames / "cases.txt")) {
        const fs::path beside = *frames / entry.document;
        const fs::path document = fs::exists(beside) ? beside : *root / entry.document;
        INFO("case " << entry.name << ": " << document.string());
        REQUIRE(fs::exists(document));
        const std::string bytes = read_all(document);
        for (const std::int64_t position : entry.positions) {
            lines.push_back(entry.name + "\t" + std::to_string(position) + "\t" + std::to_string(entry.delay_ms) +
                            "\t" + frame_json(bytes, position, entry.delay_ms));
        }
    }
    REQUIRE_FALSE(lines.empty());

    const fs::path golden = *frames / "golden.txt";
    if (const char* update = std::getenv("ICLFORGE_UPDATE_LYRIC_FRAMES"); update != nullptr && *update != '\0') {
        std::ofstream out(golden, std::ios::binary | std::ios::trunc);
        for (const std::string& line : lines) {
            out << line << "\n";
        }
        SUCCEED("golden.txt rewritten: " << lines.size() << " frames");
        return;
    }

    std::vector<std::string> expected;
    {
        std::ifstream in(golden, std::ios::binary);
        REQUIRE(in.good());
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            expected.push_back(line);
        }
    }
    REQUIRE(expected.size() == lines.size());
    for (std::size_t i = 0; i < lines.size(); ++i) {
        INFO("frame " << i << ": " << lines[i].substr(0, lines[i].find('\t', lines[i].find('\t') + 1)));
        CHECK(lines[i] == expected[i]);
    }
}

TEST_CASE("lyric frames: a gap of 3.9 s is a breath and one of 4.0 s is an instrumental break with a count-in",
          "[sink][lyrics][golden]") {
    const std::optional<fs::path> frames = directory_of("ICLFORGE_LYRIC_FRAMES_DIR", ICLFORGE_LYRIC_FRAMES_DIR);
    if (!frames) {
        SKIP("the fixture directory is not here (set ICLFORGE_LYRIC_FRAMES_DIR)");
    }
    auto parsed = lyr::parse_lyrics(read_all(*frames / "gaps.lrc"));
    REQUIRE(parsed.has_value());
    // The marker at 2.5 s ends the first line; the next begins at 6.4 s: 3.9 s.
    const lyr::Stage short_gap = lyr::make_stage(&*parsed, 4000, 0, iclforge::sink::kScreenStageOptions);
    CHECK(short_gap.mode == lyr::StageMode::kGap);
    CHECK(short_gap.gap_ms == 3900);
    CHECK_FALSE(short_gap.instrumental);
    CHECK(short_gap.count_in == 0);
    // The marker at 9.0 s; the next line begins at 13.0 s: 4.0 s.
    const lyr::Stage long_gap = lyr::make_stage(&*parsed, 11000, 0, iclforge::sink::kScreenStageOptions);
    CHECK(long_gap.mode == lyr::StageMode::kGap);
    CHECK(long_gap.gap_ms == 4000);
    CHECK(long_gap.instrumental);
    CHECK(long_gap.count_in >= 1);
}
