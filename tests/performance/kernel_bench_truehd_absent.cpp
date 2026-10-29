// The stand-in for kernel_bench_truehd.cpp when iclforge::truehd was not built
// (ICLFORGE_BUILD_TRUEHD=OFF): there are no TrueHD kernels to time, so the table has none.

#include "kernel_bench.hpp"

namespace perf::kernel_bench {

void add_truehd_kernels(std::vector<KernelResult>& /*results*/,
                        const iclforge::ac3::io::WavData& /*wav*/) {}

}  // namespace perf::kernel_bench
