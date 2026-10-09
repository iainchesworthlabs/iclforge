#include "iclforge/dsp/resampler.hpp"

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <numeric>
#include <optional>
#include <span>
#include <vector>

#include "tiered/resampler.hpp"
#include "tiered/resampler_design.hpp"

namespace iclforge::dsp {

namespace {

// The longest table, in coefficients, the converter's filter is built for (8 MB of doubles).
// A ratio whose table is longer - two rates with no common factor to speak of, up a phase for
// every output sample of a second - is designed one phase at a time as an output needs it, with
// the same design.
constexpr std::size_t kTableLimit = std::size_t{1} << 20U;

std::vector<float> resample_one(std::span<const float> input, std::uint32_t input_rate,
                                std::uint32_t output_rate) {
    const auto common = std::gcd(input_rate, output_rate);
    const auto up = static_cast<std::int64_t>(output_rate / common);
    const auto down = static_cast<std::int64_t>(input_rate / common);

    // tiered::BasicResamplerFilter designs a ratio in lowest terms; design_resampler is what it
    // calls, so the length is known before any table is.
    const tiered::ResamplerDesign design =
        tiered::design_resampler<tiered::LibmMath>(static_cast<int>(up), static_cast<int>(down));
    const auto taps = static_cast<std::int64_t>(design.taps);
    const bool tabulated =
        static_cast<std::size_t>(up) * static_cast<std::size_t>(taps) <= kTableLimit;
    std::optional<tiered::ResamplerFilter> filter;
    if (tabulated) {
        filter.emplace(static_cast<int>(up), static_cast<int>(down));
    }
    std::vector<double> row(tabulated ? 0 : static_cast<std::size_t>(taps));
    std::int64_t row_phase = -1;

    const auto in_frames = static_cast<std::int64_t>(input.size());
    // Nearest whole frame count for the exact ratio, so the converted buffer lasts as long as the
    // input as nearly as a whole frame allows.
    const auto out_frames = static_cast<std::size_t>(std::llround(
        static_cast<double>(input.size()) * static_cast<double>(up) / static_cast<double>(down)));
    std::vector<float> output(out_frames);

    // Output m is the input at m * down / up: tiered::Resampler::process's grid and dot product,
    // over the whole buffer, with the input taken to be silence before its first sample and after
    // its last. That output falls p / up past input sample `whole`, and phase p's coefficient k
    // weights input sample whole + k + 1 - taps / 2.
    for (std::size_t m = 0; m < out_frames; ++m) {
        const std::int64_t position = static_cast<std::int64_t>(m) * down;
        const std::int64_t whole = position / up;
        const std::int64_t p = position - whole * up;
        const double* coefficients = nullptr;
        if (tabulated) {
            coefficients = filter->phase(static_cast<int>(p)).coefficients;
        } else {
            if (p != row_phase) {
                tiered::design_phase<tiered::LibmMath>(design, static_cast<int>(p), row.data());
                row_phase = p;
            }
            coefficients = row.data();
        }
        const std::int64_t start = whole + 1 - taps / 2;
        double sum = 0.0;
        for (std::int64_t k = 0; k < taps; ++k) {
            const std::int64_t n = start + k;
            if (n >= 0 && n < in_frames) {
                sum += coefficients[k] * static_cast<double>(input[static_cast<std::size_t>(n)]);
            }
        }
        output[m] = static_cast<float>(sum);
    }
    return output;
}

}  // namespace

std::vector<float> resample(std::span<const float> input, std::uint32_t input_rate,
                            std::uint32_t output_rate) {
    if (input.empty()) {
        return {};
    }
    if (input_rate == output_rate) {
        // Exact identity, not "close": a 1:1 conversion has an exact answer, so return it.
        return std::vector<float>(input.begin(), input.end());
    }
    if (input_rate == 0 || output_rate == 0) {
        // A zero rate has no cutoff and no output duration, so there is no answer to give.
        return {};
    }
    return resample_one(input, input_rate, output_rate);
}

std::vector<std::vector<float>> resample_planar(std::span<const std::vector<float>> channels,
                                                std::uint32_t input_rate,
                                                std::uint32_t output_rate) {
    std::vector<std::vector<float>> result;
    result.reserve(channels.size());
    for (const auto& channel : channels) {
        result.push_back(resample(channel, input_rate, output_rate));
    }
    return result;
}

}  // namespace iclforge::dsp
