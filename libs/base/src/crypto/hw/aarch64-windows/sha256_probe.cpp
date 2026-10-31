#include <windows.h>

#include "crypto/sha256_hw.hpp"

// The Windows-on-Arm member of the SHA-256 probe: the OS reports the Cryptographic Extension
// (AES, PMULL, SHA1 and SHA2 together) as one processor feature.

namespace iclforge::internal::crypto::hw {

bool cpu_reports_sha256() noexcept {
    return IsProcessorFeaturePresent(PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE) != FALSE;
}

}  // namespace iclforge::internal::crypto::hw
