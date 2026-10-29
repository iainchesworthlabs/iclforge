#pragma once

// ECDSA over secp256r1 (NIST P-256), self-contained, for the content signature
// (iclforge/base/crypto/content_signature.hpp). Sign and verify take the SHA-256 digest of the
// message, so the caller decides what the message is. No third-party library by design, for the
// reason sha256.hpp gives; the cost of that is that this file is the thing to read when asking
// whether the private-key path leaks, and the notes in ecdsa_p256.cpp say what it does about it.
//
// Sources: SEC 2 v2 §2.4.2 (the curve), FIPS 186-4 §6.4 (ECDSA), RFC 6979 §3.2 (the deterministic
// nonce, which CS spec §2.2 allows: "implementations MAY use either random or deterministic nonce
// generation"), RFC 3279 §2.2.3 / RFC 5480 (the DER form of a signature and of a public key point).

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "iclforge/base/export.hpp"

namespace iclforge::base::crypto {

// A point on the curve in affine coordinates, each coordinate 32 bytes, big-endian. Never the
// point at infinity: that has no affine form and is not a public key.
struct P256PublicKey {
    std::array<std::byte, 32> x{};
    std::array<std::byte, 32> y{};

    friend bool operator==(const P256PublicKey&, const P256PublicKey&) = default;
};

// The signature as the content signature carries it: two 32-byte big-endian unsigned integers
// (CS spec §2.3, §3.13). DER is a conversion for tools that expect it (ecdsa_signature_to_der()).
struct EcdsaSignature {
    std::array<std::byte, 32> r{};
    std::array<std::byte, 32> s{};

    friend bool operator==(const EcdsaSignature&, const EcdsaSignature&) = default;
};

// The private scalar d, 1 <= d < n. Zeroizes its bytes on destruction, as SigningKey does, so a key
// the operator supplied does not linger in freed memory after signing. Copies scrub themselves too.
class ICLFORGE_BASE_EXPORT P256PrivateKey {
   public:
    P256PrivateKey() = default;
    ~P256PrivateKey();

    P256PrivateKey(const P256PrivateKey&) = default;
    P256PrivateKey& operator=(const P256PrivateKey&) = default;
    P256PrivateKey(P256PrivateKey&&) noexcept = default;
    P256PrivateKey& operator=(P256PrivateKey&&) noexcept = default;

    // 32 big-endian bytes with 1 <= d < n; anything else (wrong length, zero, d >= n) is nullopt.
    [[nodiscard]] static std::optional<P256PrivateKey> from_scalar(
        std::span<const std::byte> scalar_big_endian);

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] std::span<const std::byte, 32> scalar() const noexcept { return scalar_; }

   private:
    std::array<std::byte, 32> scalar_{};
    bool valid_ = false;
};

// d * G. nullopt when `key` is not valid().
[[nodiscard]] ICLFORGE_BASE_EXPORT std::optional<P256PublicKey> p256_public_key(
    const P256PrivateKey& key);

// 0x04 || X || Y (SEC 1 §2.3.3), 65 bytes. Rejects other encodings, coordinates >= p, and points
// that are not on the curve (a public key from outside is checked before use, FIPS 186-4 §6.4.2).
[[nodiscard]] ICLFORGE_BASE_EXPORT std::optional<P256PublicKey> p256_public_key_from_uncompressed(
    std::span<const std::byte> encoded);
[[nodiscard]] ICLFORGE_BASE_EXPORT std::array<std::byte, 65> p256_public_key_uncompressed(
    const P256PublicKey& key);

// True when both coordinates are below p and y^2 = x^3 - 3x + b (mod p).
[[nodiscard]] ICLFORGE_BASE_EXPORT bool p256_public_key_valid(const P256PublicKey& key);

// ECDSA signature over a 32-byte digest, with the nonce from RFC 6979 (HMAC-SHA-256). The same key
// and digest always give the same signature, so a test can compare bytes with a published vector,
// and a flawed random source cannot reuse a nonce. nullopt when `key` is not valid().
[[nodiscard]] ICLFORGE_BASE_EXPORT std::optional<EcdsaSignature> ecdsa_p256_sign(
    const P256PrivateKey& key, std::span<const std::byte, 32> digest);

// FIPS 186-4 §6.4.2. False for an invalid key, r or s outside [1, n-1], or a signature that does
// not match. Not constant time: everything it reads is public.
[[nodiscard]] ICLFORGE_BASE_EXPORT bool ecdsa_p256_verify(const P256PublicKey& key,
                                                          std::span<const std::byte, 32> digest,
                                                          const EcdsaSignature& signature);

// DER SEQUENCE { INTEGER r, INTEGER s } (CS spec v2.0 §2.3, RFC 3279 §2.2.3): minimal integers, a
// leading 0x00 where the high bit is set. For OpenSSL and certificates; the content signature
// itself carries raw r and s.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::vector<std::byte> ecdsa_signature_to_der(
    const EcdsaSignature& signature);
// Strict: definite short lengths, minimal integers, no trailing bytes, r and s in [1, n-1].
[[nodiscard]] ICLFORGE_BASE_EXPORT std::optional<EcdsaSignature> ecdsa_signature_from_der(
    std::span<const std::byte> der);

}  // namespace iclforge::base::crypto
