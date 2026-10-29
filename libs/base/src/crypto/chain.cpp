#include "iclforge/base/crypto/chain.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "iclforge/base/crypto/x509.hpp"

namespace iclforge::base::crypto {
namespace {

ChainReport failure(ChainFailure kind, std::size_t index, std::string detail) {
    ChainReport report;
    report.failure = kind;
    report.failed_index = index;
    report.detail = std::move(detail);
    return report;
}

// The certificate's signature, checked under `issuer`, or the reason it cannot be.
std::optional<ChainReport> check_signature(const Certificate& cert, std::size_t index,
                                           const P256PublicKey& issuer) {
    if (cert.signature_algorithm != SignatureAlgorithm::kEcdsaWithSha256) {
        return failure(ChainFailure::kUnsupportedAlgorithm, index,
                       describe_certificate(cert, index) + " is signed with " +
                           cert.signature_algorithm_oid +
                           ", and only ecdsa-with-SHA256 (1.2.840.10045.4.3.2) can be checked");
    }
    if (!certificate_signature_valid(cert, issuer)) {
        return failure(ChainFailure::kBadSignature, index,
                       describe_certificate(cert, index) +
                           " has a signature its issuer's key does not verify");
    }
    return std::nullopt;
}

bool matches_pin(const Certificate& cert, const CertificatePin& pin) {
    if (pin.fingerprint.size() == cert.sha1_fingerprint.size()) {
        return std::equal(pin.fingerprint.begin(), pin.fingerprint.end(),
                          cert.sha1_fingerprint.begin());
    }
    if (pin.fingerprint.size() == cert.sha256_fingerprint.size()) {
        return std::equal(pin.fingerprint.begin(), pin.fingerprint.end(),
                          cert.sha256_fingerprint.begin());
    }
    return false;
}

}  // namespace

std::string describe_certificate(const Certificate& cert, std::size_t index) {
    return "certificate " + std::to_string(index) + " (" + cert.subject.text + ", serial " +
           cert.serial_hex + ")";
}

ChainReport validate_chain(std::span<const Certificate> chain, const ChainPolicy& policy) {
    if (chain.empty()) {
        return failure(ChainFailure::kEmpty, 0, "the signature carries no certificates");
    }
    if (chain.size() > kMaxChainLength) {
        return failure(ChainFailure::kPathLength, kMaxChainLength,
                       "the chain holds " + std::to_string(chain.size()) +
                           " certificates and at most " + std::to_string(kMaxChainLength) +
                           " are accepted");
    }

    // 1. Each certificate is issued by, and signed under the key of, the one above it. The first is
    // checked against itself when it is self-issued (a root).
    if (chain.front().self_issued()) {
        if (const auto bad = check_signature(chain.front(), 0, chain.front().public_key)) {
            return *bad;
        }
    }
    for (std::size_t i = 1; i < chain.size(); ++i) {
        if (chain[i].issuer.der != chain[i - 1].subject.der) {
            return failure(ChainFailure::kIssuerMismatch, i,
                           describe_certificate(chain[i], i) + " names an issuer (" +
                               chain[i].issuer.text +
                               ") other than the subject of the one above it (" +
                               chain[i - 1].subject.text + ")");
        }
        if (const auto bad = check_signature(chain[i], i, chain[i - 1].public_key)) {
            return *bad;
        }
    }

    // 2. Everything above the leaf may issue certificates (RFC 5280 §4.2.1.9, §4.2.1.3, §6.1.4).
    for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
        const Certificate& cert = chain[i];
        if (cert.ca != std::optional<bool>(true) ||
            (cert.key_usage.has_value() && (*cert.key_usage & kKeyUsageKeyCertSign) == 0)) {
            return failure(
                ChainFailure::kNotCertificateAuthority, i,
                describe_certificate(cert, i) +
                    " issues the certificate below it but is not a certificate authority "
                    "that may sign certificates");
        }
        if (cert.path_length.has_value()) {
            // The certificates between this one and the leaf, not counting the leaf.
            const std::size_t below = chain.size() - 2 - i;
            if (below > *cert.path_length) {
                return failure(ChainFailure::kPathLength, i,
                               describe_certificate(cert, i) + " allows " +
                                   std::to_string(*cert.path_length) +
                                   " intermediate certificate(s) below it and the chain has " +
                                   std::to_string(below));
            }
        }
    }

    // 3. The leaf may sign content (CS spec §6.1 step 4).
    const Certificate& leaf = chain.back();
    const std::size_t leaf_index = chain.size() - 1;
    if (policy.require_digital_signature &&
        (!leaf.key_usage.has_value() || (*leaf.key_usage & kKeyUsageDigitalSignature) == 0)) {
        return failure(ChainFailure::kLeafKeyUsage, leaf_index,
                       describe_certificate(leaf, leaf_index) +
                           " does not list digitalSignature in its key usage");
    }
    if (!policy.required_leaf_title.empty() && leaf.subject.title != policy.required_leaf_title) {
        return failure(ChainFailure::kLeafTitle, leaf_index,
                       describe_certificate(leaf, leaf_index) + " has the title \"" +
                           leaf.subject.title.value_or("") + "\" and \"" +
                           policy.required_leaf_title + "\" is required");
    }

    // 4. Where the chain is anchored, so which certificates the time check applies to.
    ChainReport report;
    std::size_t first_judged = 0;
    for (std::size_t i = 0; i < chain.size() && !report.anchored; ++i) {
        for (const Certificate& anchor : policy.trust_anchors) {
            if (anchor.der == chain[i].der) {
                report.anchored = true;
                report.anchor_index = i;
                report.anchor_description = "trust anchor " + describe_certificate(anchor, i);
                break;
            }
        }
        for (const CertificatePin& pin : policy.pins) {
            if (!report.anchored && matches_pin(chain[i], pin)) {
                report.anchored = true;
                report.anchor_index = i;
                report.anchor_description = "pinned " + describe_certificate(chain[i], i);
            }
        }
        if (report.anchored) {
            first_judged = i;
        }
    }
    if (!report.anchored) {
        for (const Certificate& anchor : policy.trust_anchors) {
            if (chain.front().issuer.der == anchor.subject.der &&
                certificate_signature_valid(chain.front(), anchor.public_key)) {
                report.anchored = true;
                report.anchor_description = "trust anchor " + anchor.subject.text;
                break;
            }
        }
    }

    // 5. Validity at the time asked, from the anchor down; every certificate when unanchored.
    if (policy.valid_at.has_value()) {
        const std::int64_t at = *policy.valid_at;
        for (std::size_t i = first_judged; i < chain.size(); ++i) {
            if (at < chain[i].not_before) {
                return failure(ChainFailure::kNotValidAtTime, i,
                               describe_certificate(chain[i], i) + " is not valid before " +
                                   format_iso8601_utc(chain[i].not_before) + "Z (checked at " +
                                   format_iso8601_utc(at) + "Z)");
            }
            if (at > chain[i].not_after) {
                return failure(ChainFailure::kNotValidAtTime, i,
                               describe_certificate(chain[i], i) + " expired at " +
                                   format_iso8601_utc(chain[i].not_after) + "Z (checked at " +
                                   format_iso8601_utc(at) + "Z)");
            }
        }
    }
    return report;
}

}  // namespace iclforge::base::crypto
