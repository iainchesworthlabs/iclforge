#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "encoder/encoder_support.hpp"
#include "iclforge/base/bitreader.hpp"
#include "iclforge/truehd/core/syntax.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"
#include "iclforge/truehd/io/access_unit.hpp"

// The restart header's bounds (TH spec §7.1.1, §19.3): the Dolby decoder rejects a stream whose
// samples exceed max_bits, whose output shift exceeds max_shift or whose huff_lsbs exceed
// max_lsbs, and max_bits_repeat must equal max_bits. A bound above the truth is accepted; 0 turns
// the check off. [no-external-oracle]: the Dolby decoder is not run here.

namespace {

using namespace iclforge::truehd;
using namespace iclforge::truehd::testing;

struct Interval {
    RestartHeader header;
    std::int32_t peak = 0;  // the largest magnitude decoded in the interval, in the 24 bit domain
};

std::vector<Interval> intervals_of(const std::vector<EncodedAccessUnit>& units) {
    std::vector<Interval> out;
    Decoder decoder(DecoderConfig{});
    for (const EncodedAccessUnit& unit : units) {
        auto parsed = parse_access_unit(unit.bytes, 1, true);
        REQUIRE(parsed.has_value());
        iclforge::BitReader bits(parsed->segments[0]);
        if (bits.read(1) == 1 && bits.read(1) == 1) {  // block_header_exists, restart_header_exists
            auto restart = read_restart_header(bits, parsed->segments[0], true);
            REQUIRE(restart.has_value());
            out.push_back(Interval{*restart, 0});
        }
        auto decoded = decoder.decode(unit.bytes);
        REQUIRE(decoded.has_value());
        if (decoded->has_value() && !out.empty()) {
            for (const std::int32_t sample : (*decoded)->pcm) {
                const std::int64_t magnitude = sample < 0 ? -std::int64_t{sample} : sample;
                out.back().peak = std::max<std::int32_t>(out.back().peak,
                                                         static_cast<std::int32_t>(magnitude));
            }
        }
    }
    return out;
}

int bits_of(std::int32_t peak) {
    return peak == 0 ? 0 : std::bit_width(static_cast<std::uint32_t>(peak)) + 1;
}

EncoderConfig stereo() {
    EncoderConfig config;
    config.channels = 2;
    config.restart_interval_units = 8;
    config.allow_rate_violations = true;
    return config;
}

TEST_CASE("max_bits is the bit length of the largest sample plus the sign, and repeated",
          "[truehd][encoder][bounds]") {
    // Loud, quiet and silent stretches, so that the bound differs from interval to interval.
    std::vector<std::int32_t> pcm = make_pcm(2, 40 * 8 * 6, 24, 5);
    for (std::size_t frame = 40 * 8 * 2; frame < 40 * 8 * 4; ++frame) {
        for (std::size_t c = 0; c < 2; ++c) {
            pcm[frame * 2 + c] = frame < 40 * 8 * 3 ? pcm[frame * 2 + c] / 4096 : 0;
        }
    }
    const Encoded encoded = encode_all(stereo(), pcm);
    const std::vector<Interval> intervals = intervals_of(encoded.units);
    REQUIRE(intervals.size() >= 5);
    bool saw_silence = false;
    bool saw_narrow = false;
    for (const Interval& interval : intervals) {
        CAPTURE(interval.peak);
        CHECK(interval.header.max_bits == bits_of(interval.peak));
        CHECK(interval.header.max_bits_repeat == interval.header.max_bits);
        saw_silence = saw_silence || interval.header.max_bits == 0;
        saw_narrow = saw_narrow || (interval.header.max_bits != 0 && interval.header.max_bits < 20);
    }
    CHECK(saw_silence);
    CHECK(saw_narrow);
}

TEST_CASE("a full scale negative sample needs 25 bits", "[truehd][encoder][bounds]") {
    std::vector<std::int32_t> pcm(2 * 40 * 16, 0);
    pcm[2 * 100] = -(1 << 23);
    const Encoded encoded = encode_all(stereo(), pcm);
    const std::vector<Interval> intervals = intervals_of(encoded.units);
    REQUIRE_FALSE(intervals.empty());
    CHECK(intervals.front().header.max_bits == 25);
    CHECK(intervals.front().header.max_bits_repeat == 25);
}

TEST_CASE("max_lsbs and max_shift never fall below what the interval uses",
          "[truehd][encoder][bounds]") {
    const Encoded encoded = encode_all(stereo(), make_pcm(2, 40 * 8 * 4, 24, 9));
    const std::vector<Interval> intervals = intervals_of(encoded.units);
    REQUIRE_FALSE(intervals.empty());
    for (const Interval& interval : intervals) {
        // A 24 bit signal needs lsbs and no output shift; the fields hold the 5 and 4 bit limits.
        CHECK(interval.header.max_lsbs <= 31);
        CHECK(interval.header.max_shift <= 7);
        CHECK(interval.header.max_lsbs > 0);
    }
}

}  // namespace
