#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "iclforge/base/crypto/chain.hpp"
#include "iclforge/base/crypto/content_signature.hpp"
#include "iclforge/base/crypto/credentials.hpp"
#include "iclforge/base/crypto/der.hpp"
#include "iclforge/base/crypto/pem.hpp"
#include "iclforge/base/crypto/thrift_binary.hpp"
#include "iclforge/base/crypto/x509.hpp"

// The parsers a content signature is read with (libs/base/src/crypto): the Thrift reader, the DER
// and X.509 readers, PEM and base64, the credential loaders, and the verifier that joins them.
//
// A content signature arrives with the file it signs, so everything inside it is chosen by whoever
// made the file: the Thrift counts and lengths, the certificates (which are parsed before anything
// is trusted, and are the densest input here), the signed date. Key and certificate files are
// operator-supplied and may be anything. Nothing here needs a key: an arbitrary blob simply fails
// to verify, which is the point; the harness is looking for the input that crashes, loops or reads
// past its buffer on the way to that answer.
//
// Input layout: one mode byte, then the payload.
//     [0]         the entry point, modulo 7:
//                   0  Thrift: read_struct and every interpreter on every field, followed down
//                   1  DER: the element reader, and parse_certificate with its signature check
//                   2  text: PEM, base64, the certificate and key loaders, the date reader
//                   3  blob: parse_content_signature, then validate_chain on what it carried
//                   4  blob: verify_content_signature under each way of judging time, with and
//                      without the blob's own certificates as trust anchors
//                   5  certificates: parse_certificates, order_chain, make_credentials
//                   6  as 3 and 4 on the payload read as base64, the form a secret holds
//     [1 .. )     the payload
namespace {

namespace crypto = iclforge::base::crypto;
namespace tb = iclforge::base::crypto::thrift;

// Every interpreter on a field, and a struct field's own fields in turn. The depth is bounded by
// the reader's own limit (tb::kMaxDepth); this only follows what it accepted.
void walk(std::span<const tb::Field> fields, int depth) {
    if (depth > tb::kMaxDepth) {
        return;
    }
    for (const tb::Field& field : fields) {
        (void)tb::as_i32(field);
        (void)tb::as_string(field);
        (void)tb::as_byte_list(field);
        (void)tb::as_string_to_byte_list_map(field);
        if (const auto list = tb::as_struct_list(field)) {
            for (const auto element : *list) {
                if (const auto inner = tb::read_struct(element)) {
                    walk(*inner, depth + 1);
                }
            }
        }
        if (const auto inner = tb::as_struct(field)) {
            walk(*inner, depth + 1);
        }
    }
}

void thrift(std::span<const std::byte> payload) {
    std::size_t consumed = 0;
    if (const auto fields = tb::read_struct(payload, &consumed)) {
        (void)tb::find(*fields, 3, tb::Type::kStruct);
        walk(*fields, 0);
    }
}

void der(std::span<const std::byte> payload) {
    std::span<const std::byte> rest = payload;
    while (!rest.empty()) {
        const auto element = crypto::der::read(rest);
        if (!element.has_value()) {
            break;
        }
        (void)crypto::der::oid_to_string(element->content);
        (void)crypto::der::small_unsigned(element->content);
    }
    if (const auto certificate = crypto::parse_certificate(payload)) {
        (void)crypto::certificate_signature_valid(*certificate, certificate->public_key);
        (void)crypto::describe_certificate(*certificate, 0);
        (void)crypto::format_iso8601_utc(certificate->not_before);
        (void)crypto::format_iso8601_utc(certificate->not_after);
    }
}

void text(std::span<const std::byte> payload) {
    const std::string_view view(reinterpret_cast<const char*>(payload.data()), payload.size());
    (void)crypto::pem_decode(view);
    (void)crypto::base64_decode(view);
    (void)crypto::parse_iso8601_utc(view);
    (void)crypto::parse_certificates(payload);
    (void)crypto::parse_p256_private_key(payload);
}

// validate_chain under the policies a caller reaches it with, on whatever the blob carried.
void chain_of(const crypto::ParsedContentSignature& parsed) {
    if (parsed.chain.empty()) {
        return;
    }
    crypto::ChainPolicy policy;
    (void)crypto::validate_chain(parsed.chain, policy);
    if (const auto signed_at = crypto::parse_iso8601_utc(parsed.signed_date)) {
        policy.valid_at = *signed_at;
    }
    policy.required_leaf_title = std::string(crypto::kDefaultLeafTitle);
    (void)crypto::validate_chain(parsed.chain, policy);
    // The blob's first certificate as the anchor, then the last as a pin: the anchored paths.
    policy.trust_anchors = std::span<const crypto::Certificate>(parsed.chain).first(1);
    (void)crypto::validate_chain(parsed.chain, policy);
    const std::array<crypto::CertificatePin, 2> pins{
        crypto::CertificatePin{std::vector<std::byte>(parsed.chain.back().sha1_fingerprint.begin(),
                                                      parsed.chain.back().sha1_fingerprint.end())},
        crypto::CertificatePin{
            std::vector<std::byte>(parsed.chain.back().sha256_fingerprint.begin(),
                                   parsed.chain.back().sha256_fingerprint.end())}};
    policy.trust_anchors = {};
    policy.pins = pins;
    (void)crypto::validate_chain(parsed.chain, policy);
}

void blob(std::span<const std::byte> payload) {
    if (const auto parsed = crypto::parse_content_signature(payload)) {
        chain_of(*parsed);
    }
}

void verify(std::span<const std::byte> payload) {
    const std::array<std::byte, 32> digest{};
    std::vector<crypto::Certificate> anchors;
    if (const auto parsed = crypto::parse_content_signature(payload)) {
        anchors = parsed->chain;
    }
    for (const auto basis : {crypto::TimeBasis::kNow, crypto::TimeBasis::kSignedDate,
                             crypto::TimeBasis::kInstant, crypto::TimeBasis::kIgnore}) {
        crypto::VerifyPolicy policy;
        policy.time_basis = basis;
        policy.now = 1'780'000'000;
        policy.instant = 1'700'000'000;
        (void)crypto::verify_content_signature(payload, digest, policy);
        if (!anchors.empty()) {
            policy.trust_anchors = anchors;
            policy.required_leaf_title.clear();
            policy.require_digital_signature = false;
            // The signer information of CS spec v2.5 5.3 as a condition of the verdict, which
            // reads the nonce and the publisher UUID against the signer certificate's subject.
            policy.operator_metadata = crypto::OperatorMetadata::kRequire;
            (void)crypto::verify_content_signature(payload, digest, policy);
        }
    }
}

void certificates(std::span<const std::byte> payload) {
    auto parsed = crypto::parse_certificates(payload);
    if (!parsed.has_value()) {
        return;
    }
    // A key the payload itself names, so that credentials with a chain that does and does not
    // belong to it are both reached.
    std::array<std::byte, 32> scalar{};
    for (std::size_t i = 0; i < scalar.size() && i < payload.size(); ++i) {
        scalar[i] = payload[i];
    }
    auto ordered = crypto::order_chain(*parsed);
    if (const auto key = crypto::P256PrivateKey::from_scalar(scalar)) {
        (void)crypto::make_credentials(*key, ordered.has_value() ? *ordered : *parsed);
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    const auto* bytes = reinterpret_cast<const std::byte*>(data);
    const std::span<const std::byte> payload{bytes + 1, size - 1};
    switch (data[0] % 7) {
        case 0:
            thrift(payload);
            break;
        case 1:
            der(payload);
            break;
        case 2:
            text(payload);
            break;
        case 3:
            blob(payload);
            break;
        case 4:
            verify(payload);
            break;
        case 5:
            certificates(payload);
            break;
        default: {
            const std::string_view view(reinterpret_cast<const char*>(payload.data()),
                                        payload.size());
            if (const auto decoded = crypto::base64_decode(view)) {
                blob(*decoded);
                verify(*decoded);
            }
            break;
        }
    }
    return 0;
}
