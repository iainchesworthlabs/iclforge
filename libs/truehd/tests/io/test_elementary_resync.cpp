#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

#include "iclforge/truehd/io/elementary.hpp"
#include "support/stream_builder.hpp"

// How the reader finds its way when units do not read: what counts as a unit worth handing out,
// and which major sync is a way back in.

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

int count_units(ElementaryReader& reader) {
    int count = 0;
    while (reader.next()) {
        ++count;
    }
    return count;
}

}  // namespace

TEST_CASE("a restart unit that does not read is passed over for the next one that does",
          "[truehd][io]") {
    // The first unit's header check is wrong, so its major sync is not a way in: the reader and
    // probe_stream start at the unit after it.
    std::vector<std::byte> corrupt = stereo_unit(true);
    corrupt[3] ^= std::byte{0x01};  // input_timing: the check nibble no longer fits
    std::vector<std::byte> stream = corrupt;
    const std::vector<std::byte> tail = stream_of(3);
    stream.insert(stream.end(), tail.begin(), tail.end());

    auto info = probe_stream(stream);
    REQUIRE(info.has_value());
    CHECK(info->first_unit_offset == corrupt.size());
    CHECK(info->access_units == 3);

    ElementaryReader reader(stream);
    CHECK(count_units(reader) == 3);
    CHECK(reader.skipped_bytes() == corrupt.size());
}

TEST_CASE("a corrupt restart unit in the middle costs only itself", "[truehd][io]") {
    // Good, then a unit that does not read, then a corrupt restart unit, then a good restart unit:
    // the way back in is the good one, not the corrupt restart unit on the way.
    std::vector<std::byte> stream = stream_of(2);
    std::vector<std::byte> broken = stereo_unit(false);
    broken[3] ^= std::byte{0x01};
    std::vector<std::byte> corrupt = stereo_unit(true);
    corrupt[3] ^= std::byte{0x01};
    stream.insert(stream.end(), broken.begin(), broken.end());
    stream.insert(stream.end(), corrupt.begin(), corrupt.end());
    const std::vector<std::byte> good = stereo_unit(true);
    stream.insert(stream.end(), good.begin(), good.end());

    ElementaryReader reader(stream);
    CHECK(count_units(reader) == 3);
    CHECK(reader.skipped_bytes() == broken.size() + corrupt.size());
}

TEST_CASE("a unit whose directory runs past its length is not handed out", "[truehd][io]") {
    // The header's check nibble covers the header and the directory, so it is made to fit: the
    // unit reads as far as its framing goes and its segment end pointer points beyond it.
    std::vector<std::byte> stream = stream_of(2);
    std::vector<std::byte> bad = stereo_unit(false);
    bad[5] = std::byte{0xFF};  // the entry follows the 4-byte header; its low 12 bits are the end
    bad[4] = (bad[4] & std::byte{0xF0}) | std::byte{0x0F};
    stream.insert(stream.end(), bad.begin(), bad.end());
    const std::vector<std::byte> good = stereo_unit(true);
    stream.insert(stream.end(), good.begin(), good.end());
    ElementaryReader reader(stream);
    CHECK(count_units(reader) == 3);
}
