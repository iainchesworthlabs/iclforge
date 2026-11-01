#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "encoder/encoder_support.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"
#include "iclforge/truehd/io/access_unit.hpp"
#include "iclforge/truehd/io/presentation_cut.hpp"

// keep_substreams(): a stream cut to its first substreams decodes to what the whole stream decodes
// to at the presentation those substreams stand for. The oracle is the library's own decoder run on
// the whole stream with Presentation::k2ch or k6ch: it reads the same segments the cut stream
// carries, so the two are equal sample for sample if the rewritten header, directory and major sync
// say the same thing (and the decoder's checks, strict, pass on the cut stream: the nibbles, the
// major sync CRC and each segment's parity and CRC).

using namespace iclforge::truehd;
using namespace iclforge::truehd::testing;

namespace {

constexpr int kFrames = 40 * 60;

struct Cut {
    std::vector<std::vector<std::byte>> units;
    std::size_t whole_bytes = 0;
    std::size_t cut_bytes = 0;
};

Cut cut_all(const std::vector<EncodedAccessUnit>& units, int keep, int substreams) {
    Cut out;
    for (const EncodedAccessUnit& unit : units) {
        auto cut = keep_substreams(unit.bytes, keep, substreams);
        INFO((cut ? "" : to_string(cut.error().kind)) << " " << (cut ? "" : cut.error().detail));
        REQUIRE(cut.has_value());
        out.whole_bytes += unit.bytes.size();
        out.cut_bytes += cut->size();
        out.units.push_back(std::move(*cut));
    }
    return out;
}

std::vector<std::int32_t> decode_cut(const std::vector<std::vector<std::byte>>& units, int channels) {
    Decoder decoder{DecoderConfig{}};
    std::vector<std::int32_t> out;
    for (const auto& unit : units) {
        auto decoded = decoder.decode(unit);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        REQUIRE((**decoded).channels == channels);
        for (const std::int32_t sample : (**decoded).pcm) {
            out.push_back(sample);
        }
    }
    return out;
}

}  // namespace

TEST_CASE("a stream cut to its first substreams decodes to the presentation they stand for",
          "[truehd][io][presentation]") {
    for (const DownmixMode mode : {DownmixMode::kStandard, DownmixMode::kNone}) {
        EncoderConfig config;
        config.channels = 8;
        config.restart_interval_units = 8;
        config.downmix = mode;
        auto pcm = make_pcm(8, kFrames, 24, 77);
        for (auto& v : pcm) {
            v /= 3;
        }
        const Encoded encoded = encode_all(config, pcm);
        REQUIRE(encoded.units.size() > 20);
        const int substreams = Encoder::create(config)->substreams();
        REQUIRE(substreams == 3);

        SECTION(mode == DownmixMode::kStandard ? "with downmix substreams" : "as FFmpeg writes them") {
            // One substream: the 2 channel presentation.
            const Cut one = cut_all(encoded.units, 1, substreams);
            CHECK(one.cut_bytes < one.whole_bytes);
            CHECK(decode_cut(one.units, 2) == decode_all(encoded.units, 24, 2, Presentation::k2ch));
            // Two: the 6 channel presentation.
            const Cut two = cut_all(encoded.units, 2, substreams);
            CHECK(two.cut_bytes < two.whole_bytes);
            CHECK(two.cut_bytes > one.cut_bytes);
            CHECK(decode_cut(two.units, 6) == decode_all(encoded.units, 24, 6, Presentation::k6ch));
            // All three: nothing is cut, byte for byte.
            const Cut all = cut_all(encoded.units, 3, substreams);
            CHECK(all.cut_bytes == all.whole_bytes);
            for (std::size_t i = 0; i < all.units.size(); ++i) {
                CHECK(all.units[i] == encoded.units[i].bytes);
            }
        }
    }
}

TEST_CASE("a cut unit is a unit the parser reads, with one substream and a major sync where it had one",
          "[truehd][io][presentation]") {
    EncoderConfig config;
    config.channels = 6;
    config.restart_interval_units = 8;
    config.downmix = DownmixMode::kStandard;
    const Encoded encoded = encode_all(config, make_pcm(6, kFrames, 24, 5));
    const int substreams = Encoder::create(config)->substreams();
    for (const EncodedAccessUnit& unit : encoded.units) {
        const auto cut = keep_substreams(unit.bytes, 1, substreams);
        REQUIRE(cut.has_value());
        const auto parsed = parse_access_unit(*cut, 1);
        REQUIRE(parsed.has_value());
        CHECK(parsed->num_substreams == 1);
        CHECK(parsed->major_sync.has_value() == unit.has_major_sync);
        if (parsed->major_sync) {
            CHECK(parsed->major_sync->num_substreams == 1);
        }
        CHECK(parsed->length_bytes() == cut->size());
    }
}

TEST_CASE("keeping no substream, or more than four, is refused", "[truehd][io][presentation]") {
    EncoderConfig config;
    config.channels = 2;
    const Encoded encoded = encode_all(config, make_pcm(2, kFrames, 24, 3));
    REQUIRE_FALSE(encoded.units.empty());
    CHECK(keep_substreams(encoded.units.front().bytes, 0, 1).error().kind == ErrorKind::kBadParameter);
    CHECK(keep_substreams(encoded.units.front().bytes, 5, 1).error().kind == ErrorKind::kBadParameter);
    // A unit that is not one is the parser's error.
    const std::vector<std::byte> junk(12, std::byte{0x55});
    CHECK_FALSE(keep_substreams(junk, 1, 1).has_value());
}
