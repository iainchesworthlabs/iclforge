#pragma once

// What the AC-4 content signing tests of the forge command line share: a stream the encoder wrote (about
// ten frames from two tones, so the bytes are the shape of a real stream and not a made-up block),
// the AC-4 track an MP4 of it needs, and the elementary stream's framing. Keys and chains come from
// tests/support/test_pki.hpp, which builds them from a seed at run time; none is committed.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "iclforge/ac4/encoder/encoder.hpp"
#include "iclforge/ac4/io/carriage.hpp"
#include "iclforge/containers/mp4/mp4.hpp"

namespace iclforge::test::media_signing {

struct Encoded {
    std::vector<iclforge::ac4::EncodedFrame> frames;
    iclforge::containers::mp4::AudioTrack track;

    [[nodiscard]] std::vector<std::span<const std::byte>> views() const {
        std::vector<std::span<const std::byte>> out;
        for (const auto& frame : frames) {
            out.emplace_back(frame.raw_ac4_frame);
        }
        return out;
    }

    // The elementary stream: every frame in an ac4_syncframe().
    [[nodiscard]] std::vector<std::byte> elementary(bool crc = false) const {
        std::vector<std::byte> out;
        for (const auto& frame : frames) {
            const std::vector<std::byte> sync = iclforge::ac4::sync_frame(frame.raw_ac4_frame, crc);
            out.insert(out.end(), sync.begin(), sync.end());
        }
        return out;
    }
};

inline std::vector<float> tone(double hz, std::size_t count) {
    std::vector<float> x(count);
    for (std::size_t n = 0; n < count; ++n) {
        x[n] = static_cast<float>(
            0.4 * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) / 48000.0));
    }
    return x;
}

// Stereo, 48 kHz, `samples` long, with the AC-4 track the encoder describes.
inline Encoded encode(std::size_t samples = 24000) {
    auto encoder = iclforge::ac4::Encoder::create(iclforge::ac4::EncoderConfig{});
    REQUIRE(encoder.has_value());
    const std::vector<float> left = tone(440.0, samples);
    const std::vector<float> right = tone(660.0, samples);
    const std::vector<std::span<const float>> channels{left, right};
    Encoded out;
    auto first = encoder->encode(channels);
    REQUIRE(first.has_value());
    out.frames = std::move(*first);
    auto rest = encoder->flush();
    REQUIRE(rest.has_value());
    out.frames.insert(out.frames.end(), rest->begin(), rest->end());
    REQUIRE(out.frames.size() >= 3);

    namespace mp4 = iclforge::containers::mp4;
    out.track.codec_id = std::string{mp4::kCodecAc4};
    out.track.sample_rate = 48000;
    out.track.channels = 2;
    out.track.samples_per_frame = static_cast<std::uint32_t>(out.frames.front().samples);
    out.track.codec_config = iclforge::ac4::build_dac4(encoder->toc());
    out.track.rfc6381 = iclforge::ac4::rfc6381_codec_string(encoder->toc());
    REQUIRE_FALSE(out.track.codec_config.empty());
    return out;
}

// One stream for the whole binary: encoding takes a moment and every case starts from it.
inline const Encoded& shared() {
    static const Encoded stream = encode();
    return stream;
}

}  // namespace iclforge::test::media_signing
