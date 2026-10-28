#pragma once

// The pace-ccjf1 content signature of the AC-4 content signing specification: the Thrift
// structures of its section 3, building one from a digest and the operator's key and chain (the
// signing procedure of section 5), and judging one against a digest and the operator's trust
// (the verification procedure of section 6, with its error conditions as ContentSignatureError).
//
// The scheme in one line: SHA-256 of the content is the ECDSA message hash, the signature is DER,
// and a `ContentSignature` carries that signature, the X.509 chain that vouches for the signing key
// and a small record of what was signed and how. What the "content" is, and where the serialised
// payload travels (a sidecar file, an MP4 box), belong to the codec that uses this: nothing here
// knows AC-4. The same structure serves any codec whose stream can be reduced to a digest, which
// is why it lives in iclforge::base.
//
// Nothing from a vendor is compiled in. The private key and the chain are the operator's
// (credentials.hpp), the trust is the operator's (TrustStore), and the leaf constraints the
// specification names (a title, a common name, a serial number) are a LeafProfile whose defaults
// are the specification's values and which the operator can relax.
//
// What the signature does not cover. The ECDSA operation signs the content digest and nothing
// else (specification sections 2.2, 5.2 and 6.1), so `signedDate`, `canonicalVersion`, the method
// names and the chain are not authenticated by the signature: an attacker who can rewrite the
// payload can change them without invalidating it. The chain is validated against the operator's
// trust, the method names and the version are checked for their one accepted value, and the date
// is bounded by the leaf's validity and the verifier's clock, but it remains informative.

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/base/crypto/credentials.hpp"
#include "iclforge/base/crypto/p256.hpp"
#include "iclforge/base/crypto/x509.hpp"
#include "iclforge/base/export.hpp"

namespace iclforge::base::crypto {

// --- Constants of the scheme (specification sections 3.8, 3.12 and 10.2) -----------------------

inline constexpr std::string_view kContentSignatureScheme = "pace-ccjf1";
// Identifies the serialised ContentSignature where a carrier needs a name for it (the usertype of
// an MP4 `uuid` box, for example). Specification section 3.12.
inline constexpr std::string_view kContentSignatureUuid = "b715f9db-6b41-403d-8bbe-429f3209ca2e";

// The 16 bytes of a UUID written in its text form, in the order RFC 4122 gives (the order an MP4
// `uuid` box's usertype has them). Hyphens are skipped. A string that is not 32 hexadecimal digits
// in that form gives all zeros, which a static_assert below rules out for the constant that uses it.
[[nodiscard]] constexpr std::array<std::byte, 16> uuid_bytes(std::string_view text) noexcept {
    std::array<std::byte, 16> out{};
    std::size_t nibbles = 0;
    for (const char c : text) {
        if (c == '-') {
            continue;
        }
        unsigned value = 0;
        if (c >= '0' && c <= '9') {
            value = static_cast<unsigned>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            value = static_cast<unsigned>(c - 'a') + 10U;
        } else if (c >= 'A' && c <= 'F') {
            value = static_cast<unsigned>(c - 'A') + 10U;
        } else {
            return {};
        }
        if (nibbles >= 32) {
            return {};
        }
        std::byte& target = out[nibbles / 2];
        target = static_cast<std::byte>((std::to_integer<unsigned>(target) << 4) | value);
        ++nibbles;
    }
    return nibbles == 32 ? out : std::array<std::byte, 16>{};
}

inline constexpr std::array<std::byte, 16> kContentSignatureUsertype =
    uuid_bytes(kContentSignatureUuid);
static_assert(kContentSignatureUsertype[0] == std::byte{0xB7} &&
                  kContentSignatureUsertype[15] == std::byte{0x2E},
              "the content signature UUID must parse");
// kSignatureValuesVersion3: the one version this code reads and writes.
inline constexpr std::int32_t kContentSignatureVersion = 3;

inline constexpr std::string_view kChainStructure = "x509";
inline constexpr std::string_view kChainFormat = "der";
inline constexpr std::string_view kCurveName = "secp256r1";
inline constexpr std::string_view kDigestDataRef = "digest_data_ref_signer_info";
inline constexpr std::string_view kCanonicalMethod = "thrift_binary";
inline constexpr std::string_view kHashName = "sha256";
inline constexpr std::string_view kSignatureMethodName = "ecdsa";
inline constexpr std::string_view kDigestMethodName = "sha256";

// The leaf the specification describes (section 4.5 and 10.2).
inline constexpr std::string_view kSpecSignerTitle = "Code/Content Signer";
inline constexpr std::string_view kSpecSignerCommonName = "ILok";
inline constexpr std::string_view kSpecSignerSerialHex = "1798A2B5D62000000005";

// A payload larger than this is not read: four certificates and the records around them come to
// about three kilobytes. A chain longer than kMaxChainLength is not read either; ECDSA
// verification costs a millisecond or so, and a chain of a thousand certificates is an attack and
// not a PKI.
inline constexpr std::size_t kMaxContentSignatureBytes = std::size_t{1} << 20;
inline constexpr std::size_t kMaxChainLength = 16;

// --- The wire structures (specification section 3) ---------------------------------------------
// Not implemented: ContentSignatureInfo (section 3.2, which has no fields), SignedInfo (3.9, which
// no structure uses) and SymmetricKey (3.10, content encryption, outside signing).

struct CertificateChainWire {
    std::string chain_structure;                       // "x509"
    std::string format;                                // "der"
    std::string pk_standard_name;                      // "secp256r1"
    std::vector<std::vector<std::byte>> certificates;  // DER, root end first, leaf last
};

struct SignerInfoWire {
    std::string signer_serial_number;        // the leaf's serial number, hexadecimal
    std::vector<std::byte> signature_value;  // DER (r, s)
};

struct LsSignedDigestWire {
    std::vector<std::byte> digest_value;  // 32 bytes of SHA-256
    std::string reference;                // "digest_data_ref_signer_info"
};

struct LsSignedInfoWire {
    std::int32_t canonical_version = 0;
    std::string signed_date;  // ISO 8601, UTC
    std::vector<LsSignedDigestWire> digests;
};

struct SignatureValueDetailsWire {
    std::string digest_data_ref;
    std::string canonical_method;
    std::string hash;
    std::string signature_method;
    std::string pk_standard_name;
    std::string digest_method;
};

struct SignatureValuesWire {
    LsSignedInfoWire signed_info;
    SignatureValueDetailsWire signature_values;
};

struct ContentSignature {
    CertificateChainWire certificate_chain;
    SignerInfoWire signer_info;
    SignatureValuesWire signature_values;
};

// --- Errors -------------------------------------------------------------------------------------

// The first seven are the conditions of the specification's section 6.2, under its names. The rest
// are conditions the table has no row for, which a verifier has to name: an ECDSA check that fails,
// a digest that is not the content's, a missing trust store.
enum class ContentSignatureError : std::uint8_t {
    kBadOrMissingSignature,             // no payload, or Thrift / DER decoding failed
    kBadContentSignaturePKStandard,     // pkStandardName is not "secp256r1"
    kBadContentSignatureDate,           // signedDate invalid or outside the acceptable range
    kCertNotPrivilegedToSignContent,    // the leaf lacks the key usage or the profile's title/name
    kBadCredentialsForSigningContent,   // the certificate chain does not validate
    kLibDoesntSupportContentSignature,  // a version marker or method name this code does not read
    kCertifierNoPrivateKey,             // signing: no private key

    // Not in the specification's table.
    kSignatureInvalid,      // the ECDSA signature does not verify under the leaf's key
    kDigestMismatch,        // the digest the payload records is not the content's
    kSignerSerialMismatch,  // signerSerialNumber is not the leaf's serial number
    kNoTrustAnchors,        // verification with an empty trust store
    kNoClock,               // verification without a time to judge validity at
    kKeyChainMismatch,      // signing: the leaf certificate's key is not the private key's
    kNoContent,             // signing or verifying content that has no bytes (a stream of no frames)
};

// The specification's spelling ("BadOrMissingSignature") or, for the extra codes, the same form
// ("SignatureInvalid"). Stable: it is what a CLI prints and a script matches.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::string_view error_name(ContentSignatureError error);
// A sentence for a person.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::string_view describe(ContentSignatureError error);
// True for the seven codes of the specification's section 6.2.
[[nodiscard]] ICLFORGE_BASE_EXPORT bool in_specification(ContentSignatureError error) noexcept;

struct ContentSignatureFailure {
    ContentSignatureError error = ContentSignatureError::kBadOrMissingSignature;
    // What exactly failed, in words: "certificate 2 of 3 has expired", "version 2". Not parsed by
    // anything; the code above is what a caller branches on.
    std::string detail;
    // For kBadCredentialsForSigningContent that came from chain validation: which check, and where.
    std::optional<ChainFailure> chain;
};

// --- Serialisation -----------------------------------------------------------------------------
// The field ids are 1 to n in the order the specification's section 3 lists each structure's
// fields (content_signature.cpp has the table; it is item 1 of the register in
// planning/ac4-content-signing.md).

// TBinaryProtocol bytes of the structure, as written, with no checks on what the fields hold (the
// tests make invalid payloads with it). Empty if a field is too long for a 32-bit length.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::vector<std::byte> serialize_content_signature(
    const ContentSignature& signature);

// Strict: every field of every structure must be present exactly once with its type; a field the
// table does not know is skipped by its type; nothing may follow the structure. Failures are
// kBadOrMissingSignature. Nothing about the values is judged here (verify_digest does that).
[[nodiscard]] ICLFORGE_BASE_EXPORT std::expected<ContentSignature, ContentSignatureFailure>
parse_content_signature(std::span<const std::byte> payload);

// --- Signing (specification section 5) ---------------------------------------------------------

// Builds the structure for `digest`. `credentials.chain` is in wire order (root end first, leaf
// last) and its leaf must hold the public key of `credentials.key`. `signed_unix` is the time to
// record, a value the caller supplies because the library has no clock.
//
// kCertifierNoPrivateKey for an empty key; kBadCredentialsForSigningContent for an empty or
// over-long chain; kKeyChainMismatch when the leaf is not the key's certificate;
// kBadContentSignatureDate for a time outside years 1970 to 9999.
//
// The chain is not validated: the signer's job is to carry what the operator supplied, and a chain
// that has expired or has no trusted root is the verifier's finding.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::expected<ContentSignature, ContentSignatureFailure>
build_content_signature(const Digest256& digest, const SigningCredentials& credentials,
                        std::int64_t signed_unix);

// build_content_signature, serialised.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::expected<std::vector<std::byte>, ContentSignatureFailure>
sign_digest(const Digest256& digest, const SigningCredentials& credentials,
            std::int64_t signed_unix);

// --- Verification (specification section 6) ----------------------------------------------------

// What the signer's leaf certificate must look like, beyond holding the signing key. The defaults
// are the values of the specification (sections 4.5 and 10.2). A deployment whose leaf differs
// uses any() or names its own values. The leaf's serial number must always equal the payload's
// signerSerialNumber; that is a consistency check inside the payload and does not depend on this.
struct LeafProfile {
    std::optional<std::string> title = std::string{kSpecSignerTitle};  // attribute 2.5.4.12
    std::optional<std::string> common_name = std::string{kSpecSignerCommonName};
    std::optional<std::string> serial_hex = std::string{kSpecSignerSerialHex};
    // keyUsage must be present and include digitalSignature (section 6.1 step 4).
    bool require_digital_signature = true;

    // The specification's leaf.
    [[nodiscard]] static LeafProfile spec() { return LeafProfile{}; }
    // No constraint on title, name or serial; the key usage is still required.
    [[nodiscard]] static LeafProfile any() {
        LeafProfile profile;
        profile.title.reset();
        profile.common_name.reset();
        profile.serial_hex.reset();
        return profile;
    }
};

struct VerifyOptions {
    // The time chain validity is judged at (ChainPolicy::now_unix, with no default) and whether
    // an expired intermediate is tolerated. The same time bounds signedDate from above.
    ChainPolicy chain;
    LeafProfile profile;
    // How far past the verification time signedDate may be: clocks differ, and a signer a few
    // minutes ahead is not an attack. A day, as in register item 12.
    std::int64_t max_future_skew_seconds = 24 * 60 * 60;
};

struct VerifiedSignature {
    // The chain as the payload carried it, parsed, root end first. chain.back() is the leaf.
    std::vector<Certificate> chain;
    // Which part of the trust store accepted the chain (see ChainAcceptance), and any intermediate
    // accepted only because the policy allows expired ones.
    ChainAcceptance acceptance;
    std::int32_t canonical_version = 0;
    std::string signed_date;
    std::int64_t signed_unix = 0;

    [[nodiscard]] const Certificate& leaf() const noexcept { return chain.back(); }
};

// Judges `payload` (the bytes of a serialised ContentSignature) for content whose SHA-256 the
// caller has just computed. The first check that fails names the error; the order is:
//
//   structure   Thrift and DER decoding, every field present, at least one digest entry
//                                                                  kBadOrMissingSignature
//   scheme      the version, the method names and the curve names
//                    kLibDoesntSupportContentSignature, kBadContentSignaturePKStandard
//   chain       the certificates parse and the chain reaches the trust store at the verification
//               time                                          kBadCredentialsForSigningContent
//   signer      the serial number, the key usage and the profile
//                         kSignerSerialMismatch, kCertNotPrivilegedToSignContent
//   signature   ECDSA under the leaf's key over `digest`           kSignatureInvalid
//   metadata    signedDate, then the digest the payload records     kBadContentSignatureDate,
//                                                                   kDigestMismatch
//
// The scheme checks come before the chain, which differs from the specification's list (the chain
// is step 3 and the method names step 8), so that a payload this code cannot read is called
// unsupported and not untrusted, and so that no signature work is done for it. An empty trust
// store (kNoTrustAnchors) and a missing time (kNoClock) are reported first of all: they are the
// operator's to fix and do not depend on the payload.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::expected<VerifiedSignature, ContentSignatureFailure>
verify_digest(std::span<const std::byte> payload, const Digest256& digest, const TrustStore& trust,
              const VerifyOptions& options);

}  // namespace iclforge::base::crypto
