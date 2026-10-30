#pragma once

// What iclforge-kernelbench's translation units share: the sink a benched call's result flows
// through, the result row, and the timing loop. kernel_bench.cpp holds the AC-3, E-AC-3 and AC-4
// kernels. The TrueHD kernels are a translation unit of their own, kernel_bench_truehd.cpp, because
// iclforge::truehd is an option (ICLFORGE_BUILD_TRUEHD) and the tests are not: CMake builds that
// file, or kernel_bench_truehd_absent.cpp, which adds nothing, and neither carries a conditional.

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "iclforge/ac3/io/wav.hpp"

namespace perf::kernel_bench {

// Sink every kernel's output value flows through before the run ends, so no
// compiler can prove a benched call's result is unused and elide it.
inline double g_sink = 0.0;

struct KernelResult {
    std::string name;
    std::uint64_t iters = 0;
    double ns_per_call = 0.0;
};

// Runs `fn` repeatedly until at least `min_ms` have elapsed (and at least 16
// iterations, for kernels fast enough that 16 calls still beat min_ms) rather
// than a hand-picked iteration count per kernel - the ~20x cost spread across
// these kernels (a single quantize_mantissa call versus a full
// ecpl_channel_spectrum) makes one fixed count either too slow for the cheap
// kernels or too noisy for the expensive ones.
template <typename Fn>
KernelResult time_kernel(std::string name, Fn&& fn, double min_ms = 200.0) {
    for (int i = 0; i < 3; ++i) {
        fn();
    }
    std::uint64_t iters = 0;
    const auto start = std::chrono::steady_clock::now();
    double elapsed_ms = 0.0;
    do {
        fn();
        ++iters;
        elapsed_ms = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - start)
                         .count();
    } while (elapsed_ms < min_ms || iters < 16);
    return {.name = std::move(name), .iters = iters,
            .ns_per_call = elapsed_ms * 1.0e6 / static_cast<double>(iters)};
}

// Appends the TrueHD kernels' rows, timed on `wav`'s channels. Defined by kernel_bench_truehd.cpp, or
// by kernel_bench_truehd_absent.cpp when libs/truehd was not built, which appends nothing.
void add_truehd_kernels(std::vector<KernelResult>& results, const iclforge::ac3::io::WavData& wav);

}  // namespace perf::kernel_bench
