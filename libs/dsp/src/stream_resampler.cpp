#include "iclforge/dsp/stream_resampler.hpp"

#include <algorithm>
#include <memory>
#include <numeric>
#include <utility>

#include "tiered/resampler.hpp"

namespace iclforge::dsp {

struct StreamResampler::Impl {
    std::shared_ptr<const tiered::BasicResamplerFilter<tiered::Real>> filter;
    std::vector<tiered::Resampler<tiered::Real>> channels;
    std::vector<tiered::Real> input;
    std::vector<tiered::Real> output;
};

StreamResampler::StreamResampler(std::uint32_t input_rate, std::uint32_t output_rate,
                                 std::size_t channels)
    : input_rate_(input_rate),
      output_rate_(output_rate),
      channels_(channels),
      impl_(std::make_unique<Impl>()) {
    // The filter is designed for the ratio in lowest terms, as resample() does.
    const std::uint32_t common = std::gcd(input_rate, output_rate);
    const int up = common == 0 ? 1 : static_cast<int>(output_rate / common);
    const int down = common == 0 ? 1 : static_cast<int>(input_rate / common);
    impl_->filter = std::make_shared<const tiered::BasicResamplerFilter<tiered::Real>>(up, down);
    impl_->channels.reserve(channels);
    for (std::size_t channel = 0; channel < channels; ++channel) {
        impl_->channels.emplace_back(impl_->filter);
    }
}

StreamResampler::StreamResampler(StreamResampler&&) noexcept = default;
StreamResampler& StreamResampler::operator=(StreamResampler&&) noexcept = default;
StreamResampler::~StreamResampler() = default;

void StreamResampler::reset() {
    for (auto& channel : impl_->channels) {
        channel.reset();
    }
}

void StreamResampler::process(std::span<const std::span<const float>> in, std::size_t frames,
                              std::vector<std::vector<float>>& out) {
    out.resize(channels_);
    for (std::size_t channel = 0; channel < channels_; ++channel) {
        impl_->input.assign(frames, tiered::Real{});
        if (channel < in.size()) {
            const std::size_t have = std::min(frames, in[channel].size());
            std::copy_n(in[channel].begin(), have, impl_->input.begin());
        }
        impl_->output.clear();
        impl_->channels[channel].process(impl_->input, impl_->output);
        for (const tiered::Real sample : impl_->output) {
            out[channel].push_back(static_cast<float>(sample));
        }
    }
}

std::size_t StreamResampler::outputs_for(std::size_t frames) const {
    return impl_->channels.empty() ? 0 : impl_->channels.front().outputs_for(frames);
}

double StreamResampler::delay() const { return impl_->filter->delay(); }

}  // namespace iclforge::dsp
