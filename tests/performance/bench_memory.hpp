#pragma once

// What iclforge-membench's translation units share: the counters' snapshot, the result row, and the
// two functions that read and reset the allocation counters bench_memory.cpp keeps behind its
// replaced operator new. bench_memory.cpp holds the AC-3, E-AC-3 and AC-4 workloads. The TrueHD
// workloads are a translation unit of their own, bench_memory_truehd.cpp, because iclforge::truehd is
// an option (ICLFORGE_BUILD_TRUEHD) and the tests are not: CMake builds that file, or
// bench_memory_truehd_absent.cpp, which adds nothing, and neither carries a conditional.

#include <cstdint>
#include <string>
#include <vector>

#include "iclforge/ac3/io/wav.hpp"

namespace membench {

struct Snap {
    std::uint64_t allocs = 0;
    std::uint64_t bytes = 0;
    std::int64_t live = 0;
    std::int64_t peak = 0;
};

struct Result {
    std::string name;
    int frames = 0;
    std::uint64_t setup_allocs = 0;
    std::uint64_t setup_bytes = 0;
    std::uint64_t first_allocs = 0;
    std::uint64_t first_bytes = 0;
    double allocs_per_frame = 0.0;  // steady state: frames 1..N-1
    double bytes_per_frame = 0.0;
    std::int64_t steady_live_growth = 0;  // live-byte drift across steady state
    std::int64_t peak_live_delta = 0;     // peak transient above pre-loop live
};

// The allocation counters now, and the peak reset to the live bytes. Defined in bench_memory.cpp.
[[nodiscard]] Snap snap();
void reset_peak();

// Appends the TrueHD workloads' rows, measured on `audio`. Defined by bench_memory_truehd.cpp, or by
// bench_memory_truehd_absent.cpp when libs/truehd was not built, which appends nothing.
void add_truehd_results(std::vector<Result>& results, const iclforge::ac3::io::WavData& audio);

}  // namespace membench
