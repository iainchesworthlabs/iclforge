#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <string>

#include "iclforge/mlp/decoder/decoder.hpp"
#include "support/pcm_wav.hpp"

// The decoder against the golden masters FFmpeg's encoder wrote (testdata/truehd/ffmpeg, made by
// tools/generators/gen_truehd_ffmpeg_gold.py): each stream must decode to its source WAV sample
// for sample, with every check of Appendix E on, including each restart interval's lossless
// check. These streams are the evidence for the decoder half of planning/truehd.md's register.

namespace {

const std::filesystem::path kGold = ICLFORGE_TRUEHD_GOLDEN_DIR "/ffmpeg";

void check_leg(const std::string& name) {
    INFO(name);
    const auto stream = iclforge::mlp::test::read_file(kGold / (name + ".thd"));
    REQUIRE(stream.has_value());
    const auto source = iclforge::mlp::test::read_pcm_wav(kGold / (name + ".wav"));
    REQUIRE(source.has_value());
    auto decoded = iclforge::mlp::decode_stream(*stream);
    if (!decoded) {
        FAIL(std::string(iclforge::mlp::describe(decoded.error())));
    }
    CHECK(decoded->sample_rate == source->sample_rate);
    REQUIRE(decoded->channels == source->channels);
    REQUIRE(decoded->pcm.size() >= source->samples.size());
    std::size_t first_difference = source->samples.size();
    for (std::size_t i = 0; i < source->samples.size(); ++i) {
        if (decoded->pcm[i] != source->samples[i]) {
            first_difference = i;
            break;
        }
    }
    INFO("first difference at interleaved sample " << first_difference);
    CHECK(first_difference == source->samples.size());
    // Past the source's end only the encoder's padding of the last access unit: silence.
    for (std::size_t i = source->samples.size(); i < decoded->pcm.size(); ++i) {
        CHECK(decoded->pcm[i] == 0);
    }
}

}  // namespace

TEST_CASE("mlp decodes FFmpeg's stereo streams to their sources", "[mlp][decoder][gold]") {
    for (const char* leg : {"stereo-48k-16", "stereo-48k-24", "stereo-96k-16", "stereo-96k-24",
                            "stereo-44k-16", "stereo-192k-24", "stereo-48k-24-odd"}) {
        check_leg(leg);
    }
}

TEST_CASE("mlp decodes FFmpeg's 5.1 streams to their sources", "[mlp][decoder][gold]") {
    for (const char* leg : {"51-48k-16", "51-48k-24", "51-96k-16", "51-96k-24"}) {
        check_leg(leg);
    }
}

TEST_CASE("mlp decodes the 2-channel presentation of a 5.1 stream", "[mlp][decoder][gold]") {
    // FFmpeg's 5.1 stream carries L and R in substream 0 and the rest in substream 1, so its
    // 2-channel presentation is the source's first two channels.
    const auto stream = iclforge::mlp::test::read_file(kGold / "51-48k-24.thd");
    const auto source = iclforge::mlp::test::read_pcm_wav(kGold / "51-48k-24.wav");
    REQUIRE(stream.has_value());
    REQUIRE(source.has_value());
    auto decoded = iclforge::mlp::decode_stream(*stream, {.presentation_channels = 2});
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->channels == 2);
    const std::size_t frames = source->samples.size() / 6;
    bool same = true;
    for (std::size_t n = 0; n < frames && same; ++n) {
        same = decoded->pcm[n * 2] == source->samples[n * 6] &&
               decoded->pcm[n * 2 + 1] == source->samples[n * 6 + 1];
    }
    CHECK(same);
}

TEST_CASE("mlp decoder reports a damaged stream with a typed error", "[mlp][decoder][gold]") {
    auto stream = iclforge::mlp::test::read_file(kGold / "stereo-48k-24.thd");
    REQUIRE(stream.has_value());
    // A bit inside the second access unit's sample data: the substream CRC catches it.
    auto damaged = *stream;
    const std::size_t first = ((std::to_integer<std::size_t>(damaged[0]) & 0x0FU) << 8U |
                               std::to_integer<std::size_t>(damaged[1])) *
                              2U;
    damaged[first + 20] ^= std::byte{0x10};
    auto decoded = iclforge::mlp::decode_stream(damaged);
    REQUIRE_FALSE(decoded.has_value());
    CHECK((decoded.error() == iclforge::mlp::Error::kSubstreamCrc ||
           decoded.error() == iclforge::mlp::Error::kSubstreamParity ||
           decoded.error() == iclforge::mlp::Error::kHuffmanCode ||
           decoded.error() == iclforge::mlp::Error::kSegmentOverrun));
    // With the checks off the same damage reaches the samples, where the lossless check of the
    // interval holding it fails.
    auto unchecked = iclforge::mlp::decode_stream(damaged, {.verify_checks = false});
    CHECK_FALSE(unchecked.has_value());
}
