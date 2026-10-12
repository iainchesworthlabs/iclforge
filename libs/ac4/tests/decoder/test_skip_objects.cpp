// DecoderConfig::skip_objects: a decoder that places no objects leaves object audio undecoded.
// What it must keep: the channels of a presentation that has them, bit for bit, in full and core
// decoding; an intermediate spatial format, which the decoder renders into the channels itself.
// What it must change: no objects come out, and a presentation of object audio alone, which has
// nothing else to play, is refused with a reason rather than decoded to nothing.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "iclforge/ac4/decoder/decoder.hpp"
#include "iclforge/ac4/encoder/encoder.hpp"
#include "objects.hpp"

namespace {

using ac4_decoder_test::BuiltObjectStream;
using ac4_decoder_test::ObjectCase;
using iclforge::ac4::DecodingMode;

constexpr int kFrames = 12;

struct Played {
    std::vector<std::vector<float>> channels;
    std::vector<iclforge::ac4::Speaker> speakers;
    std::size_t objects = 0;
};

// Every frame of `stream`, its channels end to end.
Played play(const BuiltObjectStream& stream, bool skip, DecodingMode mode) {
    iclforge::ac4::DecoderConfig config;
    config.decoding = mode;
    config.skip_objects = skip;
    iclforge::ac4::Decoder decoder(config);
    Played out;
    for (std::size_t f = 0; f < stream.frames.size(); ++f) {
        const auto decoded = decoder.decode(stream.frames[f]);
        INFO("frame " << f << ": " << decoder.refusal_reason());
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        const iclforge::ac4::DecodedFrame& frame = **decoded;
        if (out.channels.empty()) {
            out.channels.resize(frame.channels.size());
            out.speakers = frame.speakers;
        }
        REQUIRE(frame.channels.size() == out.channels.size());
        for (std::size_t c = 0; c < frame.channels.size(); ++c) {
            out.channels[c].insert(out.channels[c].end(), frame.channels[c].begin(),
                                   frame.channels[c].end());
        }
        out.objects = std::max(out.objects, frame.objects.size());
    }
    return out;
}

double loudest(const Played& played) {
    double most = 0.0;
    for (const std::vector<float>& channel : played.channels) {
        for (const float sample : channel) {
            most = std::max(most, static_cast<double>(std::fabs(sample)));
        }
    }
    return most;
}

ObjectCase committed(ObjectCase::Kind kind) {
    for (const ObjectCase& c : ac4_decoder_test::committed_object_cases()) {
        if (c.kind == kind) {
            return c;
        }
    }
    FAIL("no committed case of that kind");
    return {};
}

}  // namespace

TEST_CASE(
    "skip_objects: a presentation's channels are the same bit for bit, and its objects are not "
    "decoded",
    "[ac4][decoder][objects][skip]") {
    using Kind = ObjectCase::Kind;
    for (const Kind kind : {Kind::kAjoc, Kind::kAjocStatic, Kind::kDynamic, Kind::kBed}) {
        ObjectCase c = committed(kind);
        c.channels = true;
        const BuiltObjectStream stream = ac4_decoder_test::build_objects(c, kFrames);
        for (const DecodingMode mode : {DecodingMode::kFull, DecodingMode::kCore}) {
            CAPTURE(c.name, static_cast<int>(mode));
            const Played whole = play(stream, false, mode);
            const Played skipped = play(stream, true, mode);
            REQUIRE(whole.channels.size() == 6);
            CHECK(loudest(whole) > 1e-3);
            // The objects come out of a decoder that decodes them, and not out of the other.
            CHECK(whole.objects > 0);
            CHECK(skipped.objects == 0);
            // And what is heard is what it was.
            CHECK(skipped.speakers == whole.speakers);
            CHECK(skipped.channels == whole.channels);
        }
    }
}

TEST_CASE("skip_objects: an intermediate spatial format is not an object: it is rendered whole",
          "[ac4][decoder][objects][skip]") {
    ObjectCase c = committed(ObjectCase::Kind::kIsf);
    for (const bool channels : {false, true}) {
        c.channels = channels;
        const BuiltObjectStream stream = ac4_decoder_test::build_objects(c, kFrames);
        CAPTURE(channels);
        const Played whole = play(stream, false, DecodingMode::kFull);
        const Played skipped = play(stream, true, DecodingMode::kFull);
        REQUIRE_FALSE(whole.channels.empty());
        CHECK(loudest(whole) > 1e-3);
        CHECK(skipped.speakers == whole.speakers);
        CHECK(skipped.channels == whole.channels);
        CHECK(skipped.objects == 0);
    }
}

TEST_CASE("skip_objects: a presentation of object audio alone has nothing to play, and says so",
          "[ac4][decoder][objects][skip]") {
    using Kind = ObjectCase::Kind;
    for (const Kind kind : {Kind::kAjoc, Kind::kAjocStatic, Kind::kDynamic, Kind::kBed}) {
        const ObjectCase c = committed(kind);
        const BuiltObjectStream stream = ac4_decoder_test::build_objects(c, 6);
        CAPTURE(c.name);
        // Decoded whole it is objects and no channels, as it always was.
        const Played whole = play(stream, false, DecodingMode::kFull);
        CHECK(whole.channels.empty());
        CHECK(whole.objects > 0);
        // Told to place none, it is refused, frame after frame, with the reason.
        iclforge::ac4::DecoderConfig config;
        config.skip_objects = true;
        iclforge::ac4::Decoder decoder(config);
        for (std::size_t f = 0; f < stream.frames.size(); ++f) {
            const auto decoded = decoder.decode(stream.frames[f]);
            REQUIRE_FALSE(decoded.has_value());
            CHECK(decoded.error() == iclforge::ac4::DecodeError::kUnsupported);
            CHECK(decoder.refusal_reason().find("object audio alone") != std::string_view::npos);
        }
    }
}

TEST_CASE("skip_objects: a presentation of channels alone is the same decoded either way",
          "[ac4][decoder][objects][skip]") {
    // The encoder's stereo stream has no object substream for the setting to leave undecoded.
    iclforge::ac4::EncoderConfig config;
    config.channels = 2;
    config.bitrate_kbps = 128;
    auto encoder = iclforge::ac4::Encoder::create(config);
    INFO(iclforge::ac4::Encoder::refusal_reason(config));
    REQUIRE(encoder.has_value());
    std::vector<std::vector<float>> planes(2, std::vector<float>(8 * 2048));
    for (std::size_t c = 0; c < planes.size(); ++c) {
        for (std::size_t n = 0; n < planes[c].size(); ++n) {
            planes[c][n] =
                0.1F * std::sin(0.05F * static_cast<float>(n) * static_cast<float>(c + 1));
        }
    }
    const std::vector<std::span<const float>> views(planes.begin(), planes.end());
    auto frames = encoder->encode(views);
    REQUIRE(frames.has_value());
    REQUIRE_FALSE(frames->empty());
    const auto decode_all = [&](bool skip) {
        iclforge::ac4::DecoderConfig dc;
        dc.skip_objects = skip;
        iclforge::ac4::Decoder decoder(dc);
        std::vector<std::vector<float>> out(2);
        for (const iclforge::ac4::EncodedFrame& frame : *frames) {
            const auto decoded = decoder.decode(frame.raw_ac4_frame);
            REQUIRE(decoded.has_value());
            if (!decoded->has_value()) {
                continue;
            }
            for (std::size_t c = 0; c < 2; ++c) {
                out[c].insert(out[c].end(), (*decoded)->channels[c].begin(),
                              (*decoded)->channels[c].end());
            }
        }
        return out;
    };
    const auto whole = decode_all(false);
    REQUIRE_FALSE(whole[0].empty());
    CHECK(decode_all(true) == whole);
}
