// Times the component's TrueHD path on the host, and prints a digest of what it gives the sink, so
// that two builds of it (before and after a change) can be compared by diffing their output.
//
//   bench --file F --rate R --layout L [--block N] [--max-channels C] [--passes P] [--mode digest|player|stream]
//
//   digest  plays the stream on the virtual clock and prints the hashes of the samples the sink was
//           given, the times they play at, the meter's levels, and the playout's and the player's figures.
//   player  plays the stream P times over on the real clock and prints the player's own mean time a
//           chunk (BurstPlayerStatus::burst_us: the decode, the render and the rest, less the sink's
//           writes), which is what the board's /status calls sendspin.burst_us.
//   stream  decodes the stream P times over through TruehdStream alone and prints the time a chunk
//           takes and the blocks it is handed over in.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "harness.hpp"
#include "truehd_stream.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct Counter {
    std::uint64_t blocks = 0;
    std::uint64_t samples = 0;
    float sink = 0.0F;
};

void count(void* context, const iclforge::TruehdStream::Block& block) {
    auto& c = *static_cast<Counter*>(context);
    ++c.blocks;
    c.samples += block.samples;
    c.sink += block.channels.empty() ? 0.0F : block.channels[0][0];
}

}  // namespace

int main(int argc, char** argv) {
    std::string file;
    std::string layout = "5.1";
    std::string mode = "player";
    std::uint32_t rate = 48000;
    std::size_t block = 256;
    bool ahead = true;
    std::size_t max_channels = 0;
    std::size_t passes = 100;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string key = argv[i];
        const char* value = argv[i + 1];
        if (key == "--file") {
            file = value;
        } else if (key == "--layout") {
            layout = value;
        } else if (key == "--mode") {
            mode = value;
        } else if (key == "--rate") {
            rate = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
        } else if (key == "--block") {
            block = std::strtoull(value, nullptr, 10);
        } else if (key == "--ahead") {
            ahead = std::strtoull(value, nullptr, 10) != 0;
        } else if (key == "--max-channels") {
            max_channels = std::strtoull(value, nullptr, 10);
        } else if (key == "--passes") {
            passes = std::strtoull(value, nullptr, 10);
        }
    }
    if (file.empty()) {
        std::fprintf(stderr, "bench --file F [--rate R] [--layout L] [--block N] [--passes P] [--mode digest|player|stream]\n");
        return 2;
    }
    const std::vector<std::uint8_t> bytes = fwhost::read_file(file);

    if (mode == "stream") {
        const auto units = fwhost::split_units(bytes);
        std::vector<std::vector<std::uint8_t>> chunks;
        for (std::size_t first = 0; first < units.size(); first += 24) {
            std::vector<std::uint8_t> chunk;
            for (std::size_t u = first; u < std::min(units.size(), first + 24); ++u) {
                chunk.insert(chunk.end(), units[u].begin(), units[u].end());
            }
            chunks.push_back(std::move(chunk));
        }
        iclforge::TruehdStream stream;
        iclforge::TruehdStream::Config config;
        config.max_channels = max_channels;
        config.sample_rate_hz = rate;
        fwhost::set_block_samples(config, block);
        fwhost::set_run_samples<iclforge::TruehdStream>(config, ahead);
        if (!stream.open(config)) {
            std::fprintf(stderr, "open failed\n");
            return 1;
        }
        Counter counter;
        std::uint64_t decoded = 0;
        const auto begin = Clock::now();
        for (std::size_t pass = 0; pass < passes; ++pass) {
            for (const auto& chunk : chunks) {
                (void)stream.decode(std::as_bytes(std::span<const std::uint8_t>(chunk)), &count, &counter);
                ++decoded;
            }
        }
        const double us = std::chrono::duration<double, std::micro>(Clock::now() - begin).count();
        std::printf("mode=stream file=%s block=%zu chunks=%llu us_per_chunk=%.2f blocks_per_chunk=%.3f samples_per_chunk=%.1f\n",
                    file.c_str(), block, static_cast<unsigned long long>(decoded), us / static_cast<double>(decoded),
                    static_cast<double>(counter.blocks) / static_cast<double>(decoded),
                    static_cast<double>(counter.samples) / static_cast<double>(decoded));
        return counter.sink == 12345.0F ? 3 : 0;
    }

    fwhost::Scenario sc;
    sc.stream = bytes;
    sc.rate = rate;
    sc.layout = layout;
    sc.block_samples = block;
    sc.decode_ahead = ahead;
    sc.max_channels = max_channels;
    if (mode == "digest") {
        const fwhost::Outcome out = fwhost::run(sc);
        const auto& p = out.status.playout;
        std::printf("samples_fnv=%016llx play_fnv=%016llx levels_fnv=%016llx writes=%llu chunks=%zu outputs=%zu frames=%zu\n",
                    static_cast<unsigned long long>(fwhost::samples_digest(out)),
                    static_cast<unsigned long long>(fwhost::fnv(1469598103934665603ULL, out.play_us.data(),
                                                                out.play_us.size() * sizeof(std::int64_t))),
                    static_cast<unsigned long long>(fwhost::levels_digest(out)),
                    static_cast<unsigned long long>(out.writes), out.chunks, out.samples.size(),
                    out.samples.empty() ? std::size_t{0} : out.samples[0].size());
        std::printf("playout blocks=%llu stream_frames=%llu silence=%llu skipped=%llu dropped=%llu repeated=%llu resyncs=%u late=%llu "
                    "underruns=%llu error_us=%lld smoothed=%lld worst=%lld last_frame=%llu last_play_us=%lld last_target_us=%lld\n",
                    static_cast<unsigned long long>(p.blocks), static_cast<unsigned long long>(p.stream_frames),
                    static_cast<unsigned long long>(p.silence_frames), static_cast<unsigned long long>(p.skipped_frames),
                    static_cast<unsigned long long>(p.dropped_frames), static_cast<unsigned long long>(p.repeated_frames),
                    p.resyncs, static_cast<unsigned long long>(p.late_blocks), static_cast<unsigned long long>(p.underruns),
                    static_cast<long long>(p.error_us), static_cast<long long>(p.smoothed_error_us),
                    static_cast<long long>(p.worst_error_us), static_cast<unsigned long long>(p.last_frame),
                    static_cast<long long>(p.last_play_us), static_cast<long long>(p.last_target_us));
        const auto& c = out.status.counters;
        std::printf("counters bursts=%llu late=%llu dropped=%llu invalid=%llu coded_bytes=%llu acmod=%d lfe=%d\n",
                    static_cast<unsigned long long>(c.bursts_played), static_cast<unsigned long long>(c.late_chunks),
                    static_cast<unsigned long long>(c.dropped_chunks), static_cast<unsigned long long>(c.invalid_chunks),
                    static_cast<unsigned long long>(out.status.coded_bytes), out.status.decoder.acmod,
                    out.status.decoder.lfe ? 1 : 0);
        for (std::size_t o = 0; o < out.status.outputs; ++o) {
            std::printf("meter[%zu] peak=%a rms=%a stream_rms=%u\n", o, static_cast<double>(out.status.peak_db[o]),
                        static_cast<double>(out.status.rms_db[o]), static_cast<unsigned>(out.status.stream_rms[o]));
        }
        return 0;
    }

    sc.keep = false;
    sc.real_clock = true;
    sc.passes = passes;
    const fwhost::Outcome out = fwhost::run(sc);
    std::printf("mode=player file=%s block=%zu chunks=%zu burst_us=%u worst_burst_us=%u late=%llu underruns=%llu bursts=%llu\n",
                file.c_str(), block, out.chunks, static_cast<unsigned>(out.status.burst_us),
                static_cast<unsigned>(out.status.worst_burst_us),
                static_cast<unsigned long long>(out.status.counters.late_chunks),
                static_cast<unsigned long long>(out.status.counters.underruns),
                static_cast<unsigned long long>(out.status.counters.bursts_played));
    return 0;
}
