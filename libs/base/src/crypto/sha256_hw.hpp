#pragma once

// The hardware half of SHA-256's compression function: what CMake selects a directory under
// src/crypto/hw/ to provide. Two translation units answer it, because they need different things
// of the compiler:
//
//   * the KERNEL (hw/x86_64/ SHA-NI, hw/aarch64/ ARMv8 SHA2, or hw/none/) is the only code built
//     with the instruction-set flag, so no other function in the library can be compiled to use an
//     instruction the deployment machine may not have;
//   * the PROBE (hw/x86_64-{msvc,builtin}/, hw/aarch64-{linux,macos,windows}/) asks the CPU or the
//     OS whether it does, which is a different API on every compiler and OS.
//
// iclforge/base/detail/sha256_impl.hpp is the library's own seam over both; nothing else includes
// this header. The same arrangement as the AVX2 probe (iclforge/base/detail/cpu_features.hpp) and
// kernels, and for the same reason: the choice is a directory, never an #ifdef.

#include <cstddef>
#include <cstdint>

namespace iclforge::internal::crypto::hw {

// True if the CPU this process is running on can execute the kernel. Uncached: the caller asks
// once.
[[nodiscard]] bool cpu_reports_sha256() noexcept;

// FIPS 180-4's compression function over `blocks` consecutive 64-byte blocks, advancing the eight
// state words a..h in place. Only called when cpu_reports_sha256() was true; the hw/none/ build
// computes it with the portable code instead, so a stray call is slow rather than wrong.
void compress(std::uint32_t* state, const std::uint8_t* data, std::size_t blocks) noexcept;

}  // namespace iclforge::internal::crypto::hw
