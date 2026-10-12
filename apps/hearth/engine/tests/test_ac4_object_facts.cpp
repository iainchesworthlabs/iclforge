#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "iclforge/ac4/encoder/encoder.hpp"
#include "session.hpp"

// What an AC-4 item says of its objects when it is opened (Session::open, ItemFacts::objects and
// objects_alone): the count a sink's stated limit is compared with, read from what the decoder
// makes of the presentation's first I-frame, and whether the presentation has channels besides -
// the bed a decoder that places no objects plays alone. Streams are the encoder's own, so that the
// counts asserted are the ones configured.

namespace {

using iclforge::hearth::ItemLoader;
using iclforge::hearth::LoadedItem;
using iclforge::hearth::Session;

constexpr int kRate = 48000;
constexpr std::size_t kFrame = 2048;
constexpr std::size_t kFrames = 12;

ItemLoader loader_of(std::vector<std::byte> bytes) {
    return
        [bytes = std::move(bytes)](const std::string&) -> std::expected<LoadedItem, std::string> {
            return LoadedItem{.bytes = bytes};
        };
}

std::vector<std::byte> wrapped(const std::vector<iclforge::ac4::EncodedFrame>& first,
                               const std::vector<iclforge::ac4::EncodedFrame>& rest) {
    std::vector<std::byte> out;
    for (const auto* frames : {&first, &rest}) {
        for (const iclforge::ac4::EncodedFrame& frame : *frames) {
            const std::vector<std::byte> sync =
                iclforge::ac4::sync_frame(frame.raw_ac4_frame, false);
            out.insert(out.end(), sync.begin(), sync.end());
        }
    }
    return out;
}

std::vector<std::vector<float>> tones(std::size_t channels) {
    std::vector<std::vector<float>> planes(channels, std::vector<float>(kFrames * kFrame));
    for (std::size_t c = 0; c < channels; ++c) {
        const double hz = 440.0 + (150.0 * static_cast<double>(c));
        for (std::size_t n = 0; n < planes[c].size(); ++n) {
            planes[c][n] = static_cast<float>(
                0.1 * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) / kRate));
        }
    }
    return planes;
}

// `objects` objects of one substream, `lfe` of them the LFE (the last), coded as `coding`.
std::vector<std::byte> object_stream(
    iclforge::ac4::ObjectCoding coding, std::size_t objects, bool lfe,
    iclforge::ac4::AjocDownmix downmix = iclforge::ac4::AjocDownmix::kComputed) {
    iclforge::ac4::EncoderConfig config;
    config.sample_rate_hz = kRate;
    config.frame_rate_index = 13;
    config.bitrate_kbps = 320;
    config.experimental.objects = true;
    iclforge::ac4::ObjectsConfig object_config;
    object_config.coding = coding;
    object_config.downmix = downmix;
    object_config.objects.resize(objects);
    for (std::size_t i = 0; i < objects; ++i) {
        object_config.objects[i].properties.position = {
            0.1 + (0.8 * static_cast<double>(i) / static_cast<double>(objects)), 0.0, 0.0};
    }
    if (lfe) {
        object_config.objects.back().lfe = true;
    }
    iclforge::ac4::SubstreamConfig substream;
    substream.objects = object_config;
    config.substreams = {substream};
    auto encoder = iclforge::ac4::Encoder::create(config);
    INFO(iclforge::ac4::Encoder::refusal_reason(config));
    REQUIRE(encoder.has_value());
    const std::vector<std::vector<float>> planes = tones(objects);
    const std::vector<std::span<const float>> views(planes.begin(), planes.end());
    auto frames = encoder->encode(views, std::span<const iclforge::ac4::ObjectMetadataUpdate>{});
    REQUIRE(frames.has_value());
    auto rest = encoder->flush();
    REQUIRE(rest.has_value());
    return wrapped(*frames, *rest);
}

std::vector<std::byte> channel_stream() {
    iclforge::ac4::EncoderConfig config;
    config.channels = 2;
    config.bitrate_kbps = 128;
    auto encoder = iclforge::ac4::Encoder::create(config);
    INFO(iclforge::ac4::Encoder::refusal_reason(config));
    REQUIRE(encoder.has_value());
    const std::vector<std::vector<float>> planes = tones(2);
    const std::vector<std::span<const float>> views(planes.begin(), planes.end());
    const auto frames = encoder->encode(views);
    REQUIRE(frames.has_value());
    const auto rest = encoder->flush();
    REQUIRE(rest.has_value());
    return wrapped(*frames, *rest);
}

// Stereo at `frame_rate_index` (Part 1 Table 83 at 48 kHz).
std::vector<std::byte> stereo_at(int frame_rate_index) {
    iclforge::ac4::EncoderConfig config;
    config.channels = 2;
    config.bitrate_kbps = 128;
    config.frame_rate_index = frame_rate_index;
    auto encoder = iclforge::ac4::Encoder::create(config);
    INFO(iclforge::ac4::Encoder::refusal_reason(config));
    REQUIRE(encoder.has_value());
    const std::vector<std::vector<float>> planes = tones(2);
    const std::vector<std::span<const float>> views(planes.begin(), planes.end());
    const auto frames = encoder->encode(views);
    REQUIRE(frames.has_value());
    const auto rest = encoder->flush();
    REQUIRE(rest.has_value());
    return wrapped(*frames, *rest);
}

}  // namespace

TEST_CASE("ac4 frame rate: an item says the frames a second its stream is coded at",
          "[hearth][ac4][frame_rate]") {
    struct Case {
        int index;
        double frames_per_second;
    };
    // The 2 048-sample frame, then the rates that go through the converter, 1000/1001 ones
    // among them, whose frames alternate between two lengths.
    for (const Case& c : {Case{13, 48000.0 / 2048.0}, Case{2, 25.0}, Case{1, 24.0},
                          Case{0, 24000.0 / 1001.0}, Case{3, 30000.0 / 1001.0}}) {
        CAPTURE(c.index);
        auto session = Session::open("item", loader_of(stereo_at(c.index)));
        REQUIRE(session.has_value());
        CHECK(session->facts().frame_rate == Catch::Approx(c.frames_per_second).epsilon(0.002));
    }
}

TEST_CASE("ac4 object facts: direct-coded objects are counted, and are objects alone",
          "[hearth][ac4][objects]") {
    for (const std::size_t objects : {std::size_t{1}, std::size_t{3}, std::size_t{5}}) {
        CAPTURE(objects);
        auto session = Session::open(
            "item", loader_of(object_stream(iclforge::ac4::ObjectCoding::kDirect, objects, false)));
        REQUIRE(session.has_value());
        CHECK(session->facts().objects == objects);
        CHECK(session->facts().objects_alone);
    }
}

TEST_CASE("ac4 object facts: the LFE is no object a sink places", "[hearth][ac4][objects]") {
    auto session = Session::open(
        "item", loader_of(object_stream(iclforge::ac4::ObjectCoding::kDirect, 4, true)));
    REQUIRE(session.has_value());
    CHECK(session->facts().objects == 3);
    CHECK(session->facts().objects_alone);
}

TEST_CASE(
    "ac4 object facts: an A-JOC presentation counts the objects its upmix rebuilds, not its "
    "downmix",
    "[hearth][ac4][objects]") {
    auto session = Session::open(
        "item", loader_of(object_stream(iclforge::ac4::ObjectCoding::kAjoc, 7, false)));
    REQUIRE(session.has_value());
    CHECK(session->facts().objects == 7);
    CHECK(session->facts().objects_alone);
}

TEST_CASE(
    "ac4 object facts: a stream of channels alone declares no objects and is not objects alone",
    "[hearth][ac4][objects]") {
    auto session = Session::open("item", loader_of(channel_stream()));
    REQUIRE(session.has_value());
    CHECK(session->facts().objects == 0);
    CHECK_FALSE(session->facts().objects_alone);
}
