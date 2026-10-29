// The stand-in for bench_encoder_truehd.cpp when iclforge::truehd was not built
// (ICLFORGE_BUILD_TRUEHD=OFF). A run of every workload has no TrueHD rows and says so; asking for one
// by name, or for the fixtures, is an error, as a name nothing matches is.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fmt/printf.h>

#include "bench_encoder.hpp"

namespace perf::bench {

void add_truehd_results(std::vector<Result>& /*results*/, const iclforge::ac3::io::WavData& /*audio*/,
                        const std::vector<std::string>& only, bool fixtures) {
    const bool named = fixtures || std::ranges::any_of(only, [](const std::string& name) {
                           return name.starts_with("truehd_") || name.starts_with("mlp_");
                       });
    if (named) {
        std::fprintf(stderr,
                     "iclforge-bench: this build has no TrueHD library (ICLFORGE_BUILD_TRUEHD=OFF)\n");
        std::exit(1);
    }
    if (only.empty()) {
        fmt::printf("TrueHD workloads skipped: this build has no TrueHD library\n");
    }
}

}  // namespace perf::bench
