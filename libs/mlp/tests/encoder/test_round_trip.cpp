#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "iclforge/mlp/decoder/decoder.hpp"
#include "iclforge/mlp/encoder/encoder.hpp"
#include "support/pcm_wav.hpp"

// The encoder's lossless property: encode, decode, and the samples are the input's, exactly. The
// encoder's own verification is on throughout (it decodes every access unit it writes), and the
// test decodes the whole stream again independently. The sources are the FFmpeg golden masters'
// WAVs (testdata/truehd/ffmpeg), with real content per channel and full-scale edges, and random
// signals over channel counts, rates, depths and lengths. Each stream is also written to the
// test's scratch directory, where tools/checks/verify_truehd_interop.py's sibling legs can hand
// it to FFmpeg.

namespace {

using namespace iclforge::mlp;

const std::filesystem::path kGold = ICLFORGE_TRUEHD_GOLDEN_DIR "/ffmpeg";
const std::filesystem::path kScratch = ICLFORGE_TEST_SCRATCH_DIR;

void save(const std::string& name, const std::vector<std::byte>& bytes) {
    std::filesystem::create_directories(kScratch);
    std::ofstream out(kScratch / name, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

void round_trip(const std::vector<std::int32_t>& pcm, const EncoderConfig& config,
                const std::string& name) {
    INFO(name);
    EncoderStats stats;
    auto stream = encode_stream(pcm, config, &stats);
    if (!stream) {
        FAIL(std::string(describe(stream.error())));
    }
    save(name + ".thd", *stream);
    auto decoded = decode_stream(*stream);
    if (!decoded) {
        FAIL(std::string(describe(decoded.error())));
    }
    REQUIRE(decoded->channels == config.channels);
    REQUIRE(decoded->pcm.size() == pcm.size());
    std::size_t first = pcm.size();
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        if (decoded->pcm[i] != pcm[i]) {
            first = i;
            break;
        }
    }
    INFO("first difference at " << first);
    CHECK(first == pcm.size());
    CHECK(stats.access_units == decoded->access_units);
}

}  // namespace

TEST_CASE("mlp encoder round-trips the golden masters' sources", "[mlp][encoder]") {
    for (const char* leg :
         {"stereo-48k-16", "stereo-48k-24", "stereo-96k-24", "51-48k-16", "51-48k-24", "51-96k-24",
          "stereo-44k-16", "stereo-192k-24", "stereo-48k-24-odd"}) {
        const auto wav = test::read_pcm_wav(kGold / (std::string(leg) + ".wav"));
        REQUIRE(wav.has_value());
        EncoderConfig config;
        config.sample_rate = wav->sample_rate;
        config.channels = wav->channels;
        round_trip(wav->samples, config, std::string("forge-") + leg);
    }
}

TEST_CASE("mlp encoder round-trips random signals over the configuration space",
          "[mlp][encoder][property]") {
    std::mt19937 rng(7U);
    const std::uint32_t rates[] = {44100, 48000, 88200, 96000, 176400, 192000};
    for (int trial = 0; trial < 24; ++trial) {
        EncoderConfig config;
        config.sample_rate = rates[rng() % 6];
        config.channels = config.sample_rate > 96000 ? 1 + static_cast<int>(rng() % 2)
                                                     : 1 + static_cast<int>(rng() % 8);
        const int bits = std::array{16, 20, 24}[rng() % 3];
        config.restart_interval = 8 + static_cast<int>(rng() % 24);
        config.downmix = (rng() % 4) != 0;
        const std::size_t frames = 300 + rng() % 4000;
        std::vector<std::int32_t> pcm(frames * static_cast<std::size_t>(config.channels));
        const int kind = static_cast<int>(rng() % 5);
        const std::int32_t full = (1 << (bits - 1)) - 1;
        for (std::size_t n = 0; n < frames; ++n) {
            for (int c = 0; c < config.channels; ++c) {
                double v = 0.0;
                switch (kind) {
                    case 0:  // noise at full range
                        v = std::uniform_real_distribution<double>(-1.0, 1.0)(rng);
                        break;
                    case 1:  // tones, one per channel
                        v = 0.9 * std::sin(0.01 * static_cast<double>(n) * (c + 1));
                        break;
                    case 2:  // silence with a click
                        v = n == frames / 2 ? 1.0 : 0.0;
                        break;
                    case 3:  // full-scale square
                        v = ((n / static_cast<std::size_t>(5 + c)) % 2) != 0 ? 1.0 : -1.0;
                        break;
                    default:  // correlated channels
                        v = 0.5 * std::sin(0.003 * static_cast<double>(n)) +
                            0.01 * c * std::uniform_real_distribution<double>(-1.0, 1.0)(rng);
                        break;
                }
                auto s = static_cast<std::int32_t>(std::lround(std::clamp(v, -1.0, 1.0) * full));
                if (v <= -1.0) {
                    s = -full - 1;
                }
                pcm[n * static_cast<std::size_t>(config.channels) + static_cast<std::size_t>(c)] =
                    s * (1 << (24 - bits));
            }
        }
        round_trip(pcm, config, "random-" + std::to_string(trial));
    }
}
