#pragma once

// The DER form of an ECDSA signature: SEQUENCE { INTEGER r, INTEGER s } (SEC 1 section C.8,
// ITU-T X.690 for DER), as section 2.3 of the AC-4 content signing specification writes it and as
// an X.509 certificate's signature carries it.

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "iclforge/base/crypto/p256.hpp"
#include "iclforge/base/export.hpp"

namespace iclforge::base::crypto {

// Each integer in its shortest form, with a leading 00 where the high bit would make it negative.
// 8 to 72 bytes; the specification's "typically 70 to 72" is the usual case of two 32-byte values.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::vector<std::byte> encode_ecdsa_der(
    const EcdsaSignature& signature);

// Strict DER only. nullopt for: a length in a longer form than needed or of indefinite form; a
// tag other than SEQUENCE and INTEGER; a negative integer; an integer with a redundant leading
// zero byte; an integer wider than 256 bits; bytes after the SEQUENCE or inside it after the
// second INTEGER; any truncation; and r or s outside 1..n-1. OpenSSL reads some of these; a
// signature that only a lenient reader accepts is a different byte string for the same (r, s),
// which a verifier that hashes or compares the encoding must not have to consider.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::optional<EcdsaSignature> decode_ecdsa_der(
    std::span<const std::byte> der);

}  // namespace iclforge::base::crypto
