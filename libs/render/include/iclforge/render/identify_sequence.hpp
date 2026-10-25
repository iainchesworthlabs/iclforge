#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

#include "iclforge/render/identify.hpp"

// The identify tone as a person starts it on a board: a short burst of pink noise out of one speaker, or out of
// each in turn, so that whoever sets up a room hears which speaker an output reaches (identify.hpp is the noise,
// and the server's own, which a server starts and stops itself).
//
// It comes out of real speakers, in a room somebody may be sitting in, so the limits are in the code and no
// caller can lift them:
//
//   level   at most kMaxLevelDb (-24 dB of full scale, RMS; pink noise peaks about 12 dB over its RMS), whatever
//           a caller asks for, and kDefaultLevelDb (-30) when it asks for nothing in particular;
//   burst   kToneMs for each speaker, with a kFadeMs smoothstep fade in and out, so no speaker is clicked;
//   total   kMaxTotalMs from start() to the end, however the sequence is arranged. A sequence that loops ends
//           there, and one that is started again begins its count again - only a caller that asks, for it.
//
// A sequence is a list of up to kMaxTargets outputs, each with the band its noise is made in (the low band for an
// LFE feed or a subwoofer, identify.hpp), and one of three shapes: once (the first target alone), each (every
// target in turn, kGapMs apart) or loop (the first target again and again, kGapMs apart, until the total limit).
// It counts samples and not time, so the same blocks give the same sound on a board and on the host.
//
// fill() writes one block for all of the outputs: the tone on the speaker whose turn it is, silence on every
// other. Standard library only, no allocation, tested on the host (libs/render/tests/test_identify_sequence.cpp).

namespace iclforge::render {

class IdentifySequence {
   public:
    static constexpr double kMinLevelDb = -60.0;
    static constexpr double kDefaultLevelDb = -30.0;
    static constexpr double kMaxLevelDb = -24.0;
    static constexpr std::uint32_t kToneMs = 1500;
    static constexpr std::uint32_t kGapMs = 500;
    static constexpr std::uint32_t kFadeMs = 20;
    static constexpr std::uint32_t kMaxTotalMs = 60'000;
    static constexpr std::size_t kMaxTargets = 16;

    enum class Mode : std::uint8_t {
        kOnce,
        kEach,
        kLoop,
    };

    struct Target {
        std::uint8_t output = 0;
        IdentifyTone::Band band = IdentifyTone::Band::kFull;
    };

    explicit IdentifySequence(std::uint32_t sample_rate_hz = 48000)
        : rate_(std::max<std::uint32_t>(sample_rate_hz, 8000)), tone_(sample_rate_hz) {
        tone_samples_ = samples_for(kToneMs);
        phase_samples_ = tone_samples_ + samples_for(kGapMs);
        fade_samples_ = samples_for(kFadeMs);
        cap_samples_ = samples_for(kMaxTotalMs);
    }

    // The level a request comes to: cut to [kMinLevelDb, kMaxLevelDb], NaN to the default.
    [[nodiscard]] static double clamp_level_db(double db) {
        if (!(db == db)) {
            return kDefaultLevelDb;
        }
        return std::clamp(db, kMinLevelDb, kMaxLevelDb);
    }

    // Begins a sequence over `targets` (the first kMaxTargets of them), replacing whatever plays. False, with
    // nothing playing, for no targets. Mode::kOnce and Mode::kLoop use the first target alone.
    bool start(Mode mode, std::span<const Target> targets, double level_db = kDefaultLevelDb) {
        active_ = false;
        if (targets.empty()) {
            return false;
        }
        count_ = std::min(targets.size(), kMaxTargets);
        std::copy_n(targets.begin(), count_, targets_.begin());
        mode_ = mode;
        (void)tone_.set_level_db(clamp_level_db(level_db));
        step_ = 0;
        position_ = 0;
        total_ = 0;
        fade_out_ = 0;
        begin_tone();
        active_ = true;
        return true;
    }

    // Ends the sequence. A tone that is sounding fades out over kFadeMs of the blocks that follow, unless
    // `immediate`, which is silence from the next block.
    void stop(bool immediate = false) {
        if (!active_) {
            return;
        }
        if (immediate || !sounding()) {
            active_ = false;
            return;
        }
        if (fade_out_ > 0) {
            return;  // already on its way down
        }
        // From where the envelope is now, down.
        fade_out_ = std::min<std::uint64_t>(fade_samples_, tone_samples_ - position_);
        fade_out_total_ = fade_out_;
        fade_out_done_ = 0;
    }

    [[nodiscard]] bool active() const { return active_; }

    // The output the tone is on at the start of the next block, or -1 in a gap and when nothing plays.
    [[nodiscard]] int output() const { return active_ && sounding() ? static_cast<int>(targets_[target_index()].output) : -1; }

    // The sequence's place: which target (0 based) the tone is on or comes to next.
    [[nodiscard]] std::size_t step() const { return active_ ? step_ : 0; }

    // Milliseconds left until the total limit ends the sequence, whatever it still has to play.
    [[nodiscard]] std::uint32_t remaining_ms() const {
        if (!active_ || total_ >= cap_samples_) {
            return 0;
        }
        return static_cast<std::uint32_t>(((cap_samples_ - total_) * 1000U) / rate_);
    }

    // The next outputs[0].size() samples of every output, each span written whole. True while the sequence goes
    // on after them. Called with the sequence over, it writes silence and returns false.
    bool fill(std::span<const std::span<float>> outputs) {
        for (const std::span<float>& out : outputs) {
            std::fill(out.begin(), out.end(), 0.0F);
        }
        if (outputs.empty()) {
            return active_;
        }
        const std::size_t n = outputs.front().size();
        std::size_t k = 0;
        while (active_ && k < n) {
            if (total_ >= cap_samples_) {
                active_ = false;
                break;
            }
            const std::uint64_t room = std::min<std::uint64_t>(n - k, cap_samples_ - total_);
            if (sounding()) {
                const Target& target = targets_[target_index()];
                const auto run = static_cast<std::size_t>(std::min<std::uint64_t>(room, tone_samples_ - position_));
                if (target.output < outputs.size()) {
                    const std::span<float> part = outputs[target.output].subspan(k, run);
                    tone_.generate(part, target.band);
                    envelope(part);
                } else {
                    // An output the block does not have: the tone's time passes, and nothing is heard.
                    skip_noise(run, target.band);
                }
                advance(run);
                k += run;
                if (fade_out_ > 0 && fade_out_done_ >= fade_out_total_) {
                    active_ = false;
                }
                if (active_ && position_ >= tone_samples_) {
                    end_of_tone();
                }
            } else {
                const auto run = static_cast<std::size_t>(std::min<std::uint64_t>(room, phase_samples_ - position_));
                advance(run);
                k += run;
                if (active_ && position_ >= phase_samples_) {
                    ++step_;
                    position_ = 0;
                    begin_tone();
                }
            }
        }
        return active_;
    }

   private:
    [[nodiscard]] std::uint64_t samples_for(std::uint32_t ms) const { return (static_cast<std::uint64_t>(ms) * rate_) / 1000U; }
    [[nodiscard]] bool sounding() const { return position_ < tone_samples_; }
    [[nodiscard]] std::size_t target_index() const { return mode_ == Mode::kEach ? std::min(step_, count_ - 1) : 0; }

    void begin_tone() {
        // The same noise for every burst.
        tone_.reset();
        fade_out_ = 0;
        fade_out_done_ = 0;
    }

    void advance(std::size_t run) {
        position_ += run;
        total_ += run;
        if (fade_out_ > 0) {
            fade_out_done_ += run;
        }
        // The limit ends the sequence in the block that reaches it, even when that is the block's last sample.
        if (total_ >= cap_samples_) {
            active_ = false;
        }
    }

    void end_of_tone() {
        // The last speaker of a sequence ends with its tone, and a single burst with its own.
        const bool last = mode_ == Mode::kOnce || (mode_ == Mode::kEach && step_ + 1 >= count_);
        if (last) {
            active_ = false;
        }
    }

    // Burns the noise generator's samples for an output the block does not carry, so a tone that is cut short by
    // the block keeps the sequence the same.
    void skip_noise(std::size_t run, IdentifyTone::Band band) {
        std::array<float, 64> scratch{};
        for (std::size_t done = 0; done < run;) {
            const std::size_t part = std::min(scratch.size(), run - done);
            tone_.generate(std::span<float>(scratch.data(), part), band);
            done += part;
        }
    }

    // The fades: a smoothstep (3x^2 - 2x^3, flat at both ends) up over the first kFadeMs of the burst and down
    // over the last, or down from a stop() over the kFadeMs that follow it. Single precision and no library call:
    // a board's double arithmetic is software.
    static float smoothstep(float x) { return x * x * (3.0F - (2.0F * x)); }

    void envelope(std::span<float> part) const {
        const auto fade = static_cast<float>(std::max<std::uint64_t>(fade_samples_, 1));
        for (std::size_t i = 0; i < part.size(); ++i) {
            const std::uint64_t at = position_ + i;
            float gain = 1.0F;
            if (at < fade_samples_) {
                gain = smoothstep((static_cast<float>(at) + 0.5F) / fade);
            }
            const std::uint64_t to_end = tone_samples_ - 1 - at;
            if (to_end < fade_samples_) {
                gain = std::min(gain, smoothstep((static_cast<float>(to_end) + 0.5F) / fade));
            }
            if (fade_out_ > 0) {
                const std::uint64_t done = fade_out_done_ + i;
                const std::uint64_t left = done < fade_out_total_ ? fade_out_total_ - done : 0;
                gain = std::min(gain, smoothstep(static_cast<float>(left) / static_cast<float>(fade_out_total_)));
            }
            part[i] *= gain;
        }
    }

    std::uint32_t rate_;
    IdentifyTone tone_;
    std::uint64_t tone_samples_ = 0;
    std::uint64_t phase_samples_ = 0;
    std::uint64_t fade_samples_ = 0;
    std::uint64_t cap_samples_ = 0;

    bool active_ = false;
    Mode mode_ = Mode::kOnce;
    std::array<Target, kMaxTargets> targets_{};
    std::size_t count_ = 0;
    std::size_t step_ = 0;
    // Samples into the current step: its tone, then its gap.
    std::uint64_t position_ = 0;
    std::uint64_t total_ = 0;
    // A stop() in progress: the samples it fades over, and how many have gone by.
    std::uint64_t fade_out_ = 0;
    std::uint64_t fade_out_total_ = 0;
    std::uint64_t fade_out_done_ = 0;
};

}  // namespace iclforge::render
