#pragma once

// What iclforge-bench's translation units share: the real-time budget, the result row, the per-frame
// timer, the warm-up length and the failure exit. bench_encoder.cpp holds the AC-3, E-AC-3 and AC-4
// workloads. The TrueHD workloads are a translation unit of their own, bench_encoder_truehd.cpp,
// because iclforge::truehd is an option (ICLFORGE_BUILD_TRUEHD) and the tests are not: CMake builds
// that file, or bench_encoder_truehd_absent.cpp, which says the codec is missing, and neither carries a
// conditional.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "iclforge/ac3/core/tables.hpp"
#include "iclforge/ac3/io/wav.hpp"

namespace perf::bench {

inline constexpr double kSampleRate = 48000.0;

inline double real_time_budget_ms(int frames) {
    return 1000.0 * static_cast<double>(frames) * iclforge::ac3::kSamplesPerFrame / kSampleRate;
}

struct Result {
    std::string name;
    int frames = 0;
    double total_ms = 0.0;
    double ms_per_frame = 0.0;
    // The tail, not just the average. A real-time codec is bounded by its
    // WORST frame, not its mean one: a block-switched frame on a transient
    // costs more than a steady-state frame, and a change that doubles the
    // tail while leaving the mean flat is invisible to a mean-only series -
    // which is exactly the shape of regression the whole performance suite
    // exists to catch. Both are per-frame milliseconds, same unit as
    // ms_per_frame, so a reader can compare them directly against the
    // real-time budget printed beside them.
    double p95_ms_per_frame = 0.0;
    double max_ms_per_frame = 0.0;
    // The frame's duration, which is the real-time budget: A/52's 1 536
    // samples for AC-3 and E-AC-3, AC-4's own frame length for AC-4. Written
    // per result because the two codecs' frames differ.
    double budget_ms_per_frame = real_time_budget_ms(1);
};

// Times each frame individually rather than the loop as a whole, so the
// distribution above can be computed. The extra cost is one steady_clock
// read per frame against a frame that takes milliseconds - under 0.01% at
// the fastest workload here, measured, and it buys the tail.
class FrameTimer {
public:
    explicit FrameTimer(int expected_frames) {
        per_frame_ms_.reserve(static_cast<std::size_t>(expected_frames));
    }

    template <typename Fn>
    void time_frame(Fn&& fn) {
        const auto start = std::chrono::steady_clock::now();
        fn();
        per_frame_ms_.push_back(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count());
    }

    [[nodiscard]] Result result(std::string name) const {
        const auto frames = static_cast<int>(per_frame_ms_.size());
        double total = 0.0;
        for (const double ms : per_frame_ms_) {
            total += ms;
        }
        // Nearest-rank percentile on a sorted copy: with 200 frames p95 is
        // the 190th slowest. Sorting a copy keeps the caller's insertion
        // order intact, which nothing needs today but costs nothing to keep.
        std::vector<double> sorted = per_frame_ms_;
        std::ranges::sort(sorted);
        const auto rank =
            static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(frames)));
        const auto p95_index = sorted.empty() ? 0 : std::min(sorted.size() - 1,
                                                             rank == 0 ? 0 : rank - 1);
        return {.name = std::move(name),
                .frames = frames,
                .total_ms = total,
                .ms_per_frame = frames > 0 ? total / frames : 0.0,
                .p95_ms_per_frame = sorted.empty() ? 0.0 : sorted[p95_index],
                .max_ms_per_frame = sorted.empty() ? 0.0 : sorted.back()};
    }

private:
    std::vector<double> per_frame_ms_;
};

// Frames run before the clock starts, on a THROWAWAY encoder of the same
// configuration. The point is to warm the caches, the branch predictors and
// the allocator's free lists - not the encoder, which is why the timed run
// below still constructs its own fresh instance and still starts at frame 0.
// Without this the first workload in program order pays every cold-start cost
// in the process and the rest do not, which is a bias between series rather
// than noise within one: it does not average out over repetitions, and it
// silently moves if the workload order ever changes.
inline constexpr int kWarmupFrames = 8;

[[noreturn]] inline void fail(const char* workload, const char* what) {
    std::fprintf(stderr, "%s: %s failed\n", workload, what);
    std::exit(1);
}

// Whether --only asked for the workload `name`, or was not given at all.
[[nodiscard]] inline bool wanted(const std::vector<std::string>& only, std::string_view name) {
    return only.empty() || std::ranges::contains(only, name);
}

// Appends the TrueHD workloads' rows: the four a trend run keeps (stereo and 5.1, encode and decode),
// and with `fixtures` a decode row, and an encode row for a TrueHD stream, for each committed stream
// under testdata/truehd. `only` is --only. Defined by bench_encoder_truehd.cpp; the stand-in in
// bench_encoder_truehd_absent.cpp appends nothing, and exits with an error if a TrueHD row was asked
// for by name.
void add_truehd_results(std::vector<Result>& results, const iclforge::ac3::io::WavData& audio,
                        const std::vector<std::string>& only, bool fixtures);

}  // namespace perf::bench
