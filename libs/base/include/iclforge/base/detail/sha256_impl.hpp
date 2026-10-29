#pragma once

#include <cstddef>
#include <cstdint>

#include "iclforge/base/export.hpp"

// ---------------------------------------------------------------------------
// SHA-256's compression function in both of the forms iclforge::base::crypto::Sha256 can run.
//
// Sha256 picks one itself, once per process: the CPU's SHA instructions when it has them (x86-64
// SHA-NI, ARMv8 SHA2), otherwise portable C++. Both forms are exposed here for exactly two
// callers, which is why this is a detail header and not part of crypto/sha256.hpp:
//   * libs/base/tests/test_crypto.cpp runs them against each other, so the hardware path is
//     checked bit-for-bit against the code that needs no hardware;
//   * tests/performance/crypto_bench.cpp times them side by side.
//
// Which kernel a build has is a CMake choice of directory (libs/base/src/crypto/hw/), the same
// shape as the AVX2 probe (cpu_features.hpp): a build for a CPU family without one gets the
// portable code behind sha256_compress_hardware() and sha256_hardware_supported() == false.
// ---------------------------------------------------------------------------

namespace iclforge::internal::crypto {

// One round of the FIPS 180-4 compression function per 64-byte block: `state` is a..h and is
// advanced in place over `blocks` consecutive blocks of `data`. No padding, no length: the caller
// has already framed the message.
ICLFORGE_BASE_EXPORT void sha256_compress_portable(std::uint32_t* state, const std::uint8_t* data,
                                                   std::size_t blocks) noexcept;

// True if this build has a hardware kernel AND the CPU running it can execute that kernel.
// Resolved once per process; every call after the first is a load of an already-computed bool.
[[nodiscard]] ICLFORGE_BASE_EXPORT bool sha256_hardware_supported() noexcept;

// The same function as sha256_compress_portable, on the CPU's SHA instructions. Calling it where
// sha256_hardware_supported() is false is not an error: it computes the portable result.
ICLFORGE_BASE_EXPORT void sha256_compress_hardware(std::uint32_t* state, const std::uint8_t* data,
                                                   std::size_t blocks) noexcept;

}  // namespace iclforge::internal::crypto
