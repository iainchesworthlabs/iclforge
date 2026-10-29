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

TEST_CASE("bytes ahead of the first access unit are skipped and counted as leading",
          "[truehd][io]") {
    // A timecode or other packet some muxers put in front of the stream, of every length and
    // content here: the stream starts at its first access unit, and none of the bytes is a loss of
    // the stream (leading_bytes is the part of skipped_bytes before the first unit).
    const std::vector<std::byte> body = stream_of(4);
    for (const std::size_t junk : {1U, 3U, 37U, 188U, 4000U}) {
        for (const unsigned fill : {0x00U, 0x55U, 0xFFU, 0xF8U}) {
            CAPTURE(junk, fill);
            std::vector<std::byte> stream(junk, static_cast<std::byte>(fill));
            stream.insert(stream.end(), body.begin(), body.end());
            REQUIRE(find_stream_start(stream) == junk);
            ElementaryReader reader(stream);
            std::size_t total = 0;
            int count = 0;
            while (const auto unit = reader.next()) {
                total += unit->size();
                ++count;
            }
            CHECK(count == 4);
            CHECK(total == body.size());
            CHECK(reader.skipped_bytes() == junk);
            CHECK(reader.leading_bytes() == junk);
            auto info = probe_stream(stream);
            REQUIRE(info.has_value());
            CHECK(info->first_unit_offset == junk);
            CHECK(info->access_units == 4);
        }
    }
    // Loss after the first unit is not leading.
    std::vector<std::byte> damaged(10, std::byte{0x55});
    damaged.insert(damaged.end(), body.begin(), body.end());
    damaged[10 + stereo_unit(true).size() + stereo_unit(false).size()] ^= std::byte{0x10};
    ElementaryReader reader(damaged);
    while (reader.next()) {
    }
    CHECK(reader.leading_bytes() == 10);
    CHECK(reader.skipped_bytes() > 10);
}

TEST_CASE("bytes that only look like a major sync are not taken for a stream", "[truehd][io]") {
    const std::vector<std::byte> body = stream_of(4);
    const std::vector<std::byte> good_unit = stereo_unit(true);

    SECTION("a sync word with a wrong CRC") {
        std::vector<std::byte> stream = good_unit;
        stream[4 + 10] ^= std::byte{0x01};  // inside the major sync, which its CRC covers
        stream.insert(stream.begin(), 20, std::byte{0});
        CHECK_FALSE(find_stream_start(stream).has_value());
        ElementaryReader reader(stream);
        CHECK_FALSE(reader.next().has_value());
        CHECK(probe_stream(stream).error().kind == ErrorKind::kBadSync);
    }
    SECTION("a good unit followed by something that is not one is skipped, the real stream found") {
        std::vector<std::byte> stream(8, std::byte{0x11});
        stream.insert(stream.end(), good_unit.begin(), good_unit.end());
        stream.insert(stream.end(), 50, std::byte{0xA5});  // the second unit does not read
        const std::size_t real = stream.size();
        stream.insert(stream.end(), body.begin(), body.end());
        CHECK(find_stream_start(stream) == real);
        ElementaryReader reader(stream);
        int count = 0;
        while (reader.next()) {
            ++count;
        }
        CHECK(count == 4);
        CHECK(reader.leading_bytes() == real);
    }
    SECTION("a single unit after junk is a stream when nothing else follows it") {
        std::vector<std::byte> stream(9, std::byte{0x22});
        stream.insert(stream.end(), good_unit.begin(), good_unit.end());
        CHECK(find_stream_start(stream) == 9);
        // ... and so is the start of a longer stream cut inside its second unit.
        std::vector<std::byte> cut(9, std::byte{0x22});
        cut.insert(cut.end(), body.begin(), body.begin() + static_cast<std::ptrdiff_t>(
                                                                good_unit.size() + 10));
        CHECK(find_stream_start(cut) == 9);
    }
    SECTION("a stream that starts at once needs no second unit") {
        CHECK(find_stream_start(good_unit) == 0);
    }
    SECTION("the search stops after kMaxLeadingBytes") {
        std::vector<std::byte> stream(kMaxLeadingBytes, std::byte{0x33});
        stream.insert(stream.end(), body.begin(), body.end());
        CHECK(find_stream_start(stream) == kMaxLeadingBytes);
        stream.insert(stream.begin(), 1, std::byte{0x33});
        CHECK_FALSE(find_stream_start(stream).has_value());
        CHECK(find_stream_start(stream, kMaxLeadingBytes + 1) == kMaxLeadingBytes + 1);
    }
    SECTION("noise is not a stream") {
        std::vector<std::byte> noise(100000);
        std::uint32_t state = 12345;
        for (std::byte& b : noise) {
            state = state * 1664525U + 1013904223U;
            b = static_cast<std::byte>(state >> 24U);
        }
        CHECK_FALSE(find_stream_start(noise).has_value());
    }
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
