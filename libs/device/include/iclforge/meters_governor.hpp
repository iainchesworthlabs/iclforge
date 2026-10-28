#pragma once

// How often the Meters view updates: twenty times a second if the panel task can keep up, ten if it cannot (planning/p4x-meters-view.md,
// "Measuring it"). Pure: the screen tells it what each update cost and how late it started, and it answers with the step to the next.
//
// "Cannot keep up" is two things, judged over the last second of updates and never on one:
//
//   cost   the average of what an update cost (the analysis, the widgets it changed and the render it asked for) is more than 70 % of
//          a 50 ms step, 35 ms: the task spends the step on the meters and little is left for the touch, the clock and the decoder's
//          own tasks that share its core;
//   late   4 of the last 20 updates started more than 15 ms later than the panel's loop could have started them (the screen passes how
//          long after its due time, less one frame of the loop: a 30 fps loop serves a 50 ms step up to 33 ms late and no fault of
//          anyone's), or a whole step was missed: something else held the task, which is the sign a number averaged over a second hides.
//
// It goes back to twenty a second only after 10 s at ten, with the cost under 60 % of a fast step (30 ms) and no late update in the
// window: the gap between 70 and 60 % and the 10 s keep it from flipping. A fall within a minute of a rise doubles the 10 s (up to
// 160 s); a minute at twenty a second puts it back. The first two seconds of a view, and of a view woken from dimming, are not judged:
// the first draw of the stage and the first run of the code from the flash (cold in the cache) are not what the view costs, and the kit's
// first run fell to ten a second in them and could not rise from it. The thresholds are the plan's guesses until the kit has measured
// the real costs (planning/p4x-meters-view.md, "Kit protocol"; the first update of the canvas drawing cost 26 to 31 ms in the 5.1 and
// 9.1.6 rooms): they are one struct so that a measurement changes numbers and not code.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace iclforge::meters {

inline constexpr std::int64_t kFastStepUs = 50'000;   // twenty a second
inline constexpr std::int64_t kSlowStepUs = 100'000;  // ten a second

struct GovernorLimits {
    double demote_load = 0.70;  // of a fast step, the window's average cost above which it falls to ten a second
    double promote_load = 0.60;  // of a fast step, the average cost under which it may rise again
    std::int64_t late_us = 15'000;  // an update that starts this long after it could have is late
    int demote_late = 4;  // late updates (or missed steps) in the window that make it fall
    std::int64_t dwell_us = 10'000'000;  // at ten a second before it tries twenty again
    std::int64_t dwell_max_us = 160'000'000;
    std::int64_t flap_us = 60'000'000;  // a fall within this long of a rise doubles the dwell
    std::int64_t grace_us = 2'000'000;  // from the first update (or the first after restart_window()): counted, but not judged
};

inline constexpr std::size_t kGovernorWindow = 20;

struct GovernorStats {
    std::uint64_t updates = 0;
    std::uint64_t late = 0;     // started more than late_us later than they could have
    std::uint64_t dropped = 0;  // whole steps that went by with no update
    std::uint32_t falls = 0;    // twenty to ten
    std::uint32_t rises = 0;    // ten to twenty
};

class RateGovernor {
   public:
    // Not explicit: a member of an aggregate is made from `{}`, which an explicit constructor does not allow.
    RateGovernor() noexcept : RateGovernor(GovernorLimits{}) {}
    explicit RateGovernor(const GovernorLimits& limits) noexcept : limits_(limits), dwell_us_(limits.dwell_us) {}

    // The step to the next update, in microseconds.
    [[nodiscard]] std::int64_t step_us() const noexcept { return slow_ ? kSlowStepUs : kFastStepUs; }
    [[nodiscard]] bool slow() const noexcept { return slow_; }
    [[nodiscard]] int rate_hz() const noexcept { return slow_ ? 10 : 20; }
    [[nodiscard]] const GovernorStats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::int64_t dwell_us() const noexcept { return dwell_us_; }

    // The average cost of the updates in the window, in microseconds (0 before any).
    [[nodiscard]] double mean_cost_us() const noexcept { return filled_ == 0 ? 0.0 : cost_sum_ / static_cast<double>(filled_); }

    // An update ran. `now_us`: when it started; `cost_us`: what it cost; `late_us`: how long after it was due, less one frame of the loop, it started (0 if on time);
    // `missed`: whole steps that went by since the last one with no update. Returns the step to the next.
    std::int64_t record(std::int64_t now_us, std::int64_t cost_us, std::int64_t late_us, std::int64_t missed) noexcept {
        ++stats_.updates;
        const bool is_late = late_us > limits_.late_us;
        stats_.late += is_late ? 1 : 0;
        stats_.dropped += static_cast<std::uint64_t>(std::max<std::int64_t>(missed, 0));
        if (graced_from_us_ < 0) {
            graced_from_us_ = now_us;
        }
        if (now_us - graced_from_us_ < limits_.grace_us) {
            return step_us();
        }
        const bool bad = is_late || missed > 0;
        push(static_cast<double>(std::max<std::int64_t>(cost_us, 0)), bad);
        if (since_us_ < 0) {
            since_us_ = now_us;
        }
        const double load = mean_cost_us() / static_cast<double>(kFastStepUs);
        if (!slow_) {
            // A minute at twenty a second after a rise is a settled state: the next fall starts from the plain dwell again.
            if (now_us - since_us_ >= limits_.flap_us) {
                dwell_us_ = limits_.dwell_us;
            }
            if (filled_ >= kGovernorWindow && (load > limits_.demote_load || bad_ >= limits_.demote_late)) {
                // A fall soon after a rise: a lasting limit rather than a moment, so the next try waits longer.
                if (rose_us_ >= 0 && now_us - rose_us_ < limits_.flap_us) {
                    dwell_us_ = std::min(dwell_us_ * 2, limits_.dwell_max_us);
                }
                slow_ = true;
                since_us_ = now_us;
                ++stats_.falls;
                clear();
            }
        } else if (!hold_slow_ && now_us - since_us_ >= dwell_us_ && filled_ >= kGovernorWindow && load < limits_.promote_load && bad_ == 0) {
            slow_ = false;
            since_us_ = now_us;
            rose_us_ = now_us;
            ++stats_.rises;
            clear();
        }
        return step_us();
    }

    // The view was closed or the stream restarted: the window starts again, and so does the grace; the rate in force stays.
    void restart_window() noexcept {
        clear();
        graced_from_us_ = -1;
    }

    // The decoder is busy and the audio comes first: the view is held at ten a second, whatever an update costs, and does not try twenty. Taken off, it
    // is where it was and may rise by the rules above. Holding is not a fall: the counters are the measure of the panel's own limits.
    void set_hold_slow(bool hold) noexcept {
        if (hold == hold_slow_) {
            return;
        }
        hold_slow_ = hold;
        if (hold && !slow_) {
            slow_ = true;
            since_us_ = -1;
            clear();
        }
    }
    [[nodiscard]] bool holding_slow() const noexcept { return hold_slow_; }

   private:
    void push(double cost, bool bad) noexcept {
        if (filled_ == kGovernorWindow) {
            cost_sum_ -= costs_[head_];
            bad_ -= bads_[head_] ? 1 : 0;
        } else {
            ++filled_;
        }
        costs_[head_] = cost;
        bads_[head_] = bad;
        cost_sum_ += cost;
        bad_ += bad ? 1 : 0;
        head_ = (head_ + 1) % kGovernorWindow;
    }

    void clear() noexcept {
        filled_ = 0;
        head_ = 0;
        cost_sum_ = 0.0;
        bad_ = 0;
    }

    GovernorLimits limits_;
    GovernorStats stats_;
    std::array<double, kGovernorWindow> costs_{};
    std::array<bool, kGovernorWindow> bads_{};
    std::size_t head_ = 0;
    std::size_t filled_ = 0;
    double cost_sum_ = 0.0;
    int bad_ = 0;
    bool slow_ = false;
    bool hold_slow_ = false;
    std::int64_t graced_from_us_ = -1;  // when the first update of this stretch ran: the grace counts from it
    std::int64_t since_us_ = -1;  // when the rate in force took effect (or the first update)
    std::int64_t rose_us_ = -1;
    std::int64_t dwell_us_;
};

}  // namespace iclforge::meters
