#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <vector>

#include "encoder/encoder_support.hpp"
#include "iclforge/base/bitreader.hpp"
#include "iclforge/truehd/core/format.hpp"
#include "iclforge/truehd/core/syntax.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"
#include "iclforge/truehd/io/access_unit.hpp"
#include "iclforge/truehd/io/major_sync.hpp"
#include "support/stream_builder.hpp"

// The Atmos stream the encoder writes (EncoderConfig::object_channels, ERRATA.md entry 63): a 7.1
// bed in substreams 0 to 2 and twelve object channels in substream 3, from real audio (the DEE
// gold set's source files). No decoder outside this repository reads restart sync 0x31EC, so the
// stream is held to this library's decoder and to tools/references/truehd_syntax.py ([no-external-
// oracle]); the audio is lossless, so what comes back is the audio that went in.

namespace {

using namespace iclforge::truehd;
using namespace iclforge::truehd::testing;

constexpr int kBedChannels = 8;
constexpr int kObjects = 12;

struct Wav {
    int channels = 0;
    int frames = 0;
    std::vector<std::int32_t> samples;  // interleaved, 24 bit
};

std::uint32_t le(const std::vector<char>& bytes, std::size_t at, int width) {
    std::uint32_t value = 0;
    for (int i = width - 1; i >= 0; --i) {
        value = (value << 8U) | static_cast<unsigned char>(bytes[at + static_cast<std::size_t>(i)]);
    }
    return value;
}

// A 24 bit PCM WAV, plain or extensible, as the DEE gold set's sources are.
Wav read_wav(const std::string& name) {
    const std::string path =
        std::string(ICLFORGE_GOLDEN_TRUEHD_DIR) + "/../truehd-dee/sources/" + name;
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    const std::vector<char> bytes((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
    Wav wav;
    REQUIRE(bytes.size() > 44);
    std::size_t at = 12;
    int bits = 0;
    while (at + 8 <= bytes.size()) {
        const std::string tag(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                              bytes.begin() + static_cast<std::ptrdiff_t>(at + 4));
        const std::size_t size = le(bytes, at + 4, 4);
        if (tag == "fmt ") {
            wav.channels = static_cast<int>(le(bytes, at + 10, 2));
            bits = static_cast<int>(le(bytes, at + 22, 2));
        } else if (tag == "data") {
            REQUIRE(bits == 24);
            REQUIRE(wav.channels > 0);
            const std::size_t count = std::min(size, bytes.size() - at - 8) / 3;
            for (std::size_t i = 0; i < count; ++i) {
                const std::uint32_t raw = le(bytes, at + 8 + 3 * i, 3);
                wav.samples.push_back(static_cast<std::int32_t>(raw << 8U) >> 8);
            }
            wav.frames = static_cast<int>(count / static_cast<std::size_t>(wav.channels));
            return wav;
        }
        at += 8 + size + (size & 1U);
    }
    FAIL("no data chunk in " << path);
    return wav;
}

// Frames of 8 bed channels (fx_tones_71) and 12 object channels: the six of fx_music_51, the two
// of fx_music_20, and four of the tones again, a frame each, so that the objects are not the bed.
struct Programme {
    std::vector<std::int32_t> pcm;
    std::vector<std::int32_t> bed;
    std::vector<std::int32_t> objects;
    int frames = 0;
};

Programme make_programme() {
    const Wav tones = read_wav("fx_tones_71.wav");
    const Wav music = read_wav("fx_music_51.wav");
    const Wav stereo = read_wav("fx_music_20.wav");
    REQUIRE(tones.channels == 8);
    REQUIRE(music.channels == 6);
    REQUIRE(stereo.channels == 2);
    Programme programme;
    programme.frames = std::min({tones.frames, music.frames, stereo.frames});
    for (int t = 0; t < programme.frames; ++t) {
        for (int c = 0; c < kBedChannels; ++c) {
            const std::int32_t v = tones.samples[static_cast<std::size_t>(t * 8 + c)];
            programme.pcm.push_back(v);
            programme.bed.push_back(v);
        }
        for (int c = 0; c < kObjects; ++c) {
            std::int32_t v = 0;
            if (c < 6) {
                v = music.samples[static_cast<std::size_t>(t * 6 + c)];
            } else if (c < 8) {
                v = stereo.samples[static_cast<std::size_t>(t * 2 + (c - 6))];
            } else {
                v = tones.samples[static_cast<std::size_t>(((t + 7 * c) % programme.frames) * 8 +
                                                           (c - 8))];
            }
            programme.pcm.push_back(v);
            programme.objects.push_back(v);
        }
    }
    return programme;
}

EncoderConfig atmos_config() {
    EncoderConfig config;
    config.channels = kBedChannels;
    config.object_channels = kObjects;
    config.object_meaning.sixteench_dialogue_norm = 31;
    config.object_meaning.sixteench_mix_level = 35;
    config.object_meaning.dyn_object_only = true;
    config.object_meaning.lfe_present = true;
    return config;
}

}  // namespace

TEST_CASE("an Atmos stream decodes to its bed and to its object channels, sample for sample",
          "[truehd][encoder][atmos]") {
    const Programme programme = make_programme();
    EncoderConfig config = atmos_config();
    config.verify = true;  // the encoder decodes each unit with both presentations as it goes
    const Encoded encoded = encode_all(config, programme.pcm);
    REQUIRE_FALSE(encoded.units.empty());
    CHECK(encoded.stats.verified_units == encoded.units.size());

    // Presentation::kBest is the 7.1 bed of substreams 0 to 2, as any 7.1 decoder gives it.
    CHECK(decode_all(encoded.units, 24, kBedChannels) == programme.bed);
    // Presentation::kAll is substream 3: the object channels.
    CHECK(decode_all(encoded.units, 24, kObjects, Presentation::kAll) == programme.objects);
    // The lower presentations are the 2 and 6 channel ones of the bed, as in any stream.
    CHECK(decode_all(encoded.units, 24, 2, Presentation::k2ch).size() ==
          static_cast<std::size_t>(programme.frames) * 2);
}

TEST_CASE("an Atmos stream's major sync names four substreams and the 16-channel extension",
          "[truehd][encoder][atmos]") {
    const Programme programme = make_programme();
    const Encoded encoded = encode_all(atmos_config(), programme.pcm);
    REQUIRE_FALSE(encoded.units.empty());
    const auto unit = parse_access_unit(encoded.units.front().bytes, 0);
    REQUIRE(unit.has_value());
    REQUIRE(unit->major_sync.has_value());
    const MajorSync& sync = *unit->major_sync;
    CHECK(unit->num_substreams == 4);
    CHECK(sync.num_substreams == 4);
    CHECK(sync.extended_substream_info == 3);
    CHECK(sync.substream_info == 0xFC);
    CHECK(sync.channel_meaning.extra_present);
    CHECK(sync.size_bytes == 32);  // as the owner's discs
    REQUIRE(sync.extra.has_value());
    CHECK(sync.extra->length == 1);
    CHECK(sync.extra->sixteench_dialogue_norm == 31);
    CHECK(sync.extra->sixteench_mix_level == 35);
    CHECK(sync.extra->sixteench_channel_count == kObjects - 1);  // less the LFE, as the discs count
    CHECK(sync.extra->dyn_object_only);
    CHECK(sync.extra->lfe_present);

    // Substream 3's first block carries a restart header with sync 0x31EC and all 12 channels.
    iclforge::BitReader reader(unit->segments[3]);
    REQUIRE(reader.read(1) == 1);  // block_header_exists
    REQUIRE(reader.read(1) == 1);  // restart_header_exists
    const auto restart = read_restart_header(reader, unit->segments[3], true);
    REQUIRE(restart.has_value());
    CHECK(restart->sync == kRestartSyncAtmos);
    CHECK(restart->min_chan == 0);
    CHECK(restart->max_chan == kObjects - 1);
    CHECK(restart->max_matrix_chan == kObjects - 1);
    // The bed's substreams keep 0x31EA and 0x31EB.
    iclforge::BitReader bed(unit->segments[2]);
    REQUIRE(bed.read(2) == 3);
    CHECK(read_restart_header(bed, unit->segments[2], true)->sync == kRestartSyncNoise);
}

TEST_CASE("the bed of an Atmos stream is the stream a 7.1 encode of the bed alone writes",
          "[truehd][encoder][atmos]") {
    // Substreams 0 to 2 do not depend on the objects: a decoder that reads three substreams (any
    // 7.1 decoder, FFmpeg's included) finds the stream it would have found without the fourth.
    const Programme programme = make_programme();
    EncoderConfig plain;
    plain.channels = kBedChannels;
    const Encoded alone = encode_all(plain, programme.bed);
    const Encoded atmos = encode_all(atmos_config(), programme.pcm);
    REQUIRE(alone.units.size() == atmos.units.size());
    for (std::size_t u = 0; u < alone.units.size(); ++u) {
        const auto a = parse_access_unit(alone.units[u].bytes, 3);
        const auto b = parse_access_unit(atmos.units[u].bytes, 4);
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        for (int s = 0; s < 3; ++s) {
            const auto first = a->segments[static_cast<std::size_t>(s)];
            const auto second = b->segments[static_cast<std::size_t>(s)];
            REQUIRE(std::equal(first.begin(), first.end(), second.begin(), second.end()));
        }
    }
}

TEST_CASE(
    "an Atmos stream with the standard downmix presentations still gives back bed and objects",
    "[truehd][encoder][atmos]") {
    // The downmix folds the bed's channels into the 2 and 6 channel presentations and the later
    // substreams undo it; substream 3 has the channels to itself and is not part of it.
    const Programme programme = make_programme();
    EncoderConfig config = atmos_config();
    config.downmix = DownmixMode::kStandard;
    config.verify = true;
    const Encoded encoded = encode_all(config, programme.pcm);
    CHECK(decode_all(encoded.units, 24, kBedChannels) == programme.bed);
    CHECK(decode_all(encoded.units, 24, kObjects, Presentation::kAll) == programme.objects);
    // The 2 channel presentation is a downmix, not the first two channels of the bed.
    const auto stereo = decode_all(encoded.units, 24, 2, Presentation::k2ch);
    REQUIRE(stereo.size() == static_cast<std::size_t>(programme.frames) * 2);
    bool differs = false;
    for (std::size_t t = 0; t < static_cast<std::size_t>(programme.frames) && !differs; ++t) {
        differs =
            stereo[t * 2] != programme.bed[t * 8] || stereo[t * 2 + 1] != programme.bed[t * 8 + 1];
    }
    CHECK(differs);
}

TEST_CASE("an Atmos configuration the stream cannot carry is refused", "[truehd][encoder][atmos]") {
    const auto refusal = [](EncoderConfig config) {
        return std::string(Encoder::refusal_reason(config));
    };
    CHECK(refusal(atmos_config()).empty());
    SECTION("a bed that is not 7.1") {
        EncoderConfig config = atmos_config();
        config.channels = 6;
        CHECK(refusal(config).find("channels must be 8") != std::string::npos);
    }
    SECTION("a rate other than 48 kHz") {
        EncoderConfig config = atmos_config();
        config.sample_rate = 96000;
        CHECK(refusal(config).find("48 kHz only") != std::string::npos);
    }
    SECTION("more than sixteen channels") {
        EncoderConfig config = atmos_config();
        config.object_channels = 17;
        CHECK(refusal(config).find("object_channels") != std::string::npos);
    }
    SECTION("a field of the extension wider than its width") {
        EncoderConfig config = atmos_config();
        config.object_meaning.sixteench_mix_level = 64;
        CHECK(refusal(config).find("sixteench_mix_level") != std::string::npos);
    }
    SECTION("the caller does not set extra_present") {
        EncoderConfig config = atmos_config();
        config.channel_meaning.extra_present = true;
        CHECK(refusal(config).find("extra_present") != std::string::npos);
    }
}
