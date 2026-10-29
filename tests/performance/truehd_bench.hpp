#pragma once

// The TrueHD workloads of the performance suite: the source PCM, the streams the library's own
// encoder writes from it, and the fixtures under testdata/truehd. Shared by iclforge-perf,
// iclforge-bench, iclforge-membench and iclforge-kernelbench the way ac4_bench.hpp is for AC-4, so
// the four cannot drift apart on what a TrueHD workload is.
//
// A TrueHD "frame" in these benches is an access unit, the codec's own unit (TH spec §3): 40 samples
// at 44.1 and 48 kHz, 80 at 88.2 and 96, 160 at 176.4 and 192, so 0.83 ms of audio at 48 kHz
// against A/52's 32 ms. Each result carries its own real-time budget, as AC-4's do.
//
// The encoder holds one restart interval of input (128 access units by default) before it writes
// it. A bench that gives it one access unit a call therefore sees 127 calls that only buffer and a
// 128th that analyses and codes the whole interval: the mean is what the codec costs, and the
// maximum is the interval's burst, which a real-time caller has to be able to absorb.
//
// The input is real programme material, like every other workload here: reference_51.wav's own
// channels, in the WAV's order (FL, FR, FC, LFE, BL, BR), which is TrueHD's 5.1 assignment. The
// file is 16 bit. A 16 bit source is coded as 16 bit; the 24 bit workloads put the 16 bit samples
// in the high bits and a deterministic low byte beneath them, which stands for the noise floor of
// a 24 bit master and is, deliberately, incompressible: it is the expensive case for the residual
// coder and the cheap one for the filter search.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"
#include "iclforge/truehd/io/elementary.hpp"
#include "real_audio.hpp"

namespace perf::truehd_bench {

inline constexpr std::array<std::size_t, 2> kStereoChannels = {0, 1};  // FL, FR
inline constexpr std::array<std::size_t, 6> kFiveOneChannels = {0, 1, 2, 3, 4, 5};

// Access units a memory-bench or hard-gate run feeds: three restart intervals.
inline constexpr int kIntervalUnits = 128;

struct Source {
    int channels = 0;
    int bits = 0;
    std::uint32_t sample_rate = 0;
    std::vector<std::int32_t> pcm;  // interleaved, `bits` wide, not left-justified

    [[nodiscard]] std::size_t frames() const { return pcm.size() / static_cast<std::size_t>(channels); }
};

// `channels` of the WAV, interleaved, `bits` wide (16 or 24).
inline Source make_source(const iclforge::ac3::io::WavData& wav,
                          std::span<const std::size_t> channels, int bits) {
    Source source{.channels = static_cast<int>(channels.size()),
                  .bits = bits,
                  .sample_rate = wav.sample_rate};
    const std::size_t frames = wav.frame_count();
    source.pcm.reserve(frames * channels.size());
    std::uint32_t lcg = 0x2545F491U;
    for (std::size_t t = 0; t < frames; ++t) {
        for (const std::size_t ch : channels) {
            const auto sixteen = static_cast<std::int32_t>(
                std::lround(wav.channels[ch % wav.channels.size()][t] * 32768.0F));
            std::int32_t value = std::clamp(sixteen, -32768, 32767);
            if (bits == 24) {
                lcg = lcg * 1664525U + 1013904223U;
                value = value * 256 + static_cast<std::int32_t>((lcg >> 24) & 0xFFU) - 128;
                value = std::clamp(value, -8388608, 8388607);
            }
            source.pcm.push_back(value);
        }
    }
    return source;
}

[[nodiscard]] inline iclforge::truehd::EncoderConfig config_for(const Source& source) {
    iclforge::truehd::EncoderConfig config;
    config.sample_rate = source.sample_rate;
    config.channels = source.channels;
    config.bits = source.bits;
    return config;
}

// One access unit's worth of interleaved PCM, wrapping at the end of the source.
[[nodiscard]] inline std::span<const std::int32_t> unit_pcm(const Source& source, int samples,
                                                            std::size_t index) {
    const std::size_t per_unit = static_cast<std::size_t>(samples) *
                                 static_cast<std::size_t>(source.channels);
    const std::size_t units = source.pcm.size() / per_unit;
    return std::span<const std::int32_t>(source.pcm).subspan((index % units) * per_unit, per_unit);
}

[[nodiscard]] inline double budget_ms(std::uint32_t sample_rate, int samples_per_unit) {
    return 1000.0 * static_cast<double>(samples_per_unit) / static_cast<double>(sample_rate);
}

struct Stream {
    std::vector<std::vector<std::byte>> units;  // access units as the encoder wrote them
    int samples_per_unit = 0;
    std::uint32_t sample_rate = 0;
};

// The stream the library's encoder writes for `source`, kept as access units. Exits on failure: a
// bench that cannot make its input has measured nothing.
[[nodiscard]] inline Stream encode_stream(const Source& source,
                                          const iclforge::truehd::EncoderConfig& config) {
    auto encoder = iclforge::truehd::Encoder::create(config);
    if (!encoder) {
        std::fprintf(stderr, "truehd bench: the encoder refused its configuration\n");
        std::exit(1);
    }
    Stream stream{.samples_per_unit = encoder->samples_per_access_unit(),
                  .sample_rate = config.sample_rate};
    const auto take = [&](auto&& result) {
        if (!result) {
            std::fprintf(stderr, "truehd bench: the encoder failed building its source stream\n");
            std::exit(1);
        }
        for (auto& unit : *result) {
            stream.units.push_back(std::move(unit.bytes));
        }
    };
    take(encoder->encode(source.pcm));
    take(encoder->flush());
    return stream;
}

// --- the committed fixtures ------------------------------------------------------------------

struct Fixture {
    std::string name;  // the file's stem: truehd_st48_s24
    std::vector<std::byte> bytes;
    iclforge::truehd::StreamInfo info;
    bool truehd = false;  // the encoder can write this kind of stream; an .mlp it cannot
    int bits = 0;         // 16 or 24, from the name
};

// Every .thd and .mlp under testdata/truehd (not handbuilt/), in name order.
[[nodiscard]] inline std::vector<Fixture> load_fixtures(const std::string& directory) {
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        const auto extension = entry.path().extension().string();
        if (entry.is_regular_file() && (extension == ".thd" || extension == ".mlp")) {
            paths.push_back(entry.path());
        }
    }
    std::ranges::sort(paths);
    std::vector<Fixture> fixtures;
    for (const auto& path : paths) {
        std::ifstream in(path, std::ios::binary);
        std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        Fixture fixture{.name = path.stem().string()};
        fixture.bytes.resize(raw.size());
        std::ranges::transform(raw, fixture.bytes.begin(),
                               [](char c) { return static_cast<std::byte>(c); });
        auto info = iclforge::truehd::probe_stream(fixture.bytes);
        if (!info) {
            std::fprintf(stderr, "truehd bench: cannot probe fixture '%s'\n", fixture.name.c_str());
            std::exit(1);
        }
        fixture.info = *info;
        fixture.truehd = path.extension() == ".thd";
        fixture.bits = fixture.name.ends_with("_s16") ? 16 : 24;
        fixtures.push_back(std::move(fixture));
    }
    if (fixtures.empty()) {
        std::fprintf(stderr, "truehd bench: no fixtures under '%s'\n", directory.c_str());
        std::exit(1);
    }
    return fixtures;
}

// A fixture's access units, split once so a timed loop holds no scan.
[[nodiscard]] inline std::vector<std::span<const std::byte>> split_units(
    const std::vector<std::byte>& bytes) {
    std::vector<std::span<const std::byte>> units;
    iclforge::truehd::ElementaryReader reader(bytes);
    while (const auto unit = reader.next()) {
        units.push_back(*unit);
    }
    return units;
}

// The PCM a fixture decodes to, interleaved, for the encode workload over the same material.
[[nodiscard]] inline Source decode_to_source(const Fixture& fixture) {
    iclforge::truehd::Decoder decoder;
    Source source{.channels = 0, .bits = fixture.bits, .sample_rate = fixture.info.sample_rate};
    for (const auto unit : split_units(fixture.bytes)) {
        auto result = decoder.decode(unit);
        if (!result) {
            std::fprintf(stderr, "truehd bench: fixture '%s' did not decode\n",
                         fixture.name.c_str());
            std::exit(1);
        }
        if (*result) {
            source.channels = (*result)->channels;
            // The decoder's samples are 24 bits wide; a 16 bit stream's sit in the high 16 of them.
            for (const std::int32_t value : (*result)->pcm) {
                source.pcm.push_back(value >> (24 - fixture.bits));
            }
        }
    }
    return source;
}

}  // namespace perf::truehd_bench
