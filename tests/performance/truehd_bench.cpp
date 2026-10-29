#include "truehd_bench.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>

#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"

namespace perf::truehd_bench {
namespace {

constexpr std::size_t kChannels = 6;
constexpr std::uint32_t kSampleRate = 48000;
constexpr double kFullScale24 = 8388608.0;  // 2^23: the fixture's [-1, 1) as 24-bit samples

// The fixture's samples as the encoder takes them: interleaved, 24 bits, not left-justified. The WAV's
// channel order (FL FR FC LFE BL BR) is the one the encoder's default 6-channel assignment names (L R C
// LFE Ls Rs), so there is no permutation. Frames wrap at the end of the fixture, as the other sources
// do; at kUnits they do not need to.
std::vector<std::int32_t> interleaved_pcm(const iclforge::ac3::io::WavData& wav,
                                          std::size_t frames) {
    std::vector<std::int32_t> pcm(frames * kChannels);
    const std::size_t available = wav.frame_count();
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::size_t ch = 0; ch < kChannels; ++ch) {
            const double scaled = std::round(
                static_cast<double>(wav.channels[ch][frame % available]) * kFullScale24);
            pcm[frame * kChannels + ch] = static_cast<std::int32_t>(
                std::clamp(scaled, -kFullScale24, kFullScale24 - 1.0));
        }
    }
    return pcm;
}

}  // namespace

DecodeRun time_decode(const iclforge::ac3::io::WavData& wav, int frames, int warmup_units) {
    DecodeRun run;
    run.built = true;
    if (wav.channels.size() < kChannels) {
        run.error = "the fixture has fewer than six channels";
        return run;
    }

    auto encoder = iclforge::truehd::Encoder::create(
        {.sample_rate = kSampleRate, .channels = static_cast<int>(kChannels), .bits = 24});
    if (!encoder) {
        run.error = "the TrueHD encoder refused the configuration";
        return run;
    }
    const auto per_unit = static_cast<std::size_t>(encoder->samples_per_access_unit());
    const auto units = static_cast<std::size_t>(frames) * static_cast<std::size_t>(kUnitsPerFrame);
    run.budget_ms = 1000.0 * static_cast<double>(per_unit * kUnitsPerFrame) /
                    static_cast<double>(kSampleRate);

    // The stream, whole, before anything is timed: a restart interval leaves the encoder only when it is
    // complete, so the units arrive in runs of 128 and flush() brings the rest.
    const std::vector<std::int32_t> pcm = interleaved_pcm(wav, per_unit * units);
    std::vector<std::vector<std::byte>> stream;
    stream.reserve(units + 1);
    auto coded = encoder->encode(pcm);
    if (!coded) {
        run.error = "the TrueHD encoder failed on the fixture";
        return run;
    }
    for (auto& unit : *coded) {
        stream.push_back(std::move(unit.bytes));
    }
    auto tail = encoder->flush();
    if (!tail) {
        run.error = "the TrueHD encoder failed to flush";
        return run;
    }
    for (auto& unit : *tail) {
        stream.push_back(std::move(unit.bytes));
    }
    if (stream.size() < units) {
        run.error = "the TrueHD encoder wrote fewer access units than the samples it was given";
        return run;
    }

    {
        iclforge::truehd::Decoder warm;
        for (std::size_t i = 0; i < static_cast<std::size_t>(warmup_units) && i < stream.size();
             ++i) {
            (void)warm.decode(stream[i]);
        }
    }

    iclforge::truehd::Decoder decoder;
    run.frame_ms.reserve(static_cast<std::size_t>(frames));
    for (std::size_t first = 0; first < units; first += kUnitsPerFrame) {
        bool refused = false;
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t i = first; i < first + kUnitsPerFrame; ++i) {
            refused = !decoder.decode(stream[i]) || refused;
        }
        run.frame_ms.push_back(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count());
        if (refused) {
            run.error = "the TrueHD decoder refused a unit the TrueHD encoder wrote";
            run.frame_ms.clear();
            return run;
        }
    }
    return run;
}

}  // namespace perf::truehd_bench
