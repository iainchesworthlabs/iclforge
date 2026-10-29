// Encode 5.1 PCM to TrueHD and decode it again, as a disc authoring tool and a player would. The
// encoder (iclforge::mlp) takes the samples in pieces of any size and hands back access units as
// each restart interval completes; flush() codes the rest. The decoder takes one access unit at a
// time, as scan() frames them, and gives the presentation it is asked for: the 6-channel one gives
// back every input sample exactly, and the 2-channel one is the stereo fold-down the encoder built
// for a player that has only two speakers.
//
// Usage: truehd_round_trip. ctest runs it; it exits 1 if the round trip is not exact.

#include <fmt/printf.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include "iclforge/mlp/decoder/decoder.hpp"
#include "iclforge/mlp/encoder/encoder.hpp"
#include "iclforge/mlp/io/elementary.hpp"

namespace {

constexpr std::uint32_t kRate = 48000;
constexpr int kChannels = 6;  // L R C LFE Ls Rs, section 13.1's order

// Half a second of 24-bit 5.1: a different tone on each channel, with a little noise so the
// prediction filters have something to do. The length is not a whole number of access units (40
// samples at 48 kHz), so the last one is padded and the decoder drops the padding.
std::vector<std::int32_t> synthesise() {
    const std::size_t frames = kRate / 2 + 17;
    std::vector<std::int32_t> pcm(frames * kChannels);
    std::uint32_t noise = 12345;
    for (std::size_t i = 0; i < frames; ++i) {
        for (int ch = 0; ch < kChannels; ++ch) {
            noise = noise * 1664525U + 1013904223U;
            const double tone = std::sin(2.0 * std::numbers::pi * (110.0 * (ch + 1)) *
                                         static_cast<double>(i) / kRate);
            const double dither = (static_cast<double>(noise >> 8) / 16777216.0 - 0.5) * 0.002;
            pcm[i * kChannels + static_cast<std::size_t>(ch)] =
                static_cast<std::int32_t>(std::lround((0.4 * tone + dither) * 8388607.0));
        }
    }
    return pcm;
}

// Every access unit of `stream` through a decoder of the presentation with `channels`.
std::vector<std::int32_t> decode(std::span<const std::byte> stream, int channels) {
    iclforge::mlp::DecoderConfig config;
    config.presentation_channels = channels;
    iclforge::mlp::Decoder decoder(config);
    std::vector<std::int32_t> pcm;
    for (const iclforge::mlp::AccessUnitInfo& unit : iclforge::mlp::scan(stream).units) {
        const auto decoded = decoder.decode(stream.subspan(unit.offset, unit.size));
        if (!decoded) {
            fmt::printf("decode failed: %s\n",
                        std::string(iclforge::mlp::describe(decoded.error())));
            return {};
        }
        pcm.insert(pcm.end(), decoded->pcm.begin(), decoded->pcm.end());
    }
    return pcm;
}

}  // namespace

int main() {
    const std::vector<std::int32_t> source = synthesise();

    iclforge::mlp::EncoderConfig config;
    config.sample_rate = kRate;
    config.channels = kChannels;
    auto encoder = iclforge::mlp::Encoder::create(config);
    if (!encoder) {
        fmt::printf("encoder: %s\n", std::string(iclforge::mlp::describe(encoder.error())));
        return 1;
    }

    // The samples arrive in pieces of 1000 frames, which is not a multiple of anything the
    // encoder cares about.
    std::vector<std::byte> stream;
    const std::size_t piece = 1000 * kChannels;
    for (std::size_t at = 0; at < source.size(); at += piece) {
        const std::size_t count = std::min(piece, source.size() - at);
        const auto units = encoder->encode(std::span(source).subspan(at, count));
        if (!units) {
            fmt::printf("encode: %s\n", std::string(iclforge::mlp::describe(units.error())));
            return 1;
        }
        stream.insert(stream.end(), units->begin(), units->end());
    }
    const auto tail = encoder->flush();
    if (!tail) {
        fmt::printf("flush: %s\n", std::string(iclforge::mlp::describe(tail.error())));
        return 1;
    }
    stream.insert(stream.end(), tail->begin(), tail->end());

    const iclforge::mlp::EncoderStats& stats = encoder->stats();
    fmt::printf(
        "%zu bytes of PCM in, %zu bytes of TrueHD out (%llu access units, peak %llu kbit/s)\n",
        source.size() * 3, stream.size(), static_cast<unsigned long long>(stats.access_units),
        static_cast<unsigned long long>(stats.peak_bits_per_second / 1000));

    const std::vector<std::int32_t> six = decode(stream, 6);
    const bool exact = six == source;
    fmt::printf("6-channel presentation: %s\n", exact ? "identical to the source" : "DIFFERS");

    // The stereo presentation is the fold-down: L + 0.707 (C + Ls), R + 0.707 (C + Rs), scaled by
    // the encoder's limiter if that would clip. Show how loud it is against the front pair.
    const std::vector<std::int32_t> two = decode(stream, 2);
    std::int32_t peak_two = 0;
    for (const std::int32_t s : two) {
        peak_two = std::max(peak_two, s < 0 ? -s : s);
    }
    fmt::printf("2-channel presentation: %zu frames, peak %.1f dBFS\n", two.size() / 2,
                20.0 * std::log10(std::max(1, peak_two) / 8388608.0));
    return exact && two.size() / 2 == source.size() / kChannels ? 0 : 1;
}
