#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

#include "iclforge/render/routing.hpp"

// Per-output trim and delay: a level in dB and a time offset in samples for
// each output a patch writes (ac3/render/routing.hpp), the speaker-level and
// speaker-distance settings an AVR's speaker setup has.
//
// Speaker management only, as planning/hearth-reference-player.md scopes it:
// no measurement, no equalisation, no filter beyond the renderer's crossover.
//
// The delay lines are the caller's storage, sized with storage_floats() and
// handed to configure(), so that a board can place them in PSRAM and a
// computer in a vector; nothing here allocates. Sixteen outputs of 20 ms at
// 48 kHz is 15,360 floats, about 61 KB. Per sample the work is a swap into a
// ring and a float multiply, and an output at 0 dB with no delay costs
// nothing. Tested on the host in libs/render/tests/test_trim_delay.cpp.
//
// A trim set with set_trim_db() is in force from the next sample; one set with ramp_trim_db() moves to its level
// in a straight line over the next block that reaches the output, so a listener who steps a speaker's level
// while a song plays hears the step and not a click.

namespace iclforge::render {

class TrimDelay {
   public:
    static constexpr std::size_t kMaxOutputs = Routing::kMaxOutputs;
    // What set_trim_db() accepts. A trim is for matching speakers to each
    // other; muting one is the patch's business.
    static constexpr double kMinTrimDb = -24.0;
    static constexpr double kMaxTrimDb = 12.0;

    // Floats of storage for `outputs` outputs, each delayable by up to
    // `max_delay_samples`.
    [[nodiscard]] static constexpr std::size_t storage_floats(std::size_t outputs,
                                                              std::size_t max_delay_samples) {
        return outputs * max_delay_samples;
    }

    // Milliseconds at `sample_rate_hz` as whole samples, to the nearest; 0 for
    // a negative or non-finite duration.
    [[nodiscard]] static std::size_t samples_for_ms(double ms, std::uint32_t sample_rate_hz) {
        if (!(ms > 0.0) || !std::isfinite(ms)) {
            return 0;
        }
        const double samples = ms * static_cast<double>(sample_rate_hz) / 1000.0;
        return static_cast<std::size_t>(std::lround(samples));
    }

    // Nothing configured: no outputs, and process() does nothing.
    TrimDelay() = default;

    // `outputs` outputs at 0 dB with no delay, each able to delay by up to
    // `max_delay_samples` over `storage`, which this zeroes and keeps a view
    // of. False, leaving the object unconfigured, for more than kMaxOutputs
    // outputs or less storage than storage_floats() says.
    bool configure(std::span<float> storage, std::size_t outputs, std::size_t max_delay_samples) {
        *this = TrimDelay{};
        if (outputs > kMaxOutputs || storage.size() < storage_floats(outputs, max_delay_samples)) {
            return false;
        }
        storage_ = storage.first(storage_floats(outputs, max_delay_samples));
        std::fill(storage_.begin(), storage_.end(), 0.0F);
        outputs_ = outputs;
        max_delay_ = max_delay_samples;
        return true;
    }

    [[nodiscard]] std::size_t outputs() const { return outputs_; }
    [[nodiscard]] std::size_t max_delay() const { return max_delay_; }

    // Sets an output's trim. False, changing nothing, for an output out of
    // range or a trim outside [kMinTrimDb, kMaxTrimDb], NaN included.
    bool set_trim_db(std::size_t output, double db) {
        if (output >= outputs_ || !(db >= kMinTrimDb && db <= kMaxTrimDb)) {
            return false;
        }
        trim_db_[output] = db;
        gain_[output] = static_cast<float>(std::pow(10.0, db / 20.0));
        shown_[output] = gain_[output];
        return true;
    }

    // set_trim_db() for a trim that moves to its level over the next block that reaches the output, from the
    // level it had. The same refusals.
    bool ramp_trim_db(std::size_t output, double db) {
        if (output >= outputs_ || !(db >= kMinTrimDb && db <= kMaxTrimDb)) {
            return false;
        }
        trim_db_[output] = db;
        gain_[output] = static_cast<float>(std::pow(10.0, db / 20.0));
        return true;
    }

    // Sets an output's delay, in samples. A change empties that output's
    // line, so what was delayed under the old setting is dropped rather than
    // played out of order: the output is silent for the new delay's length.
    // False, changing nothing, for an output out of range or a delay over
    // max_delay().
    bool set_delay(std::size_t output, std::size_t samples) {
        if (output >= outputs_ || samples > max_delay_) {
            return false;
        }
        if (samples != delay_[output]) {
            delay_[output] = static_cast<std::uint32_t>(samples);
            position_[output] = 0;
            std::fill_n(line(output), samples, 0.0F);
        }
        return true;
    }

    [[nodiscard]] double trim_db(std::size_t output) const {
        return output < outputs_ ? trim_db_[output] : 0.0;
    }
    [[nodiscard]] std::size_t delay(std::size_t output) const {
        return output < outputs_ ? delay_[output] : 0;
    }

    // The linear gain an output's trim comes to (the level it moves to, for one that is ramping).
    [[nodiscard]] float gain(std::size_t output) const { return output < outputs_ ? gain_[output] : 1.0F; }

    // Whether any output has a trim or a delay; process() changes nothing
    // when none has.
    [[nodiscard]] bool active() const {
        for (std::size_t o = 0; o < outputs_; ++o) {
            if (delay_[o] != 0 || gain_[o] != 1.0F || shown_[o] != 1.0F) {
                return true;
            }
        }
        return false;
    }

    // One block of each output, in place: delayed, then trimmed. `out` is one
    // span per output, in output order; spans past outputs() are left alone,
    // and each output takes its own span's length.
    void process(std::span<const std::span<float>> out) {
        const std::size_t count = std::min(out.size(), outputs_);
        for (std::size_t o = 0; o < count; ++o) {
            process_output(out[o], o);
        }
    }

    // process() for the outputs in `only` (bit n for output n) and no others, which are not
    // touched: nothing is delayed, trimmed or advanced for them. An output with a delay keeps a
    // line that moves with the blocks, so a caller leaves such an output out of `only` only when
    // it has none (delay() is 0); a trim alone cannot make a block of zeros anything else.
    void process(std::span<const std::span<float>> out, std::uint32_t only) {
        const std::size_t count = std::min(out.size(), outputs_);
        for (std::size_t o = 0; o < count; ++o) {
            if (((only >> o) & 1U) != 0) {
                process_output(out[o], o);
            }
        }
    }

    // Whether any output has a delay: its line carries audio past the block that gave it.
    [[nodiscard]] bool delayed() const { return delayed_outputs() != 0; }

    // The outputs that have a delay (bit n for output n): the only ones that can carry audio in a block that no
    // channel patched to them reached.
    [[nodiscard]] std::uint32_t delayed_outputs() const {
        std::uint32_t mask = 0;
        for (std::size_t o = 0; o < outputs_; ++o) {
            if (delay_[o] != 0) {
                mask |= std::uint32_t{1} << o;
            }
        }
        return mask;
    }

    // Silence in every delay line, for a new stream; the settings stay, and a ramp under way is over.
    void reset() {
        std::fill(storage_.begin(), storage_.end(), 0.0F);
        position_.fill(0);
        shown_ = gain_;
    }

   private:
    static constexpr std::array<float, kMaxOutputs> unity() {
        std::array<float, kMaxOutputs> out{};
        for (float& g : out) {
            g = 1.0F;
        }
        return out;
    }

    float* line(std::size_t output) { return storage_.data() + (output * max_delay_); }

    // One output's block, in place: delayed, then trimmed.
    void process_output(std::span<float> block, std::size_t o) {
        float* const samples = block.data();
        const std::size_t n = block.size();
        const std::size_t d = delay_[o];
        if (d != 0) {
            // The line holds the last d samples in arrival order from
            // position_; swapping a run in takes the oldest out.
            float* const ring = line(o);
            std::size_t p = position_[o];
            std::size_t k = 0;
            while (k < n) {
                const std::size_t run = std::min(n - k, d - p);
                std::swap_ranges(samples + k, samples + k + run, ring + p);
                k += run;
                p += run;
                if (p == d) {
                    p = 0;
                }
            }
            position_[o] = static_cast<std::uint32_t>(p);
        }
        const float g = gain_[o];
        if (shown_[o] != g) {
            // A straight line from the level it had to the level it has: the last sample of the block is the
            // new level, and the next block starts there.
            const float from = shown_[o];
            const float step = (g - from) / static_cast<float>(n);
            for (std::size_t k = 0; k < n; ++k) {
                samples[k] *= from + (step * static_cast<float>(k + 1));
            }
            shown_[o] = g;
        } else if (g != 1.0F) {
            for (std::size_t k = 0; k < n; ++k) {
                samples[k] *= g;
            }
        }
    }

    std::span<float> storage_;
    std::size_t outputs_ = 0;
    std::size_t max_delay_ = 0;
    std::array<double, kMaxOutputs> trim_db_{};
    std::array<float, kMaxOutputs> gain_ = unity();
    // The gain in force at the end of the last block that reached the output: gain_, unless a ramp is under way.
    std::array<float, kMaxOutputs> shown_ = unity();
    std::array<std::uint32_t, kMaxOutputs> delay_{};
    std::array<std::uint32_t, kMaxOutputs> position_{};
};

}  // namespace iclforge::render
