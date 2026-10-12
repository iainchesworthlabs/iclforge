#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <string>
#include <vector>

#include "platform/process.hpp"

#include "iclforge/ac3/io/wav.hpp"

// `lyrics=` on the encoders (apps/forge/cli/src/commands/lyrics.cpp): a lyrics file in any format
// libs/lyrics reads, written beside the stream as its own UTF-8 `.lrc`, where Hearth finds it. The
// library's own contract is held in libs/lyrics/tests; what is checked here is the part only the
// binary has: the key is read before the encode, the exit codes tell a bad file from a bad
// command line, a file already there is left alone, and the words are written once the stream is.
//
// Same subprocess plumbing as test_cli_probe.cpp's run_cli, duplicated per this project's
// per-file test-helper convention.

namespace fs = std::filesystem;

namespace {

fs::path scratch_dir() {
    auto dir = fs::path{ICLFORGE_TEST_SCRATCH_DIR} /
               ("cli_lyrics_" + iclforge::test::platform::process_id());
    fs::create_directories(dir);
    return dir;
}

int run_cli(const std::string& args, const fs::path& log) {
    const std::string command =
        "\"" + std::string(ICLFORGE_CLI_EXE) + "\" " + args + " > \"" + log.string() + "\" 2>&1";
    return iclforge::test::platform::run_shell(command);
}

std::string read_file(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

fs::path write_file(const fs::path& path, const std::string& text) {
    std::ofstream out{path, std::ios::binary};
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(out.good());
    return path;
}

std::string quoted(const fs::path& path) { return "\"" + path.string() + "\""; }

// A short stereo WAV of two tones, which every encoder here takes.
fs::path make_wav(const std::string& name) {
    constexpr std::size_t kLength = 48000;
    std::vector<std::vector<float>> channels(2, std::vector<float>(kLength));
    for (std::size_t i = 0; i < kLength; ++i) {
        const double t = static_cast<double>(i) / 48000.0;
        channels[0][i] = static_cast<float>(0.1 * std::sin(2.0 * std::numbers::pi * 440.0 * t));
        channels[1][i] = static_cast<float>(0.1 * std::sin(2.0 * std::numbers::pi * 660.0 * t));
    }
    const auto path = scratch_dir() / name;
    REQUIRE(iclforge::ac3::io::write_wav_f32(path.string(), channels, 48000).has_value());
    return path;
}

const std::string kSubRip =
    "1\n00:00:01,000 --> 00:00:03,000\nShould auld acquaintance\n\n"
    "2\n00:00:05,000 --> 00:00:07,000\nBe forgot\n";

}  // namespace

TEST_CASE("encode lyrics= writes the stream's own UTF-8 .lrc from a SubRip file", "[cli][lyrics]") {
    const auto dir = scratch_dir() / "writes";
    fs::create_directories(dir);
    const auto wav = make_wav("lyrics_writes.wav");
    const auto words = write_file(dir / "words.srt", kSubRip);
    const auto out = dir / "song.ac3";
    const auto log = dir / "log.txt";

    REQUIRE(run_cli("encode " + quoted(wav) + " " + quoted(out) + " 192 lyrics=" + quoted(words),
                    log) == 0);
    const auto text = read_file(log);
    INFO(text);
    REQUIRE(fs::exists(out));
    const fs::path lrc = dir / "song.lrc";
    REQUIRE(fs::exists(lrc));
    // The cue's end where the silence begins is a time with no text, so that the gap reads as one.
    CHECK(read_file(lrc) == "[00:01.00]Should auld acquaintance\n[00:03.00]\n[00:05.00]Be forgot\n");
    CHECK(text.find("wrote") != std::string::npos);
    CHECK(text.find("song.lrc") != std::string::npos);
    CHECK_FALSE(fs::exists(dir / "song.lrc.part"));
}

TEST_CASE("encode lyrics= leaves a .lrc already beside the stream as it was", "[cli][lyrics]") {
    const auto dir = scratch_dir() / "exists";
    fs::create_directories(dir);
    const auto wav = make_wav("lyrics_exists.wav");
    const auto words = write_file(dir / "words.srt", kSubRip);
    const auto mine = write_file(dir / "song.lrc", "[00:00.00]Mine\n");
    const auto log = dir / "log.txt";

    REQUIRE(run_cli("encode " + quoted(wav) + " " + quoted(dir / "song.ac3") + " 192 lyrics=" +
                        quoted(words),
                    log) == 0);
    CHECK(read_file(mine) == "[00:00.00]Mine\n");
    const auto text = read_file(log);
    INFO(text);
    CHECK(text.find("left") != std::string::npos);
}

TEST_CASE("encode lyrics= refuses a file that is not lyrics before it encodes anything",
          "[cli][lyrics]") {
    const auto dir = scratch_dir() / "refuses";
    fs::create_directories(dir);
    const auto wav = make_wav("lyrics_refuses.wav");
    const auto log = dir / "log.txt";

    SECTION("a code page is named, with the input exit code, and no stream is written") {
        const auto latin1 = write_file(dir / "cafe.lrc", "[00:01.00]Caf\xE9\n");
        const auto out = dir / "cafe.ac3";
        CHECK(run_cli("encode " + quoted(wav) + " " + quoted(out) + " 192 lyrics=" + quoted(latin1),
                      log) == 2);
        const auto text = read_file(log);
        INFO(text);
        CHECK(text.find("not UTF-8") != std::string::npos);
        CHECK_FALSE(fs::exists(out));
        CHECK_FALSE(fs::exists(dir / "cafe.lrc.part"));
    }

    SECTION("a file that is not there is the input exit code too") {
        const auto out = dir / "missing.ac3";
        CHECK(run_cli("encode " + quoted(wav) + " " + quoted(out) + " 192 lyrics=" +
                          quoted(dir / "nothing.lrc"),
                      log) == 2);
        CHECK_FALSE(fs::exists(out));
    }

    SECTION("standard output has no name to put a file beside, which is a usage error") {
        const auto words = write_file(dir / "words.srt", kSubRip);
        CHECK(run_cli("encode " + quoted(wav) + " - 192 lyrics=" + quoted(words), log) == 1);
        const auto text = read_file(log);
        INFO(text);
        CHECK(text.find("no name to put one beside") != std::string::npos);
    }

    SECTION("the key belongs to the encoders: a decode refuses it") {
        const auto words = write_file(dir / "words2.srt", kSubRip);
        CHECK(run_cli("decode " + quoted(dir / "x.ac3") + " " + quoted(dir / "x.wav") +
                          " lyrics=" + quoted(words),
                      log) == 1);
        const auto text = read_file(log);
        INFO(text);
        CHECK(text.find("lyrics= goes with encode") != std::string::npos);
    }

    SECTION("lyrics= needs a path") {
        CHECK(run_cli("encode " + quoted(wav) + " " + quoted(dir / "e.ac3") + " 192 lyrics=", log) ==
              1);
    }
}

TEST_CASE("eac3-encode and ac4-encode take lyrics= too", "[cli][lyrics]") {
    const auto dir = scratch_dir() / "codecs";
    fs::create_directories(dir);
    const auto wav = make_wav("lyrics_codecs.wav");
    const auto words = write_file(
        dir / "words.lrc", "[00:01.00]<00:01.00>Should <00:01.50>auld\n[00:03.00]Be forgot\n");
    const auto log = dir / "log.txt";

    REQUIRE(run_cli("eac3-encode " + quoted(wav) + " " + quoted(dir / "plus.ec3") + " 192 lyrics=" +
                        quoted(words),
                    log) == 0);
    CHECK(read_file(dir / "plus.lrc") ==
          "[00:01.00]<00:01.00>Should <00:01.50>auld\n[00:03.00]Be forgot\n");

    REQUIRE(run_cli("ac4-encode " + quoted(wav) + " " + quoted(dir / "next.ac4") + " 128 lyrics=" +
                        quoted(words),
                    log) == 0);
    CHECK(fs::exists(dir / "next.lrc"));
}
