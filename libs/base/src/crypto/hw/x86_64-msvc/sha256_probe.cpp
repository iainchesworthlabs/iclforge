#include <array>

#include <intrin.h>

#include "crypto/sha256_hw.hpp"

// The MSVC/clang-cl member of the SHA-256 probe: selected by libs/base/CMakeLists.txt for the
// same compilers as variants/cpu-probe-msvc/ and for the same reason - clang-cl does not reliably
// have __builtin_cpu_supports, and <intrin.h> works on both.
//
// Unlike the AVX2 probe this needs no XGETBV step: the SHA instructions work on the XMM registers
// every x86-64 OS already saves, so CPUID alone is the whole answer. The kernel also uses SSSE3
// (pshufb, palignr) and SSE4.1 (pblendw), which every CPU with SHA-NI has, but the bits are asked
// for anyway so the probe cannot say yes to a CPU the kernel would fault on.

namespace iclforge::internal::crypto::hw {

bool cpu_reports_sha256() noexcept {
    std::array<int, 4> leaf{};

    __cpuid(leaf.data(), 0);
    if (leaf[0] < 7) {
        return false;
    }

    __cpuidex(leaf.data(), 1, 0);
    constexpr int kSsse3Bit = 1 << 9;
    constexpr int kSse41Bit = 1 << 19;
    if ((leaf[2] & kSsse3Bit) == 0 || (leaf[2] & kSse41Bit) == 0) {
        return false;
    }

    __cpuidex(leaf.data(), 7, 0);
    constexpr int kShaBit = 1 << 29;
    return (leaf[1] & kShaBit) != 0;
}

}  // namespace iclforge::internal::crypto::hw
