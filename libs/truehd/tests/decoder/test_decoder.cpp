#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/io/elementary.hpp"
#include "support/stream_builder.hpp"

using namespace iclforge::truehd;
using namespace iclforge::truehd::testing;

namespace {

void check_built_samples(const DecodedAccessUnit& unit) {
    REQUIRE(unit.channels == 2);
    REQUIRE(unit.samples == kBuiltBlockSize);
    REQUIRE(unit.pcm.size() == static_cast<std::size_t>(2 * kBuiltBlockSize));
    for (int i = 0; i < unit.samples; ++i) {
        for (int ch = 0; ch < 2; ++ch) {
            CHECK(unit.pcm[static_cast<std::size_t>(i * 2 + ch)] == expected_sample(ch, i));
        }
    }
}

}  // namespace

TEST_CASE("the decoder gives back the samples of a built stream", "[truehd][decoder]") {
    Decoder decoder;
    auto first = decoder.decode(stereo_unit(true));
    REQUIRE(first.has_value());
    REQUIRE(first->has_value());
    CHECK((*first)->sample_rate == 48000);
    CHECK((*first)->restarted);
    CHECK((*first)->substream == 0);
    check_built_samples(**first);
    REQUIRE((*first)->layout.size() == 2);
    CHECK((*first)->layout[0] == Channel::kL);
    CHECK((*first)->layout[1] == Channel::kR);

    // A unit that carries no header keeps the parameters of the one before.
    for (int n = 0; n < 3; ++n) {
        auto next = decoder.decode(stereo_unit(false));
        REQUIRE(next.has_value());
        REQUIRE(next->has_value());
        CHECK_FALSE((*next)->restarted);
        check_built_samples(**next);
    }
    CHECK(decoder.counters().units == 4);
    CHECK(decoder.counters().units_decoded == 4);
    CHECK(decoder.counters().restarts == 1);
    REQUIRE(decoder.major_sync().has_value());
    CHECK(decoder.major_sync()->num_substreams == 1);
}

TEST_CASE("a unit before the first major sync gives nothing and no error", "[truehd][decoder]") {
    Decoder decoder;
    auto waiting = decoder.decode(stereo_unit(false));
    REQUIRE(waiting.has_value());
    CHECK_FALSE(waiting->has_value());
    CHECK(decoder.counters().units_waiting == 1);

    // After the major sync it decodes; reset() forgets the stream again.
    auto started = decoder.decode(stereo_unit(true));
    REQUIRE(started.has_value());
    CHECK(started->has_value());
    decoder.reset();
    auto again = decoder.decode(stereo_unit(false));
    REQUIRE(again.has_value());
    CHECK_FALSE(again->has_value());
}

TEST_CASE("strict decoding refuses a corrupt substream, lenient counts it", "[truehd][decoder]") {
    std::vector<std::byte> bad = stereo_unit(true);
    bad[bad.size() - 5] ^= std::byte{0x01};  // a payload bit: the parity and the CRC no longer fit

    Decoder strict;
    auto refused = strict.decode(bad);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().kind == ErrorKind::kBadParity);

    DecoderConfig config;
    config.strictness = Strictness::kLenient;
    Decoder lenient(config);
    auto counted = lenient.decode(bad);
    REQUIRE(counted.has_value());
    CHECK(counted->has_value());
    CHECK(lenient.counters().check_failures == 2);  // the parity byte and the CRC byte
}

TEST_CASE("a failed substream waits for its next restart header", "[truehd][decoder]") {
    Decoder decoder;
    REQUIRE(decoder.decode(stereo_unit(true)).has_value());
    std::vector<std::byte> bad = stereo_unit(false);
    bad[bad.size() - 1] ^= std::byte{0x40};
    CHECK_FALSE(decoder.decode(bad).has_value());
    auto waiting = decoder.decode(stereo_unit(false));
    REQUIRE(waiting.has_value());
    CHECK_FALSE(waiting->has_value());
    auto restarted = decoder.decode(stereo_unit(true));
    REQUIRE(restarted.has_value());
    CHECK(restarted->has_value());
}

TEST_CASE("the restart header's lossless check is that of the interval before it",
          "[truehd][decoder]") {
    // The built restart headers carry lossless_check 0, which is not the fold of the samples before
    // them.
    Decoder decoder;
    REQUIRE(decoder.decode(stereo_unit(true)).has_value());
    auto second = decoder.decode(stereo_unit(true));
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().kind == ErrorKind::kBadLosslessCheck);
    CHECK(decoder.counters().lossless_check_failures == 1);

    DecoderConfig config;
    config.strictness = Strictness::kLenient;
    Decoder lenient(config);
    REQUIRE(lenient.decode(stereo_unit(true)).has_value());
    auto passed = lenient.decode(stereo_unit(true));
    REQUIRE(passed.has_value());
    CHECK(passed->has_value());
    CHECK(lenient.counters().lossless_check_failures == 1);
}

TEST_CASE("a whole built stream decodes through the elementary reader", "[truehd][decoder]") {
    std::vector<std::byte> stream;
    for (int n = 0; n < 8; ++n) {
        const std::vector<std::byte> unit = stereo_unit(n == 0);
        stream.insert(stream.end(), unit.begin(), unit.end());
    }
    ElementaryReader reader(stream);
    Decoder decoder;
    int decoded = 0;
    while (const auto unit = reader.next()) {
        auto result = decoder.decode(*unit);
        REQUIRE(result.has_value());
        REQUIRE(result->has_value());
        ++decoded;
    }
    CHECK(decoded == 8);
}

TEST_CASE("noise generation is refused, not guessed", "[truehd][decoder]") {
    // TH spec 17 gives the seed and the shift and no generator (ERRATA open question 5): restart sync 0x31EB
    // is kUnsupported, with the decoder left waiting for another restart.
    Decoder decoder;
    auto refused = decoder.decode(stereo_unit(true, 0, kRestartSyncNoise));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().kind == ErrorKind::kUnsupported);
    auto waiting = decoder.decode(stereo_unit(false));
    REQUIRE(waiting.has_value());
    CHECK_FALSE(waiting->has_value());
    auto restarted = decoder.decode(stereo_unit(true));
    REQUIRE(restarted.has_value());
    CHECK(restarted->has_value());
}

TEST_CASE("the sixteen-channel restart sync decodes like the others", "[truehd][decoder]") {
    Decoder decoder;
    auto atmos = decoder.decode(stereo_unit(true, 0, kRestartSyncAtmos));
    REQUIRE(atmos.has_value());
    REQUIRE(atmos->has_value());
    check_built_samples(**atmos);
}
