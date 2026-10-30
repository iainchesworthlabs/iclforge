#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "iclforge/truehd/decoder/decoder.hpp"

// The comparison behind EncoderConfig::verify, in a header of its own so that a test can hand it a
// decode that is wrong (the encoder never produces one).

namespace iclforge::truehd::detail {

// Why `got`, the decode of an access unit the encoder has just written, is not the source it was
// made from, or an empty view when it is. `source[c][base + t]` is channel c at sample t of the
// unit, and `expected_samples` the samples the unit holds less its padding.
[[nodiscard]] inline std::string_view verify_mismatch(
    const DecodedAccessUnit& got, int channels, int expected_samples,
    std::span<const std::vector<std::int32_t>> source, std::size_t base) noexcept {
    if (got.channels != channels || got.samples != expected_samples ||
        got.pcm.size() !=
            static_cast<std::size_t>(channels) * static_cast<std::size_t>(expected_samples)) {
        return "the decoded unit has a different shape";
    }
    for (int t = 0; t < expected_samples; ++t) {
        for (int c = 0; c < channels; ++c) {
            const std::int32_t want =
                source[static_cast<std::size_t>(c)][base + static_cast<std::size_t>(t)];
            const std::int32_t have =
                got.pcm[static_cast<std::size_t>(t) * static_cast<std::size_t>(channels) +
                        static_cast<std::size_t>(c)];
            if (want != have) {
                return "the decoded PCM differs from the input";
            }
        }
    }
    return {};
}

}  // namespace iclforge::truehd::detail
