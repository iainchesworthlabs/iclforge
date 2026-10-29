// libFuzzer harness: the TrueHD encoder over arbitrary configurations and PCM. The first bytes pick
// a configuration (rate, channels, width, restart interval, FIR order, matrix search, downmix,
// chunk size), the rest is the PCM. The encoder must refuse a configuration it does not write with
// an error, and for any other encode what it writes must decode back to the PCM:
// EncoderConfig::verify has the in-repo decoder read every unit, and a failure of that, or an
// internal error, is a bug.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "iclforge/truehd/encoder/encoder.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace iclforge::truehd;
    constexpr std::size_t kHeader = 8;
    if (size < kHeader) {
        return 0;
    }
    constexpr std::array<std::uint32_t, 6> kRates{48000, 44100, 96000, 88200, 192000, 176400};
    constexpr std::array<int, 3> kWidths{24, 16, 20};
    constexpr std::array<int, 6> kRestarts{8, 1, 9, 16, 50, 128};
    constexpr std::array<std::size_t, 4> kChunks{0, 1, 77, 4096};

    EncoderConfig config;
    config.sample_rate = kRates[data[0] % kRates.size()];
    config.channels = 1 + data[1] % 8;
    config.bits = kWidths[data[2] % kWidths.size()];
    config.restart_interval_units = kRestarts[data[3] % kRestarts.size()];
    config.max_fir_order = data[4] % 9;
    config.matrix_search = (data[5] & 1U) != 0;
    config.downmix = (data[5] & 2U) != 0 ? DownmixMode::kStandard : DownmixMode::kNone;
    config.stereo_mode = (data[5] >> 2U) % 3U;
    config.allow_rate_violations = true;
    config.verify = true;
    const std::size_t chunk_frames = kChunks[(data[5] >> 4U) % kChunks.size()];

    auto encoder = Encoder::create(config);
    if (!encoder) {
        return 0;  // a configuration it does not write, refused
    }

    // The rest of the input as PCM, `bits` wide, and at most a few restart intervals of it.
    const std::size_t channels = static_cast<std::size_t>(config.channels);
    const std::size_t samples = std::min((size - kHeader) / 2, channels * 6000);
    std::vector<std::int32_t> pcm(samples - samples % channels);
    const int up = 32 - config.bits;
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        std::int16_t raw = 0;
        std::memcpy(&raw, data + kHeader + 2 * i, sizeof(raw));
        // 16 bit values spread over the width: the low bits come from the next sample's high byte
        // so that wide widths are not all zero below the 16th.
        const std::uint32_t mix =
            (static_cast<std::uint32_t>(static_cast<std::uint16_t>(raw)) << 16U) |
            static_cast<std::uint16_t>(raw * 40503);
        pcm[i] = static_cast<std::int32_t>(mix) >> up;
    }

    const auto check = [](const auto& result) {
        if (!result && (result.error().kind == EncodeErrorKind::kVerifyFailed ||
                        result.error().kind == EncodeErrorKind::kInternal ||
                        result.error().kind == EncodeErrorKind::kBadInput)) {
            __builtin_trap();
        }
    };
    const std::size_t step = chunk_frames == 0 ? pcm.size() : chunk_frames * channels;
    for (std::size_t at = 0; at < pcm.size(); at += std::max<std::size_t>(step, channels)) {
        const std::size_t count = std::min(std::max<std::size_t>(step, channels), pcm.size() - at);
        check(encoder->encode(std::span<const std::int32_t>(pcm).subspan(at, count)));
    }
    check(encoder->flush());
    return 0;
}
