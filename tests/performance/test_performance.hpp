#pragma once

// What iclforge-perf's translation units share: the slack the guards allow and the real-audio fixture
// they are fed. test_performance.cpp holds the AC-3, E-AC-3 and AC-4 guards. The TrueHD guards are a
// translation unit of their own, test_performance_truehd.cpp, because iclforge::truehd is an option
// (ICLFORGE_BUILD_TRUEHD) and the tests are not: CMake builds that file, or
// test_performance_truehd_absent.cpp, which skips, and neither carries a conditional.

#include <cstddef>

#include "iclforge/ac3/core/tables.hpp"
#include "iclforge/ac3/io/wav.hpp"
#include "real_audio.hpp"

namespace perf::gate {

// How many times its real-time budget a guarded run may take: see the header of test_performance.cpp.
inline constexpr double kSlackFactor = 2.0;

// reference_51.wav, read once.
inline const iclforge::ac3::io::WavData& fixture() {
    static const iclforge::ac3::io::WavData audio = perf::load_real_audio(
        perf::kReference51Wav, 6, static_cast<std::size_t>(iclforge::ac3::kSamplesPerFrame));
    return audio;
}

}  // namespace perf::gate
