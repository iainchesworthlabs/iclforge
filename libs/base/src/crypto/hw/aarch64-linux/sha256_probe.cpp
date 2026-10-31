#include <asm/hwcap.h>
#include <sys/auxv.h>

#include "crypto/sha256_hw.hpp"

// The Linux and Android member of the SHA-256 probe for AArch64: the kernel reports the
// Cryptographic Extension's SHA2 instructions in the auxiliary vector's hardware-capability word.

namespace iclforge::internal::crypto::hw {

bool cpu_reports_sha256() noexcept {
    return (getauxval(AT_HWCAP) & HWCAP_SHA2) != 0;
}

}  // namespace iclforge::internal::crypto::hw
