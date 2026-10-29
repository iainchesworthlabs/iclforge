#pragma once

// The part of X.509 (RFC 5280) the content signature's certificate chain needs: read one DER
// certificate into its fields, and check that one certificate's signature was made by a given key.
// Only EC P-256 keys and ecdsa-with-SHA256 signatures are understood (CS spec §1.1, §4: "All
// certificates use EC keys on the secp256r1 curve"); a certificate in any other form is reported as
// that, not guessed at. This is not a general path validator: chain.hpp composes these into the
// policy the content signature wants.

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/base/crypto/ecdsa_p256.hpp"
#include "iclforge/base/export.hpp"

namespace iclforge::base::crypto {

enum class X509Error : std::uint8_t {
    kMalformed,             // not DER, or not the structure RFC 5280 §4.1 gives
    kUnsupportedPublicKey,  // not an EC key on P-256, or a point that is not on the curve
    kUnsupportedVersion,    // not v3 (v1 and v2 carry no extensions to check)
    kUnsupportedCriticalExtension,
};

[[nodiscard]] ICLFORGE_BASE_EXPORT const char* to_string(X509Error error) noexcept;

enum class SignatureAlgorithm : std::uint8_t {
    kEcdsaWithSha256,  // 1.2.840.10045.4.3.2, the only one this reader can check
    kEcdsaWithSha384,  // 1.2.840.10045.4.3.3, recognised and refused with a reason
    kEcdsaWithSha512,  // 1.2.840.10045.4.3.4, likewise
    kOther,
};

// Key usage bits as RFC 5280 §4.2.1.3 numbers them (bit 0 = digitalSignature).
inline constexpr std::uint16_t kKeyUsageDigitalSignature = 1U << 0;
inline constexpr std::uint16_t kKeyUsageKeyCertSign = 1U << 5;
inline constexpr std::uint16_t kKeyUsageCrlSign = 1U << 6;

struct DistinguishedName {
    std::vector<std::byte> der;  // the Name as encoded; two names are equal when these are
    std::string text;            // "CN=Example, title=Code/Content Signer, ..." for people to read
    std::optional<std::string> common_name;
    std::optional<std::string> title;  // the `title` attribute, 2.5.4.12 (CS spec §6.1 step 4)
    // The value of every attribute, in the order written, escaped as `text` is. CS spec v2.5
    // §5.3 asks that the publisher UUID match the one bound to the signing certificate and does
    // not say which attribute holds it, so a caller looks through all of them.
    std::vector<std::string> values;
};

struct Certificate {
    std::vector<std::byte> der;  // the whole certificate
    std::size_t tbs_offset = 0;  // the TBSCertificate within `der`, the bytes the signature covers
    std::size_t tbs_size = 0;

    std::vector<std::byte> serial;  // INTEGER value octets without a leading zero pad
    std::string serial_hex;         // upper-case hex of `serial`, as CS spec §5 writes serials
    DistinguishedName issuer;
    DistinguishedName subject;
    std::int64_t not_before = 0;  // seconds since 1970-01-01T00:00:00Z
    std::int64_t not_after = 0;
    P256PublicKey public_key;

    SignatureAlgorithm signature_algorithm = SignatureAlgorithm::kOther;
    std::string signature_algorithm_oid;
    EcdsaSignature signature{};     // valid when `signature_parsed`
    bool signature_parsed = false;  // the BIT STRING held a strict DER ECDSA signature

    std::optional<std::uint16_t> key_usage;  // absent when the extension is
    std::optional<bool> ca;                  // basicConstraints cA, absent without the extension
    std::optional<std::uint64_t> path_length;

    std::array<std::byte, 20> sha1_fingerprint{};  // CS spec §10.1's form
    std::array<std::byte, 32> sha256_fingerprint{};

    [[nodiscard]] bool self_issued() const { return issuer.der == subject.der; }
    [[nodiscard]] std::span<const std::byte> tbs() const {
        return std::span<const std::byte>(der).subspan(tbs_offset, tbs_size);
    }
};

// Parses one DER certificate. A critical extension other than basicConstraints and keyUsage is an
// error (RFC 5280 §4.2: a certificate with an unrecognised critical extension must be rejected);
// a non-critical extension of any kind is ignored.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::expected<Certificate, X509Error> parse_certificate(
    std::span<const std::byte> der);

// True when `cert.signature` is a valid ecdsa-with-SHA256 signature over `cert.tbs()` by `issuer`.
// False for any other signature algorithm too: check cert.signature_algorithm first to say why.
[[nodiscard]] ICLFORGE_BASE_EXPORT bool certificate_signature_valid(const Certificate& cert,
                                                                    const P256PublicKey& issuer);

// `text` made safe to print where a person reads it: a backslash, control characters, invalid UTF-8
// and the code points that act on a terminal or can reorder or hide text (U+0080 to U+009F, line
// and paragraph separators, zero-width and bidirectional controls) become \\, \xHH or \u{XXXX};
// valid printable UTF-8 is kept. Every name and string that comes out of a certificate or a
// signature goes through it before it reaches a message, so a hostile one cannot move the cursor,
// change a colour or begin a line that reads like a verdict.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::string escape_text(std::string_view text);

// "YYYY-MM-DDTHH:MM:SS" (UTC, with or without a trailing 'Z') as the content signature's signedDate
// is written (CS spec §3, the sample's value is "2026-04-13T23:38:29"), to seconds since the epoch.
// nullopt for anything that is not a real calendar instant.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::optional<std::int64_t> parse_iso8601_utc(
    std::string_view text);
[[nodiscard]] ICLFORGE_BASE_EXPORT std::string format_iso8601_utc(std::int64_t epoch_seconds);

}  // namespace iclforge::base::crypto
