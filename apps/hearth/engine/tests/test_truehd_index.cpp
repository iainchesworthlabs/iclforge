#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "item_source.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/elementary.hpp"
#include "truehd_index.hpp"
#include "truehd_test_support.hpp"

// The index and the cursor Hearth plays a TrueHD stream through (apps/hearth/engine/src/
// truehd_index.hpp), against the streams of testdata/truehd and the library's own reader.

namespace {

using namespace iclforge::hearth;
using namespace iclforge::hearth::testing;
namespace th = iclforge::truehd;

// A source over memory that remembers the largest read it was asked for and how many it served,
// so that a test can say a stream was read in windows and not whole.
class CountingSource final : public ByteSource {
public:
    explicit CountingSource(std::vector<std::byte> bytes) : inner_(std::move(bytes)) {}
    [[nodiscard]] std::uint64_t size() const override { return inner_.size(); }
    [[nodiscard]] std::size_t read(std::uint64_t offset, std::span<std::byte> out) override {
        ++reads;
        largest = std::max(largest, out.size());
        return inner_.read(offset, out);
    }
    std::size_t reads = 0;
    std::size_t largest = 0;

private:
    MemoryByteSource inner_;
};

std::string fixture_path(const nlohmann::json& fixture) {
    return truehd_dir() + "/" + fixture["file"].get<std::string>();
}

// The units of a stream, and the numbers of those that carry a major sync, by the library's reader.
struct Reference {
    std::vector<std::vector<std::byte>> units;
    std::vector<std::size_t> sync_units;
    std::size_t skipped = 0;
};

Reference reference_of(const std::vector<std::byte>& stream) {
    Reference out;
    th::ElementaryReader reader(stream);
    int substreams = 0;
    while (const auto unit = reader.next()) {
        const auto parsed = th::parse_access_unit(*unit, substreams, true);
        REQUIRE(parsed.has_value());
        if (parsed->major_sync) {
            substreams = parsed->major_sync->num_substreams;
            out.sync_units.push_back(out.units.size());
        }
        out.units.emplace_back(unit->begin(), unit->end());
    }
    out.skipped = reader.skipped_bytes();
    return out;
}

}  // namespace

TEST_CASE("truehd stream: the index of every FFmpeg stream states what the manifest states of it",
          "[hearth][truehd]") {
    for (const nlohmann::json& fixture : manifest()["fixtures"]) {
        DYNAMIC_SECTION(fixture["name"].get<std::string>()) {
            MemoryByteSource source(read_file(fixture_path(fixture)));
            const auto index = index_truehd(source);
            REQUIRE(index.has_value());
            CHECK(index->sample_rate == fixture["sample_rate"].get<std::uint32_t>());
            CHECK(index->units == fixture["access_units"].get<std::uint64_t>());
            // The samples are exact: whole units, less the padding the last unit's marker counts.
            CHECK(index->samples() == fixture["pcm_samples"].get<std::uint64_t>());
            CHECK(index->samples() + index->padding_samples ==
                  index->units * index->samples_per_unit);
            CHECK(index->skipped_bytes == 0);
            CHECK(index->unit_bytes == fixture["stream_bytes"].get<std::uint64_t>());
            REQUIRE_FALSE(index->restarts.empty());
            CHECK(index->restarts.front().unit == 0);
            CHECK(index->restarts.front().offset == 0);
            CHECK(index->channels_of(th::Presentation::kBest) == fixture["channels"].get<int>());
        }
    }
}

TEST_CASE("truehd stream: the padding of the last unit is what makes the sample count exact",
          "[hearth][truehd]") {
    // 44.1 kHz: 276 units of 40 samples is 11,040, and the stream codes 11,025.
    MemoryByteSource source(read_file(truehd_dir() + "/truehd_st44_s24.thd"));
    const auto index = index_truehd(source);
    REQUIRE(index.has_value());
    CHECK(index->units == 276);
    CHECK(index->padding_samples == 15);
    CHECK(index->samples() == 11025);
}

TEST_CASE("truehd stream: a restart is listed for each unit with a major sync, thinned by the spacing",
          "[hearth][truehd]") {
    // truehd_st48_s24.thd: 300 units, a restart header and a major sync every 16.
    const std::vector<std::byte> stream = read_file(truehd_dir() + "/truehd_st48_s24.thd");
    const Reference reference = reference_of(stream);
    REQUIRE(reference.sync_units.size() == 19);

    MemoryByteSource all_source(stream);
    const auto all = index_truehd(all_source, 1);
    REQUIRE(all.has_value());
    CHECK(all->major_syncs == 19);
    REQUIRE(all->restarts.size() == 19);
    for (std::size_t i = 0; i < 19; ++i) {
        CHECK(all->restarts[i].unit == reference.sync_units[i]);
    }

    // Kept at least 64 units apart: 0, 64, 128, 192 and 256.
    MemoryByteSource thin_source(stream);
    const auto thin = index_truehd(thin_source, 64);
    REQUIRE(thin.has_value());
    CHECK(thin->major_syncs == 19);
    std::vector<std::uint64_t> units;
    for (const TruehdRestart& restart : thin->restarts) {
        units.push_back(restart.unit);
    }
    CHECK(units == std::vector<std::uint64_t>{0, 64, 128, 192, 256});

    // Looking a unit up gives the last one kept at or before it.
    CHECK(thin->restart_at_or_before(0).unit == 0);
    CHECK(thin->restart_at_or_before(63).unit == 0);
    CHECK(thin->restart_at_or_before(64).unit == 64);
    CHECK(thin->restart_at_or_before(299).unit == 256);
}

TEST_CASE("truehd stream: a decoder joins at every unit with a major sync, and decodes what an unbroken decode does",
          "[hearth][truehd]") {
    // The index lists units with a major sync as the places to join, on the strength of this: a
    // decoder started there waits for nothing and gives the samples the whole decode gives.
    for (const nlohmann::json& fixture : manifest()["fixtures"]) {
        DYNAMIC_SECTION(fixture["name"].get<std::string>()) {
            const std::vector<std::byte> stream = read_file(fixture_path(fixture));
            const Reference reference = reference_of(stream);
            th::Decoder whole;
            std::vector<std::vector<std::int32_t>> expected;
            for (const auto& unit : reference.units) {
                const auto decoded = whole.decode(unit);
                REQUIRE(decoded.has_value());
                REQUIRE(decoded->has_value());
                expected.emplace_back((*decoded)->pcm.begin(), (*decoded)->pcm.end());
            }
            REQUIRE_FALSE(reference.sync_units.empty());
            for (const std::size_t from : reference.sync_units) {
                th::Decoder joined;
                for (std::size_t unit = from; unit < reference.units.size() && unit < from + 40;
                     ++unit) {
                    const auto decoded = joined.decode(reference.units[unit]);
                    REQUIRE(decoded.has_value());
                    REQUIRE(decoded->has_value());
                    REQUIRE(std::ranges::equal((*decoded)->pcm, expected[unit]));
                }
            }
        }
    }
}

TEST_CASE("truehd stream: the cursor hands out the units the library's reader does, a window at a time",
          "[hearth][truehd]") {
    const std::vector<std::byte> stream = read_file(truehd_dir() + "/truehd_m51_s24.thd");
    const Reference reference = reference_of(stream);
    REQUIRE(stream.size() > 100 * 1024);

    CountingSource source(stream);
    constexpr std::size_t kWindow = 32 * 1024;
    TruehdCursor cursor(source, kWindow);
    REQUIRE(cursor.rewind());
    std::size_t count = 0;
    while (const auto unit = cursor.next()) {
        REQUIRE(count < reference.units.size());
        REQUIRE(std::ranges::equal(*unit, reference.units[count]));
        CHECK(cursor.last_unit() == count);
        CHECK(cursor.last_has_major_sync() ==
              std::ranges::binary_search(reference.sync_units, count));
        ++count;
    }
    CHECK(count == reference.units.size());
    CHECK(cursor.skipped_bytes() == 0);
    // The stream was read in windows: never more than one asked for, and more than one of them.
    CHECK(source.largest <= kWindow);
    CHECK(source.reads >= stream.size() / kWindow);
}

TEST_CASE("truehd stream: the index reads the source in windows and never holds it whole",
          "[hearth][truehd]") {
    CountingSource source(read_file(truehd_dir() + "/truehd_m51_s24.thd"));
    constexpr std::size_t kWindow = 32 * 1024;
    const auto index = index_truehd(source, kTruehdRestartSpacing, kWindow);
    REQUIRE(index.has_value());
    CHECK(index->units == 300);
    CHECK(source.largest <= kWindow);
    CHECK(source.reads > 3);
}

TEST_CASE("truehd stream: the cursor seeks to a restart and reads on from there", "[hearth][truehd]") {
    const std::vector<std::byte> stream = read_file(truehd_dir() + "/truehd_st48_s24.thd");
    const Reference reference = reference_of(stream);
    MemoryByteSource source(stream);
    const auto index = index_truehd(source, 1);
    REQUIRE(index.has_value());
    TruehdCursor cursor(source);
    for (const TruehdRestart& restart : index->restarts) {
        cursor.seek(restart);
        const auto unit = cursor.next();
        REQUIRE(unit.has_value());
        CHECK(cursor.last_unit() == restart.unit);
        CHECK(cursor.last_offset() == restart.offset);
        CHECK(std::ranges::equal(*unit, reference.units[restart.unit]));
        CHECK(cursor.last_has_major_sync());
    }
}

TEST_CASE("truehd stream: damage is passed over as the library's reader passes it, and counted",
          "[hearth][truehd]") {
    std::vector<std::byte> stream = read_file(truehd_dir() + "/truehd_st48_s24.thd");
    const Reference clean = reference_of(stream);
    // Break the check nibble of unit 20, which the reader cannot read as a unit.
    std::size_t offset = 0;
    for (std::size_t i = 0; i < 20; ++i) {
        offset += clean.units[i].size();
    }
    stream[offset] = static_cast<std::byte>(std::to_integer<unsigned>(stream[offset]) ^ 0xF0U);
    const Reference damaged = reference_of(stream);
    REQUIRE(damaged.skipped > 0);

    for (const std::size_t window : {std::size_t{16 * 1024}, std::size_t{256 * 1024}}) {
        MemoryByteSource source(stream);
        TruehdCursor cursor(source, window);
        REQUIRE(cursor.rewind());
        std::size_t count = 0;
        while (const auto unit = cursor.next()) {
            REQUIRE(count < damaged.units.size());
            CHECK(std::ranges::equal(*unit, damaged.units[count]));
            ++count;
        }
        CHECK(count == damaged.units.size());
        CHECK(cursor.skipped_bytes() == damaged.skipped);

        MemoryByteSource indexed(stream);
        const auto index = index_truehd(indexed, 1, window);
        REQUIRE(index.has_value());
        CHECK(index->units == damaged.units.size());
        CHECK(index->skipped_bytes == damaged.skipped);
    }
}

TEST_CASE("truehd stream: bytes that are not a TrueHD stream have no index", "[hearth][truehd]") {
    MemoryByteSource noise(std::vector<std::byte>(5000, std::byte{0x5A}));
    const auto index = index_truehd(noise);
    REQUIRE_FALSE(index.has_value());
    CHECK(index.error().find("major sync") != std::string::npos);

    MemoryByteSource empty{std::vector<std::byte>{}};
    CHECK_FALSE(index_truehd(empty).has_value());
}
