#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "iclforge/ac3/decoder/decoder.hpp"
#include "iclforge/pitch/reference.hpp"
#include "reference_melody.hpp"
#include "sanitized.hpp"

// apps/hearth/engine/src/reference_melody.*: the melody a singer is scored against, taken from the
// karaoke stream's own channels (planning/karaoke-microphone.md, D9). The stream is the
// repository's own example (examples/karaoke/auld-lang-syne, made by
// tools/generators/gen_karaoke_example.py from arithmetic): a guide melody on a clarinet-like tone,
// a lead vocal on the same tune, and a second vocal a diatonic third below it.

namespace {

namespace fs = std::filesystem;

using iclforge::hearth::ReferencePart;

std::vector<std::byte> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    std::transform(raw.begin(), raw.end(), bytes.begin(),
                   [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}

fs::path example() {
    return fs::path{ICLFORGE_KARAOKE_EXAMPLE_DIR} / "auld-lang-syne.ac3";
}

// The cases that read a part's first notes take the stream's first ten seconds under the
// sanitizers (tests/support/sanitized.hpp), in whole syncframes: the eleventh note, the "mind?"
// that ends the second line, is over at eight. A normal build takes the whole stream.
std::vector<std::byte> notes_of(const std::vector<std::byte>& stream) {
    if (!iclforge::test::kSanitized) {
        return stream;
    }
    const auto frames = iclforge::ac3::split_frames(stream);
    REQUIRE(frames.has_value());
    constexpr std::size_t kFrames = 10 * 48000 / 1536;
    REQUIRE(frames->size() > kFrames);
    const std::byte* end = (*frames)[kFrames - 1].data() + (*frames)[kFrames - 1].size();
    return {stream.data(), end};
}

fs::path repo_root() {
    return fs::path{ICLFORGE_KARAOKE_EXAMPLE_DIR}.parent_path().parent_path().parent_path();
}

// The notes of a contour as rounded MIDI numbers with each run of the same note collapsed to one: a
// tune's repeated pitch across syllables is one sustained note to a tracker, or two with a gap.
std::vector<int> tune_of(const iclforge::pitch::Contour& contour) {
    std::vector<int> out;
    for (const auto& note : iclforge::pitch::segment_notes(contour)) {
        const int midi = static_cast<int>(std::lround(note.midi));
        if (out.empty() || out.back() != midi) {
            out.push_back(midi);
        }
    }
    return out;
}

// Auld Lang Syne as the generator writes it, repeated notes collapsed: line 1, then line 2's start.
const std::vector<int> kTuneStart = {60, 65, 69, 67, 65, 67, 69, 65, 69, 72, 74};
// The harmony, a diatonic third below in F major.
const std::vector<int> kHarmonyStart = {57, 62, 65, 64, 62, 64, 65, 62, 65, 69, 70};

}  // namespace

TEST_CASE("reference melody: the guide melody of a karaoke stream is its tune",
          "[hearth][reference]") {
    const auto result = iclforge::hearth::reference_from_karaoke(read_file(example()));
    REQUIRE(result.has_value());
    CHECK(result->part == ReferencePart::kMelodyGuide);
    CHECK(result->description == "the melody guide (M)");
    CHECK(result->contour.hop_ms == 10);
    CHECK(result->contour.duration_ms() > 16000);
    CHECK(result->voiced_share > 0.5);

    const auto tune = tune_of(result->contour);
    REQUIRE(tune.size() >= kTuneStart.size());
    for (std::size_t i = 0; i < kTuneStart.size(); ++i) {
        INFO("note " << i);
        CHECK(tune[i] == kTuneStart[i]);
    }
}

TEST_CASE("reference melody: vocal 1 is the same tune, and vocal 2 the harmony",
          "[hearth][reference]") {
    const auto bytes = notes_of(read_file(example()));
    {
        const auto result = iclforge::hearth::reference_from_karaoke(bytes, ReferencePart::kVocal1);
        REQUIRE(result.has_value());
        CHECK(result->part == ReferencePart::kVocal1);
        CHECK(result->description == "vocal 1 (V1)");
        const auto tune = tune_of(result->contour);
        REQUIRE(tune.size() >= kTuneStart.size());
        for (std::size_t i = 0; i < kTuneStart.size(); ++i) {
            INFO("V1 note " << i);
            CHECK(tune[i] == kTuneStart[i]);
        }
    }
    {
        const auto result = iclforge::hearth::reference_from_karaoke(bytes, ReferencePart::kVocal2);
        REQUIRE(result.has_value());
        CHECK(result->part == ReferencePart::kVocal2);
        const auto tune = tune_of(result->contour);
        REQUIRE(tune.size() >= kHarmonyStart.size());
        for (std::size_t i = 0; i < kHarmonyStart.size(); ++i) {
            INFO("V2 note " << i);
            CHECK(tune[i] == kHarmonyStart[i]);
        }
    }
}

TEST_CASE("reference melody: a stream that is not karaoke has none, and says so",
          "[hearth][reference]") {
    const auto result = iclforge::hearth::reference_from_karaoke(
        read_file(repo_root() / "firmware" / "hearth-sink" / "www" / "ac3-20.ac3"));
    REQUIRE(!result.has_value());
    CHECK(result.error().find("not a karaoke stream") != std::string::npos);
}

TEST_CASE("reference melody: nothing, or something that is not AC-3, is an error not a crash",
          "[hearth][reference]") {
    CHECK(!iclforge::hearth::reference_from_karaoke({}).has_value());
    const std::vector<std::byte> junk(4096, std::byte{0x5A});
    CHECK(!iclforge::hearth::reference_from_karaoke(junk).has_value());
}

TEST_CASE("reference melody: a cancel is honoured", "[hearth][reference]") {
    const std::atomic<bool> cancel{true};
    const auto result =
        iclforge::hearth::reference_from_karaoke(read_file(example()), std::nullopt, &cancel);
    REQUIRE(!result.has_value());
    CHECK(result.error().find("cancelled") != std::string::npos);
}
