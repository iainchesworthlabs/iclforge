#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "iclforge/containers/iamf/container.hpp"
#include "iclforge/containers/iamf/iamf.hpp"
#include "iclforge/containers/iamf/model.hpp"
#include "iclforge/containers/iamf/sequence.hpp"

// The seeds of fuzz_iamf_parse (libs/containers/fuzz/seeds/fuzz_iamf_parse/, made by
// iclforge-iamf-seed-tool through tools/fuzz/generate-seeds.sh) are the one input the harness
// starts from. A seed its reader refuses is a corpus that spends its first minutes on rejections,
// which is what the harness had before they existed, so each is held to being read by the reader of
// its own framing and to decoding to PCM.

namespace iamf = iclforge::containers::iamf;
namespace fs = std::filesystem;

namespace {

[[nodiscard]] std::vector<std::byte> read_all(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> chars((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(chars.size());
    std::ranges::transform(chars, bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}

[[nodiscard]] std::vector<fs::path> seeds_with(const std::string& extension) {
    std::vector<fs::path> found;
    for (const auto& entry : fs::directory_iterator(ICLFORGE_IAMF_SEED_DIR)) {
        if (entry.is_regular_file() && entry.path().extension() == extension) {
            found.push_back(entry.path());
        }
    }
    std::ranges::sort(found);
    return found;
}

}  // namespace

TEST_CASE("fuzz_iamf_parse seeds: each OBU stream reads and decodes", "[iamf][seeds]") {
    const auto seeds = seeds_with(".obu");
    REQUIRE(seeds.size() >= 6);
    for (const auto& path : seeds) {
        INFO(path.filename().string());
        const auto bytes = read_all(path);
        auto sequence = iamf::read_sequence(bytes);
        REQUIRE(sequence.has_value());
        CHECK_FALSE(sequence->temporal_units.empty());
        REQUIRE_FALSE(sequence->audio_elements.empty());
        // Opus, AAC-LC and FLAC substreams are carried, not decoded (decode_pcm is for ipcm).
        if (path.filename().string().starts_with("coded-")) {
            continue;
        }
        for (const auto& element : sequence->audio_elements) {
            auto decoded = iamf::decode_pcm(*sequence, element.audio_element_id);
            REQUIRE(decoded.has_value());
            CHECK_FALSE(decoded->channels.empty());
        }
    }
}

TEST_CASE("fuzz_iamf_parse seeds: each ISO-BMFF file reads and decodes", "[iamf][seeds]") {
    const auto seeds = seeds_with(".mp4");
    REQUIRE(seeds.size() >= 7);
    bool fragmented = false;
    for (const auto& path : seeds) {
        INFO(path.filename().string());
        const auto bytes = read_all(path);
        auto file = iamf::read_isobmff(bytes);
        REQUIRE(file.has_value());
        CHECK_FALSE(file->sequence.temporal_units.empty());
        fragmented = fragmented || file->info.fragmented;
        auto tracks = iamf::read_isobmff_tracks(bytes);
        REQUIRE(tracks.has_value());
        CHECK(tracks->size() == 1);
        if (path.filename().string().starts_with("coded-")) {
            continue;
        }
        for (const auto& element : file->sequence.audio_elements) {
            auto decoded = iamf::decode_pcm(file->sequence, element.audio_element_id);
            REQUIRE(decoded.has_value());
            CHECK_FALSE(decoded->channels.empty());
        }
    }
    // The movie-fragment walk is a branch of the reader only a fragmented seed reaches.
    CHECK(fragmented);
}

TEST_CASE("fuzz_iamf_parse seeds: one reader refuses the other framing", "[iamf][seeds]") {
    // A seed of one framing is an input the other reader rejects at its first check; that is why
    // each programme is written both ways.
    const auto obu = read_all(seeds_with(".obu").front());
    CHECK_FALSE(iamf::read_isobmff(obu).has_value());
    const auto mp4 = read_all(seeds_with(".mp4").front());
    CHECK_FALSE(iamf::read_sequence(mp4).has_value());
}
