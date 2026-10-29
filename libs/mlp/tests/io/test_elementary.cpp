#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "iclforge/mlp/decoder/decoder.hpp"
#include "iclforge/mlp/encoder/encoder.hpp"
#include "iclforge/mlp/io/elementary.hpp"
#include "iclforge/mlp/io/extra_data.hpp"
#include "support/pcm_wav.hpp"

// The elementary stream layer: scan() and probe() over FFmpeg's golden masters, a stream cut from
// the middle of another (found from its next major sync), and the extra data container's round
// trip and checks.

namespace {

using namespace iclforge::mlp;

const std::filesystem::path kGold = ICLFORGE_TRUEHD_GOLDEN_DIR "/ffmpeg";

}  // namespace

TEST_CASE("mlp scan and probe FFmpeg's 5.1 stream", "[mlp][io]") {
    const auto stream = test::read_file(kGold / "51-48k-24.thd");
    REQUIRE(stream.has_value());
    const ScanResult walk = scan(*stream);
    CHECK_FALSE(walk.stopped_at.has_value());
    REQUIRE(walk.units.size() == 240);  // 9600 samples, 40 an access unit
    CHECK(walk.units[0].major_sync);
    std::size_t bytes = 0;
    for (const auto& u : walk.units) {
        bytes += u.size;
    }
    CHECK(bytes == stream->size());
    const auto summary = probe(*stream);
    REQUIRE(summary.has_value());
    CHECK(summary->major_sync.sample_rate() == 48000U);
    CHECK(summary->major_sync.num_substreams == 2);
    CHECK(summary->presentations == std::vector<int>{2, 6});
    CHECK(summary->samples == 9600U);
    CHECK_FALSE(summary->has_extra_data);
}

TEST_CASE("mlp scan finds a cut stream's next major sync", "[mlp][io]") {
    const auto stream = test::read_file(kGold / "stereo-48k-24.thd");
    REQUIRE(stream.has_value());
    const std::vector<std::byte> cut(stream->begin() + 1000, stream->end());
    const ScanResult walk = scan(cut);
    REQUIRE_FALSE(walk.units.empty());
    CHECK(walk.units[0].major_sync);
    CHECK(walk.units[0].offset > 0);
}

TEST_CASE("mlp extra data round-trips its payloads and checks its parity", "[mlp][io]") {
    std::vector<EvolutionPayload> payloads(2);
    payloads[0].type = static_cast<std::uint8_t>(EvolutionPayloadType::kOamd);
    payloads[0].data = {std::byte{1}, std::byte{2}, std::byte{3}};
    payloads[1].type = static_cast<std::uint8_t>(EvolutionPayloadType::kContentSignature);
    payloads[1].data.assign(70, std::byte{0x5A});
    auto bytes = write_extra_data(payloads);
    REQUIRE(bytes.has_value());
    CHECK(bytes->size() % 2 == 0);
    auto back = read_extra_data(*bytes);
    REQUIRE(back.has_value());
    REQUIRE(back->size() == 2);
    CHECK((*back)[0].type == payloads[0].type);
    CHECK((*back)[0].data == payloads[0].data);
    CHECK((*back)[1].data == payloads[1].data);
    auto broken = *bytes;
    broken[5] ^= std::byte{0x01};
    CHECK(read_extra_data(broken).error() == Error::kExtraDataParity);
    broken = *bytes;
    broken[1] = std::byte{0xFF};
    CHECK_FALSE(read_extra_data(broken).has_value());
}

TEST_CASE("mlp evolution payloads ride through the encoder and decoder", "[mlp][io]") {
    EncoderConfig config;
    config.channels = 2;
    std::vector<std::int32_t> pcm(2 * 40 * 20, 0);
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        pcm[i] = static_cast<std::int32_t>((i * 7919U) % 65536U) - 32768;
    }
    auto encoder = Encoder::create(config);
    REQUIRE(encoder.has_value());
    std::vector<std::byte> stream;
    for (int unit = 0; unit < 20; ++unit) {
        EvolutionPayload p;
        p.type = static_cast<std::uint8_t>(EvolutionPayloadType::kOamd);
        p.data = {static_cast<std::byte>(unit)};
        encoder->attach(p);
        auto bytes =
            encoder->encode(std::span(pcm).subspan(static_cast<std::size_t>(unit) * 80, 80));
        REQUIRE(bytes.has_value());
        stream.insert(stream.end(), bytes->begin(), bytes->end());
    }
    auto tail = encoder->flush();
    REQUIRE(tail.has_value());
    stream.insert(stream.end(), tail->begin(), tail->end());
    auto decoded = decode_stream(stream);
    REQUIRE(decoded.has_value());
    CHECK(decoded->pcm == pcm);
    REQUIRE(decoded->evolution.size() == 20);
    for (int unit = 0; unit < 20; ++unit) {
        REQUIRE(decoded->evolution[static_cast<std::size_t>(unit)].size() == 1);
        CHECK(decoded->evolution[static_cast<std::size_t>(unit)][0].data[0] ==
              static_cast<std::byte>(unit));
    }
    const auto summary = probe(stream);
    REQUIRE(summary.has_value());
    CHECK(summary->has_extra_data);
}
