#pragma once

// A reader for X.509 v3 certificates (RFC 5280) and the validation of a chain of them, for the
// profile the AC-4 content signing specification uses: EC keys on P-256 and ECDSA with SHA-256.
// Dependency-free, like the rest of iclforge::base's crypto.
//
// Anything this reader cannot interpret it refuses, in the direction of rejecting a certificate:
// another key type or curve, another signature algorithm, a critical extension it does not
// implement, a malformed time, a BER form where DER is required, bytes after the end. A
// certificate with a feature this code does not understand is never "probably fine".
//
// The library has no clock. A chain is judged at a time the caller passes in
// (ChainPolicy::now_unix), and a policy without one fails with kNoClock.

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/base/crypto/p256.hpp"
#include "iclforge/base/export.hpp"

namespace iclforge::base::crypto {

// Why a certificate did not parse.
enum class X509Error : std::uint8_t {
    kMalformed,             // not DER, truncated, or a field out of shape
    kTrailingData,          // bytes after the certificate
    kUnsupportedVersion,    // not v1 or v3
    kUnsupportedSignature,  // not ecdsa-with-SHA256, or the two algorithm fields differ
    kUnsupportedKey,        // not an EC key on prime256v1 in uncompressed form
    kKeyNotOnCurve,
    kBadTime,                   // an impossible date, or a time form other than UTC / generalized
    kBadSerial,                 // negative, empty or longer than 20 bytes
    kUnknownCriticalExtension,  // a critical extension other than keyUsage and basicConstraints
    kDuplicateExtension,
};

[[nodiscard]] ICLFORGE_BASE_EXPORT std::string_view describe(X509Error error);

// A distinguished name: the DER of the whole Name, which chain links compare byte for byte, and
// its attributes in the order they appear.
struct Name {
    struct Attribute {
        std::string oid;  // dotted decimal
        std::string
            value;  // the string's bytes; "#" and hex for a type this reader does not decode
    };

    std::vector<std::byte> der;
    std::vector<Attribute> attributes;

    // The first attribute with this OID: kCommonName, kTitle and so on.
    [[nodiscard]] std::optional<std::string_view> find(std::string_view oid) const;
    // "CN=ILok, title=Code/Content Signer, serialNumber=1798A2", the attributes in the order they
    // appear, for messages. Not a canonical form and not parsed back.
    [[nodiscard]] std::string to_string() const;
};

inline constexpr std::string_view kOidCommonName = "2.5.4.3";
inline constexpr std::string_view kOidSerialNumber = "2.5.4.5";
inline constexpr std::string_view kOidTitle = "2.5.4.12";

// keyUsage bits (RFC 5280 section 4.2.1.3), as the bit number in the extension.
inline constexpr std::uint16_t kKeyUsageDigitalSignature = 1u << 0;
inline constexpr std::uint16_t kKeyUsageKeyCertSign = 1u << 5;

struct BasicConstraints {
    bool ca = false;
    std::optional<std::uint32_t> path_length;
};

class ICLFORGE_BASE_EXPORT Certificate {
   public:
    std::vector<std::byte> der;
    int version = 3;                // 1 or 3
    std::vector<std::byte> serial;  // big endian, no sign byte
    Name issuer;
    Name subject;
    std::int64_t not_before = 0;  // seconds since 1970-01-01T00:00:00Z
    std::int64_t not_after = 0;
    P256PublicKey public_key;
    std::optional<std::uint16_t>
        key_usage;  // bit i set = keyUsage bit i; empty without the extension
    std::optional<BasicConstraints> basic_constraints;
    EcdsaSignature signature;

    // The bytes the issuer signed: the to-be-signed certificate, exactly as they are in `der`.
    [[nodiscard]] std::span<const std::byte> tbs() const noexcept {
        return std::span<const std::byte>{der}.subspan(tbs_offset_, tbs_size_);
    }

    // Uppercase hexadecimal of `serial`, "00" for the serial zero: "1798A2B5D62000000005".
    [[nodiscard]] std::string serial_hex() const;
    // The fingerprints of the DER, in the colon-separated uppercase form that openssl prints and
    // the specification's section 10.1 lists. SHA-1 identifies a certificate to a person and is
    // never used to judge one.
    [[nodiscard]] std::string sha1_fingerprint() const;
    [[nodiscard]] std::string sha256_fingerprint() const;
    // issuer and subject are the same bytes.
    [[nodiscard]] bool self_issued() const noexcept { return issuer.der == subject.der; }

   private:
    friend ICLFORGE_BASE_EXPORT std::expected<Certificate, X509Error> parse_certificate(
        std::span<const std::byte> der);
    std::size_t tbs_offset_ = 0;
    std::size_t tbs_size_ = 0;
};

// One certificate in DER, with nothing after it.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::expected<Certificate, X509Error> parse_certificate(
    std::span<const std::byte> der);

// A SubjectPublicKeyInfo (RFC 5280 section 4.1.2.7) holding an EC key on prime256v1 in
// uncompressed form, as a certificate carries it and as a PEM "PUBLIC KEY" block holds it. The DER
// of the whole structure, with nothing after it.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::expected<P256PublicKey, X509Error>
parse_subject_public_key_info(std::span<const std::byte> der);

// Seconds since the epoch for a civil UTC time, nullopt for an impossible date. Exposed for the
// parsing of the content signature's own timestamp.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::optional<std::int64_t> unix_time_from_civil(
    int year, int month, int day, int hour, int minute, int second);

// --- Trust and chains ---------------------------------------------------------------------

struct TrustAnchor {
    Certificate certificate;
    std::string label;  // where it came from, for reports: a file name
};

struct TrustedKey {
    P256PublicKey key;
    std::string label;
};

// What the operator trusts. A certificate is an anchor whether it is a root or an intermediate:
// trusting one issuing CA does not require holding its root. A trusted key accepts a signer whose
// leaf certificate holds that key. An empty store accepts nothing.
struct TrustStore {
    std::vector<TrustAnchor> anchors;
    std::vector<TrustedKey> keys;

    [[nodiscard]] bool empty() const noexcept { return anchors.empty() && keys.empty(); }
};

struct ChainPolicy {
    // The time every validity period is judged at. No default: the library has no clock.
    std::optional<std::int64_t> now_unix;
    // The opt-in relaxation. A certificate that is neither the leaf nor the anchor may be past its
    // notAfter; its notBefore, its signature, its CA flags and its name link are still checked.
    bool allow_expired_intermediates = false;
};

enum class ChainError : std::uint8_t {
    kNoClock,
    kNoTrust,  // the trust store is empty
    kEmptyChain,
    kBrokenLink,    // a certificate's issuer is not the subject of the one before it
    kBadSignature,  // a certificate's signature does not verify under its issuer's key
    kNotCa,         // an issuer without cA, keyCertSign, or beyond its path length
    kNotYetValid,
    kExpired,
    kUntrusted,  // no anchor or trusted key vouches for the chain
};

[[nodiscard]] ICLFORGE_BASE_EXPORT std::string_view describe(ChainError error);

struct ChainFailure {
    ChainError error = ChainError::kUntrusted;
    // The position in the chain (0 is the first, the root end) the failure is about; 0 when it is
    // not about one certificate.
    std::size_t index = 0;
};

struct ChainAcceptance {
    enum class Kind : std::uint8_t { kAnchor, kTrustedKey };
    Kind kind = Kind::kAnchor;
    std::size_t trust_index = 0;  // into TrustStore::anchors or ::keys, the first that accepted
    // Positions in the chain accepted only because of allow_expired_intermediates.
    std::vector<std::size_t> expired_intermediates;
};

// `chain` is in the order of the wire format: the root end first, the leaf last. The chain is
// accepted when:
//   - every certificate after the first names the one before it as its issuer, by the bytes of the
//     name, and its signature verifies under that certificate's key; a self-issued first
//     certificate verifies under its own key;
//   - every issuer is a CA (basicConstraints cA, and keyCertSign if keyUsage is present, within
//     its path length); a version 1 certificate is a CA only as a self-issued first certificate;
//   - the chain reaches the trust store: a certificate of the chain is an anchor (same subject and
//     key), or the first certificate is issued by an anchor and verifies under its key, or the
//     leaf holds a trusted key;
//   - from the anchor down to the leaf, and for the anchor itself, notBefore <= now <= notAfter,
//     except as the policy allows.
// Certificates above a matching anchor are not consulted.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::expected<ChainAcceptance, ChainFailure> validate_chain(
    std::span<const Certificate> chain, const TrustStore& trust, const ChainPolicy& policy);

// "2026-10-29T12:00:00Z" for a time, for reports.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::string format_iso8601_utc(std::int64_t unix_seconds);

// Parses "YYYY-MM-DDThh:mm:ssZ", with an optional fraction of a second that is ignored. nullopt
// for anything else, including a zone offset: the content signature's date is UTC.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::optional<std::int64_t> parse_iso8601_utc(
    std::string_view text);

}  // namespace iclforge::base::crypto
