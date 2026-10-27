#pragma once

// What the Meters view takes from the player's audio path, and nothing else (planning/p4x-meters-view.md): a mono mix of the rendered outputs
// in a ring of 16-bit samples that a panel task reads every 50 ms, and each output's peak over 50 ms. It is a BlockTap the player calls once a
// block (256 frames) while the screen wants it, and is otherwise never called.
//
// The mix is the sum of the live outputs, shifted down by half of log2 of the number of outputs, rounded up (2 outputs: halved, 3 to 16: a
// quarter), so that a room does not clip it; a full-scale sine on every output is a full-scale mix and no more. It is a float sum of the block
// the meter reads, converted once a sample to an int16: the one conversion of the whole path is the mix's, not each output's.
//
// One writer (the player's decode task) and one reader (the panel's); the ring is lock-free and the reader says when it was too slow
// (a copy of the newest samples that the writer overran is dropped, not returned torn). The ring is 32 KB: allocate the whole MeterTap once,
// in PSRAM where there is some, not on a stack.
//
// The cost is counted where it is spent: a clock is read around each block (when one is given), and the time, the blocks, the frames, the
// worst block, the windows closed and the torn reads are kept for GET /panel-stats and the developer view.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

#include "iclforge/block_tap.hpp"

namespace iclforge::meters {

inline constexpr std::size_t kTapMaxOutputs = 16;
// The newest samples a spectrum frame reads (iclforge::dsp::spectrum::kHistory).
inline constexpr std::size_t kTapHistory = 8192;
// A peak window: 50 ms at 48 kHz.
inline constexpr std::size_t kFastWindowFrames = 2400;
// The most frames the player hands over at once.
inline constexpr std::size_t kTapBlockFrames = 256;
inline constexpr float kTapSilentDb = -120.0F;

// The smallest s with 4^s >= outputs: the mix is divided by 2^s (1 output: not at all; 2: by 2; 3 to 16: by 4; 17 to 64: by 8).
[[nodiscard]] constexpr unsigned mix_shift(std::size_t outputs) {
    unsigned s = 0;
    std::size_t reach = 1;
    while (reach < outputs) {
        reach *= 4;
        ++s;
    }
    return s;
}

// One writer, one reader, 16-bit mono.
class MonoRing {
   public:
    static constexpr std::size_t kCapacity = 16384;  // a power of two

    // The writer: a block of at most kTapBlockFrames samples.
    void write(std::span<const std::int16_t> block) noexcept {
        std::uint64_t head = head_.load(std::memory_order_relaxed);
        for (const std::int16_t v : block) {
            data_[static_cast<std::size_t>(head) & (kCapacity - 1)].store(v, std::memory_order_relaxed);
            ++head;
        }
        head_.store(head, std::memory_order_release);
    }

    // True when the reader's copy, taken from `start` (the absolute index of its first sample), may have been overwritten by the time the writer
    // had reached `head_after`: the writer overwrites a sample when it writes the one `kCapacity` after it, and may be a block ahead of the head it has
    // published.
    [[nodiscard]] static constexpr bool torn(std::uint64_t start, std::uint64_t head_after) {
        return head_after + kTapBlockFrames > start + kCapacity;
    }

    // The reader: the newest out.size() samples, oldest first. False before that many have been written, or when the copy was overrun.
    [[nodiscard]] bool read_newest(std::span<std::int16_t> out) const noexcept {
        const std::uint64_t head = head_.load(std::memory_order_acquire);
        if (head < out.size()) {
            return false;
        }
        const std::uint64_t start = head - out.size();
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = data_[static_cast<std::size_t>(start + i) & (kCapacity - 1)].load(std::memory_order_relaxed);
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        return !torn(start, head_.load(std::memory_order_relaxed));
    }

    [[nodiscard]] std::uint64_t written() const noexcept { return head_.load(std::memory_order_acquire); }

   private:
    std::atomic<std::uint64_t> head_{0};
    std::array<std::atomic<std::int16_t>, kCapacity> data_{};
};

// Each output's peak, over the last window that closed.
struct FastLevels {
    std::uint32_t serial = 0;  // windows closed so far: a new window moves it by one
    std::size_t outputs = 0;
    std::array<float, kTapMaxOutputs> peak_db{};
};

// What the tap has cost and done.
struct TapStats {
    std::uint64_t blocks = 0;
    std::uint64_t frames = 0;
    std::uint64_t us_total = 0;  // the time spent in block(), when a clock was given
    std::uint32_t us_max_block = 0;
    std::uint32_t windows = 0;  // peak windows closed
    std::uint32_t reads = 0;    // history reads asked for
    std::uint32_t torn = 0;     // of them, dropped because the writer overran the copy
    std::uint32_t short_reads = 0;  // of them, before enough was written
    // Where the time of a block goes, from every sixteenth block (which is timed in parts, at the price of three more reads of the clock): the pass
    // over the outputs (the mix and the peaks), the scale to 16 bits and the ring's write, and what a read of the clock itself cost. The kit's
    // first run measured 370 to 860 us a burst for a tap that adds and compares 1,536 samples an output, and these say whether it is the
    // loops, the memory they read, or the clock.
    std::uint32_t sampled_blocks = 0;
    std::uint32_t us_pass = 0;
    std::uint32_t us_pack = 0;
    std::uint32_t us_clock = 0;

    // Microseconds a 1,536-frame burst cost the audio path, on average.
    [[nodiscard]] double us_per_burst() const { return frames == 0 ? 0.0 : static_cast<double>(us_total) * 1536.0 / static_cast<double>(frames); }
    // The same for a part: the sampled blocks' time a block, times the six blocks of a burst.
    [[nodiscard]] double part_per_burst(std::uint32_t part_us) const {
        return sampled_blocks == 0 ? 0.0 : static_cast<double>(part_us) * 6.0 / static_cast<double>(sampled_blocks);
    }
    // What one read of the clock cost, in microseconds.
    [[nodiscard]] double us_per_clock_read() const { return sampled_blocks == 0 ? 0.0 : static_cast<double>(us_clock) / static_cast<double>(sampled_blocks); }
};

class MeterTap final : public iclforge::BlockTap {
   public:
    using Clock = std::uint64_t (*)() noexcept;

    // `now_us`: a microsecond clock to time the blocks with, or none.
    explicit MeterTap(Clock now_us = nullptr) noexcept : clock_(now_us) {}

    // The screen asks for blocks while it shows the Meters view and a stream plays.
    void set_wanted(bool wanted) noexcept { wanted_.store(wanted, std::memory_order_release); }
    [[nodiscard]] bool wanted() const noexcept { return wanted_.load(std::memory_order_acquire); }

    [[nodiscard]] bool wants_blocks() const noexcept override { return wanted_.load(std::memory_order_acquire); }

    void block(std::span<const std::span<float>> outputs, std::size_t frames, std::uint32_t live) noexcept override {
        const std::uint64_t t0 = clock_ != nullptr ? clock_() : 0;
        // Every sixteenth block is timed in parts.
        const bool sampled = clock_ != nullptr && (seen_++ & 15U) == 15U;
        const std::uint64_t t_clock = sampled ? clock_() : 0;
        frames = std::min(frames, kTapBlockFrames);
        std::array<float, kTapBlockFrames> mix{};
        const std::size_t count = std::min(outputs.size(), kTapMaxOutputs);
        for (std::size_t o = 0; o < count; ++o) {
            if (((live >> o) & 1U) == 0) {
                continue;
            }
            const std::span<float> samples = outputs[o];
            float peak = peak_[o];
            for (std::size_t k = 0; k < frames; ++k) {
                const float x = samples[k];
                mix[k] += x;
                peak = std::max(peak, std::fabs(x));
            }
            peak_[o] = peak;
        }
        const std::uint64_t t_pass = sampled ? clock_() : 0;
        // The sum, scaled to the range of 16 bits and rounded.
        const float scale = 32767.0F / static_cast<float>(1U << mix_shift(count));
        std::array<std::int16_t, kTapBlockFrames> block16{};
        for (std::size_t k = 0; k < frames; ++k) {
            const float v = std::clamp(mix[k] * scale, -32768.0F, 32767.0F);
            block16[k] = static_cast<std::int16_t>(v >= 0.0F ? v + 0.5F : v - 0.5F);
        }
        ring_.write(std::span<const std::int16_t>(block16.data(), frames));
        if (sampled) {
            const std::uint64_t t_pack = clock_();
            // One writer: a store of the sum is all a counter needs.
            const auto add = [](std::atomic<std::uint32_t>& counter, std::uint64_t us) {
                counter.store(counter.load(std::memory_order_relaxed) + static_cast<std::uint32_t>(us), std::memory_order_relaxed);
            };
            add(us_pass_, t_pass - t_clock);
            add(us_pack_, t_pack - t_pass);
            add(us_clock_, t_clock - t0);
            add(sampled_blocks_, 1);
        }
        window_frames_ += frames;
        if (window_frames_ >= kFastWindowFrames) {
            close_window(count);
        }
        if (clock_ != nullptr) {
            const std::uint64_t spent = clock_() - t0;
            us_total_.fetch_add(spent, std::memory_order_relaxed);
            if (spent > us_max_block_.load(std::memory_order_relaxed)) {
                us_max_block_.store(static_cast<std::uint32_t>(spent), std::memory_order_relaxed);
            }
        }
        blocks_.fetch_add(1, std::memory_order_relaxed);
        frames_.fetch_add(frames, std::memory_order_relaxed);
    }

    // The newest kTapHistory samples of the mix, oldest first: false before enough have been written, or when the writer overran the copy.
    [[nodiscard]] bool read_history(std::span<std::int16_t, kTapHistory> out) const noexcept {
        reads_.fetch_add(1, std::memory_order_relaxed);
        if (ring_.written() < kTapHistory) {
            short_reads_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (!ring_.read_newest(out)) {
            torn_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    [[nodiscard]] FastLevels fast_levels() const noexcept {
        FastLevels out;
        out.serial = serial_.load(std::memory_order_acquire);
        out.outputs = published_outputs_.load(std::memory_order_relaxed);
        for (std::size_t o = 0; o < out.outputs; ++o) {
            out.peak_db[o] = published_[o].load(std::memory_order_relaxed);
        }
        return out;
    }

    // Forgets the longest block so far, so that a view that has just opened reads its own. A block being timed as this runs may put its time
    // back: the figure is a measurement, not a count.
    void reset_max() noexcept { us_max_block_.store(0, std::memory_order_relaxed); }

    [[nodiscard]] TapStats stats() const noexcept {
        TapStats s;
        s.blocks = blocks_.load(std::memory_order_relaxed);
        s.frames = frames_.load(std::memory_order_relaxed);
        s.us_total = us_total_.load(std::memory_order_relaxed);
        s.us_max_block = us_max_block_.load(std::memory_order_relaxed);
        s.windows = serial_.load(std::memory_order_relaxed);
        s.reads = reads_.load(std::memory_order_relaxed);
        s.torn = torn_.load(std::memory_order_relaxed);
        s.short_reads = short_reads_.load(std::memory_order_relaxed);
        s.sampled_blocks = sampled_blocks_.load(std::memory_order_relaxed);
        s.us_pass = us_pass_.load(std::memory_order_relaxed);
        s.us_pack = us_pack_.load(std::memory_order_relaxed);
        s.us_clock = us_clock_.load(std::memory_order_relaxed);
        return s;
    }

   private:
    void close_window(std::size_t outputs) noexcept {
        for (std::size_t o = 0; o < outputs; ++o) {
            published_[o].store(peak_[o] <= 1e-6F ? kTapSilentDb : 20.0F * std::log10(peak_[o]), std::memory_order_relaxed);
            peak_[o] = 0.0F;
        }
        published_outputs_.store(outputs, std::memory_order_relaxed);
        serial_.fetch_add(1, std::memory_order_release);
        window_frames_ = 0;
    }

    Clock clock_ = nullptr;
    std::atomic<bool> wanted_{false};
    MonoRing ring_;
    // The writer's own state.
    std::array<float, kTapMaxOutputs> peak_{};
    std::size_t window_frames_ = 0;
    std::uint32_t seen_ = 0;
    // What the reader reads.
    std::array<std::atomic<float>, kTapMaxOutputs> published_{};
    std::atomic<std::size_t> published_outputs_{0};
    std::atomic<std::uint32_t> serial_{0};
    // The counters.
    std::atomic<std::uint64_t> blocks_{0};
    std::atomic<std::uint64_t> frames_{0};
    std::atomic<std::uint64_t> us_total_{0};
    std::atomic<std::uint32_t> us_max_block_{0};
    std::atomic<std::uint32_t> sampled_blocks_{0};
    std::atomic<std::uint32_t> us_pass_{0};
    std::atomic<std::uint32_t> us_pack_{0};
    std::atomic<std::uint32_t> us_clock_{0};
    mutable std::atomic<std::uint32_t> reads_{0};
    mutable std::atomic<std::uint32_t> torn_{0};
    mutable std::atomic<std::uint32_t> short_reads_{0};
};

}  // namespace iclforge::meters
