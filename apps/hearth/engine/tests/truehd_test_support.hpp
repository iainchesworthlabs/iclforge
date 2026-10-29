#pragma once

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "iclforge/base/crypto/sha256.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/truehd/core/format.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"

// What the Hearth tests of TrueHD share: the FFmpeg-made streams of testdata/truehd and their
// manifest (tools/generators/gen_truehd_ffmpeg.py), the digest the manifest states, and the output
// layout that has exactly a stream's channels.

namespace iclforge::hearth::testing {

inline std::string truehd_dir() { return std::string(ICLFORGE_GOLDEN_TRUEHD_DIR); }

inline std::vector<std::byte> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

inline nlohmann::json manifest() {
    std::ifstream in(truehd_dir() + "/manifest.json");
    REQUIRE(in.good());
    return nlohmann::json::parse(in);
}

inline std::string hex(const std::array<std::byte, 32>& digest) {
    constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (const std::byte b : digest) {
        out.push_back(kDigits[std::to_integer<unsigned>(b) >> 4U]);
        out.push_back(kDigits[std::to_integer<unsigned>(b) & 15U]);
    }
    return out;
}

// What FFmpeg's `-f s32le` writes for 24-bit samples: each shifted left by 8, little-endian.
inline void append_s32le(std::vector<std::byte>& out, std::int32_t sample) {
    const auto word = static_cast<std::uint32_t>(sample) << 8U;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::byte>((word >> shift) & 0xFFU));
    }
}

// The place on a Hearth output layout of each speaker a TrueHD stream names, which is the same
// speaker (stream_decoder.cpp's truehd_location()).
inline iclforge::base::Location location_of(iclforge::truehd::Channel channel) {
    using iclforge::base::Location;
    using iclforge::truehd::Channel;
    switch (channel) {
        case Channel::kL: return Location::kLeft;
        case Channel::kR: return Location::kRight;
        case Channel::kC: return Location::kCentre;
        case Channel::kLfe: return Location::kLfe;
        case Channel::kLs: return Location::kLeftSurround;
        case Channel::kRs: return Location::kRightSurround;
        case Channel::kLvh: return Location::kVhl;
        case Channel::kRvh: return Location::kVhr;
        case Channel::kLc: return Location::kLc;
        case Channel::kRc: return Location::kRc;
        case Channel::kLrs: return Location::kLrs;
        case Channel::kRrs: return Location::kRrs;
        case Channel::kCs: return Location::kCs;
        case Channel::kTs: return Location::kTs;
        case Channel::kLsd: return Location::kLsd;
        case Channel::kRsd: return Location::kRsd;
        case Channel::kLw: return Location::kLw;
        case Channel::kRw: return Location::kRw;
        case Channel::kCvh: return Location::kVhc;
        case Channel::kLfe2: return Location::kLfe2;
    }
    return Location::kLeft;
}

// The output layout with exactly these channels, in this order, so that what a renderer puts on
// slot n is channel n.
inline iclforge::render::OutputLayout layout_of(std::span<const iclforge::truehd::Channel> channels) {
    std::vector<iclforge::base::Location> locations;
    for (const auto channel : channels) {
        locations.push_back(location_of(channel));
    }
    const auto layout = iclforge::render::OutputLayout::from_locations(locations);
    REQUIRE(layout.has_value());
    return *layout;
}

// A stream of the library's encoder, long enough to have many restart intervals and big enough to
// pass the window a cursor reads a file in, with the 24-bit PCM it was made from: a different tone
// in every channel, a slow change of level, noise, and a burst of louder noise in the middle.
struct MadeStream {
    std::vector<std::byte> bytes;
    std::vector<std::int32_t> pcm;  // interleaved, 24-bit
    std::vector<std::uint64_t> unit_offsets;
    std::vector<bool> has_major_sync;  // by unit
    std::uint32_t sample_rate = 48000;
    int channels = 0;
    int samples_per_unit = 0;
    std::uint64_t frames = 0;
};

inline MadeStream make_stream(int channels, std::uint64_t frames, std::uint32_t sample_rate = 48000,
                              int restart_interval_units = 128, std::uint32_t seed = 7,
                              int silent_channel = -1) {
    MadeStream out;
    out.channels = channels;
    out.frames = frames;
    out.sample_rate = sample_rate;
    out.pcm.resize(static_cast<std::size_t>(channels) * frames);
    std::uint32_t state = seed * 2654435761U + 12345U;
    const double full = 8388607.0;
    for (std::uint64_t t = 0; t < frames; ++t) {
        for (int c = 0; c < channels; ++c) {
            state = state * 1664525U + 1013904223U;
            const double noise = static_cast<double>(state >> 8U) / 16777216.0 - 0.5;
            const double tone = std::sin(2.0 * std::numbers::pi * (220.0 + 137.0 * c) *
                                         static_cast<double>(t) / sample_rate);
            const double level =
                0.05 + 0.4 * (0.5 + 0.5 * std::sin(2.0 * std::numbers::pi * 0.7 *
                                                   static_cast<double>(t) / sample_rate));
            double v = level * tone + 0.01 * noise;
            if (t > frames / 2 && t < frames / 2 + frames / 10) {
                v += 0.3 * noise;
            }
            out.pcm[static_cast<std::size_t>(t) * static_cast<std::size_t>(channels) +
                    static_cast<std::size_t>(c)] =
                c == silent_channel
                    ? 0
                    : static_cast<std::int32_t>(std::lround(std::clamp(v, -0.999, 0.999) * full));
        }
    }
    iclforge::truehd::EncoderConfig config;
    config.sample_rate = sample_rate;
    config.channels = channels;
    config.bits = 24;
    config.restart_interval_units = restart_interval_units;
    auto encoder = iclforge::truehd::Encoder::create(config);
    REQUIRE(encoder.has_value());
    out.samples_per_unit = encoder->samples_per_access_unit();
    const auto take = [&out](std::vector<iclforge::truehd::EncodedAccessUnit>&& units) {
        for (auto& unit : units) {
            out.unit_offsets.push_back(out.bytes.size());
            out.has_major_sync.push_back(unit.has_major_sync);
            out.bytes.insert(out.bytes.end(), unit.bytes.begin(), unit.bytes.end());
        }
    };
    auto units = encoder->encode(out.pcm);
    REQUIRE(units.has_value());
    take(std::move(*units));
    auto tail = encoder->flush();
    REQUIRE(tail.has_value());
    take(std::move(*tail));
    return out;
}

}  // namespace iclforge::hearth::testing
