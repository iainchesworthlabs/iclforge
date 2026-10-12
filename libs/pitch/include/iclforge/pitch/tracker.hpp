#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// A fundamental-frequency tracker for a singing voice (planning/karaoke-microphone.md, D7; findings
// F5). It answers, every hop, the pitch of the last frame of audio pushed, or that the frame is not
// voiced. The algorithm is the McLeod Pitch Method (McLeod and Wyvill, ICMC 2005): the normalised
// square difference function of a frame, the first of its key maxima that comes within a fraction
// of the highest, and parabolic interpolation of that peak. It is written from the paper and
// shares no code with any other implementation.
//
// Standard library only, no clock and no allocation after construction except in `push()`'s output
// vector, so that Hearth, the browser build and the sink firmware can all link it.

namespace iclforge::pitch {

// A semitone is 100 cents; MIDI note 69 is 440 Hz. Fractional note numbers are what a pitch is.
[[nodiscard]] inline double midi_from_hz(double hz) {
    return 69.0 + 12.0 * std::log2(hz / 440.0);
}

[[nodiscard]] inline double hz_from_midi(double midi) {
    return 440.0 * std::exp2((midi - 69.0) / 12.0);
}

// The interval from `reference_hz` to `hz` in cents, positive when `hz` is sharp.
[[nodiscard]] inline double cents_between(double reference_hz, double hz) {
    return 1200.0 * std::log2(hz / reference_hz);
}

// Folds an interval in cents into (-600, +600]: the octave a singer chose does not matter.
[[nodiscard]] inline double fold_octaves(double cents) {
    double folded = std::fmod(cents, 1200.0);
    if (folded > 600.0) {
        folded -= 1200.0;
    } else if (folded <= -600.0) {
        folded += 1200.0;
    }
    return folded;
}

// What the tracker answers for one frame.
struct Frame {
    // The index, in the samples pushed (at the input rate), of the frame's centre sample. The
    // decimator's delay is already taken out, so this is where in the input the pitch is.
    std::int64_t centre_sample = 0;
    // The fundamental in Hz; 0 when the frame is not voiced.
    float hz = 0.0F;
    // The normalised autocorrelation at the chosen lag, 0 to 1: how periodic the frame is. 0 when
    // the frame is not voiced.
    float clarity = 0.0F;
    // The frame's RMS level in dBFS, floored at -120.
    float level_db = -120.0F;

    [[nodiscard]] bool voiced() const { return hz > 0.0F; }
};

struct TrackerConfig {
    // The rate of the samples pushed, and the rate the analysis runs at. The input rate must be a
    // whole multiple of the analysis rate: the samples are low-passed and decimated by the ratio.
    std::uint32_t input_rate = 16000;
    std::uint32_t analysis_rate = 16000;
    // The analysis window and the distance between windows, in milliseconds. 64 ms holds two
    // periods of the lowest note at `min_hz` with room for the lag search.
    std::uint32_t frame_ms = 64;
    std::uint32_t hop_ms = 10;
    // The range of fundamentals reported; a pitch outside it is "not voiced".
    float min_hz = 70.0F;
    float max_hz = 1100.0F;
    // A frame whose clarity is below this is not voiced.
    float clarity_min = 0.6F;
    // A frame quieter than this (RMS, dBFS) is not voiced, whatever its shape.
    float level_floor_db = -55.0F;
    // The first key maximum within this fraction of the highest is the pitch (McLeod's k).
    float peak_ratio = 0.9F;

    // The configuration for samples arriving at `input_rate`: the lowest analysis rate up to 16 kHz
    // that divides it, and the defaults above otherwise. 48 kHz gives 16 kHz, 44.1 kHz gives
    // 14.7 kHz, and 8 kHz stays 8 kHz.
    [[nodiscard]] static TrackerConfig for_input_rate(std::uint32_t input_rate);
};

class Tracker {
   public:
    // Nothing when the configuration cannot work: an input rate that is not a whole multiple of
    // the analysis rate, a range that is empty or leaves no room in the window, or a zero.
    [[nodiscard]] static std::optional<Tracker> make(const TrackerConfig& config);

    // Pushes samples at the input rate and appends a Frame to `out` for every hop completed. The
    // first frame appears once a whole window has been seen. Any chunk size gives the same frames.
    void push(std::span<const float> samples, std::vector<Frame>& out);

    // Forgets every sample: the next push starts a new stream at sample 0.
    void reset();

    [[nodiscard]] const TrackerConfig& config() const { return config_; }
    // How many input samples make one analysis sample.
    [[nodiscard]] std::uint32_t decimation() const { return factor_; }
    // The window and the hop, in analysis samples.
    [[nodiscard]] std::uint32_t frame_samples() const { return frame_; }
    [[nodiscard]] std::uint32_t hop_samples() const { return hop_; }

   private:
    Tracker(const TrackerConfig& config, std::uint32_t factor, std::uint32_t frame,
            std::uint32_t hop, std::uint32_t tau_min, std::uint32_t tau_max,
            std::vector<float> taps);

    void push_analysis(float sample, std::vector<Frame>& out);
    [[nodiscard]] Frame analyse(std::int64_t centre_sample);

    TrackerConfig config_;
    std::uint32_t factor_;
    std::uint32_t frame_;
    std::uint32_t hop_;
    std::uint32_t tau_min_;
    std::uint32_t tau_max_;

    // The low-pass FIR (odd length, linear phase) and the input history it reads.
    std::vector<float> taps_;
    std::vector<float> history_;
    std::size_t history_at_ = 0;
    std::uint32_t phase_ = 0;
    std::int64_t input_seen_ = 0;

    // The analysis samples, a ring, and the scratch the window is copied to.
    std::vector<float> ring_;
    std::size_t ring_at_ = 0;
    std::int64_t analysis_seen_ = 0;
    std::vector<float> window_;
    std::vector<double> nsdf_;
    std::vector<double> energy_;
};

}  // namespace iclforge::pitch
