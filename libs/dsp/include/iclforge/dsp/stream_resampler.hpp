#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "iclforge/dsp/export.hpp"

// Sample-rate conversion of audio that arrives in blocks: iclforge::dsp::resample's converter, kept
// running between calls. A player that sends one group member audio at a rate the stream does not
// have (a 96 kHz item to a sink that plays 48 kHz) converts as it decodes, and cannot hold the
// item to convert it whole.
//
// The filter is resample()'s: the AC-4 sample rate converter's Kaiser-windowed sinc, polyphase,
// designed for the ratio in lowest terms, the passband to 0.86 of the lower rate's Nyquist
// frequency and the stopband 100 dB down. Fed in blocks of any sizes, it gives the same samples
// as resample() does for the whole input, with the input taken to be silence before its first
// sample: after n input frames there are exactly floor(n * output_rate / input_rate) output frames,
// and the output lags the input by delay() input frames. It is not flushed at the end: the last
// delay() frames of the input are the ones whose outputs have not been made, which for a player
// that goes on to the next item is correct, and for one that stops is an end of under a
// millisecond that is not played.
//
// Each channel is converted alone; none influences another.

namespace iclforge::dsp {

class ICLFORGE_DSP_EXPORT StreamResampler {
public:
    // A converter for `channels` channels from `input_rate` to `output_rate`, both in Hz and not 0.
    // Equal rates are an identity.
    StreamResampler(std::uint32_t input_rate, std::uint32_t output_rate, std::size_t channels);
    StreamResampler(StreamResampler&&) noexcept;
    StreamResampler& operator=(StreamResampler&&) noexcept;
    StreamResampler(const StreamResampler&) = delete;
    StreamResampler& operator=(const StreamResampler&) = delete;
    ~StreamResampler();

    [[nodiscard]] std::uint32_t input_rate() const { return input_rate_; }
    [[nodiscard]] std::uint32_t output_rate() const { return output_rate_; }
    [[nodiscard]] std::size_t channels() const { return channels_; }

    // Forgets the input: the next frame is the first of a stream, after silence.
    void reset();

    // Takes the first `frames` frames of each of `in` (a channel the span does not have is
    // silence) and appends to each of `out`, which is resized to channels() vectors, the output
    // frames they complete. The same count for every channel.
    void process(std::span<const std::span<const float>> in, std::size_t frames,
                 std::vector<std::vector<float>>& out);

    // How many output frames the next `frames` input frames complete.
    [[nodiscard]] std::size_t outputs_for(std::size_t frames) const;

    // The converter's delay, in input frames.
    [[nodiscard]] double delay() const;

private:
    struct Impl;
    std::uint32_t input_rate_ = 0;
    std::uint32_t output_rate_ = 0;
    std::size_t channels_ = 0;
    std::unique_ptr<Impl> impl_;
};

}  // namespace iclforge::dsp
