#pragma once

// ECDSA on NIST P-256 (secp256r1) with SHA-256 digests - FIPS 186-4 section 6, SEC 1 v2 section
// 4.1, and RFC 6979 for the nonce - written from the standards and dependency-free, as sha256.hpp
// is. It is the signature scheme of the AC-4 content signing specification (section 2.2) and of
// the X.509 chains that specification names (x509.hpp).
//
// The private key is the operator's own, supplied at runtime and never compiled in: P256PrivateKey
// scrubs its scalar when it dies, as SigningKey (signing_key.hpp) scrubs its bytes. Signing takes
// the same time and touches the same memory whatever the key and the digest are; see
// src/crypto/p256_math.hpp for how, and for what is and is not claimed.
//
// A "digest" here is the 32-byte SHA-256 of the signed content, used directly as the ECDSA message
// hash e (a 256-bit hash needs no truncation for a 256-bit group order).

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "iclforge/base/export.hpp"

namespace iclforge::base::crypto {

using Digest256 = std::array<std::byte, 32>;

// A point on the curve, other than infinity. Only a point that satisfies the curve equation can be
// constructed, so a function that takes one never has to check.
class ICLFORGE_BASE_EXPORT P256PublicKey {
   public:
    P256PublicKey() = default;

    // SEC 1 section 2.3.4's uncompressed form: 0x04, X, Y, 65 bytes. nullopt for any other length
    // or prefix, a coordinate that is not below p, or a point off the curve.
    [[nodiscard]] static std::optional<P256PublicKey> from_uncompressed(
        std::span<const std::byte> point);

    [[nodiscard]] std::array<std::byte, 65> uncompressed() const;
    [[nodiscard]] std::span<const std::byte, 32> x() const noexcept {
        return std::span<const std::byte, 32>{coordinates_.data(), 32};
    }
    [[nodiscard]] std::span<const std::byte, 32> y() const noexcept {
        return std::span<const std::byte, 32>{coordinates_.data() + 32, 32};
    }

    [[nodiscard]] bool operator==(const P256PublicKey&) const = default;

   private:
    std::array<std::byte, 64> coordinates_{};  // X then Y, big endian
};

// The private scalar d, 1 <= d <= n-1.
class ICLFORGE_BASE_EXPORT P256PrivateKey {
   public:
    P256PrivateKey() = default;
    ~P256PrivateKey();

    P256PrivateKey(const P256PrivateKey&) = default;
    P256PrivateKey& operator=(const P256PrivateKey&) = default;
    P256PrivateKey(P256PrivateKey&&) noexcept = default;
    P256PrivateKey& operator=(P256PrivateKey&&) noexcept = default;

    // 32 bytes, big endian. nullopt unless 1 <= d <= n-1.
    [[nodiscard]] static std::optional<P256PrivateKey> from_scalar(
        std::span<const std::byte> scalar);

    [[nodiscard]] bool empty() const noexcept { return !set_; }
    // The scalar, 32 bytes big endian, for a caller that has to write the key out (the key
    // generator the tests use). Every copy of the key scrubs its own bytes when it dies.
    [[nodiscard]] std::span<const std::byte, 32> scalar() const noexcept {
        return std::span<const std::byte, 32>{scalar_.data(), 32};
    }
    // d * G. Empty key: nullopt.
    [[nodiscard]] std::optional<P256PublicKey> public_key() const;

   private:
    std::array<std::byte, 32> scalar_{};
    bool set_ = false;
};

// (r, s), each 1..n-1, as 32 bytes big endian.
struct EcdsaSignature {
    std::array<std::byte, 32> r{};
    std::array<std::byte, 32> s{};

    [[nodiscard]] bool operator==(const EcdsaSignature&) const = default;
};

// True when the 32 bytes read as a big-endian integer in 1..n-1.
[[nodiscard]] ICLFORGE_BASE_EXPORT bool p256_scalar_in_range(std::span<const std::byte, 32> value);

// RFC 6979 section 3.2 with HMAC-SHA-256: the deterministic nonce k for `digest`, taking the
// `skip`th acceptable candidate (0 is the first, which is the one a signature uses unless r or s
// came out zero). Exposed so that the RFC's published values can be checked; a caller signs with
// ecdsa_sign. nullopt for an empty key.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::optional<std::array<std::byte, 32>> rfc6979_nonce(
    const P256PrivateKey& key, const Digest256& digest, std::uint32_t skip = 0);

// ECDSA signature of `digest` with the deterministic nonce (RFC 6979). Two calls with the same key
// and digest give the same signature; the specification allows either that or a random nonce
// (section 2.2). nullopt for an empty key. The signature is not normalised to a low s.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::optional<EcdsaSignature> ecdsa_sign(
    const P256PrivateKey& key, const Digest256& digest);

// FIPS 186-4 section 6.4.2. False for r or s outside 1..n-1.
[[nodiscard]] ICLFORGE_BASE_EXPORT bool ecdsa_verify(const P256PublicKey& key,
                                                     const Digest256& digest,
                                                     const EcdsaSignature& signature);

}  // namespace iclforge::base::crypto
