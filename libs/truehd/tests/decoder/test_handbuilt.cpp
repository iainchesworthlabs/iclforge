#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
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
// filter and its new_states, LSB bypass, several matrices with a permuted ch_assign and output
// shifts, a negative output shift, noise generators 0 and 1, a 16 bit MLP stream, the block data
// check, the end-of-stream marker, extra data, the directory's DRC word, and the 16-channel
// substream.
//
// The expected PCM is the signal each was built from. FFmpeg 6.1.1's strict decode of most of them
// gives the same signal (tools/checks/verify_truehd_interop.py --handbuilt holds it to the outcome
// the manifest's `ffmpeg` field names for each stream), so for those the layout is FFmpeg's and
// the tests tagged [ffmpeg-derived] carry values read from its decode. What FFmpeg does not read
// stays without an oracle: the extra data, the values of the directory's DRC word, the
// 16-channel substream (restart sync 0x31EC), the end-of-stream marker in every unit; those tests
// are tagged [no-external-oracle].

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

// The samples (interleaved, 24 bit) the library decodes a stereo hand-built stream to.
std::vector<std::int32_t> decode_stereo(const std::string& file) {
    const std::vector<std::byte> stream = read_file(dir() + "/" + file);
    Decoder decoder{DecoderConfig{}};
    ElementaryReader reader(stream);
    std::vector<std::int32_t> pcm;
    while (const auto unit = reader.next()) {
        auto decoded = decoder.decode(*unit);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        pcm.insert(pcm.end(), (*decoded)->pcm.begin(), (*decoded)->pcm.end());
    }
    return pcm;
}

// Samples first..first+N-1 of both channels against values read from FFmpeg 6.1.1's strict decode
// of the same stream (see the tests below for how that decode is made).
template <std::size_t N>
void check_stereo_against_ffmpeg(const std::string& file, std::size_t first,
                                 const std::array<std::array<std::int32_t, 2>, N>& ffmpeg) {
    const std::vector<std::int32_t> pcm = decode_stereo(file);
    REQUIRE(pcm.size() >= 2 * (first + N));
    for (std::size_t i = 0; i < N; ++i) {
        CAPTURE(file, first + i);
        CHECK(pcm[2 * (first + i)] == ffmpeg[i][0]);
        CHECK(pcm[2 * (first + i) + 1] == ffmpeg[i][1]);
    }
}

}  // namespace

TEST_CASE("every hand-built stream decodes to the signal it was built from",
          "[truehd][handbuilt]") {
    std::ifstream manifest_file(dir() + "/manifest.json");
    REQUIRE(manifest_file.good());
    const nlohmann::json manifest = nlohmann::json::parse(manifest_file);
    REQUIRE(manifest["fixtures"].size() >= 11);
    for (const nlohmann::json& fixture : manifest["fixtures"]) {
        const std::string name = fixture["name"];
        DYNAMIC_SECTION(name) {
            // The manifest names what FFmpeg's strict decode of the stream does; the python leg
            // holds FFmpeg to it. Here the library is held to the signal.
            const std::string outcome = fixture["ffmpeg"]["outcome"];
            CHECK((outcome == "equal" || outcome == "short" || outcome == "refused" ||
                   outcome == "differs"));
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

TEST_CASE("the filters' histories run on across a restart, as in FFmpeg's decode",
          "[truehd][handbuilt][ffmpeg-derived]") {
    // ERRATA entry 13, open question 18. hb_filters restarts every 16 access units (640 samples)
    // and filters from the first sample after the restart. The 16 values are FFmpeg 6.1.1's strict
    // decode of the stream (major sync format_info 0x0050A001, which FFmpeg needs and the
    // library does not read, CRC fixed up): samples 636..643 of both channels. A decoder that
    // zeroed the histories at the restart differs from sample 640.
    check_stereo_against_ffmpeg<8>("hb_filters.thd", 636,
                                   {{{-327458, -70839},
                                     {-235891, 12679},
                                     {-149264, 84069},
                                     {-66977, 143105},
                                     {9747, 189754},
                                     {79803, 224382},
                                     {144297, 248880},
                                     {201579, 262857}}});
}

TEST_CASE("noise generator 0 runs on a 16 bit shr7, as in FFmpeg's decode",
          "[truehd][handbuilt][ffmpeg-derived]") {
    // ERRATA entry 23, open question 5. The values are FFmpeg 6.1.1's strict decode of
    // hb_noise_generator_0 (major sync format_info 0x0050A001, CRC fixed up): samples 0..5, and
    // 638..643 across its first restart, where the seed loads again. The 32 bit shr7 of the
    // text's recurrence differs from the third noise sample on.
    check_stereo_against_ffmpeg<6>("hb_noise_generator_0.thd", 0,
                                   {{{-1036717, 901566},
                                     {-1096244, 751029},
                                     {-1127533, 597425},
                                     {-1131739, 441298},
                                     {-1108854, 285388},
                                     {-1061793, 129299}}});
    check_stereo_against_ffmpeg<6>("hb_noise_generator_0.thd", 638,
                                   {{{424084, -119223},
                                     {694647, -100374},
                                     {952588, -77833},
                                     {1190253, -50144},
                                     {1401569, -18659},
                                     {1579950, 16890}}});
}

TEST_CASE("hb_matrices carries the lossless check FFmpeg calculates for it",
          "[truehd][handbuilt][ffmpeg-derived]") {
    // ERRATA entries 12 and 42: the fold shifts by the matrix channel the output channel takes.
    // The stream's first writer shifted by the output position and stored 0x23 for its second
    // restart interval; FFmpeg's decode of it said "Lossless check failed - expected 23, calculated
    // ec". The stream carries 0xEC now, and tools/checks/verify_truehd_interop.py's FFmpeg leg
    // decodes it without that message.
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

TEST_CASE("the noise_type 1 streams are under restart sync 0x31EB and decode with noise and LSB bypass",
          "[truehd][handbuilt][ffmpeg-derived]") {
    // Restart sync 0x31EB, generator 1 (TH spec §17.3, §17.4, ERRATA.md, noise generator 1). The
    // first test of this file holds each of these streams to the signal it was built from, which
    // includes what its matrices add from the table; the Python writer computes that
    // independently of the library, and FFmpeg 6.1.1's strict decode returns the same signal for
    // each (tools/checks/verify_truehd_interop.py --handbuilt). Here: that each one is a
    // noise_type stream, that the ones with a matrix noise shift have matrices, and that LSB
    // bypass under 0x31EB is read (TH spec §10.2.1).
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

    std::ifstream manifest_file(dir() + "/manifest.json");
    const nlohmann::json manifest = nlohmann::json::parse(manifest_file);
    int shifted = 0;
    int bypassed = 0;
    for (const nlohmann::json& fixture : manifest["fixtures"]) {
        const auto& features = fixture["features"];
        if (std::find(features.begin(), features.end(), "noise_type_1") == features.end()) {
            continue;
        }
        const std::string name = fixture["name"];
        CAPTURE(name);
        const std::vector<std::byte> stream = read_file(dir() + "/" + std::string(fixture["file"]));
        CHECK(restart_of(stream).noise_type());
        Decoder decoder;
        ElementaryReader reader(stream);
        while (const auto unit = reader.next()) {
            auto decoded = decoder.decode(*unit);
            REQUIRE(decoded.has_value());
            REQUIRE(decoded->has_value());
        }
        CHECK(decoder.counters().lossless_check_failures == 0);
        CHECK(decoder.counters().lossless_checks_ok > 0);
        if (std::find(features.begin(), features.end(), "matrix_noise_shift") != features.end()) {
            CHECK(decoder.counters().matrix_headers > 0);
            ++shifted;
        }
        if (std::find(features.begin(), features.end(), "lsb_bypass") != features.end()) {
            CHECK(decoder.counters().lsb_bypass_matrices > 0);
            ++bypassed;
        }
    }
    CHECK(shifted >= 5);  // the stereo stream, the three rates, the LSB bypass stream
    CHECK(bypassed >= 1);
}

TEST_CASE("the end-of-stream marker removes its padding samples", "[truehd][handbuilt]") {
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
