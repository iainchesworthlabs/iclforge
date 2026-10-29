#include <sys/sysctl.h>

#include <cstddef>

#include "crypto/sha256_hw.hpp"

// The macOS member of the SHA-256 probe for AArch64. Every Apple-silicon CPU has the SHA2
// instructions, but macOS publishes the fact as a sysctl (macOS 12 and later), so the probe asks
// rather than assumes; a system that does not have the name takes the portable code.

namespace iclforge::internal::crypto::hw {

bool cpu_reports_sha256() noexcept {
    int present = 0;
    std::size_t size = sizeof(present);
    return sysctlbyname("hw.optional.arm.FEAT_SHA256", &present, &size, nullptr, 0) == 0 &&
           present == 1;
}

}  // namespace iclforge::internal::crypto::hw
