#pragma once

// SHA-1 (FIPS 180-4), self-contained, for one purpose: the certificate fingerprints a verifier is
// asked to pin (CS spec v2.0 §10.1 listed the SHA-1 fingerprints of the sample's chain, and an
// operator comparing a certificate against the one on a provisioning sheet is comparing that).
//
// A pin is a trust decision, not only a restriction: a chain that holds a pinned certificate is
// reported as anchored (chain.hpp), and SHA-1 no longer stands against someone who can have two
// certificates made, one for the operator to pin and one of their own with the same fingerprint.
// The chain is still checked link by link with ECDSA over SHA-256, but the pinned certificate is
// the one the fingerprint names. A SHA-256 fingerprint has no such weakness, and the operator who
// can choose should choose it; whether the SHA-1 form should be accepted at all is an owner's
// question (planning/static-analysis-truehd-signing-cenc.md, question 1).

#include <array>
#include <cstddef>
#include <span>

#include "iclforge/base/export.hpp"

namespace iclforge::base::crypto {

[[nodiscard]] ICLFORGE_BASE_EXPORT std::array<std::byte, 20> sha1(std::span<const std::byte> data);

}  // namespace iclforge::base::crypto
