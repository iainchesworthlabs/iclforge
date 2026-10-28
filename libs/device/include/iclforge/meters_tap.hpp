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
//
// The cost is also what shapes the code, because it is spent on the decode task's core, inside its burst: the kit measured 570 us a 1,536-frame burst
// in a 5.1 room and 700 us in a 7.1.4 one, against bursts of 4 and 21 ms, for a tap that adds and compares a few thousand floats. The ESP32-P4's cores
// run one instruction a cycle and the first version took 14 to 25 instructions a sample, in a loop an output at a time that stored and loaded
// the mix between them, a staging array the ring was then copied from, and a call into the C library's lock (a critical section on both cores, for
// every 64-bit atomic, which a 32-bit core does not have) six times a block. So: the outputs are taken four at a time with the mix and the four
// peaks held in registers; the conversion writes the ring in place; the head is 32 bits, and the counters the writer adds to are 32-bit AMOs
// that the reader drains into 64-bit totals; and the peaks are published as they are, the logarithm left to the reader. The numbers it produces are
// the same, bit for bit (test_meters_tap.cpp holds them against the first version's loops).

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
//
// The head is the count of samples written, modulo 2^32: 24.8 hours of 48 kHz audio. Every comparison of two heads is a difference taken modulo 2^32, so
// the wrap changes nothing but one thing: for the 170 ms after it, fewer than a spectrum frame's samples have been "written", and the read says so
// (a short read, one frame the view does not get). A 64-bit head would be one critical section on both cores for the writer's every load and store.
class MonoRing {
   public:
    static constexpr std::size_t kCapacity = 16384;  // a power of two
    static_assert((kCapacity & (kCapacity - 1)) == 0);

    // `first_head`: where the count of samples starts; only a test has a reason to say (the wrap, 24.8 hours in).
    explicit MonoRing(std::uint32_t first_head = 0) noexcept : head_(first_head) {}

    // The writer: a block of at most kTapBlockFrames samples.
    void write(std::span<const std::int16_t> block) noexcept {
        write_runs(block.size(), [&](std::atomic<std::int16_t>* slot, std::size_t from, std::size_t count) noexcept {
            for (std::size_t i = 0; i < count; ++i) {
                slot[i].store(block[from + i], std::memory_order_relaxed);
            }
        });
    }

    // The writer, in place: `fill(slot, from, count)` is called for each of at most two runs of the ring's slots (the second only when the block crosses its
    // end) and stores the block's samples from..from+count into slot[0..count); the head moves once the samples are in.
    template <typename Fill>
    void write_runs(std::size_t count, Fill&& fill) noexcept {
        const std::uint32_t head = head_.load(std::memory_order_relaxed);
        const std::size_t first = static_cast<std::size_t>(head) & (kCapacity - 1);
        const std::size_t run = std::min(count, kCapacity - first);
        fill(&data_[first], std::size_t{0}, run);
        if (run < count) {
            fill(&data_[0], run, count - run);
        }
        head_.store(head + static_cast<std::uint32_t>(count), std::memory_order_release);
    }

    // True when the reader's copy, taken from `start` (the index of its first sample), may have been overwritten by the time the writer
    // had reached `head_after`: the writer overwrites a sample when it writes the one `kCapacity` after it, and may be a block ahead of the head it has
    // published. Both are indices modulo 2^32.
    [[nodiscard]] static constexpr bool torn(std::uint64_t start, std::uint64_t head_after) {
        return static_cast<std::uint32_t>(head_after - start) + kTapBlockFrames > kCapacity;
    }

    // The reader: the newest out.size() samples, oldest first. False before that many have been written, or when the copy was overrun.
    [[nodiscard]] bool read_newest(std::span<std::int16_t> out) const noexcept {
        const std::uint32_t head = head_.load(std::memory_order_acquire);
        if (head < out.size()) {
            return false;
        }
        const std::uint32_t start = head - static_cast<std::uint32_t>(out.size());
        // Two runs, as the writer's: from the slot of `start` to the ring's end, then from its beginning.
        const std::size_t first = static_cast<std::size_t>(start) & (kCapacity - 1);
        const std::size_t run = std::min(out.size(), kCapacity - first);
        for (std::size_t i = 0; i < run; ++i) {
            out[i] = data_[first + i].load(std::memory_order_relaxed);
        }
        for (std::size_t i = run; i < out.size(); ++i) {
            out[i] = data_[i - run].load(std::memory_order_relaxed);
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        return !torn(start, head_.load(std::memory_order_relaxed));
    }

    // The samples written, modulo 2^32.
    [[nodiscard]] std::uint64_t written() const noexcept { return head_.load(std::memory_order_acquire); }

   private:
    static_assert(sizeof(std::atomic<std::int16_t>) == sizeof(std::int16_t), "the ring's slots are stored in runs");
    std::atomic<std::uint32_t> head_{0};
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
        const std::size_t count = std::min(outputs.size(), kTapMaxOutputs);
        // The live outputs, in their order, and where each one's peak is kept.
        std::array<const float*, kTapMaxOutputs> in;
        std::array<float*, kTapMaxOutputs> peak_of;
        std::size_t live_count = 0;
        for (std::size_t o = 0; o < count; ++o) {
            if (((live >> o) & 1U) != 0) {
                in[live_count] = outputs[o].data();
                peak_of[live_count] = &peak_[o];
                ++live_count;
            }
        }
        // The first group writes the mix and the others add to it, so it needs no clearing; with no live output it is silence.
        std::array<float, kTapBlockFrames> mix;
        if (live_count == 0) {
            std::fill_n(mix.begin(), frames, 0.0F);
        }
        std::size_t done = 0;
        while (done < live_count) {
            const std::size_t taken = done == 0 ? add_group<true>(in.data(), peak_of.data(), live_count, mix.data(), frames)
                                                : add_group<false>(in.data() + done, peak_of.data() + done, live_count - done, mix.data(), frames);
            done += taken;
        }
        const std::uint64_t t_pass = sampled ? clock_() : 0;
        // The sum, scaled to the range of 16 bits and rounded half away from zero, into the ring where it will be read.
        const float scale = 32767.0F / static_cast<float>(1U << mix_shift(count));
        ring_.write_runs(frames, [&](std::atomic<std::int16_t>* slot, std::size_t from, std::size_t run) noexcept {
            // Four samples at a time, all four converted before any is stored: one sample is a chain of eight float operations, each waiting for the last
            // on a core that issues in order (the kit measured 39 cycles a sample for the one-at-a-time loop), and four chains side by side fill the waits.
            std::size_t i = 0;
            for (; i + 4 <= run; i += 4) {
                const float* const m = mix.data() + from + i;
                const std::int16_t a = to_s16(m[0], scale);
                const std::int16_t b = to_s16(m[1], scale);
                const std::int16_t c = to_s16(m[2], scale);
                const std::int16_t d = to_s16(m[3], scale);
                slot[i].store(a, std::memory_order_relaxed);
                slot[i + 1].store(b, std::memory_order_relaxed);
                slot[i + 2].store(c, std::memory_order_relaxed);
                slot[i + 3].store(d, std::memory_order_relaxed);
            }
            for (; i < run; ++i) {
                slot[i].store(to_s16(mix[from + i], scale), std::memory_order_relaxed);
            }
        });
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
            us_total_.fetch_add(static_cast<std::uint32_t>(spent), std::memory_order_relaxed);
            if (spent > us_max_block_.load(std::memory_order_relaxed)) {
                us_max_block_.store(static_cast<std::uint32_t>(spent), std::memory_order_relaxed);
            }
        }
        blocks_.fetch_add(1, std::memory_order_relaxed);
        frames_.fetch_add(static_cast<std::uint32_t>(frames), std::memory_order_relaxed);
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
            // The window's peak as the writer left it; the logarithm is the reader's.
            const float peak = published_[o].load(std::memory_order_relaxed);
            out.peak_db[o] = peak <= 1e-6F ? kTapSilentDb : 20.0F * std::log10(peak);
        }
        return out;
    }

    // Forgets the longest block so far, so that a view that has just opened reads its own. A block being timed as this runs may put its time
    // back: the figure is a measurement, not a count.
    void reset_max() noexcept { us_max_block_.store(0, std::memory_order_relaxed); }

    [[nodiscard]] TapStats stats() const noexcept {
        TapStats s;
        s.blocks = drain(blocks_, blocks_total_);
        s.frames = drain(frames_, frames_total_);
        s.us_total = drain(us_total_, us_total_total_);
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
    // One sample of the mix as it goes to the ring: scaled, clamped to 16 bits and rounded half away from zero.
    [[nodiscard]] static std::int16_t to_s16(float sum, float scale) noexcept {
        const float v = std::fmin(std::fmax(sum * scale, -32768.0F), 32767.0F);
        return static_cast<std::int16_t>(v + std::copysign(0.5F, v));
    }

    // Adds the next outputs of `in` (at most four of the `left`) to the mix, in their order, so that the sum is the one a loop over the outputs makes, and
    // takes each one's peak; the first group sets the mix and the others add to it. Returns how many outputs it took. The four outputs of a group, and
    // their four peaks, stay in registers for the whole block.
    template <bool kFirst>
    static std::size_t add_group(const float* const* in, float* const* peak, std::size_t left, float* mix, std::size_t frames) noexcept {
        switch (std::min<std::size_t>(left, 4)) {
            case 1:
                group<1, kFirst>(in, peak, mix, frames);
                return 1;
            case 2:
                group<2, kFirst>(in, peak, mix, frames);
                return 2;
            case 3:
                group<3, kFirst>(in, peak, mix, frames);
                return 3;
            default:
                group<4, kFirst>(in, peak, mix, frames);
                return 4;
        }
    }

    template <std::size_t N, bool kFirst>
    static void group(const float* const* in, float* const* peak, float* mix, std::size_t frames) noexcept {
        [[maybe_unused]] const float* const a = in[0];
        [[maybe_unused]] const float* const b = N > 1 ? in[1] : a;
        [[maybe_unused]] const float* const c = N > 2 ? in[2] : a;
        [[maybe_unused]] const float* const d = N > 3 ? in[3] : a;
        float pa = *peak[0];
        [[maybe_unused]] float pb = N > 1 ? *peak[1] : 0.0F;
        [[maybe_unused]] float pc = N > 2 ? *peak[2] : 0.0F;
        [[maybe_unused]] float pd = N > 3 ? *peak[3] : 0.0F;
        for (std::size_t k = 0; k < frames; ++k) {
            const float xa = a[k];
            float sum = kFirst ? xa : mix[k] + xa;
            pa = std::fmax(pa, std::fabs(xa));
            if constexpr (N > 1) {
                const float xb = b[k];
                sum += xb;
                pb = std::fmax(pb, std::fabs(xb));
            }
            if constexpr (N > 2) {
                const float xc = c[k];
                sum += xc;
                pc = std::fmax(pc, std::fabs(xc));
            }
            if constexpr (N > 3) {
                const float xd = d[k];
                sum += xd;
                pd = std::fmax(pd, std::fabs(xd));
            }
            mix[k] = sum;
        }
        *peak[0] = pa;
        if constexpr (N > 1) {
            *peak[1] = pb;
        }
        if constexpr (N > 2) {
            *peak[2] = pc;
        }
        if constexpr (N > 3) {
            *peak[3] = pd;
        }
    }

    void close_window(std::size_t outputs) noexcept {
        for (std::size_t o = 0; o < outputs; ++o) {
            published_[o].store(peak_[o], std::memory_order_relaxed);
            peak_[o] = 0.0F;
        }
        published_outputs_.store(outputs, std::memory_order_relaxed);
        serial_.fetch_add(1, std::memory_order_release);
        window_frames_ = 0;
    }

    // What the writer added since the last look, taken (an atomic exchange against the writer's atomic add, so that none is lost or counted twice) and
    // put to the reader's total.
    static std::uint64_t drain(std::atomic<std::uint32_t>& pending, std::atomic<std::uint64_t>& total) noexcept {
        const std::uint32_t added = pending.exchange(0, std::memory_order_relaxed);
        return total.fetch_add(added, std::memory_order_relaxed) + added;
    }

    Clock clock_ = nullptr;
    std::atomic<bool> wanted_{false};
    MonoRing ring_;
    // The writer's own state.
    std::array<float, kTapMaxOutputs> peak_{};
    std::size_t window_frames_ = 0;
    std::uint32_t seen_ = 0;
    // What the reader reads: each output's peak over the window that closed, as a magnitude.
    std::array<std::atomic<float>, kTapMaxOutputs> published_{};
    std::atomic<std::size_t> published_outputs_{0};
    std::atomic<std::uint32_t> serial_{0};
    // The counters: 32 bits that the writer adds to, and the totals the reader keeps (a 64-bit atomic is a lock on a 32-bit core, and not the writer's to take).
    mutable std::atomic<std::uint32_t> blocks_{0};
    mutable std::atomic<std::uint32_t> frames_{0};
    mutable std::atomic<std::uint32_t> us_total_{0};
    mutable std::atomic<std::uint64_t> blocks_total_{0};
    mutable std::atomic<std::uint64_t> frames_total_{0};
    mutable std::atomic<std::uint64_t> us_total_total_{0};
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
