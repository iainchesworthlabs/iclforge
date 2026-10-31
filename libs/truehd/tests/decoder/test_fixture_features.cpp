#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/base/crypto/sha256.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/elementary.hpp"

// What each of the later FFmpeg fixtures in testdata/truehd was made for
// (tools/generators/gen_truehd_ffmpeg.py, the `covers` field of the manifest). The generic test in
// test_ffmpeg_fixtures.cpp holds every fixture to FFmpeg's decode; these hold the decode to the
// feature, so that a fixture that stopped carrying it, or a decoder that reads it as something
// else and happens to give the same samples, fails here. tools/checks/check_truehd_coverage.py is
// the same question asked of the streams alone.

namespace {

using namespace iclforge::truehd;

std::vector<std::byte> read_file(const std::string& name) {
    std::ifstream in(std::string(ICLFORGE_GOLDEN_TRUEHD_DIR) + "/" + name, std::ios::binary);
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

std::string layout_text(std::span<const Channel> layout) {
    std::string out;
    for (const Channel channel : layout) {
        out += out.empty() ? "" : " ";
        out += channel_name(channel);
    }
    return out;
}

// What a whole decode of a fixture shows.
struct Seen {
    std::string layout;
    int channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint64_t samples = 0;
    std::uint32_t low_bits_set = 0;  // OR of every sample: the bits any sample uses
    DecoderCounters counters{};
    int samples_per_access_unit = 0;
};

Seen decode_fixture(const std::string& file) {
    const std::vector<std::byte> stream = read_file(file);
    const auto info = probe_stream(stream);
    REQUIRE(info.has_value());
    Seen seen;
    seen.samples_per_access_unit = info->samples_per_access_unit;
    Decoder decoder;
    ElementaryReader reader(stream);
    while (const auto unit = reader.next()) {
        const auto decoded = decoder.decode(*unit);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        const DecodedAccessUnit& au = **decoded;
        seen.layout = layout_text(au.layout);
        seen.channels = au.channels;
        seen.sample_rate = au.sample_rate;
        seen.samples += static_cast<std::uint64_t>(au.samples);
        for (const std::int32_t sample : au.pcm) {
            seen.low_bits_set |= static_cast<std::uint32_t>(sample);
        }
    }
    seen.counters = decoder.counters();
    return seen;
}

struct Expectation {
    std::string_view file;
    std::string_view channel_names;  // empty for MLP, whose assignment names no speakers
    int channels;
    std::uint32_t sample_rate;
    int samples_per_access_unit;
    std::uint64_t restart_headers;
    std::uint32_t clear_low_bits;  // the low bits of every sample that are zero
};

// restart_headers: the substreams times ceil(access units / the gap between major syncs).
constexpr std::array<Expectation, 14> kExpectations{{
    {"truehd_st88_s24.thd", "L R", 2, 88200, 80, 6, 0},
    {"truehd_st176_s24.thd", "L R", 2, 176400, 160, 6, 0},
    {"truehd_st48_s20.thd", "L R", 2, 48000, 40, 6, 4},
    {"truehd_c30_s16.thd", "L R C", 3, 48000, 40, 12, 8},
    {"truehd_c21_s16.thd", "L R LFE", 3, 48000, 40, 12, 8},
    {"truehd_c40_s16.thd", "L R C Cs", 4, 48000, 40, 12, 8},
    {"truehd_c31_s16.thd", "L R C LFE", 4, 48000, 40, 12, 8},
    {"truehd_c50_s16.thd", "L R C Ls Rs", 5, 48000, 40, 12, 8},
    {"truehd_c41_s16.thd", "L R C LFE Cs", 5, 48000, 40, 12, 8},
    {"mlp_quad_s16.mlp", "", 4, 48000, 40, 6, 8},
    {"mlp_c30back_s16.mlp", "", 3, 48000, 40, 6, 8},
    {"truehd_st48_s24_gap8.thd", "L R", 2, 48000, 40, 12, 0},
    {"truehd_st48_s24_gap128.thd", "L R", 2, 48000, 40, 2, 0},
    {"truehd_mono48_s24.thd", "C", 1, 48000, 40, 19, 0},
}};

}  // namespace

TEST_CASE("each later FFmpeg fixture carries the feature it was made for", "[truehd][fixtures]") {
    for (const Expectation& expected : kExpectations) {
        DYNAMIC_SECTION(expected.file) {
            const Seen seen = decode_fixture(std::string(expected.file));
            CHECK(seen.channels == expected.channels);
            CHECK(seen.sample_rate == expected.sample_rate);
            CHECK(seen.samples_per_access_unit == expected.samples_per_access_unit);
            CHECK(seen.counters.restarts == expected.restart_headers);
            CHECK(seen.counters.lossless_check_failures == 0);
            if (!expected.channel_names.empty()) {
                CHECK(seen.layout == expected.channel_names);
            }
            // The bits the samples use: those below the source's word size are an output shift
            // and are zero in every sample, and the first bit above them is used somewhere.
            const std::uint32_t clear = (1U << expected.clear_low_bits) - 1U;
            CHECK((seen.low_bits_set & clear) == 0U);
            CHECK((seen.low_bits_set & (clear + 1U)) != 0U);
        }
    }
}

TEST_CASE("every later FFmpeg fixture is named in the manifest with what it covers",
          "[truehd][fixtures]") {
    const nlohmann::json fixtures = manifest()["fixtures"];
    std::size_t found = 0;
    for (const Expectation& expected : kExpectations) {
        bool listed = false;
        for (const nlohmann::json& fixture : fixtures) {
            if (fixture["file"].get<std::string>() == expected.file) {
                listed = true;
                // The 12 original fixtures say nothing of why they exist; the later ones do.
                found += fixture.contains("covers") ? 1U : 0U;
            }
        }
        CHECK(listed);
    }
    CHECK(found == kExpectations.size() - 1);  // all but truehd_mono48_s24
}

TEST_CASE("two joined streams: FFmpeg decodes through the join, the library's strict decode stops",
          "[truehd][fixtures][splice]") {
    // The second stream's first restart header carries a lossless check that checks nothing
    // (ERRATA 12, "the first header's value checks nothing"). Read as the check of the first
    // stream's last interval it fails, and a strict decoder stops there; a lenient one counts it
    // and goes on, to the samples FFmpeg's decode gives (which only warns). ERRATA question 20.
    const nlohmann::json spliced = manifest()["spliced"];
    REQUIRE(spliced.size() == 1);
    const nlohmann::json& fixture = spliced[0];
    const std::vector<std::byte> stream = read_file(fixture["file"].get<std::string>());
    const auto join = fixture["join_access_units"][0].get<std::size_t>();
    REQUIRE(fixture["parts"].get<int>() == 2);

    SECTION("strict: every unit before the join decodes, the unit at the join does not") {
        Decoder decoder;
        ElementaryReader reader(stream);
        std::size_t index = 0;
        while (const auto unit = reader.next()) {
            const auto decoded = decoder.decode(*unit);
            if (index < join) {
                REQUIRE(decoded.has_value());
            } else if (index == join) {
                REQUIRE_FALSE(decoded.has_value());
                CHECK(decoded.error().kind == ErrorKind::kBadLosslessCheck);
                break;
            }
            ++index;
        }
        CHECK(index == join);
        CHECK(decoder.counters().lossless_check_failures == 1);
    }

    SECTION("lenient: all of it decodes to FFmpeg's samples, with the one failure counted") {
        DecoderConfig config;
        config.strictness = Strictness::kLenient;
        Decoder decoder(config);
        ElementaryReader reader(stream);
        std::vector<std::byte> pcm;
        while (const auto unit = reader.next()) {
            const auto decoded = decoder.decode(*unit);
            REQUIRE(decoded.has_value());
            REQUIRE(decoded->has_value());
            for (const std::int32_t sample : (*decoded)->pcm) {
                const auto word = static_cast<std::uint32_t>(sample) << 8U;
                for (unsigned shift = 0; shift < 32; shift += 8) {
                    pcm.push_back(static_cast<std::byte>((word >> shift) & 0xFFU));
                }
            }
        }
        CHECK(hex(iclforge::base::crypto::sha256(pcm)) == fixture["pcm_sha256"].get<std::string>());
        CHECK(decoder.counters().lossless_check_failures == 1);
        CHECK(decoder.counters().units_decoded == fixture["access_units"].get<std::uint64_t>());
    }
}
