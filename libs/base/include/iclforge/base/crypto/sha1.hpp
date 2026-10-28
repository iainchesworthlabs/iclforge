#pragma once

// SHA-1 (FIPS 180-4), for one purpose: the certificate fingerprints that the AC-4 content
// signing specification publishes as conformance data (iclforge::base::crypto::Certificate::
// sha1_fingerprint). A fingerprint identifies a certificate to a person who compares two
// strings; it is never evidence about a signature. Nothing in this tree judges a signature, a
// chain or a digest with SHA-1: signatures use SHA-256 (sha256.hpp) and a certificate whose own
// signature uses any other algorithm is refused (x509.hpp).

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "iclforge/base/export.hpp"

namespace iclforge::base::crypto {

ICLFORGE_BASE_EXPORT std::array<std::byte, 20> sha1(std::span<const std::byte> data);

}  // namespace iclforge::base::crypto
