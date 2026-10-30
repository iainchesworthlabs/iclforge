#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/elementary.hpp"
#include "iclforge/truehd/io/scan.hpp"

// The scan of a stream (io/scan.hpp), against the streams of testdata/truehd: the facts the manifest
// states of each, the restarts the scan lists, and the claim they are listed on, that a decoder
// started at one gives the samples an unbroken decode gives from there on.

namespace {

using namespace iclforge::truehd;

std::string truehd_dir() { return ICLFORGE_GOLDEN_TRUEHD_DIR; }

std::vector<std::byte> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    std::ranges::transform(raw, bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}

nlohmann::json manifest() {
    std::ifstream in(truehd_dir() + "/manifest.json");
    REQUIRE(in.good());
    return nlohmann::json::parse(in);
}

std::vector<std::byte> fixture_bytes(const nlohmann::json& fixture) {
    return read_file(truehd_dir() + "/" + fixture["file"].get<std::string>());
}

// The PCM of a decode that starts at `data[0]` and runs to the end, interleaved, with the padding
// the last unit counts left out as the decoder leaves it.
struct Decode {
    int channels = 0;
    std::vector<std::int32_t> pcm;
};

Decode decode_from(std::span<const std::byte> data) {
    Decode out;
    Decoder decoder;
    ElementaryReader reader(data);
    while (const auto unit = reader.next()) {
        const auto decoded = decoder.decode(*unit);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        out.channels = (*decoded)->channels;
        out.pcm.insert(out.pcm.end(), (*decoded)->pcm.begin(), (*decoded)->pcm.end());
    }
    return out;
}

}  // namespace

TEST_CASE("scan: every FFmpeg stream is described as its manifest describes it", "[truehd][scan]") {
    for (const nlohmann::json& fixture : manifest()["fixtures"]) {
        DYNAMIC_SECTION(fixture["name"].get<std::string>()) {
            const std::vector<std::byte> stream = fixture_bytes(fixture);
            const auto scan = scan_stream(stream);
            REQUIRE(scan.has_value());
            CHECK(scan->sample_rate == fixture["sample_rate"].get<std::uint32_t>());
            CHECK(scan->units == fixture["access_units"].get<std::uint64_t>());
            // The samples are exact: whole units, less the padding the last unit's marker counts.
            CHECK(scan->samples() == fixture["pcm_samples"].get<std::uint64_t>());
            CHECK(scan->samples() + scan->padding_samples == scan->units * scan->samples_per_unit);
            CHECK(scan->skipped_bytes == 0);
            CHECK(scan->leading_bytes == 0);
            CHECK(scan->unit_bytes == fixture["stream_bytes"].get<std::uint64_t>());
            REQUIRE_FALSE(scan->restarts.empty());
            CHECK(scan->restarts.front() == RestartPoint{.unit = 0, .offset = 0});
            CHECK(scan->channels[static_cast<std::size_t>(scan->num_substreams) - 1U] ==
                  fixture["channels"].get<int>());
            CHECK(scan->duration_seconds() ==
                  Catch::Approx(static_cast<double>(fixture["pcm_samples"].get<std::uint64_t>()) /
                                fixture["sample_rate"].get<double>()));
        }
    }
}

TEST_CASE("scan: the padding of the last unit is what makes the sample count exact",
          "[truehd][scan]") {
    // 44.1 kHz: 276 units of 40 samples is 11,040, and the stream codes 11,025.
    const auto scan = scan_stream(read_file(truehd_dir() + "/truehd_st44_s24.thd"));
    REQUIRE(scan.has_value());
    CHECK(scan->units == 276);
    CHECK(scan->padding_samples == 15);
    CHECK(scan->samples() == 11025);
}

TEST_CASE("scan: a restart is listed for each unit with a major sync, thinned by the spacing",
          "[truehd][scan]") {
    // truehd_st48_s24.thd: 300 units, a restart header and a major sync every 16.
    const std::vector<std::byte> stream = read_file(truehd_dir() + "/truehd_st48_s24.thd");

    const auto all = scan_stream(stream, ScanOptions{.restart_spacing = 1});
    REQUIRE(all.has_value());
    CHECK(all->major_syncs == 19);
    REQUIRE(all->restarts.size() == 19);
    for (std::size_t i = 0; i < all->restarts.size(); ++i) {
        CHECK(all->restarts[i].unit == i * 16);
    }
    // A spacing of 0 is a spacing of 1.
    const auto zero = scan_stream(stream, ScanOptions{.restart_spacing = 0});
    REQUIRE(zero.has_value());
    CHECK(zero->restarts == all->restarts);

    // Kept at least 64 units apart: 0, 64, 128, 192 and 256.
    const auto thin = scan_stream(stream, ScanOptions{.restart_spacing = 64});
    REQUIRE(thin.has_value());
    CHECK(thin->major_syncs == 19);
    std::vector<std::uint64_t> units;
    for (const RestartPoint& restart : thin->restarts) {
        units.push_back(restart.unit);
    }
    CHECK(units == std::vector<std::uint64_t>{0, 64, 128, 192, 256});

    // Looking a unit up gives the last one kept at or before it.
    CHECK(thin->restart_at_or_before(0).unit == 0);
    CHECK(thin->restart_at_or_before(63).unit == 0);
    CHECK(thin->restart_at_or_before(64).unit == 64);
    CHECK(thin->restart_at_or_before(299).unit == 256);

    // The default is the same thinning the Hearth player has always used.
    const auto standard = scan_stream(stream);
    REQUIRE(standard.has_value());
    CHECK(standard->restarts.size() == 2);
}

TEST_CASE("scan: time and sample numbers map to units and back", "[truehd][scan]") {
    const auto scan = scan_stream(read_file(truehd_dir() + "/truehd_st48_s24.thd"));
    REQUIRE(scan.has_value());
    REQUIRE(scan->samples_per_unit == 40);
    CHECK(scan->unit_start_sample(0) == 0);
    CHECK(scan->unit_start_sample(3) == 120);
    CHECK(scan->unit_at_sample(0) == 0);
    CHECK(scan->unit_at_sample(39) == 0);
    CHECK(scan->unit_at_sample(40) == 1);
    CHECK(scan->unit_at_sample(scan->samples() - 1) == 299);
    // The first sample past the end belongs to no unit.
    CHECK_FALSE(scan->unit_at_sample(scan->samples()).has_value());
    CHECK_FALSE(scan->restart_for_sample(scan->samples()).has_value());

    CHECK(scan->sample_at_seconds(0.0) == 0);
    CHECK(scan->sample_at_seconds(-3.0) == 0);
    CHECK(scan->sample_at_seconds(0.125) == 6000);
    CHECK(scan->sample_at_seconds(1.0) == 48000);
    CHECK_FALSE(scan->unit_at_sample(scan->sample_at_seconds(1.0)).has_value());
}

TEST_CASE("scan: a decoder started at the restart for a sample gives the sample an unbroken decode "
          "does",
          "[truehd][scan]") {
    // The restart is the seek granule. This is the claim it is listed on: join there, decode on,
    // and the samples from the wanted one are those of the whole decode.
    for (const nlohmann::json& fixture : manifest()["fixtures"]) {
        DYNAMIC_SECTION(fixture["name"].get<std::string>()) {
            const std::vector<std::byte> stream = fixture_bytes(fixture);
            const auto scan = scan_stream(stream, ScanOptions{.restart_spacing = 1});
            REQUIRE(scan.has_value());
            const Decode whole = decode_from(stream);
            REQUIRE(whole.channels > 0);
            const std::uint64_t samples = scan->samples();
            const auto channels = static_cast<std::size_t>(whole.channels);
            for (const std::uint64_t sample : {std::uint64_t{0}, std::uint64_t{1}, samples / 3,
                                               samples / 2 + 7, samples - 1}) {
                const auto restart = scan->restart_for_sample(sample);
                REQUIRE(restart.has_value());
                CHECK(scan->unit_start_sample(restart->unit) <= sample);
                const Decode joined = decode_from(std::span(stream).subspan(restart->offset));
                const std::uint64_t first = scan->unit_start_sample(restart->unit);
                // A frame is `channels` values; the joined decode starts at the restart's unit.
                const std::size_t at = static_cast<std::size_t>(sample - first) * channels;
                REQUIRE(at + channels <= joined.pcm.size());
                CHECK(std::equal(joined.pcm.begin() + static_cast<std::ptrdiff_t>(at),
                                 joined.pcm.begin() + static_cast<std::ptrdiff_t>(at + channels),
                                 whole.pcm.begin() +
                                     static_cast<std::ptrdiff_t>(static_cast<std::size_t>(sample) *
                                                                 channels)));
            }
        }
    }
}

TEST_CASE("scan: bytes ahead of the first unit are counted and shift every offset",
          "[truehd][scan]") {
    const std::vector<std::byte> clean = read_file(truehd_dir() + "/truehd_st48_s24.thd");
    std::vector<std::byte> stream(37, std::byte{0x55});
    stream.insert(stream.end(), clean.begin(), clean.end());

    const auto reference = scan_stream(clean, ScanOptions{.restart_spacing = 1});
    const auto shifted = scan_stream(stream, ScanOptions{.restart_spacing = 1});
    REQUIRE(reference.has_value());
    REQUIRE(shifted.has_value());
    CHECK(shifted->leading_bytes == 37);
    CHECK(shifted->skipped_bytes == 37);
    CHECK(shifted->units == reference->units);
    CHECK(shifted->unit_bytes == reference->unit_bytes);
    REQUIRE(shifted->restarts.size() == reference->restarts.size());
    for (std::size_t i = 0; i < shifted->restarts.size(); ++i) {
        CHECK(shifted->restarts[i].unit == reference->restarts[i].unit);
        CHECK(shifted->restarts[i].offset == reference->restarts[i].offset + 37);
    }
}

TEST_CASE("scan: damage is passed over as the reader passes it, and counted", "[truehd][scan]") {
    std::vector<std::byte> stream = read_file(truehd_dir() + "/truehd_st48_s24.thd");
    const auto clean = scan_stream(stream, ScanOptions{.restart_spacing = 1});
    REQUIRE(clean.has_value());
    // Spoil the check nibble of the unit at the third restart: the reader passes it, resyncs at the
    // next major sync, and the scan counts what it passed.
    const RestartPoint victim = clean->restarts[2];
    stream[static_cast<std::size_t>(victim.offset) + 1] ^= std::byte{0xF0};
    const auto damaged = scan_stream(stream, ScanOptions{.restart_spacing = 1});
    REQUIRE(damaged.has_value());
    CHECK(damaged->skipped_bytes > 0);
    CHECK(damaged->units < clean->units);
    CHECK(damaged->restarts.front() == clean->restarts.front());
    // Every restart it lists is a place a decoder can start: a unit with a major sync that reads.
    for (const RestartPoint& restart : damaged->restarts) {
        ElementaryReader reader(std::span(stream).subspan(restart.offset));
        const auto unit = reader.next();
        REQUIRE(unit.has_value());
        CHECK(reader.skipped_bytes() == 0);
    }
}

TEST_CASE("scan: bytes that are not a TrueHD stream have no scan", "[truehd][scan]") {
    const std::vector<std::byte> junk(4096, std::byte{0x5A});
    const auto scan = scan_stream(junk);
    REQUIRE_FALSE(scan.has_value());
    CHECK(scan.error().kind == ErrorKind::kBadSync);
    CHECK_FALSE(scan_stream({}).has_value());
}
