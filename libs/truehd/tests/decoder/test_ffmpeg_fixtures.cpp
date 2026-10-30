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

#include "iclforge/base/crypto/sha256.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/elementary.hpp"

// The interoperability proof (planning/truehd.md, "Oracles"): streams FFmpeg's encoders wrote
// (tools/generators/gen_truehd_ffmpeg.py), decoded here, against the SHA-256 of what FFmpeg's
// strict decode of the same file gives. The codec is lossless, so the digests are equal or the
// decode is wrong.

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

std::string hex(const std::array<std::byte, 32>& digest) {
    constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (const std::byte b : digest) {
        out.push_back(kDigits[std::to_integer<unsigned>(b) >> 4U]);
        out.push_back(kDigits[std::to_integer<unsigned>(b) & 15U]);
    }
    return out;
}

nlohmann::json manifest() {
    std::ifstream in(std::string(ICLFORGE_GOLDEN_TRUEHD_DIR) + "/manifest.json");
    REQUIRE(in.good());
    return nlohmann::json::parse(in);
}

// What FFmpeg's `-f s32le` writes for 24-bit samples: each shifted left by 8, little-endian.
void append_s32le(std::vector<std::byte>& out, std::span<const std::int32_t> pcm) {
    for (const std::int32_t sample : pcm) {
        const auto word = static_cast<std::uint32_t>(sample) << 8U;
        for (unsigned shift = 0; shift < 32; shift += 8) {
            out.push_back(static_cast<std::byte>((word >> shift) & 0xFFU));
        }
    }
}

// The access units of a stream, each its own copy so that one can be damaged or left out.
std::vector<std::vector<std::byte>> units_of(const std::vector<std::byte>& stream) {
    std::vector<std::vector<std::byte>> units;
    ElementaryReader reader(stream);
    while (const auto unit = reader.next()) {
        units.emplace_back(unit->begin(), unit->end());
    }
    REQUIRE(reader.skipped_bytes() == 0);
    return units;
}

// The samples each unit of an undamaged stream decodes to.
std::vector<std::vector<std::int32_t>> decode_each(
    const std::vector<std::vector<std::byte>>& units) {
    Decoder decoder;
    std::vector<std::vector<std::int32_t>> pcm;
    for (const auto& unit : units) {
        auto decoded = decoder.decode(unit);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        pcm.emplace_back((*decoded)->pcm.begin(), (*decoded)->pcm.end());
    }
    return pcm;
}

bool same_samples(const DecodedAccessUnit& unit, const std::vector<std::int32_t>& expected) {
    return std::ranges::equal(unit.pcm, expected);
}

// truehd_st48_s24.thd is 300 units of stereo with a restart header, and the major sync with it,
// every 16 units: units 0, 16, 32, ...
constexpr std::size_t kDamaged = 20;
constexpr std::size_t kNextRestart = 32;

}  // namespace

TEST_CASE("every FFmpeg fixture decodes to the samples of FFmpeg's strict decode",
          "[truehd][interop]") {
    const nlohmann::json fixtures = manifest()["fixtures"];
    REQUIRE(fixtures.size() >= 12);
    for (const nlohmann::json& fixture : fixtures) {
        const std::string name = fixture["name"];
        DYNAMIC_SECTION(name) {
            const std::vector<std::byte> stream = read_file(
                std::string(ICLFORGE_GOLDEN_TRUEHD_DIR) + "/" + std::string(fixture["file"]));
            CHECK(hex(iclforge::base::crypto::sha256(stream)) ==
                  fixture["stream_sha256"].get<std::string>());

            auto info = probe_stream(stream);
            REQUIRE(info.has_value());
            CHECK(info->sample_rate == fixture["sample_rate"].get<std::uint32_t>());
            CHECK(info->access_units == fixture["access_units"].get<std::size_t>());
            CHECK(info->major_sync.kind == (fixture["codec"].get<std::string>() == "truehd"
                                                ? StreamKind::kTrueHd
                                                : StreamKind::kMlp));

            Decoder decoder;
            ElementaryReader reader(stream);
            std::vector<std::byte> pcm;
            std::size_t samples = 0;
            int channels = 0;
            int padding = 0;
            while (const auto unit = reader.next()) {
                auto decoded = decoder.decode(*unit);
                REQUIRE(decoded.has_value());
                REQUIRE(decoded->has_value());
                channels = (*decoded)->channels;
                samples += static_cast<std::size_t>((*decoded)->samples);
                padding += (*decoded)->padding_samples;
                append_s32le(pcm, (*decoded)->pcm);
            }
            CHECK(channels == fixture["channels"].get<int>());
            CHECK(samples == fixture["pcm_samples"].get<std::size_t>());
            // The end-of-stream marker of the last unit (ERRATA 14) is what makes up the difference
            // from whole units.
            CHECK(samples + static_cast<std::size_t>(padding) ==
                  fixture["access_units"].get<std::size_t>() *
                      static_cast<std::size_t>(info->samples_per_access_unit));
            CHECK(hex(iclforge::base::crypto::sha256(pcm)) ==
                  fixture["pcm_sha256"].get<std::string>());

            const DecoderCounters& counters = decoder.counters();
            CHECK(counters.units == fixture["access_units"].get<std::uint64_t>());
            CHECK(counters.units_decoded == counters.units);
            CHECK(counters.check_failures == 0);
            CHECK(counters.lossless_check_failures == 0);
            CHECK(counters.lossless_checks_ok > 0);
            CHECK(reader.skipped_bytes() == 0);
        }
    }
}

TEST_CASE("a 5.1 TrueHD fixture decodes its stereo presentation from substream 0",
          "[truehd][interop]") {
    const std::vector<std::byte> stream =
        read_file(std::string(ICLFORGE_GOLDEN_TRUEHD_DIR) + "/truehd_m51_s24.thd");
    DecoderConfig config;
    config.presentation = Presentation::k2ch;
    Decoder stereo(config);
    Decoder full;
    ElementaryReader first(stream);
    ElementaryReader second(stream);
    int units = 0;
    while (const auto unit = first.next()) {
        const auto both = second.next();
        REQUIRE(both.has_value());
        auto two = stereo.decode(*unit);
        auto six = full.decode(*both);
        REQUIRE(two.has_value());
        REQUIRE(six.has_value());
        REQUIRE(two->has_value());
        REQUIRE(six->has_value());
        CHECK((*two)->channels == 2);
        CHECK((*two)->substream == 0);
        CHECK((*six)->channels == 6);
        CHECK((*six)->substream == 1);
        CHECK((*two)->samples == (*six)->samples);
        ++units;
    }
    CHECK(units == 300);
}

TEST_CASE("lenient decoding of a clean fixture counts nothing", "[truehd][interop]") {
    const std::vector<std::byte> stream =
        read_file(std::string(ICLFORGE_GOLDEN_TRUEHD_DIR) + "/mlp_m51_s24.mlp");
    DecoderConfig config;
    config.strictness = Strictness::kLenient;
    Decoder decoder(config);
    ElementaryReader reader(stream);
    while (const auto unit = reader.next()) {
        auto decoded = decoder.decode(*unit);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
    }
    CHECK(decoder.counters().check_failures == 0);
    CHECK(decoder.counters().lossless_check_failures == 0);
}

TEST_CASE("a unit that does not read leaves the decoder waiting for the next restart",
          "[truehd][decoder][resync]") {
    // ERRATA-free behaviour, documented on Strictness: the unit is an error, the units after it
    // give no samples and no error, because the state they would continue from is gone, and the
    // decode is exact again at the next restart header.
    const auto units =
        units_of(read_file(std::string(ICLFORGE_GOLDEN_TRUEHD_DIR) + "/truehd_st48_s24.thd"));
    REQUIRE(units.size() == 300);
    const auto clean = decode_each(units);

    auto damaged = units;
    damaged[kDamaged][3] ^= std::byte{0x01};  // input_timing: the header's check nibble is wrong
    Decoder decoder;
    for (std::size_t i = 0; i < damaged.size(); ++i) {
        CAPTURE(i);
        auto result = decoder.decode(damaged[i]);
        if (i == kDamaged) {
            CHECK_FALSE(result.has_value());
            continue;
        }
        REQUIRE(result.has_value());
        if (i > kDamaged && i < kNextRestart) {
            CHECK_FALSE(result->has_value());
        } else {
            REQUIRE(result->has_value());
            CHECK(same_samples(**result, clean[i]));
        }
    }
    CHECK(decoder.counters().units_waiting == kNextRestart - kDamaged - 1);
}

TEST_CASE("a caller that dropped a unit says so and the decoder waits for the next restart",
          "[truehd][decoder][resync]") {
    const auto units =
        units_of(read_file(std::string(ICLFORGE_GOLDEN_TRUEHD_DIR) + "/truehd_st48_s24.thd"));
    const auto clean = decode_each(units);

    DecoderConfig config;
    config.strictness = Strictness::kLenient;
    Decoder decoder(config);
    for (std::size_t i = 0; i < units.size(); ++i) {
        CAPTURE(i);
        if (i == kDamaged) {
            decoder.lost_data();  // the unit never reaches the decoder
            continue;
        }
        auto result = decoder.decode(units[i]);
        REQUIRE(result.has_value());
        if (i > kDamaged && i < kNextRestart) {
            CHECK_FALSE(result->has_value());
        } else {
            REQUIRE(result->has_value());
            CHECK(same_samples(**result, clean[i]));
        }
    }
    CHECK(decoder.counters().units_waiting == kNextRestart - kDamaged - 1);
    CHECK(decoder.counters().lossless_check_failures == 0);
    REQUIRE(decoder.major_sync().has_value());  // lost_data() keeps the stream, reset() would not
}

TEST_CASE("the lossless check of a stream's last restart interval is never made",
          "[truehd][decoder][resync]") {
    // A restart header carries the check of the interval before it, and none follows the last
    // interval: of the 19 intervals of the fixture, 18 are checked (documented on Strictness).
    const auto units =
        units_of(read_file(std::string(ICLFORGE_GOLDEN_TRUEHD_DIR) + "/truehd_st48_s24.thd"));
    Decoder decoder;
    for (const auto& unit : units) {
        REQUIRE(decoder.decode(unit).has_value());
    }
    CHECK(decoder.counters().restarts == 19);
    CHECK(decoder.counters().lossless_checks_ok == 18);
    CHECK(decoder.counters().lossless_check_failures == 0);
}
