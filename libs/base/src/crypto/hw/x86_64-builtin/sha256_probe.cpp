#include <cpuid.h>

#include "crypto/sha256_hw.hpp"

// The GCC/Clang/AppleClang member of the SHA-256 probe: selected for every x86-64 compiler that is
// not MSVC or clang-cl (see hw/x86_64-msvc/sha256_probe.cpp for the other half, and why
// clang-cl shares that one). <cpuid.h> rather than __builtin_cpu_supports("sha"), whose spelling
// of the feature is newer than the oldest compiler this project builds with.

namespace iclforge::internal::crypto::hw {

bool cpu_reports_sha256() noexcept {
    unsigned eax = 0;
    unsigned ebx = 0;
    unsigned ecx = 0;
    unsigned edx = 0;

    if (__get_cpuid_max(0, nullptr) < 7) {
        return false;
    }

    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx) == 0) {
        return false;
    }
    constexpr unsigned kSsse3Bit = 1U << 9;
    constexpr unsigned kSse41Bit = 1U << 19;
    if ((ecx & kSsse3Bit) == 0 || (ecx & kSse41Bit) == 0) {
        return false;
    }

    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx) == 0) {
        return false;
    }
    constexpr unsigned kShaBit = 1U << 29;
    return (ebx & kShaBit) != 0;
}

}  // namespace iclforge::internal::crypto::hw
