// The TrueHD and MLP C entry points (iclforge_truehd_*): the committed FFmpeg fixtures decoded through
// them, held to the SHA-256 of what FFmpeg's strict decode of the same file gives (the codec is
// lossless, so the digests are equal or the decode is wrong), and what a NULL argument, an index past
// the end, a stream that is not TrueHD and a unit that does not decode leave each call to answer. The
// build here has ICLFORGE_BUILD_TRUEHD on; test_capi_truehd_absent.cpp is the other build's.

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

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
// (tools/checks/check_platform_macros.ps1 allows none in tests).
#define ICLFORGE_TEST_STRINGIFY_VALUE(x) #x
#define ICLFORGE_TEST_STRINGIFY(x) ICLFORGE_TEST_STRINGIFY_VALUE(x)

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
    CHECK(std::string_view(ICLFORGE_TEST_STRINGIFY(ICLFORGE_HAS_TRUEHD)).empty());
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
            REQUIRE(iclforge_truehd_decoder_decode(
                        decoder.handle, stream.data() + iclforge_truehd_units_offset(units.handle, 0),
                        iclforge_truehd_units_size(units.handle, 0), &decoded.handle) ==
                    ICLFORGE_OK);
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
        return std::vector<std::uint8_t>(stream.begin() + static_cast<std::ptrdiff_t>(offset),
                                         stream.begin() + static_cast<std::ptrdiff_t>(
                                                              offset + iclforge_truehd_units_size(
                                                                           units.handle, i)));
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

TEST_CASE("the TrueHD entry points answer a NULL, an index past the end and an enumerator that is "
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
