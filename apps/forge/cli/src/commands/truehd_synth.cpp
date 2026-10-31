#include <fmt/base.h>
#include <fmt/format.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../exit_codes.hpp"
#include "../support.hpp"
#include "iclforge/ac3/io/wav.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"
#include "truehd.hpp"
#include "truehd_encode_core.hpp"

// `silence` and `sine` with a `.thd` output (planning/truehd-parity-forge.md, row P4 and owner
// question 6: the suffix chooses the codec, as `transcode`'s does): a generated signal coded by the
// lossless encoder, so that a TrueHD test stream needs no WAV first. The stream is what `truehd-encode`
// makes of the same signal as a WAV with the channel mask below, which tests/test_cli_truehd.cpp
// holds. AC-3's bit rate and metadata options have no meaning for it and are refused by name.

namespace forge_cli::commands {

namespace {

namespace th_encode = iclforge::apps::truehd_encode;

struct Shape {
    std::string_view name;
    std::size_t channels;
    std::uint32_t mask;  // the WAV channel mask: what `decode` writes for the layout
};

// The layouts a generated TrueHD stream takes, as `decode` names them.
constexpr Shape kShapes[] = {
    {"mono", 1, 0x4},       {"stereo", 2, 0x3},   {"2/0", 2, 0x3},   {"51", 6, 0x60F},
    {"5.1", 6, 0x60F},      {"3/2.1", 6, 0x60F},  {"71", 8, 0x63F}, {"7.1", 8, 0x63F},
};

}  // namespace

int run_truehd_synth(std::string_view out_path, std::uint32_t seconds, bool sine,
                     std::uint32_t freq_hz, std::uint32_t amplitude_pct, std::string_view layout,
                     const Options& meta) {
    if (const int refused = check_options_applied(meta, {}); refused != kExitOk) {
        return refused;
    }
    const std::string_view wanted = layout.empty() ? std::string_view{"stereo"} : layout;
    const Shape* shape = nullptr;
    for (const Shape& candidate : kShapes) {
        if (candidate.name == wanted) {
            shape = &candidate;
        }
    }
    if (shape == nullptr) {
        fmt::println(stderr,
                     "error: a generated TrueHD stream takes the layout mono, stereo, 51 or 71 "
                     "(got '{}')",
                     wanted);
        return kExitUsage;
    }
    constexpr std::uint32_t kRate = 48000;
    const std::size_t frames = static_cast<std::size_t>(seconds) * kRate;
    if (frames == 0) {
        fmt::println(stderr, "error: a stream of no seconds has nothing to code");
        return kExitUsage;
    }
    iclforge::ac3::io::WavData wav;
    wav.sample_rate = kRate;
    wav.channel_mask = shape->mask;
    wav.channels.assign(shape->channels, std::vector<float>(frames, 0.0F));
    if (sine) {
        // A tone in each channel, so that a mix-up of channels shows: freq_hz in the first, and each
        // later channel a quarter of freq_hz higher than the one before.
        const double amplitude = static_cast<double>(amplitude_pct) / 100.0;
        for (std::size_t c = 0; c < shape->channels; ++c) {
            const double hz = static_cast<double>(freq_hz) * (1.0 + 0.25 * static_cast<double>(c));
            for (std::size_t n = 0; n < frames; ++n) {
                wav.channels[c][n] = static_cast<float>(
                    amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) /
                                         static_cast<double>(kRate)));
            }
        }
    }
    const auto mapped = th_encode::layout_of(wav);
    if (!mapped) {
        fmt::println(stderr, "error: {}", mapped.error());
        return kExitInternal;
    }
    const auto outcome = th_encode::encode_wav(wav, *mapped, iclforge::truehd::EncoderConfig{});
    if (!outcome) {
        fmt::println(stderr, "error: {}{}{}", iclforge::truehd::to_string(outcome.error().error.kind),
                     outcome.error().error.detail.empty() ? "" : ": ", outcome.error().error.detail);
        return kExitInternal;
    }
    EncodedStreamSink sink;
    if (!sink.open(out_path, /*keep_partial=*/false)) {
        return kExitOutput;
    }
    if (!sink.push(std::span<const std::byte>(outcome->stream)) || !sink.close()) {
        sink.abort();
        return kExitOutput;
    }
    status_println(status_stream(out_path),
                   "wrote {} s of {} ({} channel(s), {} access units, {} bytes) to {}", seconds,
                   sine ? "sine" : "silence", shape->channels, outcome->access_units,
                   outcome->stream.size(), out_path);
    return kExitOk;
}

}  // namespace forge_cli::commands
