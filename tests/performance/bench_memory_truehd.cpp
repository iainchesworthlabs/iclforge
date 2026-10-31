// The TrueHD workloads of iclforge-membench (bench_memory.hpp says why they are a file of their own).
// bench_memory_truehd_absent.cpp takes this file's place when ICLFORGE_BUILD_TRUEHD is off.

#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "bench_memory.hpp"
#include "truehd_bench.hpp"

namespace membench {
namespace {

// TrueHD: a frame is an access unit (truehd_bench.hpp). The encoder takes one access unit's PCM a
// call and does its analysis on the call that completes a restart interval, so the steady state is
// three whole intervals (the first call only buffers), and the peak is what an interval holds.
Result bench_truehd_encode(std::string name, const perf::truehd_bench::Source& source,
                           const iclforge::truehd::EncoderConfig& config) {
    constexpr int kUnits = 3 * perf::truehd_bench::kIntervalUnits + 1;
    Result r{.name = std::move(name), .frames = kUnits};

    const Snap before = snap();
    auto encoder = iclforge::truehd::Encoder::create(config);
    const Snap after = snap();
    if (!encoder) {
        std::fprintf(stderr, "%s: iclforge::truehd::Encoder::create failed\n", r.name.c_str());
        std::exit(1);
    }
    r.setup_allocs = after.allocs - before.allocs;
    r.setup_bytes = after.bytes - before.bytes;
    const int samples = encoder->samples_per_access_unit();
    const auto feed = [&](int unit) {
        if (!encoder->encode(perf::truehd_bench::unit_pcm(source, samples,
                                                          static_cast<std::size_t>(unit)))) {
            std::fprintf(stderr, "%s: iclforge::truehd::Encoder::encode failed\n", r.name.c_str());
            std::exit(1);
        }
    };

    const Snap before_first = snap();
    reset_peak();
    feed(0);
    const Snap after_first = snap();
    r.first_allocs = after_first.allocs - before_first.allocs;
    r.first_bytes = after_first.bytes - before_first.bytes;

    const Snap steady_start = snap();
    reset_peak();
    for (int unit = 1; unit < kUnits; ++unit) {
        feed(unit);
    }
    const Snap steady_end = snap();
    r.allocs_per_frame = static_cast<double>(steady_end.allocs - steady_start.allocs) / (kUnits - 1);
    r.bytes_per_frame = static_cast<double>(steady_end.bytes - steady_start.bytes) / (kUnits - 1);
    r.steady_live_growth = steady_end.live - steady_start.live;
    r.peak_live_delta = steady_end.peak - steady_start.live;
    return r;
}

Result bench_truehd_decode(std::string name, const perf::truehd_bench::Stream& stream) {
    if (stream.units.size() < 2) {
        std::fprintf(stderr, "%s: encoding the TrueHD source stream failed\n", name.c_str());
        std::exit(1);
    }

    Result r{.name = std::move(name), .frames = static_cast<int>(stream.units.size())};

    const Snap before = snap();
    iclforge::truehd::Decoder decoder;
    const Snap after = snap();
    r.setup_allocs = after.allocs - before.allocs;
    r.setup_bytes = after.bytes - before.bytes;

    const Snap before_first = snap();
    reset_peak();
    if (!decoder.decode(stream.units[0])) {
        std::fprintf(stderr, "%s: iclforge::truehd::Decoder::decode failed\n", r.name.c_str());
        std::exit(1);
    }
    const Snap after_first = snap();
    r.first_allocs = after_first.allocs - before_first.allocs;
    r.first_bytes = after_first.bytes - before_first.bytes;

    const Snap steady_start = snap();
    reset_peak();
    for (std::size_t i = 1; i < stream.units.size(); ++i) {
        if (!decoder.decode(stream.units[i])) {
            std::fprintf(stderr, "%s: iclforge::truehd::Decoder::decode failed\n", r.name.c_str());
            std::exit(1);
        }
    }
    const Snap steady_end = snap();
    const auto steady_frames = static_cast<double>(stream.units.size() - 1);
    r.allocs_per_frame =
        static_cast<double>(steady_end.allocs - steady_start.allocs) / steady_frames;
    r.bytes_per_frame = static_cast<double>(steady_end.bytes - steady_start.bytes) / steady_frames;
    r.steady_live_growth = steady_end.live - steady_start.live;
    r.peak_live_delta = steady_end.peak - steady_start.live;
    return r;
}

}  // namespace

void add_truehd_results(std::vector<Result>& results, const iclforge::ac3::io::WavData& audio) {
    // TrueHD at 24 bits: stereo, 5.1 and 7.1 at 48 kHz, 5.1 at 96 and stereo at 192, from the same
    // programme material as the others.
    {
        namespace tb = perf::truehd_bench;
        for (const auto& layout : tb::kLayouts) {
            const std::string base = std::string("truehd_") + layout.name;
            const tb::Source source =
                tb::make_source(audio, layout.channels, 24, layout.sample_rate);
            const auto config = tb::config_for(source);
            results.push_back(bench_truehd_encode(base + "_encode", source, config));
            results.push_back(bench_truehd_decode(base + "_decode", tb::encode_stream(source, config)));
        }
    }
}

}  // namespace membench
