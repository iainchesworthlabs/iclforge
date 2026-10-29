#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

#include "iclforge/mlp/decoder/decoder.hpp"
#include "iclforge/mlp/encoder/atmos.hpp"
#include "iclforge/mlp/io/elementary.hpp"

// Atmos in TrueHD: a 7.1 bed and moving objects, encoded and decoded. The 16-channel
// presentation must give back the bed and the objects exactly and the OAMD every frame carried;
// the 8-channel presentation must be the bed with each object rendered in at the gains its
// position gave, the fold a decoder without Atmos plays.

namespace {

using namespace iclforge::mlp;
namespace oba = iclforge::objects::oba;

constexpr int kObjects = 6;
constexpr std::size_t kFrames = 48000 / 5;  // 0.2 s
constexpr std::size_t kStep = 160;          // a new object state every four access units

oba::DynamicObject object_at(int o, std::size_t frame) {
    oba::DynamicObject d;
    const double t = static_cast<double>(frame) / 48000.0;
    const double angle = 2.0 * 3.14159265358979 * t * (1.0 + 0.5 * o);
    d.position.x = 0.5 + 0.4 * std::cos(angle);
    d.position.y = 0.5 + 0.4 * std::sin(angle);
    d.position.z = o % 2 == 0 ? 0.0 : 0.5;
    d.gain_db = -3.0 - o;
    return d;
}

}  // namespace

TEST_CASE("mlp Atmos round-trips bed, objects and metadata", "[mlp][atmos]") {
    std::mt19937 rng(15U);
    std::vector<std::int32_t> bed(kFrames * 8);
    std::vector<std::int32_t> objects(kFrames * kObjects);
    for (std::size_t n = 0; n < kFrames; ++n) {
        for (int c = 0; c < 8; ++c) {
            bed[n * 8 + static_cast<std::size_t>(c)] = static_cast<std::int32_t>(
                std::lround(2.0e6 * std::sin(0.001 * static_cast<double>(n) * (c + 1))));
        }
        for (int o = 0; o < kObjects; ++o) {
            objects[n * kObjects + static_cast<std::size_t>(o)] = static_cast<std::int32_t>(
                std::lround(1.5e6 * std::sin(0.0031 * static_cast<double>(n) * (o + 2)) +
                            std::uniform_int_distribution<int>(-20000, 20000)(rng)));
        }
    }
    AtmosConfig config;
    config.objects = kObjects;
    auto encoder = AtmosEncoder::create(config);
    REQUIRE(encoder.has_value());
    std::vector<std::byte> stream;
    for (std::size_t at = 0; at < kFrames; at += kStep) {
        std::vector<oba::DynamicObject> state;
        for (int o = 0; o < kObjects; ++o) {
            state.push_back(object_at(o, at));
        }
        auto bytes =
            encoder->encode(std::span(bed).subspan(at * 8, kStep * 8),
                            std::span(objects).subspan(at * kObjects, kStep * kObjects), state);
        if (!bytes) {
            FAIL(std::string(describe(bytes.error())));
        }
        stream.insert(stream.end(), bytes->begin(), bytes->end());
    }
    auto tail = encoder->flush();
    REQUIRE(tail.has_value());
    stream.insert(stream.end(), tail->begin(), tail->end());
    {
        std::filesystem::create_directories(ICLFORGE_TEST_SCRATCH_DIR);
        std::ofstream out(std::filesystem::path(ICLFORGE_TEST_SCRATCH_DIR) / "atmos.thd",
                          std::ios::binary);
        out.write(reinterpret_cast<const char*>(stream.data()),
                  static_cast<std::streamsize>(stream.size()));
    }

    auto atmos = decode_atmos(stream);
    if (!atmos) {
        FAIL(std::string(describe(atmos.error())));
    }
    CHECK(atmos->objects == kObjects);
    CHECK(atmos->bed == bed);
    CHECK(atmos->object_pcm == objects);
    // One payload per state, and one at every restart (every 640 samples) that falls between.
    REQUIRE(atmos->metadata.size() >= kFrames / kStep);
    CHECK(atmos->metadata[0].first_sample == 0);
    REQUIRE(atmos->metadata[0].metadata.objects.size() == kObjects);
    CHECK(std::abs(atmos->metadata[0].metadata.objects[1].position.x - object_at(1, 0).position.x) <
          0.02);

    const auto summary = probe(stream);
    REQUIRE(summary.has_value());
    CHECK(summary->presentations == std::vector<int>{2, 6, 8, 14});
    CHECK(summary->major_sync.extra_channel_meaning.has_value());

    // The 8-channel presentation: the bed with each object folded in by its render, in each
    // access unit, give or take the limiter's scaling and the rounding of each fold step.
    auto seven_one = decode_stream(stream, {.presentation_channels = 8});
    REQUIRE(seven_one.has_value());
    REQUIRE(seven_one->channels == 8);
    // Each gain is carried to 2^-14 (10.2), so the fold departs from the exact render by at most
    // half that step times each object's magnitude, and by a sample per rounding.
    double worst_excess = 0.0;
    for (std::size_t n = 0; n < kFrames; ++n) {
        const std::size_t state_at = n / kStep * kStep;
        for (int c = 0; c < 8; ++c) {
            double want = bed[n * 8 + static_cast<std::size_t>(c)];
            double bound = kObjects + 1.0;
            for (int o = 0; o < kObjects; ++o) {
                const double x = objects[n * kObjects + static_cast<std::size_t>(o)];
                want += render_gains(object_at(o, state_at))[static_cast<std::size_t>(c)] * x;
                bound += std::abs(x) * 0.5 / 16384.0;
            }
            const double got = seven_one->pcm[n * 8 + static_cast<std::size_t>(c)];
            worst_excess = std::max(worst_excess, std::abs(got - want) - bound);
        }
    }
    INFO("worst departure of the 7.1 fold from bed + render, past its bound: " << worst_excess);
    CHECK(worst_excess <= 0.0);
}
