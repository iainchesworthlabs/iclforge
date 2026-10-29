#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include "iclforge/mlp/decoder/decoder.hpp"
#include "iclforge/mlp/encoder/encoder.hpp"

// iclforge::mlp::Encoder on a configuration and samples taken from the input: the first bytes
// choose the channel count, the sample rate, the restart interval, the filter orders and the
// options, the rest are 24-bit samples. The encoder's own verification decodes every access
// unit it writes; the harness then decodes the whole stream again and requires the samples
// back exactly. Only kRateExceeded (a lossless stream too large for its peak rate) is an
// expected refusal; a kVerificationFailed or a mismatch is an encoder bug and aborts.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    namespace mlp = iclforge::mlp;
    if (size < 6) {
        return 0;
    }
    static constexpr std::uint32_t kRates[] = {44100, 48000, 88200, 96000, 176400, 192000};
    mlp::EncoderConfig config;
    config.sample_rate = kRates[data[0] % 6U];
    config.channels = config.sample_rate > 96000 ? 1 + data[1] % 2U : 1 + data[1] % 8U;
    config.restart_interval = 8 + data[2] % 24U;
    config.max_fir_order = data[3] % 9U;
    config.max_iir_order = data[4] % 5U;
    config.downmix = (data[5] & 1U) != 0;
    config.decorrelate = (data[5] & 2U) != 0;
    // Samples from the rest: three bytes each, sign-extended from 24 bits, a whole number of
    // frames.
    const std::size_t channels = static_cast<std::size_t>(config.channels);
    const std::size_t count = (size - 6) / 3 / channels * channels;
    std::vector<std::int32_t> pcm(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t* p = data + 6 + i * 3;
        const auto v = static_cast<std::uint32_t>(p[0]) << 24U |
                       static_cast<std::uint32_t>(p[1]) << 16U |
                       static_cast<std::uint32_t>(p[2]) << 8U;
        pcm[i] = static_cast<std::int32_t>(v) >> 8;
    }
    auto stream = mlp::encode_stream(pcm, config);
    if (!stream) {
        if (stream.error() == mlp::Error::kRateExceeded) {
            return 0;
        }
        std::abort();
    }
    auto decoded = mlp::decode_stream(*stream);
    if (!decoded || decoded->pcm != pcm) {
        std::abort();
    }
    return 0;
}
