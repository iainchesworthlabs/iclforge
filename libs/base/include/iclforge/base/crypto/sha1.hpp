#pragma once

// SHA-1 (FIPS 180-4), self-contained, for one purpose: the certificate fingerprints a verifier is
// asked to pin (CS spec §10.1 lists SHA-1 fingerprints, and an operator comparing a certificate
// against the one on a provisioning sheet is comparing that). It is never used for a security
// decision: a pin is an additional, optional restriction, and the chain is still checked with
// ECDSA over SHA-256. Collision attacks on SHA-1 do not matter to a fingerprint the operator
// copied from a trusted source, which is the only way a pin is used here.

#include <array>
#include <cstddef>
#include <span>

#include "iclforge/base/export.hpp"

namespace iclforge::base::crypto {

[[nodiscard]] ICLFORGE_BASE_EXPORT std::array<std::byte, 20> sha1(std::span<const std::byte> data);

}  // namespace iclforge::base::crypto
