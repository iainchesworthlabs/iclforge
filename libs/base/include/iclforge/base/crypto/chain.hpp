#pragma once

// Checks a certificate chain the way CS spec §6.1 steps 3 and 4 ask, with the choices the operator
// makes (trust anchors, a time to judge validity at, the leaf's required title) as arguments rather
// than defaults. It answers two questions kept apart on purpose:
//   - is the chain well formed (each certificate issued and signed by the one above it, issuers
//     allowed to issue, the leaf allowed to sign content, every certificate valid at the time
//     asked)?
//   - is it anchored (does it lead to a certificate the operator said to trust)?
// A chain that is well formed and not anchored is reported as that. The caller must not call it
// "valid": it says only that the signature agrees with the certificates the file carries.
//
// What is checked: at most kMaxChainLength certificates; each one issued by the one above it (the
// issuer's DER name equals the subject's) and signed under that one's key with ECDSA over SHA-256;
// every certificate above the leaf has basicConstraints cA TRUE, keyCertSign where it states a key
// usage, and a pathLenConstraint not exceeded by the certificates below it; the leaf's
// digitalSignature and title where the policy asks; validity at the time the policy names. What is
// not: revocation (CRL, OCSP), name constraints and policy constraints (a certificate that marks
// either critical is refused by parse_certificate(); one that does not is read past), extended key
// usage, the authority and subject key identifiers, and, for a trust anchor the operator names, its
// own basicConstraints, key usage, validity and path length (an anchor is trusted as given, like a
// pin). The specification is silent on all of these (CS spec §6.1 steps 3 and 4); libs/ac4/ERRATA.md
// open questions 7 and 8 and planning/static-analysis-truehd-signing-cenc.md say what has been asked.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "iclforge/base/crypto/x509.hpp"
#include "iclforge/base/export.hpp"

namespace iclforge::base::crypto {

// The longest chain read from a signature, taken from an operator's files or validated: a root, a
// few intermediates and the signer. Beyond it a chain is refused before any signature in it is
// checked, so a hostile signature cannot make the verifier verify a thousand certificates.
inline constexpr std::size_t kMaxChainLength = 8;

// A certificate the operator named by fingerprint: 20 bytes (SHA-1, the form CS spec v2.0 §10.1 lists)
// or 32 bytes (SHA-256). A pinned certificate is trusted wherever it appears in the chain.
struct CertificatePin {
    std::vector<std::byte> fingerprint;
};

struct ChainPolicy {
    // Certificates the operator trusts as the top of a chain. A chain is anchored when one of its
    // certificates is byte-identical to one of these, or its top certificate was issued by one.
    std::span<const Certificate> trust_anchors;
    std::span<const CertificatePin> pins;
    // The instant every certificate must be valid at (CS spec §6.1 step 3: "now"); nullopt skips
    // the check. Certificates above the anchor are not judged: a path is validated from its anchor
    // down (RFC 5280 §6.1), so an expired intermediate above a pinned one does not matter.
    std::optional<std::int64_t> valid_at;
    // CS spec §6.1 step 4: the leaf's `title` attribute. Empty to skip.
    std::string required_leaf_title;
    // CS spec §6.1 step 4: the leaf's key usage includes digitalSignature.
    bool require_digital_signature = true;
};

enum class ChainFailure : std::uint8_t {
    kNone,
    kEmpty,                    // no certificates
    kIssuerMismatch,           // a certificate's issuer is not the subject of the one above it
    kBadSignature,             // a certificate's signature does not verify under its issuer's key
    kUnsupportedAlgorithm,     // a signature algorithm this build cannot check (ecdsa-with-SHA384,
                               // ...)
    kNotCertificateAuthority,  // an issuer without cA, or without keyCertSign where key usage is
                               // set
    kPathLength,               // a pathLenConstraint exceeded
    kNotValidAtTime,           // a certificate is not yet valid, or has expired, at `valid_at`
    kLeafKeyUsage,             // the leaf's key usage lacks digitalSignature
    kLeafTitle,                // the leaf's title is not the required one
};

struct ChainReport {
    ChainFailure failure = ChainFailure::kNone;
    std::size_t failed_index = 0;  // the certificate (index into the chain, root first) at fault
    std::string detail;            // a sentence naming it, for a message to a person
    // Meaningful when failure == kNone.
    bool anchored = false;
    // Which chain certificate matched an anchor or pin; nullopt when the anchor lies above the
    // chain (it issued chain[0]) or when the chain is not anchored.
    std::optional<std::size_t> anchor_index;
    std::string anchor_description;

    [[nodiscard]] bool ok() const noexcept { return failure == ChainFailure::kNone; }
};

// `chain` is root first, leaf last, as CertificateChainWire carries it (CS spec §3.7, §4.1).
[[nodiscard]] ICLFORGE_BASE_EXPORT ChainReport validate_chain(std::span<const Certificate> chain,
                                                              const ChainPolicy& policy);

// "certificate 2 (CN=Example Signer, serial 0A1B2C, ...)" for messages.
[[nodiscard]] ICLFORGE_BASE_EXPORT std::string describe_certificate(const Certificate& cert,
                                                                    std::size_t index);

}  // namespace iclforge::base::crypto
