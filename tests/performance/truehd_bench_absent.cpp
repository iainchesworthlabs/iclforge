#include "truehd_bench.hpp"

namespace perf::truehd_bench {

// iclforge::truehd was not built (ICLFORGE_BUILD_TRUEHD=OFF): nothing to time, and the callers say so.
DecodeRun time_decode(const iclforge::ac3::io::WavData& /*wav*/, int /*units*/,
                      int /*warmup_units*/) {
    return {};
}

}  // namespace perf::truehd_bench
