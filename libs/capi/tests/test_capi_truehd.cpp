// The TrueHD and MLP C entry points (iclforge_truehd_*): the committed FFmpeg fixtures decoded
// through them, held to the SHA-256 of what FFmpeg's strict decode of the same file gives (the
// codec is lossless, so the digests are equal or the decode is wrong), and what a NULL argument, an
// index past the end, a stream that is not TrueHD and a unit that does not decode leave each call
// to answer. The build here has ICLFORGE_BUILD_TRUEHD on; test_capi_truehd_absent.cpp is the other
// build's.

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "iclforge/base/crypto/sha256.hpp"
#include "iclforge_c/iclforge.h"
#include "iclforge_c/version.h"

// The text a macro expands to, or the macro's own name when it is not defined: how a test reads
// whether version.h #cmakedefine'd ICLFORGE_HAS_TRUEHD without a preprocessor conditional
// (tools/checks/check_platform_macros.ps1 allows none in tests). #cmakedefine writes a macro with
// no value, which would be an empty argument (MSVC warns, C4003), so the text carries a "|" after it.
#define ICLFORGE_TEST_STRINGIFY_VALUE(x) #x
#define ICLFORGE_TEST_STRINGIFY(x) ICLFORGE_TEST_STRINGIFY_VALUE(x |)

namespace {

// A pointer no decode returns, to tell "left untouched" from "set to NULL".
iclforge_truehd_decoded_t* sentinel() {
    static char marker;
    return reinterpret_cast<iclforge_truehd_decoded_t*>(&marker);
}

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::uint8_t> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>(raw[i]);
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

std::vector<std::uint8_t> fixture_bytes(const std::string& file) {
    return read_file(std::string(ICLFORGE_GOLDEN_TRUEHD_DIR) + "/" + file);
}

// What FFmpeg's `-f s32le` writes for 24-bit samples: each shifted left by 8, little-endian.
void append_s32le(std::vector<std::byte>& out, const std::int32_t* pcm, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        const auto word = static_cast<std::uint32_t>(pcm[i]) << 8U;
        for (unsigned shift = 0; shift < 32; shift += 8) {
            out.push_back(static_cast<std::byte>((word >> shift) & 0xFFU));
        }
    }
}

struct Units {
    iclforge_truehd_units_t* handle = nullptr;
    Units() = default;
    Units(const Units&) = delete;
    Units& operator=(const Units&) = delete;
    ~Units() { iclforge_truehd_units_destroy(handle); }
};

struct Decoder {
    iclforge_truehd_decoder_t* handle = nullptr;
    explicit Decoder(const iclforge_truehd_decoder_config_t& config) {
        REQUIRE(iclforge_truehd_decoder_create(&config, &handle) == ICLFORGE_OK);
        REQUIRE(handle != nullptr);
    }
    Decoder() : Decoder(default_config()) {}
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    ~Decoder() { iclforge_truehd_decoder_destroy(handle); }

    static iclforge_truehd_decoder_config_t default_config() {
        iclforge_truehd_decoder_config_t config;
        iclforge_truehd_decoder_config_init(&config);
        return config;
    }
};

struct Decoded {
    iclforge_truehd_decoded_t* handle = nullptr;
    Decoded() = default;
    Decoded(const Decoded&) = delete;
    Decoded& operator=(const Decoded&) = delete;
    ~Decoded() { iclforge_truehd_decoded_destroy(handle); }
};

iclforge_truehd_decoder_config_t config_for(iclforge_truehd_presentation_t presentation,
                                            bool lenient = false) {
    iclforge_truehd_decoder_config_t config;
    iclforge_truehd_decoder_config_init(&config);
    config.presentation = presentation;
    config.lenient = lenient ? 1 : 0;
    return config;
}

}  // namespace

TEST_CASE("version.h says the TrueHD surface is built into this library", "[capi][truehd]") {
    CHECK(std::string_view(ICLFORGE_TEST_STRINGIFY(ICLFORGE_HAS_TRUEHD)) == "|");
}

TEST_CASE("iclforge_truehd_decoder_config_init gives the C++ defaults", "[capi][truehd]") {
    iclforge_truehd_decoder_config_t config;
    std::memset(&config, 0xAB, sizeof config);
    iclforge_truehd_decoder_config_init(&config);
    CHECK(config.presentation == ICLFORGE_TRUEHD_PRESENTATION_BEST);
    CHECK(config.lenient == 0);

    iclforge_truehd_decoder_config_init(nullptr);  // documented no-op
}

TEST_CASE("every FFmpeg fixture decodes through the C API to FFmpeg's samples", "[capi][truehd]") {
    const nlohmann::json fixtures = manifest()["fixtures"];
    REQUIRE(fixtures.size() >= 12);
    for (const nlohmann::json& fixture : fixtures) {
        const std::string name = fixture["name"];
        DYNAMIC_SECTION(name) {
            const std::vector<std::uint8_t> stream = fixture_bytes(fixture["file"]);

            iclforge_truehd_stream_info_t info;
            REQUIRE(iclforge_truehd_probe(stream.data(), stream.size(), &info) == ICLFORGE_OK);
            CHECK(info.is_mlp == (fixture["codec"].get<std::string>() == "mlp" ? 1 : 0));
            CHECK(info.sample_rate == fixture["sample_rate"].get<std::uint32_t>());
            CHECK(info.access_units == fixture["access_units"].get<std::size_t>());
            REQUIRE(info.substreams >= 1);
            REQUIRE(info.substreams <= 4);
            // The last substream is the one a best-presentation decode delivers.
            const auto last = static_cast<std::size_t>(info.substreams - 1);
            CHECK(info.channels[last] == fixture["channels"].get<int>());
            CHECK(info.samples_per_access_unit > 0);
            CHECK(info.has_object_substream == 0);

            Units units;
            REQUIRE(iclforge_truehd_scan(stream.data(), stream.size(), &units.handle) ==
                    ICLFORGE_OK);
            const std::size_t count = iclforge_truehd_units_count(units.handle);
            CHECK(count == fixture["access_units"].get<std::size_t>());
            CHECK(iclforge_truehd_units_skipped_bytes(units.handle) == 0);
            std::size_t covered = 0;
            for (std::size_t i = 0; i < count; ++i) {
                CHECK(iclforge_truehd_units_offset(units.handle, i) == covered);
                covered += iclforge_truehd_units_size(units.handle, i);
            }
            CHECK(covered == stream.size());

            Decoder decoder;
            std::vector<std::byte> pcm;
            std::size_t samples = 0;
            std::size_t restarted_units = 0;
            int padding = 0;
            for (std::size_t i = 0; i < count; ++i) {
                Decoded decoded;
                REQUIRE(iclforge_truehd_decoder_decode(
                            decoder.handle,
                            stream.data() + iclforge_truehd_units_offset(units.handle, i),
                            iclforge_truehd_units_size(units.handle, i),
                            &decoded.handle) == ICLFORGE_OK);
                REQUIRE(decoded.handle != nullptr);
                CHECK(iclforge_truehd_decoded_sample_rate(decoded.handle) ==
                      fixture["sample_rate"].get<std::uint32_t>());
                CHECK(iclforge_truehd_decoded_channels(decoded.handle) ==
                      fixture["channels"].get<int>());
                const auto per_channel =
                    static_cast<std::size_t>(iclforge_truehd_decoded_samples(decoded.handle));
                const auto channels =
                    static_cast<std::size_t>(iclforge_truehd_decoded_channels(decoded.handle));
                CHECK(iclforge_truehd_decoded_substream(decoded.handle) == info.substreams - 1);
                const int restarted = iclforge_truehd_decoded_restarted(decoded.handle);
                CHECK((i != 0 || restarted == 1));  // the first unit opens with a restart header
                restarted_units += static_cast<std::size_t>(restarted);
                samples += per_channel;
                padding += iclforge_truehd_decoded_padding_samples(decoded.handle);
                append_s32le(pcm, iclforge_truehd_decoded_pcm(decoded.handle),
                             per_channel * channels);
            }
            CHECK(restarted_units >= 1);
            CHECK(samples == fixture["pcm_samples"].get<std::size_t>());
            CHECK(samples + static_cast<std::size_t>(padding) ==
                  count * static_cast<std::size_t>(info.samples_per_access_unit));
            CHECK(hex(iclforge::base::crypto::sha256(pcm)) ==
                  fixture["pcm_sha256"].get<std::string>());

            iclforge_truehd_counters_t counters;
            iclforge_truehd_decoder_counters(decoder.handle, &counters);
            CHECK(counters.units == count);
            CHECK(counters.units_decoded == count);
            CHECK(counters.units_waiting == 0);
            CHECK(counters.check_failures == 0);
            CHECK(counters.lossless_check_failures == 0);
            CHECK(counters.lossless_checks_ok > 0);
        }
    }
}

TEST_CASE("a decoded unit outlives the next decode and names its speakers", "[capi][truehd]") {
    const std::vector<std::uint8_t> stream = fixture_bytes("truehd_m51_s24.thd");
    Units units;
    REQUIRE(iclforge_truehd_scan(stream.data(), stream.size(), &units.handle) == ICLFORGE_OK);
    REQUIRE(iclforge_truehd_units_count(units.handle) > 1);

    Decoder decoder;
    Decoded first;
    Decoded second;
    REQUIRE(iclforge_truehd_decoder_decode(
                decoder.handle, stream.data() + iclforge_truehd_units_offset(units.handle, 0),
                iclforge_truehd_units_size(units.handle, 0), &first.handle) == ICLFORGE_OK);
    REQUIRE(first.handle != nullptr);
    const std::size_t values =
        static_cast<std::size_t>(iclforge_truehd_decoded_samples(first.handle)) *
        static_cast<std::size_t>(iclforge_truehd_decoded_channels(first.handle));
    const std::vector<std::int32_t> before(iclforge_truehd_decoded_pcm(first.handle),
                                           iclforge_truehd_decoded_pcm(first.handle) + values);
    REQUIRE(iclforge_truehd_decoder_decode(
                decoder.handle, stream.data() + iclforge_truehd_units_offset(units.handle, 1),
                iclforge_truehd_units_size(units.handle, 1), &second.handle) == ICLFORGE_OK);
    CHECK(std::vector<std::int32_t>(iclforge_truehd_decoded_pcm(first.handle),
                                    iclforge_truehd_decoded_pcm(first.handle) + values) == before);

    CHECK(iclforge_truehd_decoded_channels(first.handle) == 6);
    const std::array<std::string_view, 6> expected{"L", "R", "C", "LFE", "Ls", "Rs"};
    for (std::size_t c = 0; c < expected.size(); ++c) {
        CHECK(std::string_view(iclforge_truehd_decoded_channel_name(first.handle, c)) ==
              expected[c]);
    }
    CHECK(std::string_view(iclforge_truehd_decoded_channel_name(first.handle, 6)).empty());

    // The extra data is what follows the last segment: none in a stream FFmpeg's encoder wrote.
    std::size_t extra = 99;
    CHECK(iclforge_truehd_decoded_extra_data(first.handle, &extra) == nullptr);
    CHECK(extra == 0);
}

TEST_CASE("a presentation reads the substreams it names", "[capi][truehd]") {
    const std::vector<std::uint8_t> stream = fixture_bytes("truehd_m51_s24.thd");
    Units units;
    REQUIRE(iclforge_truehd_scan(stream.data(), stream.size(), &units.handle) == ICLFORGE_OK);

    iclforge_truehd_stream_info_t info;
    REQUIRE(iclforge_truehd_probe(stream.data(), stream.size(), &info) == ICLFORGE_OK);
    CHECK(info.channels[0] == 2);
    CHECK(info.channels[1] == 6);

    const std::array<std::pair<iclforge_truehd_presentation_t, int>, 3> cases{{
        {ICLFORGE_TRUEHD_PRESENTATION_2CH, 2},
        {ICLFORGE_TRUEHD_PRESENTATION_6CH, 6},
        {ICLFORGE_TRUEHD_PRESENTATION_BEST, 6},
    }};
    for (const auto& [presentation, channels] : cases) {
        DYNAMIC_SECTION("presentation " << static_cast<int>(presentation)) {
            Decoder decoder(config_for(presentation));
            Decoded decoded;
            REQUIRE(
                iclforge_truehd_decoder_decode(
                    decoder.handle, stream.data() + iclforge_truehd_units_offset(units.handle, 0),
                    iclforge_truehd_units_size(units.handle, 0), &decoded.handle) == ICLFORGE_OK);
            REQUIRE(decoded.handle != nullptr);
            CHECK(iclforge_truehd_decoded_channels(decoded.handle) == channels);
        }
    }
}

TEST_CASE("a unit before any major sync gives no samples, and a corrupt one is refused",
          "[capi][truehd]") {
    const std::vector<std::uint8_t> stream = fixture_bytes("truehd_st48_s24.thd");
    Units units;
    REQUIRE(iclforge_truehd_scan(stream.data(), stream.size(), &units.handle) == ICLFORGE_OK);
    REQUIRE(iclforge_truehd_units_count(units.handle) > 2);
    const auto unit = [&](std::size_t i) {
        const std::size_t offset = iclforge_truehd_units_offset(units.handle, i);
        return std::vector<std::uint8_t>(
            stream.begin() + static_cast<std::ptrdiff_t>(offset),
            stream.begin() +
                static_cast<std::ptrdiff_t>(offset + iclforge_truehd_units_size(units.handle, i)));
    };

    SECTION("joined part way") {
        Decoder decoder;
        const std::vector<std::uint8_t> later = unit(1);
        Decoded decoded;
        decoded.handle = sentinel();
        CHECK(iclforge_truehd_decoder_decode(decoder.handle, later.data(), later.size(),
                                             &decoded.handle) == ICLFORGE_OK);
        CHECK(decoded.handle == nullptr);
        iclforge_truehd_counters_t counters;
        iclforge_truehd_decoder_counters(decoder.handle, &counters);
        CHECK(counters.units == 1);
        CHECK(counters.units_decoded == 0);
    }

    SECTION("lost data makes the decoder wait for the next restart") {
        Decoder decoder;
        Decoded first;
        const std::vector<std::uint8_t> opening = unit(0);
        REQUIRE(iclforge_truehd_decoder_decode(decoder.handle, opening.data(), opening.size(),
                                               &first.handle) == ICLFORGE_OK);
        REQUIRE(first.handle != nullptr);
        iclforge_truehd_decoder_lost_data(decoder.handle);
        // Unit 1 carries no restart header: with the data between lost it gives nothing.
        const std::vector<std::uint8_t> next = unit(1);
        Decoded none;
        none.handle = sentinel();
        CHECK(iclforge_truehd_decoder_decode(decoder.handle, next.data(), next.size(),
                                             &none.handle) == ICLFORGE_OK);
        CHECK(none.handle == nullptr);
        iclforge_truehd_counters_t counters;
        iclforge_truehd_decoder_counters(decoder.handle, &counters);
        CHECK(counters.units_waiting == 1);
    }

    SECTION("strict refuses a flipped payload bit and says why") {
        Decoder decoder;
        std::vector<std::uint8_t> bad = unit(0);
        bad[bad.size() - 5] ^= 0x01U;
        iclforge_truehd_decoded_t* out = sentinel();
        const iclforge_status_t status =
            iclforge_truehd_decoder_decode(decoder.handle, bad.data(), bad.size(), &out);
        CHECK(status == ICLFORGE_ERROR_TRUEHD_BAD_PARITY);
        CHECK(out == sentinel());  // untouched
        CHECK(std::string_view(iclforge_truehd_decoder_last_error_detail(decoder.handle)).size() >
              0);
        CHECK(std::string_view(iclforge_status_message(status)) == "bad TrueHD substream parity");

        iclforge_truehd_counters_t counters;
        iclforge_truehd_decoder_counters(decoder.handle, &counters);
        CHECK(counters.check_failures >= 1);

        // The sentence belongs to the decode that made it: the next, which fails differently or
        // not at all, replaces it. A reset forgets the stream and the sentence.
        iclforge_truehd_decoder_reset(decoder.handle);
        CHECK(std::string_view(iclforge_truehd_decoder_last_error_detail(decoder.handle)).empty());
        Decoded good;
        const std::vector<std::uint8_t> first = unit(0);
        CHECK(iclforge_truehd_decoder_decode(decoder.handle, first.data(), first.size(),
                                             &good.handle) == ICLFORGE_OK);
        CHECK(good.handle != nullptr);
    }

    SECTION("lenient counts the failed checks and decodes on") {
        Decoder decoder(config_for(ICLFORGE_TRUEHD_PRESENTATION_BEST, true));
        std::vector<std::uint8_t> bad = unit(0);
        bad[bad.size() - 5] ^= 0x01U;
        Decoded decoded;
        (void)iclforge_truehd_decoder_decode(decoder.handle, bad.data(), bad.size(),
                                             &decoded.handle);
        iclforge_truehd_counters_t counters;
        iclforge_truehd_decoder_counters(decoder.handle, &counters);
        CHECK(counters.check_failures == 2);  // the parity byte and the CRC byte
    }

    SECTION("a truncated unit") {
        Decoder decoder;
        std::vector<std::uint8_t> cut = unit(0);
        cut.resize(cut.size() / 2);
        iclforge_truehd_decoded_t* out = sentinel();
        const iclforge_status_t status =
            iclforge_truehd_decoder_decode(decoder.handle, cut.data(), cut.size(), &out);
        CHECK(status >= ICLFORGE_ERROR_TRUEHD_TRUNCATED);
        CHECK(status <= ICLFORGE_ERROR_TRUEHD_UNSUPPORTED);
        CHECK(out == sentinel());
    }
}

TEST_CASE("data that is not TrueHD is refused", "[capi][truehd]") {
    const std::array<std::uint8_t, 64> noise{};  // zeros: no major sync anywhere

    iclforge_truehd_stream_info_t info;
    info.sample_rate = 1234;
    CHECK(iclforge_truehd_probe(noise.data(), noise.size(), &info) ==
          ICLFORGE_ERROR_TRUEHD_BAD_SYNC);
    CHECK(info.sample_rate == 1234);  // untouched on an error

    Units units;
    CHECK(iclforge_truehd_scan(noise.data(), noise.size(), &units.handle) ==
          ICLFORGE_ERROR_TRUEHD_BAD_SYNC);
    CHECK(units.handle == nullptr);

    // An AC-3 syncframe header is not a TrueHD major sync either.
    const std::array<std::uint8_t, 8> ac3{0x0B, 0x77, 0, 0, 0x1E, 0x40, 0xE1, 0};
    CHECK(iclforge_truehd_probe(ac3.data(), ac3.size(), &info) == ICLFORGE_ERROR_TRUEHD_BAD_SYNC);
}

TEST_CASE(
    "the TrueHD entry points answer a NULL, an index past the end and an enumerator that is "
    "not one",
    "[capi][truehd]") {
    iclforge_truehd_stream_info_t info;
    iclforge_truehd_units_t* units = nullptr;
    iclforge_truehd_decoder_t* decoder = nullptr;
    iclforge_truehd_decoded_t* decoded = nullptr;
    const std::array<std::uint8_t, 8> bytes{};

    CHECK(iclforge_truehd_probe(nullptr, 0, &info) == ICLFORGE_ERROR_INVALID_ARGUMENT);
    CHECK(iclforge_truehd_probe(bytes.data(), bytes.size(), nullptr) ==
          ICLFORGE_ERROR_INVALID_ARGUMENT);
    CHECK(iclforge_truehd_scan(nullptr, 0, &units) == ICLFORGE_ERROR_INVALID_ARGUMENT);
    CHECK(iclforge_truehd_scan(bytes.data(), bytes.size(), nullptr) ==
          ICLFORGE_ERROR_INVALID_ARGUMENT);

    CHECK(iclforge_truehd_units_count(nullptr) == 0);
    CHECK(iclforge_truehd_units_offset(nullptr, 0) == 0);
    CHECK(iclforge_truehd_units_size(nullptr, 0) == 0);
    CHECK(iclforge_truehd_units_skipped_bytes(nullptr) == 0);
    iclforge_truehd_units_destroy(nullptr);

    const iclforge_truehd_decoder_config_t config = config_for(ICLFORGE_TRUEHD_PRESENTATION_BEST);
    CHECK(iclforge_truehd_decoder_create(nullptr, &decoder) == ICLFORGE_ERROR_INVALID_ARGUMENT);
    CHECK(iclforge_truehd_decoder_create(&config, nullptr) == ICLFORGE_ERROR_INVALID_ARGUMENT);

    iclforge_truehd_decoder_config_t odd = config;
    const int out_of_range = 99;
    std::memcpy(&odd.presentation, &out_of_range, sizeof out_of_range);
    CHECK(iclforge_truehd_decoder_create(&odd, &decoder) == ICLFORGE_ERROR_INVALID_ARGUMENT);
    CHECK(decoder == nullptr);

    CHECK(iclforge_truehd_decoder_decode(nullptr, bytes.data(), bytes.size(), &decoded) ==
          ICLFORGE_ERROR_INVALID_ARGUMENT);
    {
        Decoder live;
        CHECK(iclforge_truehd_decoder_decode(live.handle, nullptr, 0, &decoded) ==
              ICLFORGE_ERROR_INVALID_ARGUMENT);
        CHECK(iclforge_truehd_decoder_decode(live.handle, bytes.data(), bytes.size(), nullptr) ==
              ICLFORGE_ERROR_INVALID_ARGUMENT);
    }
    iclforge_truehd_decoder_reset(nullptr);
    iclforge_truehd_decoder_lost_data(nullptr);
    iclforge_truehd_decoder_destroy(nullptr);
    CHECK(std::string_view(iclforge_truehd_decoder_last_error_detail(nullptr)).empty());
    iclforge_truehd_counters_t counters;
    iclforge_truehd_decoder_counters(nullptr, &counters);  // documented no-ops
    iclforge_truehd_decoder_counters(nullptr, nullptr);

    CHECK(iclforge_truehd_decoded_sample_rate(nullptr) == 0);
    CHECK(iclforge_truehd_decoded_channels(nullptr) == 0);
    CHECK(iclforge_truehd_decoded_samples(nullptr) == 0);
    CHECK(iclforge_truehd_decoded_substream(nullptr) == 0);
    CHECK(iclforge_truehd_decoded_restarted(nullptr) == 0);
    CHECK(iclforge_truehd_decoded_padding_samples(nullptr) == 0);
    CHECK(iclforge_truehd_decoded_pcm(nullptr) == nullptr);
    std::size_t extra = 7;
    CHECK(iclforge_truehd_decoded_extra_data(nullptr, &extra) == nullptr);
    CHECK(extra == 0);
    CHECK(iclforge_truehd_decoded_extra_data(nullptr, nullptr) == nullptr);
    CHECK(std::string_view(iclforge_truehd_decoded_channel_name(nullptr, 0)).empty());
    iclforge_truehd_decoded_destroy(nullptr);
}

TEST_CASE("an index past the last unit is 0", "[capi][truehd]") {
    const std::vector<std::uint8_t> stream = fixture_bytes("truehd_st48_s24.thd");
    Units units;
    REQUIRE(iclforge_truehd_scan(stream.data(), stream.size(), &units.handle) == ICLFORGE_OK);
    const std::size_t count = iclforge_truehd_units_count(units.handle);
    CHECK(iclforge_truehd_units_offset(units.handle, count) == 0);
    CHECK(iclforge_truehd_units_size(units.handle, count) == 0);
    CHECK(iclforge_truehd_units_size(units.handle, count - 1) > 0);
}

TEST_CASE("a scan skips the bytes before the first major sync and counts them", "[capi][truehd]") {
    const std::vector<std::uint8_t> stream = fixture_bytes("truehd_st48_s24.thd");
    std::vector<std::uint8_t> padded(13, 0x00);
    padded.insert(padded.end(), stream.begin(), stream.end());

    Units units;
    REQUIRE(iclforge_truehd_scan(padded.data(), padded.size(), &units.handle) == ICLFORGE_OK);
    CHECK(iclforge_truehd_units_offset(units.handle, 0) == 13);
    CHECK(iclforge_truehd_units_count(units.handle) > 0);

    iclforge_truehd_stream_info_t info;
    REQUIRE(iclforge_truehd_probe(padded.data(), padded.size(), &info) == ICLFORGE_OK);
    CHECK(info.first_unit_offset == 13);
}

TEST_CASE("every TrueHD status has its own message", "[capi][truehd]") {
    std::vector<std::string> seen;
    for (int status = ICLFORGE_ERROR_TRUEHD_TRUNCATED; status <= ICLFORGE_ERROR_TRUEHD_UNSUPPORTED;
         ++status) {
        const std::string message = iclforge_status_message(static_cast<iclforge_status_t>(status));
        INFO(status);
        CHECK_FALSE(message.empty());
        CHECK(message != "unknown status");
        for (const std::string& earlier : seen) {
            CHECK(message != earlier);
        }
        seen.push_back(message);
    }
}

// --- The encoder (iclforge_truehd_encoder_*) -----------------------------------------------------

namespace {

struct Encoder {
    iclforge_truehd_encoder_t* handle = nullptr;
    explicit Encoder(const iclforge_truehd_encoder_config_t& config) {
        REQUIRE(iclforge_truehd_encoder_create(&config, &handle) == ICLFORGE_OK);
        REQUIRE(handle != nullptr);
    }
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;
    ~Encoder() { iclforge_truehd_encoder_destroy(handle); }
};

struct Encoded {
    iclforge_truehd_encoded_t* handle = nullptr;
    Encoded() = default;
    Encoded(const Encoded&) = delete;
    Encoded& operator=(const Encoded&) = delete;
    ~Encoded() { iclforge_truehd_encoded_destroy(handle); }
};

// Appends the units of one encode() or flush() to the stream; the number of units it held.
std::size_t take(const Encoded& encoded, std::vector<std::uint8_t>& stream) {
    const std::size_t count = iclforge_truehd_encoded_count(encoded.handle);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t* data = iclforge_truehd_encoded_data(encoded.handle, i);
        stream.insert(stream.end(), data, data + iclforge_truehd_encoded_size(encoded.handle, i));
    }
    return count;
}

struct DecodedStream {
    std::vector<std::int32_t> pcm;  // interleaved
    std::vector<std::byte> s32le;   // as FFmpeg's -f s32le gives it
    std::size_t units = 0;
    int channels = 0;
    std::size_t frames = 0;
};

// Every unit of `stream` through the C API's decoder, strict.
DecodedStream decode_all(const std::vector<std::uint8_t>& stream) {
    DecodedStream out;
    Units units;
    REQUIRE(iclforge_truehd_scan(stream.data(), stream.size(), &units.handle) == ICLFORGE_OK);
    out.units = iclforge_truehd_units_count(units.handle);
    Decoder decoder;
    for (std::size_t i = 0; i < out.units; ++i) {
        Decoded decoded;
        REQUIRE(iclforge_truehd_decoder_decode(
                    decoder.handle, stream.data() + iclforge_truehd_units_offset(units.handle, i),
                    iclforge_truehd_units_size(units.handle, i), &decoded.handle) == ICLFORGE_OK);
        REQUIRE(decoded.handle != nullptr);
        out.channels = iclforge_truehd_decoded_channels(decoded.handle);
        const auto samples =
            static_cast<std::size_t>(iclforge_truehd_decoded_samples(decoded.handle));
        out.frames += samples;
        const std::int32_t* pcm = iclforge_truehd_decoded_pcm(decoded.handle);
        const std::size_t values = samples * static_cast<std::size_t>(out.channels);
        out.pcm.insert(out.pcm.end(), pcm, pcm + values);
        append_s32le(out.s32le, pcm, values);
    }
    return out;
}

}  // namespace

TEST_CASE("iclforge_truehd_encoder_config_init gives the C++ defaults", "[capi][truehd]") {
    iclforge_truehd_encoder_config_t config;
    std::memset(&config, 0xAB, sizeof config);
    iclforge_truehd_encoder_config_init(&config);
    CHECK(config.sample_rate == 48000);
    CHECK(config.channels == 2);
    CHECK(config.bits == 24);
    CHECK(config.channel_assignment == 0);
    CHECK(config.restart_interval_units == 128);
    CHECK(config.peak_data_rate_bps == 18'000'000);
    CHECK(config.allow_rate_violations == 0);
    CHECK(config.downmix == ICLFORGE_TRUEHD_DOWNMIX_NONE);
    CHECK(config.downmix_centre == 0.7071067811865476);
    CHECK(config.downmix_surround == 0.7071067811865476);
    CHECK(config.downmix_lfe == 0.0);
    CHECK(config.downmix_rear_to_surround == 0.7071067811865476);
    CHECK(config.stereo_mode == 0);
    CHECK(config.max_fir_order == 8);
    CHECK(config.matrix_search == 1);
    CHECK(config.verify == 0);
    CHECK(std::string_view(iclforge_truehd_encoder_refusal_reason(&config)).empty());

    iclforge_truehd_encoder_config_init(nullptr);  // documented no-op
    CHECK(iclforge_truehd_default_channel_assignment(2) != 0);
    CHECK(iclforge_truehd_default_channel_assignment(8) != 0);
    CHECK(iclforge_truehd_default_channel_assignment(9) == 0);
}

TEST_CASE("every TrueHD fixture decoded, encoded and decoded again through the C API gives its PCM",
          "[capi][truehd]") {
    const nlohmann::json fixtures = manifest()["fixtures"];
    for (const nlohmann::json& fixture : fixtures) {
        if (fixture["codec"] != "truehd") {
            continue;  // the encoder writes TrueHD, not MLP
        }
        const std::string name = fixture["name"];
        DYNAMIC_SECTION(name) {
            const DecodedStream source = decode_all(fixture_bytes(fixture["file"]));
            REQUIRE(source.channels == fixture["channels"].get<int>());

            iclforge_truehd_encoder_config_t config;
            iclforge_truehd_encoder_config_init(&config);
            config.sample_rate = fixture["sample_rate"].get<std::uint32_t>();
            config.channels = source.channels;
            config.verify = 1;
            Encoder encoder(config);
            const unsigned family = config.sample_rate % 44100U == 0 ? 44100U : 48000U;
            CHECK(iclforge_truehd_encoder_samples_per_access_unit(encoder.handle) ==
                  40 * static_cast<int>(config.sample_rate / family));

            std::vector<std::uint8_t> stream;
            std::size_t units = 0;
            const std::size_t chunk = 1000;
            for (std::size_t at = 0; at < source.frames; at += chunk) {
                const std::size_t frames = std::min(chunk, source.frames - at);
                Encoded encoded;
                REQUIRE(iclforge_truehd_encoder_encode(
                            encoder.handle,
                            source.pcm.data() + at * static_cast<std::size_t>(source.channels),
                            frames, &encoded.handle) == ICLFORGE_OK);
                units += take(encoded, stream);
            }
            Encoded last;
            REQUIRE(iclforge_truehd_encoder_flush(encoder.handle, &last.handle) == ICLFORGE_OK);
            REQUIRE(last.handle != nullptr);
            const std::size_t last_count = iclforge_truehd_encoded_count(last.handle);
            units += take(last, stream);
            // The end-of-stream marker counts the zero samples the last unit was filled up with:
            // whole units less the padding are the frames that went in.
            const int per_unit = iclforge_truehd_encoded_samples(last.handle, last_count - 1);
            CHECK(source.frames + static_cast<std::size_t>(iclforge_truehd_encoded_padding_samples(
                                      last.handle, last_count - 1)) ==
                  units * static_cast<std::size_t>(per_unit));

            iclforge_truehd_encoder_stats_t stats;
            iclforge_truehd_encoder_stats(encoder.handle, &stats);
            CHECK(stats.access_units == units);
            CHECK(stats.bytes == stream.size());
            CHECK(stats.input_samples == source.frames);
            CHECK(stats.verified_units == stats.access_units);
            CHECK(stats.rate_violations == 0);

            // Decoded again, the stream is the PCM that went in, and the PCM FFmpeg's strict decode
            // gives the fixture.
            const DecodedStream again = decode_all(stream);
            CHECK(again.units == units);
            CHECK(again.channels == source.channels);
            CHECK(again.frames == source.frames);
            CHECK(again.pcm == source.pcm);
            CHECK(hex(iclforge::base::crypto::sha256(again.s32le)) ==
                  fixture["pcm_sha256"].get<std::string>());
        }
    }
}

TEST_CASE("the encoder says which substreams and access unit length a configuration has",
          "[capi][truehd]") {
    const std::array<std::array<int, 4>, 4> cases{{
        // channels, rate, substreams, samples per access unit
        {{2, 48000, 1, 40}},
        {{6, 96000, 2, 80}},
        {{8, 48000, 3, 40}},
        {{2, 192000, 1, 160}},
    }};
    for (const auto& item : cases) {
        iclforge_truehd_encoder_config_t config;
        iclforge_truehd_encoder_config_init(&config);
        config.channels = item[0];
        config.sample_rate = static_cast<std::uint32_t>(item[1]);
        Encoder encoder(config);
        CHECK(iclforge_truehd_encoder_substreams(encoder.handle) == item[2]);
        CHECK(iclforge_truehd_encoder_samples_per_access_unit(encoder.handle) == item[3]);
    }
}

TEST_CASE("the encoder refuses what it does not write, and a stream it cannot send in time",
          "[capi][truehd]") {
    iclforge_truehd_encoder_config_t config;
    iclforge_truehd_encoder_config_init(&config);

    SECTION("a configuration is refused with a reason, and nothing is made") {
        config.channels = 9;
        iclforge_truehd_encoder_t* encoder = nullptr;
        CHECK_FALSE(std::string_view(iclforge_truehd_encoder_refusal_reason(&config)).empty());
        CHECK(iclforge_truehd_encoder_create(&config, &encoder) ==
              ICLFORGE_ERROR_TRUEHD_ENCODE_INVALID_CONFIG);
        CHECK(encoder == nullptr);
        config.channels = 2;
        config.sample_rate = 32000;
        CHECK(iclforge_truehd_encoder_create(&config, &encoder) ==
              ICLFORGE_ERROR_TRUEHD_ENCODE_INVALID_CONFIG);
        config.sample_rate = 48000;
        config.restart_interval_units = 5;  // 1, or at least 8
        CHECK(iclforge_truehd_encoder_create(&config, &encoder) ==
              ICLFORGE_ERROR_TRUEHD_ENCODE_INVALID_CONFIG);
    }

    SECTION("a downmix that is not one of the two is an invalid argument, read as the int stored") {
        const int odd = 7;
        std::memcpy(&config.downmix, &odd, sizeof odd);
        iclforge_truehd_encoder_t* encoder = nullptr;
        CHECK(iclforge_truehd_encoder_create(&config, &encoder) == ICLFORGE_ERROR_INVALID_ARGUMENT);
        CHECK(std::string_view(iclforge_truehd_encoder_refusal_reason(&config)).find("downmix") !=
              std::string_view::npos);
    }

    SECTION("PCM outside the declared width is bad input, with a sentence") {
        config.bits = 16;
        Encoder encoder(config);
        const std::array<std::int32_t, 4> pcm{40000, -40000, 0, 0};
        Encoded encoded;
        iclforge_truehd_encoded_t* sentinel = reinterpret_cast<iclforge_truehd_encoded_t*>(&config);
        encoded.handle = sentinel;
        const iclforge_status_t status =
            iclforge_truehd_encoder_encode(encoder.handle, pcm.data(), 2, &encoded.handle);
        CHECK(status == ICLFORGE_ERROR_TRUEHD_ENCODE_BAD_INPUT);
        CHECK(encoded.handle == sentinel);  // untouched on an error
        encoded.handle = nullptr;
        CHECK_FALSE(
            std::string_view(iclforge_truehd_encoder_last_error_detail(encoder.handle)).empty());
        CHECK(std::string_view(iclforge_status_message(status)) == "invalid TrueHD encoder input");
    }

    SECTION(
        "noise cannot be sent at 1 Mbit/s, the least a peak rate can be: an error, or counted") {
        config.peak_data_rate_bps = 1'000'000;
        config.restart_interval_units = 8;
        std::vector<std::int32_t> noise;
        std::uint32_t state = 99;
        for (std::size_t i = 0; i < 48000 * 2; ++i) {
            state = state * 1664525U + 1013904223U;
            noise.push_back(static_cast<std::int32_t>(state >> 9U) - (1 << 22));
        }
        {
            Encoder encoder(config);
            iclforge_status_t status = ICLFORGE_OK;
            Encoded encoded;
            status = iclforge_truehd_encoder_encode(encoder.handle, noise.data(), 48000,
                                                    &encoded.handle);
            if (status == ICLFORGE_OK) {
                Encoded last;
                status = iclforge_truehd_encoder_flush(encoder.handle, &last.handle);
            }
            CHECK(status == ICLFORGE_ERROR_TRUEHD_ENCODE_RATE_EXCEEDED);
        }
        config.allow_rate_violations = 1;
        Encoder lenient(config);
        Encoded encoded;
        REQUIRE(iclforge_truehd_encoder_encode(lenient.handle, noise.data(), 48000,
                                               &encoded.handle) == ICLFORGE_OK);
        Encoded last;
        REQUIRE(iclforge_truehd_encoder_flush(lenient.handle, &last.handle) == ICLFORGE_OK);
        iclforge_truehd_encoder_stats_t stats;
        iclforge_truehd_encoder_stats(lenient.handle, &stats);
        CHECK(stats.rate_violations > 0);
    }
}

TEST_CASE("the TrueHD encoder entry points answer a NULL and an index past the end",
          "[capi][truehd]") {
    iclforge_truehd_encoder_config_t config;
    iclforge_truehd_encoder_config_init(&config);
    iclforge_truehd_encoder_t* encoder = nullptr;
    iclforge_truehd_encoded_t* encoded = nullptr;
    const std::array<std::int32_t, 2> pcm{};

    CHECK(iclforge_truehd_encoder_create(nullptr, &encoder) == ICLFORGE_ERROR_INVALID_ARGUMENT);
    CHECK(iclforge_truehd_encoder_create(&config, nullptr) == ICLFORGE_ERROR_INVALID_ARGUMENT);
    CHECK(std::string_view(iclforge_truehd_encoder_refusal_reason(nullptr)).empty());
    CHECK(iclforge_truehd_encoder_encode(nullptr, pcm.data(), 1, &encoded) ==
          ICLFORGE_ERROR_INVALID_ARGUMENT);
    CHECK(iclforge_truehd_encoder_flush(nullptr, &encoded) == ICLFORGE_ERROR_INVALID_ARGUMENT);
    CHECK(std::string_view(iclforge_truehd_encoder_last_error_detail(nullptr)).empty());
    CHECK(iclforge_truehd_encoder_samples_per_access_unit(nullptr) == 0);
    CHECK(iclforge_truehd_encoder_substreams(nullptr) == 0);
    iclforge_truehd_encoder_stats_t stats;
    iclforge_truehd_encoder_stats(nullptr, &stats);  // documented no-ops
    iclforge_truehd_encoder_destroy(nullptr);

    {
        Encoder live(config);
        CHECK(iclforge_truehd_encoder_encode(live.handle, nullptr, 1, &encoded) ==
              ICLFORGE_ERROR_INVALID_ARGUMENT);
        CHECK(iclforge_truehd_encoder_encode(live.handle, pcm.data(), 1, nullptr) ==
              ICLFORGE_ERROR_INVALID_ARGUMENT);
        CHECK(iclforge_truehd_encoder_flush(live.handle, nullptr) ==
              ICLFORGE_ERROR_INVALID_ARGUMENT);
        // No frames is a call that completes nothing, and takes no PCM pointer.
        CHECK(iclforge_truehd_encoder_encode(live.handle, nullptr, 0, &encoded) == ICLFORGE_OK);
        CHECK(encoded == nullptr);
        // A flush with nothing held gives no unit either.
        CHECK(iclforge_truehd_encoder_flush(live.handle, &encoded) == ICLFORGE_OK);
        CHECK(encoded == nullptr);
    }

    CHECK(iclforge_truehd_encoded_count(nullptr) == 0);
    CHECK(iclforge_truehd_encoded_data(nullptr, 0) == nullptr);
    CHECK(iclforge_truehd_encoded_size(nullptr, 0) == 0);
    CHECK(iclforge_truehd_encoded_samples(nullptr, 0) == 0);
    CHECK(iclforge_truehd_encoded_padding_samples(nullptr, 0) == 0);
    CHECK(iclforge_truehd_encoded_has_major_sync(nullptr, 0) == 0);
    iclforge_truehd_encoded_destroy(nullptr);
}

TEST_CASE("an index past the last encoded unit is 0 or NULL", "[capi][truehd]") {
    iclforge_truehd_encoder_config_t config;
    iclforge_truehd_encoder_config_init(&config);
    config.restart_interval_units = 8;
    Encoder encoder(config);
    const std::vector<std::int32_t> silence(2 * 4000, 0);
    Encoded encoded;
    REQUIRE(iclforge_truehd_encoder_encode(encoder.handle, silence.data(), 4000, &encoded.handle) ==
            ICLFORGE_OK);
    Encoded last;
    REQUIRE(iclforge_truehd_encoder_flush(encoder.handle, &last.handle) == ICLFORGE_OK);
    REQUIRE(last.handle != nullptr);
    const std::size_t count = iclforge_truehd_encoded_count(last.handle);
    CHECK(iclforge_truehd_encoded_data(last.handle, count) == nullptr);
    CHECK(iclforge_truehd_encoded_size(last.handle, count) == 0);
    CHECK(iclforge_truehd_encoded_size(last.handle, count - 1) > 0);
    CHECK(iclforge_truehd_encoded_samples(last.handle, 0) == 40);
}

TEST_CASE("every TrueHD encoder status has its own message", "[capi][truehd]") {
    std::vector<std::string> seen;
    for (int status = ICLFORGE_ERROR_TRUEHD_ENCODE_INVALID_CONFIG;
         status <= ICLFORGE_ERROR_TRUEHD_ENCODE_INTERNAL; ++status) {
        const std::string message = iclforge_status_message(static_cast<iclforge_status_t>(status));
        INFO(status);
        CHECK_FALSE(message.empty());
        CHECK(message != "unknown status");
        CHECK(std::find(seen.begin(), seen.end(), message) == seen.end());
        seen.push_back(message);
    }
}
