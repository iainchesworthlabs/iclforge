// The stand-in for bench_memory_truehd.cpp when iclforge::truehd was not built
// (ICLFORGE_BUILD_TRUEHD=OFF): there are no TrueHD workloads to measure, so the table has none.

#include "bench_memory.hpp"

namespace membench {

void add_truehd_results(std::vector<Result>& /*results*/,
                        const iclforge::ac3::io::WavData& /*audio*/) {}

}  // namespace membench
