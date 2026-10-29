#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "iclforge/base/bitreader.hpp"
#include "iclforge/base/crypto/sha256.hpp"
#include "iclforge/truehd/core/syntax.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/access_unit.hpp"
#include "iclforge/truehd/io/elementary.hpp"

// The hand-built streams of testdata/truehd/handbuilt (tools/references/truehd_writer.py): an IIR
// filter, LSB bypass, several matrices with a permuted ch_assign and an output shift, the block
// data check, the end-of-stream marker, extra data, the directory's DRC word, and the 16-channel
// substream.
//
// NONE OF THESE HAS AN EXTERNAL ORACLE. No decoder but this one and
// tools/references/truehd_syntax.py reads them. The expected PCM is the signal each was built from,
// so a decode that differs is wrong, but the layout they follow is ERRATA.md's, and a misreading of
// the specification would be in the writer and in both decoders alike.

namespace {

using namespace iclforge::truehd;

std::vector<std::byte> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

std::string hex(std::span<const std::byte> bytes) {
    constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (const std::byte b : bytes) {
        out.push_back(kDigits[std::to_integer<unsigned>(b) >> 4U]);
        out.push_back(kDigits[std::to_integer<unsigned>(b) & 15U]);
    }
    return out;
}

std::string dir() {
    return std::string(ICLFORGE_GOLDEN_TRUEHD_DIR) + "/handbuilt";
}

Presentation presentation_of(const std::string& name) {
    return name == "all" ? Presentation::kAll : Presentation::kBest;
}

}  // namespace

TEST_CASE("every hand-built stream decodes to the signal it was built from",
          "[truehd][handbuilt][no-external-oracle]") {
    std::ifstream manifest_file(dir() + "/manifest.json");
    REQUIRE(manifest_file.good());
    const nlohmann::json manifest = nlohmann::json::parse(manifest_file);
    REQUIRE(manifest["fixtures"].size() >= 11);
    for (const nlohmann::json& fixture : manifest["fixtures"]) {
        const std::string name = fixture["name"];
        DYNAMIC_SECTION(name) {
            CHECK(fixture["oracle"] == "none");
            const std::vector<std::byte> stream =
                read_file(dir() + "/" + std::string(fixture["file"]));
            CHECK(hex(iclforge::base::crypto::sha256(stream)) ==
                  fixture["stream_sha256"].get<std::string>());

            DecoderConfig config;
            config.presentation = presentation_of(fixture["presentation"]);
            Decoder decoder(config);
            ElementaryReader reader(stream);
            std::vector<std::byte> pcm;
            std::size_t samples = 0;
            int channels = 0;
            while (const auto unit = reader.next()) {
                auto decoded = decoder.decode(*unit);
                REQUIRE(decoded.has_value());
                REQUIRE(decoded->has_value());
                channels = (*decoded)->channels;
                samples += static_cast<std::size_t>((*decoded)->samples);
                for (const std::int32_t sample : (*decoded)->pcm) {
                    const auto word = static_cast<std::uint32_t>(sample) << 8U;
                    for (unsigned shift = 0; shift < 32; shift += 8) {
                        pcm.push_back(static_cast<std::byte>((word >> shift) & 0xFFU));
                    }
                }
            }
            CHECK(channels == fixture["channels"].get<int>());
            CHECK(samples == fixture["pcm_samples"].get<std::size_t>());
            CHECK(hex(iclforge::base::crypto::sha256(pcm)) ==
                  fixture["pcm_sha256"].get<std::string>());
            CHECK(decoder.counters().units == fixture["access_units"].get<std::uint64_t>());
            CHECK(decoder.counters().check_failures == 0);
            CHECK(decoder.counters().lossless_check_failures == 0);
            CHECK(decoder.counters().lossless_checks_ok > 0);
            CHECK(reader.skipped_bytes() == 0);
        }
    }
}

TEST_CASE("hb_matrices carries the lossless check FFmpeg calculates for it",
          "[truehd][handbuilt][ffmpeg-derived]") {
    // ERRATA entry 12: the fold shifts by ch_assign[output channel]. The stream's first writer
    // shifted by the output position and stored 0x23 for its second restart interval; FFmpeg's
    // decode of it said "Lossless check failed - expected 23, calculated ec". The stream carries
    // 0xEC now, and tools/checks/verify_truehd_interop.py's FFmpeg leg decodes it without that
    // message.
    const std::vector<std::byte> stream = read_file(dir() + "/hb_matrices.thd");
    ElementaryReader reader(stream);
    std::vector<int> checks;
    while (const auto unit = reader.next()) {
        auto parsed = parse_access_unit(*unit, 1);
        REQUIRE(parsed.has_value());
        iclforge::BitReader bits(parsed->segments[0]);
        if (bits.read(1) == 1 && bits.read(1) == 1) {  // block_header_exists, restart_header_exists
            auto restart = read_restart_header(bits, parsed->segments[0], true);
            REQUIRE(restart.has_value());
            checks.push_back(restart->lossless_check);
        }
    }
    REQUIRE(checks.size() >= 2);
    CHECK(checks[0] == 0);  // the first header's value checks nothing
    CHECK(checks[1] == 0xEC);
}

TEST_CASE("noise_type 1 decodes when no matrix has a noise shift and is refused when one has",
          "[truehd][handbuilt][no-external-oracle]") {
    // Restart sync 0x31EB, generator 1 (TH spec 17.3): its table is not available, so a matrix with
    // a non-zero noise shift is kUnsupported (ERRATA entry 32). With every shift zero nothing
    // draws on it and the stream decodes: the fixture hb_noise_type1 is in the manifest's list.
    const auto restart_of = [](const std::vector<std::byte>& stream) {
        ElementaryReader reader(stream);
        const auto unit = reader.next();
        REQUIRE(unit.has_value());
        auto parsed = parse_access_unit(*unit, 0);
        REQUIRE(parsed.has_value());
        iclforge::BitReader bits(parsed->segments[0]);
        REQUIRE(bits.read(1) == 1);  // block_header_exists
        REQUIRE(bits.read(1) == 1);  // restart_header_exists
        auto restart = read_restart_header(bits, parsed->segments[0], true);
        REQUIRE(restart.has_value());
        return *restart;
    };

    const std::vector<std::byte> plain = read_file(dir() + "/hb_noise_type1.thd");
    CHECK(restart_of(plain).noise_type());

    std::ifstream manifest_file(dir() + "/manifest.json");
    const nlohmann::json manifest = nlohmann::json::parse(manifest_file);
    REQUIRE(manifest["refused"].size() >= 1);
    for (const nlohmann::json& fixture : manifest["refused"]) {
        DYNAMIC_SECTION(std::string(fixture["name"])) {
            const std::vector<std::byte> stream =
                read_file(dir() + "/" + std::string(fixture["file"]));
            CHECK(hex(iclforge::base::crypto::sha256(stream)) ==
                  fixture["stream_sha256"].get<std::string>());
            CHECK(restart_of(stream).noise_type());
            ElementaryReader reader(stream);
            Decoder decoder;
            const auto unit = reader.next();
            REQUIRE(unit.has_value());
            auto refused = decoder.decode(*unit);
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().kind == ErrorKind::kUnsupported);
        }
    }
}

TEST_CASE("the end-of-stream marker removes its padding samples",
          "[truehd][handbuilt][no-external-oracle]") {
    const std::vector<std::byte> stream = read_file(dir() + "/hb_marker_last.thd");
    Decoder decoder;
    ElementaryReader reader(stream);
    int padding = 0;
    int units = 0;
    while (const auto unit = reader.next()) {
        auto decoded = decoder.decode(*unit);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        padding += (*decoded)->padding_samples;
        ++units;
    }
    CHECK(units == 20);
    CHECK(padding == 17);
}

TEST_CASE("extra data comes back with the unit it was in",
          "[truehd][handbuilt][no-external-oracle]") {
    const std::vector<std::byte> stream = read_file(dir() + "/hb_extra_data.thd");
    auto info = probe_stream(stream);
    REQUIRE(info.has_value());
    ElementaryReader reader(stream);
    Decoder decoder;
    const auto unit = reader.next();
    REQUIRE(unit.has_value());
    auto parsed = parse_access_unit(*unit, 0);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->extra_data.has_value());
    CHECK(parsed->extra_data->check_ok);
    CHECK(parsed->extra_data->framing_ok);
    CHECK(parsed->extra_data->parity_ok);

    std::ifstream manifest_file(dir() + "/manifest.json");
    const nlohmann::json manifest = nlohmann::json::parse(manifest_file);
    for (const nlohmann::json& fixture : manifest["fixtures"]) {
        if (fixture["name"] == "hb_extra_data") {
            CHECK(hex(parsed->extra_data->evolution_payload) ==
                  fixture["extra_data_first_hex"].get<std::string>());
        }
    }
    auto decoded = decoder.decode(*unit);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->has_value());
    CHECK((*decoded)->extra_data.size() == parsed->extra_data->bytes.size());
}

TEST_CASE("the directory's DRC word is read and does not touch the samples",
          "[truehd][handbuilt][no-external-oracle]") {
    const std::vector<std::byte> stream = read_file(dir() + "/hb_drc_word.thd");
    ElementaryReader reader(stream);
    int index = 0;
    while (const auto unit = reader.next()) {
        auto parsed = parse_access_unit(*unit, 1);
        REQUIRE(parsed.has_value());
        CHECK(parsed->entries[0].extra_substream_word);
        CHECK(parsed->entries[0].drc_gain_update == -100 + index);
        CHECK(parsed->entries[0].drc_time_update == index % 8);
        ++index;
    }
    CHECK(index == 20);
}

TEST_CASE("the 16-channel stream has an object substream and four directory entries",
          "[truehd][handbuilt][no-external-oracle]") {
    const std::vector<std::byte> stream = read_file(dir() + "/hb_sixteen_channels.thd");
    auto info = probe_stream(stream);
    REQUIRE(info.has_value());
    CHECK(info->num_substreams == 4);
    CHECK(info->has_object_substream);
    CHECK(info->channels[0] == 2);
    CHECK(info->channels[1] == 6);
    CHECK(info->channels[2] == 8);
    CHECK(info->channels[3] == 16);
}
