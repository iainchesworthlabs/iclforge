#pragma once

// The content signature (CS spec v2.5, scheme "pace-ccjf1"): an ECDSA P-256 signature, with the
// certificate chain that vouches for its key and the digests it covers, serialised with Thrift's
// binary protocol into one blob. This file is codec-blind: the content is any bytes (the AC-4 file
// before the blob, an evolution frame payload, ...) and the carriage of the blob is the caller's.
//
// What the signature covers (CS spec v2.5 §2.2, §5.2 step 6, §6.1 step 6; v2.3's text said
// otherwise and a real signed blob showed the layout, which libs/ac4/ERRATA.md records):
//   - SignedInfoWire (SignatureEntryWire field 1) holds two digests: the SHA-256 of the
//   SignerInfoWire
//     struct as encoded (ContentSignature field 3) and the SHA-256 of the content.
//   - The ECDSA signature is made over the SHA-256 of SignedInfoWire's own encoded bytes. The
//   content
//     is covered through the digest inside it, not signed directly.
//   - Both digests and the signature are checked against the bytes as they were received, not
//   against
//     a re-serialisation.
//
// Signing is the operator's act with the operator's own key and chain (see credentials.hpp);
// nothing here carries a key or a certificate. Verifying says what the blob and the chain it
// carries agree on; whether the operator trusts that chain is the policy's trust anchors and pins,
// and a blob that checks out against a chain nobody anchored is reported as exactly that.

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/base/crypto/chain.hpp"
#include "iclforge/base/crypto/credentials.hpp"
#include "iclforge/base/crypto/ecdsa_p256.hpp"
#include "iclforge/base/crypto/x509.hpp"
#include "iclforge/base/export.hpp"

namespace iclforge::base::crypto {

// CS spec §1.1: the content signature UUID, which the sample carries as the signatureId of both
// SignedInfoWire and SignatureDataWire.
inline constexpr std::string_view kContentSignatureUuid = "b715f9db-6b41-403d-8bbe-429f3209ca2e";
inline constexpr std::string_view kSigningMethod = "pace-ccjf1";
inline constexpr std::string_view kSignerInfoReference = "digest_data_ref_signer_info";
inline constexpr std::string_view kContentReference = "digest_data_ref_win_signature_data";
inline constexpr std::string_view kCanonicalThriftBinary = "canonical_method_thrift_binary";
inline constexpr std::string_view kCanonicalRaw = "canonical_method_raw";
inline constexpr std::string_view kDefaultLeafTitle = "Code/Content Signer";
// CS spec v2.5 §3.2 and §5.3: signerNonce is 128 bytes.
inline constexpr std::size_t kSignerNonceBytes = 128;

// CS spec §6.2's error conditions, with the two the spec's table leaves implicit (a digest that
// differs from the one stored, and a signature that does not verify).
enum class SignatureError : std::uint8_t {
    kBadOrMissingSignature,             // no blob, or it is not the structure of §3 / §7.6
    kBadContentSignaturePkStandard,     // the curve or algorithm is not secp256r1 / ecdsa
    kBadContentSignatureDate,           // signedDate is not a date, or lies outside what it can be
    kCertNotPrivilegedToSignContent,    // the leaf lacks the key usage or the title
    kBadCredentialsForSigningContent,   // the chain does not hold together, or is not valid then
    kLibDoesntSupportContentSignature,  // an unrecognised version marker, scheme or method
    kCertifierNoPrivateKey,             // (signing) no usable key
    kDigestMismatch,                    // a computed digest differs from the stored one
    kSignatureInvalid,                  // the ECDSA signature does not verify under the leaf's key
};

[[nodiscard]] ICLFORGE_BASE_EXPORT const char* to_string(SignatureError error) noexcept;

struct SignatureFault {
    SignatureError code = SignatureError::kBadOrMissingSignature;
    std::string message;  // a sentence naming the specific thing
};

// The description of who signed and with what (CS spec v2.5 §5.3). The library writes what it is
// given. The publisher's UUID is checked against the signing certificate when a signature is made
// (see check_publisher_binding); the rest is free text, bound by the signature and checked by no
// one. `platform` and `application` are informational. The defaults name this library and no
// issuer.
struct SignerMetadata {
    std::int32_t publisher_type = 1;  // 1: an authorised publisher, as the sample has it
    std::string publisher_uuid;       // the UUID of the operator's PACE publisher account
    std::string publisher_name;
    std::string publisher_role = "publisher";
    std::int32_t product_id = 0;
    std::string product_name;
    std::string wrapper_name;
    std::string product_role = "product";
    std::string platform_id;
    std::string os_name;
    std::string os_version;
    std::string os_description;
    std::int32_t application_id = 0;
    std::string application_name = "ICL Forge";
    std::string application_description;
    std::string copyright;
    std::string application_version;
    std::string build_hash;

    friend bool operator==(const SignerMetadata&, const SignerMetadata&) = default;
};

// True for the 8-4-4-4-12 form of a UUID in hexadecimal digits, either case.
[[nodiscard]] ICLFORGE_BASE_EXPORT bool is_uuid(std::string_view text) noexcept;

// CS spec v2.5 §5.3 says `publisher.publisherUUID` shall match the UUID bound to the signing
// certificate and that a verifier may reject a mismatch. It does not say where a certificate binds
// a UUID. The sample the owner supplied names a publisher UUID that none of its four certificates
// carries in any attribute or extension. So the reading taken is the narrowest one that cannot
// reject a genuine signature: the claim is compared with the attributes of the signer certificate's
// subject, and only a subject that carries a UUID of its own, and not this one, is a mismatch.
enum class PublisherBinding : std::uint8_t {
    kMatches,   // an attribute of the subject is this UUID
    kMismatch,  // the subject carries a UUID, and none of its UUIDs is this one
    kUnbound,   // the subject carries no UUID: there is nothing to compare with
    kNotAUuid,  // the claimed publisherUUID is empty or is not a UUID
};

struct PublisherBindingResult {
    PublisherBinding binding = PublisherBinding::kNotAUuid;
    std::string detail;  // a sentence, with what the signature and the certificate say (escaped)
};

[[nodiscard]] ICLFORGE_BASE_EXPORT PublisherBindingResult
check_publisher_binding(std::string_view publisher_uuid, const Certificate& signer);

struct SignOptions {
    SignerMetadata metadata;
    // "YYYY-MM-DDTHH:MM:SS" (UTC) as the sample writes it. The caller supplies the clock. CS spec
    // v2.5 §5.3: it shall be the time of signing; a caller that gives another is not following it.
    std::string signed_date;
    // The signerNonce: kSignerNonceBytes (128) bytes, fresh for each signature (§5.3: reusing one
    // is a signing fault). Empty: the library draws one from std::random_device, which is the
    // platform's entropy source (hardware or the operating system's, never a fixed seed) on the
    // toolchains this builds with. A caller that supplies one is a test; any other length is
    // refused.
    std::vector<std::byte> nonce;
    // How the chain is checked before anything is signed with it: an empty `valid_at` checks no
    // validity period; the caller sets it (the CLI sets it to the signed date). Left at its
    // defaults the leaf must be allowed to sign (digitalSignature); set `required_leaf_title` to
    // also demand a title, as the verifier's default does.
    ChainPolicy chain_policy;
};

// Signs `content_digest` (the SHA-256 of the content). The chain is checked first with
// `options.chain_policy` (an expired or malformed chain is refused rather than signed with).
[[nodiscard]] ICLFORGE_BASE_EXPORT std::expected<std::vector<std::byte>, SignatureFault>
sign_content_digest(std::span<const std::byte, 32> content_digest,
                    const SigningCredentials& credentials, const SignOptions& options);

// SHA-256 of `content`, then sign_content_digest().
[[nodiscard]] ICLFORGE_BASE_EXPORT std::expected<std::vector<std::byte>, SignatureFault>
sign_content(std::span<const std::byte> content, const SigningCredentials& credentials,
             const SignOptions& options);

// ---- Reading ------------------------------------------------------------------------------

struct DigestEntry {
    std::int32_t version = 0;
    std::int32_t schema_version = 0;
    std::string reference;
    std::string canonical_method;
    std::string hash_algorithm;
    std::vector<std::byte> value;
};

// A blob taken apart. Offsets locate the two structs the digests and the signature are over inside
// `blob`, which this object owns.
struct ParsedContentSignature {
    std::vector<std::byte> blob;
    std::int32_t version = 0;
    std::int32_t schema_version = 0;

    std::vector<std::byte> signer_nonce;
    SignerMetadata signer;
    std::int32_t signer_version = 0;
    std::int32_t signer_schema_version = 0;
    std::string signed_date;
    std::int32_t chain_format_version = 0;
    std::string signing_method;
    std::string encoding_format;
    std::vector<Certificate> chain;  // root first, as carried
    std::string chain_error;         // set when `certificates` did not parse; chain is then empty

    std::int32_t canonical_version = 0;
    std::int32_t signed_info_version = 0;
    std::string signature_id;
    std::string canonical_method;
    std::string canonical_hash;
    std::string signature_algorithm;
    std::string signature_curve;
    DigestEntry signer_info_digest;
    std::vector<DigestEntry> content_digests;
    std::int32_t signature_data_version = 0;
    std::int32_t signature_type = 0;
    std::string signature_data_id;
    EcdsaSignature signature;

    std::size_t signer_info_offset = 0;  // ContentSignature field 3's struct, through its STOP
    std::size_t signer_info_size = 0;
    std::size_t signed_info_offset = 0;  // SignatureEntryWire field 1's struct, through its STOP
    std::size_t signed_info_size = 0;

    [[nodiscard]] std::span<const std::byte> signer_info_bytes() const {
        return std::span<const std::byte>(blob).subspan(signer_info_offset, signer_info_size);
    }
    [[nodiscard]] std::span<const std::byte> signed_info_bytes() const {
        return std::span<const std::byte>(blob).subspan(signed_info_offset, signed_info_size);
    }
};

// Reads the structure of CS spec v2.5 §3 and §7.6. Unknown fields are skipped and field order is
// free, as Thrift allows. Does not judge the certificates or check any digest or signature.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::expected<ParsedContentSignature, SignatureFault>
parse_content_signature(std::span<const std::byte> blob);

// ---- Verifying ----------------------------------------------------------------------------

enum class TimeBasis : std::uint8_t {
    kNow,         // every certificate valid at `now` (CS spec v2.5 §6.1 steps 3 and 4)
    kSignedDate,  // ... at the blob's own signedDate; only as trustworthy as the signer
    kInstant,     // ... at `instant`
    kIgnore,      // validity periods are not checked
};

// What a finding about the operator metadata of CS spec v2.5 §5.3 does to the verdict. The
// specification says a verifier "may" reject on these, so the policy is the caller's.
enum class OperatorMetadata : std::uint8_t {
    kReport,   // reported in VerifyReport::notes; the verdict is not affected
    kRequire,  // a nonce that is not 128 bytes, a publisherUUID that is not a UUID, or one the
               // signer certificate's subject contradicts, makes the report invalid
};

struct VerifyPolicy {
    std::span<const Certificate> trust_anchors;
    std::span<const CertificatePin> pins;
    TimeBasis time_basis = TimeBasis::kNow;
    std::int64_t now = 0;      // seconds since the epoch; the caller supplies the clock
    std::int64_t instant = 0;  // for kInstant
    // CS spec §6.1 step 4. Empty skips the title check.
    std::string required_leaf_title{kDefaultLeafTitle};
    bool require_digital_signature = true;
    // A signedDate later than `now` by more than this is refused (kNow only).
    std::int64_t max_future_skew_seconds = 86400;
    OperatorMetadata operator_metadata = OperatorMetadata::kReport;
};

struct CheckResult {
    std::string name;
    bool ok = false;
    std::string detail;
};

enum class Verdict : std::uint8_t {
    kValid,                  // every check passed and the chain leads to a trust anchor or pin
    kChecksChainUnanchored,  // every check passed; no anchor or pin was given, or none matched
    kInvalid,
};

struct VerifyReport {
    Verdict verdict = Verdict::kInvalid;
    std::optional<SignatureFault> fault;  // the first thing wrong, when verdict == kInvalid
    std::vector<CheckResult> checks;      // in order, each with its outcome
    // What the operator metadata of CS spec v2.5 §5.3 shows, one finding each (the signer nonce and
    // the publisher UUID). With OperatorMetadata::kReport these never change the verdict; with
    // kRequire the ones that fail are in `checks` as well.
    std::vector<CheckResult> notes;
    std::string signer;  // the leaf's subject
    std::string signer_serial;
    std::string signed_date;
    // What the signer says about itself. Bound by the signature and trusted no further than the
    // signer is: all escaped, none checked beyond the notes above.
    std::string publisher;         // "name (UUID uuid)"
    std::string product;           // the product and wrapper names, when given
    std::string platform;          // the operating system the signer ran on, when given
    std::string application;       // the signing application, its version and build, when given
    std::int64_t verified_at = 0;  // the instant the chain was judged at; 0 when not checked
    std::string anchor;            // what anchored it, when it is anchored
};

// Verifies `blob` against `content_digest`, the SHA-256 of the content as it is now. The checks run
// in the order of CS spec v2.5 §6.1, all of them reported: structure, versions and methods, curve,
// chain parsed, chain valid, signedDate, then the signature over SignedInfoWire, then the
// signer-info digest and the content digest it holds. The signature comes first because the digests
// carry weight only once it has verified (§6.1, "Verification order matters"): when it has not,
// the digest checks still run and say so.
[[nodiscard]] ICLFORGE_BASE_EXPORT VerifyReport
verify_content_signature(std::span<const std::byte> blob,
                         std::span<const std::byte, 32> content_digest, const VerifyPolicy& policy);

}  // namespace iclforge::base::crypto
