#include <cstddef>
#include <cstdint>

#include "crypto/sha256_hw.hpp"
#include "iclforge/base/detail/sha256_impl.hpp"

// The build with no SHA-256 kernel: an architecture or operating system the other directories under
// hw/ do not cover, or ICLFORGE_SIMD=generic. Nothing is reported as supported, so the portable
// code runs; compress() is only here so the symbol exists, and computes the same result.

namespace iclforge::internal::crypto::hw {

bool cpu_reports_sha256() noexcept {
    return false;
}

void compress(std::uint32_t* state, const std::uint8_t* data, std::size_t blocks) noexcept {
    sha256_compress_portable(state, data, blocks);
}

}  // namespace iclforge::internal::crypto::hw
