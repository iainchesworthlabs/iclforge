#pragma once

// The TrueHD half of the performance suite's inputs: one 5.1 stream the TrueHD encoder writes from the
// real-audio fixture, and the time of each access unit's decode. Shared by iclforge-perf and
// iclforge-bench so a decode number is always read against the same stream; each binary keeps its own
// verdict, as it does for AC-3 and AC-4.
//
// The decoder is timed a frame at a time, as the AC-3 and AC-4 decoders are. A TrueHD access unit is 40
// samples at 44.1 and 48 kHz, 0.83 ms of audio and a few microseconds of decoding, which a clock read
// and the scheduler's noise would dominate; a frame here is 40 of them, 1 600 samples and 33.3 ms, about
// the length of an AC-3 frame, and the decoder runs them back to back.
//
// The measurement is in its own translation unit because iclforge::truehd is an option
// (ICLFORGE_BUILD_TRUEHD, default ON) and the tests are not: truehd_bench.cpp is the real one,
// truehd_bench_absent.cpp answers `built == false`, and tests/performance/CMakeLists.txt picks one.

#include <string>
#include <vector>

#include "iclforge/ac3/io/wav.hpp"

namespace perf::truehd_bench {

// The access units timed together as one frame, and the frames of a run: 3 000 access units, the whole of the
// fixture's 2.5 seconds, so none of the run is a wrapped or padded tail.
inline constexpr int kUnitsPerFrame = 40;
inline constexpr int kFrames = 75;
// The access units decoded first on a throwaway decoder: one frame.
inline constexpr int kWarmupUnits = kUnitsPerFrame;

struct DecodeRun {
    // false: libs/truehd was not built (ICLFORGE_BUILD_TRUEHD=OFF), and nothing else here is set.
    bool built = false;
    // Not empty: why the stream could not be made or decoded.
    std::string error;
    // The time of each frame's kUnitsPerFrame decode() calls, in milliseconds, in stream order.
    std::vector<double> frame_ms;
    // One frame's duration, which is its real-time budget.
    double budget_ms = 0.0;
};

// Encodes `frames` frames of the fixture's six channels (L R C LFE Ls Rs, the WAV's own order) as 24 bit
// TrueHD, then decodes them with a fresh decoder, timing each frame. `warmup_units` access units are decoded
// first on a throwaway decoder, for the reason bench_encoder.cpp gives for kWarmupFrames.
DecodeRun time_decode(const iclforge::ac3::io::WavData& wav, int frames, int warmup_units);

}  // namespace perf::truehd_bench
