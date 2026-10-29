#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

#include "iclforge/truehd/io/elementary.hpp"
#include "support/stream_builder.hpp"

using namespace iclforge::truehd;
using namespace iclforge::truehd::testing;

namespace {

std::vector<std::byte> stream_of(int units) {
    std::vector<std::byte> stream;
    for (int n = 0; n < units; ++n) {
        const std::vector<std::byte> unit = stereo_unit(n == 0, static_cast<std::uint16_t>(n * 40));
        stream.insert(stream.end(), unit.begin(), unit.end());
    }
    return stream;
}

}  // namespace

TEST_CASE("the reader hands out the units of a stream", "[truehd][io]") {
    const std::vector<std::byte> stream = stream_of(5);
    ElementaryReader reader(stream);
    int count = 0;
    std::size_t total = 0;
    while (const auto unit = reader.next()) {
        total += unit->size();
        ++count;
    }
    CHECK(count == 5);
    CHECK(total == stream.size());
    CHECK(reader.units_read() == 5);
    CHECK(reader.skipped_bytes() == 0);
}

TEST_CASE("the reader starts at the first major sync and counts what it passed", "[truehd][io]") {
    std::vector<std::byte> stream(37, std::byte{0x55});
    const std::vector<std::byte> body = stream_of(3);
    stream.insert(stream.end(), body.begin(), body.end());
    ElementaryReader reader(stream);
    int count = 0;
    while (reader.next()) {
        ++count;
    }
    CHECK(count == 3);
    CHECK(reader.skipped_bytes() == 37);
}

TEST_CASE("the reader finds its way back after a broken unit", "[truehd][io]") {
    // Two good units, then a unit whose check nibble is wrong, then a unit that carries a major
    // sync again.
    std::vector<std::byte> stream = stream_of(3);
    const std::size_t third = stereo_unit(true).size() + stereo_unit(false).size();
    stream[third] ^= std::byte{0x10};
    const std::vector<std::byte> tail = stereo_unit(true);
    stream.insert(stream.end(), tail.begin(), tail.end());
    ElementaryReader reader(stream);
    int count = 0;
    while (reader.next()) {
        ++count;
    }
    CHECK(count == 3);
    CHECK(reader.skipped_bytes() == stereo_unit(false).size());
}

TEST_CASE("a truncated last unit ends the stream", "[truehd][io]") {
    std::vector<std::byte> stream = stream_of(2);
    stream.resize(stream.size() - 4);
    ElementaryReader reader(stream);
    int count = 0;
    while (reader.next()) {
        ++count;
    }
    CHECK(count == 1);
}

TEST_CASE("probing a stream reports its format and its units", "[truehd][io]") {
    const std::vector<std::byte> stream = stream_of(6);
    auto info = probe_stream(stream);
    REQUIRE(info.has_value());
    CHECK(info->sample_rate == 48000);
    CHECK(info->num_substreams == 1);
    CHECK(info->channels[0] == 2);
    CHECK(info->samples_per_access_unit == 40);
    CHECK(info->access_units == 6);
    CHECK(info->first_unit_offset == 0);
    CHECK_FALSE(info->has_object_substream);

    const std::vector<std::byte> nothing(100, std::byte{0});
    CHECK(probe_stream(nothing).error().kind == ErrorKind::kBadSync);
}
