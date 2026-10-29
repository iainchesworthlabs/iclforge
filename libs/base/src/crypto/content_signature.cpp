#include "iclforge/base/crypto/content_signature.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "iclforge/base/crypto/chain.hpp"
#include "iclforge/base/crypto/credentials.hpp"
#include "iclforge/base/crypto/der.hpp"
#include "iclforge/base/crypto/ecdsa_p256.hpp"
#include "iclforge/base/crypto/sha256.hpp"
#include "iclforge/base/crypto/thrift_binary.hpp"
#include "iclforge/base/crypto/x509.hpp"

namespace iclforge::base::crypto {
namespace {

namespace tb = thrift;
using Bytes = std::vector<std::byte>;

// Every wire version field is 1 (CS spec v2.5 §3.16: "an implementation SHALL write wire version =
// 1"); "scheme version 3" (kSignatureValuesVersion3) names the scheme's release and is not a wire
// field. A different marker is a structure this reader has not been given.
constexpr std::int32_t kSupportedVersion = 1;

std::unexpected<SignatureFault> fault(SignatureError code, std::string message) {
    return std::unexpected(SignatureFault{code, std::move(message)});
}

SignatureError error_for(ChainFailure failure) {
    switch (failure) {
        case ChainFailure::kLeafKeyUsage:
        case ChainFailure::kLeafTitle:
            return SignatureError::kCertNotPrivilegedToSignContent;
        case ChainFailure::kUnsupportedAlgorithm:
            return SignatureError::kLibDoesntSupportContentSignature;
        default:
            return SignatureError::kBadCredentialsForSigningContent;
    }
}

// ---- Writing ------------------------------------------------------------------------------

void write_digest(tb::Writer& w, std::string_view reference, std::string_view canonical_method,
                  std::span<const std::byte> value) {
    w.field_i32(1, kSupportedVersion);
    w.field_i32(2, kSupportedVersion);
    w.field_string(21, reference);
    w.field_struct(31);
    w.field_string(1, canonical_method);
    w.field_string(2, "sha256");
    w.stop();
    w.field_byte_list(32, value);
    w.stop();
}

// The DER length of a value, as X.690 §8.1.3 writes it.
void append_der_length(Bytes& out, std::size_t length) {
    if (length < 0x80) {
        out.push_back(static_cast<std::byte>(length));
        return;
    }
    std::array<std::byte, 8> digits{};
    std::size_t n = 0;
    while (length != 0) {
        digits[n++] = static_cast<std::byte>(length & 0xFFU);
        length >>= 8;
    }
    out.push_back(static_cast<std::byte>(0x80U | n));
    for (std::size_t i = n; i > 0; --i) {
        out.push_back(digits[i - 1]);
    }
}

// CertificateChainWire field 4 as the measured blob has it: a list<byte> that opens with an ASN.1
// application-class tag 0 and a DER length (`40 82 LL LL` for a few kilobytes) around the
// certificates in DER, root first.
Bytes certificates_field(std::span<const Certificate> chain) {
    Bytes inner;
    for (const Certificate& cert : chain) {
        inner.insert(inner.end(), cert.der.begin(), cert.der.end());
    }
    Bytes out;
    out.push_back(std::byte{0x40});
    append_der_length(out, inner.size());
    out.insert(out.end(), inner.begin(), inner.end());
    return out;
}

Bytes make_nonce(const SignOptions& options) {
    if (!options.nonce.empty()) {
        return options.nonce;
    }
    // CS spec v2.5 §5.3: fresh randomness from a CSPRNG for every signature. std::random_device is
    // the platform's entropy source on the toolchains this builds with (hardware or the operating
    // system's generator under libstdc++, libc++ and MSVC), never a fixed seed.
    std::random_device device;
    Bytes nonce;
    nonce.reserve(kSignerNonceBytes);
    while (nonce.size() < kSignerNonceBytes) {
        std::uint32_t word = device();
        for (int i = 0; i < 4 && nonce.size() < kSignerNonceBytes; ++i) {
            nonce.push_back(static_cast<std::byte>(word & 0xFFU));
            word >>= 8;
        }
    }
    return nonce;
}

// SignerInfoWire (CS spec v2.5 §3.2 to §3.7), fields in ascending id order.
Bytes write_signer_info(std::span<const std::byte> nonce, const SignerMetadata& m,
                        std::string_view signed_date, std::span<const Certificate> chain) {
    tb::Writer w;
    w.field_byte_list(1, nonce);
    w.field_i32(2, kSupportedVersion);
    w.field_i32(3, kSupportedVersion);
    w.field_struct(4);  // PublisherWire
    w.field_i32(1, m.publisher_type);
    w.field_string(2, m.publisher_uuid);
    w.field_string(3, m.publisher_name);
    w.field_string(4, m.publisher_role);
    w.stop();
    w.field_struct(5);  // ProductWire
    w.field_i32(1, m.product_id);
    w.field_string(2, m.product_name);
    w.field_string(3, m.wrapper_name);
    w.field_string(4, m.product_role);
    w.stop();
    w.field_struct(6);  // PlatformWire
    w.field_string(1, m.platform_id);
    w.field_string(2, m.os_name);
    w.field_string(3, m.os_version);
    w.field_string(4, m.os_description);
    w.stop();
    w.field_struct(7);  // ApplicationWire; ApplicationInfoWire has no field 2
    w.field_struct(1);
    w.field_i32(1, m.application_id);
    w.field_string(3, m.application_name);
    w.field_string(4, m.application_description);
    w.stop();
    w.field_string(2, m.copyright);
    w.field_string(3, m.application_version);
    w.field_string(4, m.build_hash);
    w.stop();
    w.field_string(8, signed_date);
    w.field_struct(10);  // CertificateChainWire
    w.field_i32(1, kSupportedVersion);
    w.field_string(2, kSigningMethod);
    w.field_string(3, "der");
    w.field_byte_list(4, certificates_field(chain));
    w.stop();
    w.stop();
    return w.take();
}

// SignedInfoWire (§3.9): the struct the ECDSA signature is over.
Bytes write_signed_info(std::span<const std::byte> signer_digest,
                        std::span<const std::byte> content_digest) {
    tb::Writer w;
    w.field_i32(1, kSupportedVersion);
    w.field_i32(2, kSupportedVersion);
    w.field_string(3, kContentSignatureUuid);
    w.field_struct(4);  // CanonicalMethodWire
    w.field_string(1, kCanonicalThriftBinary);
    w.field_string(2, "sha256");
    w.stop();
    w.field_struct(5);  // SignatureMethodWire
    w.field_string(1, "ecdsa");
    w.field_string(2, "secp256r1");
    w.stop();
    w.field_struct(6);  // signerInfoDigest
    write_digest(w, kSignerInfoReference, kCanonicalThriftBinary, signer_digest);
    w.field(tb::Type::kList, 7);  // contentDigests
    w.list_header(tb::Type::kStruct, 1);
    write_digest(w, kContentReference, kCanonicalRaw, content_digest);
    w.stop();
    return w.take();
}

}  // namespace

const char* to_string(SignatureError error) noexcept {
    switch (error) {
        case SignatureError::kBadOrMissingSignature:
            return "BadOrMissingSignature";
        case SignatureError::kBadContentSignaturePkStandard:
            return "BadContentSignaturePKStandard";
        case SignatureError::kBadContentSignatureDate:
            return "BadContentSignatureDate";
        case SignatureError::kCertNotPrivilegedToSignContent:
            return "CertNotPrivilegedToSignContent";
        case SignatureError::kBadCredentialsForSigningContent:
            return "BadCredentialsForSigningContent";
        case SignatureError::kLibDoesntSupportContentSignature:
            return "LibDoesntSupportContentSignature";
        case SignatureError::kCertifierNoPrivateKey:
            return "CertifierNoPrivateKey";
        case SignatureError::kDigestMismatch:
            return "DigestMismatch";
        case SignatureError::kSignatureInvalid:
            return "SignatureInvalid";
    }
    return "Unknown";
}

bool is_uuid(std::string_view text) noexcept {
    if (text.size() != 36) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') {
                return false;
            }
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

namespace {

bool equal_ignoring_case(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](char c) {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
        };
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

}  // namespace

PublisherBindingResult check_publisher_binding(std::string_view publisher_uuid,
                                               const Certificate& signer) {
    PublisherBindingResult result;
    if (!is_uuid(publisher_uuid)) {
        result.binding = PublisherBinding::kNotAUuid;
        result.detail =
            publisher_uuid.empty()
                ? "publisherUUID is empty; CS spec v2.5 section 5.3 asks for the UUID of the "
                  "operator's publisher account"
                : "publisherUUID \"" + escape_text(publisher_uuid) + "\" is not a UUID";
        return result;
    }
    std::string other;
    for (const std::string& value : signer.subject.values) {
        if (!is_uuid(value)) {
            continue;
        }
        if (equal_ignoring_case(value, publisher_uuid)) {
            result.binding = PublisherBinding::kMatches;
            result.detail = "publisher UUID " + escape_text(publisher_uuid) +
                            " is the one the signer certificate's subject carries";
            return result;
        }
        if (other.empty()) {
            other = value;
        }
    }
    if (!other.empty()) {
        result.binding = PublisherBinding::kMismatch;
        result.detail = "the signature names publisher UUID " + escape_text(publisher_uuid) +
                        " and the signer certificate's subject carries UUID " + other;
        return result;
    }
    result.binding = PublisherBinding::kUnbound;
    result.detail = "publisher UUID " + escape_text(publisher_uuid) +
                    " was not compared: the signer certificate's subject carries no UUID, and CS "
                    "spec v2.5 section 5.3 does not say where a certificate binds one";
    return result;
}

std::expected<Bytes, SignatureFault> sign_content_digest(
    std::span<const std::byte, 32> content_digest, const SigningCredentials& credentials,
    const SignOptions& options) {
    if (!credentials.key.valid() || credentials.chain.empty()) {
        return fault(SignatureError::kCertifierNoPrivateKey,
                     "signing needs a private key and the certificate chain that vouches for it");
    }
    const auto derived = p256_public_key(credentials.key);
    if (!derived.has_value() || !(*derived == credentials.chain.back().public_key)) {
        return fault(SignatureError::kBadCredentialsForSigningContent,
                     "the private key does not belong to the leaf certificate (" +
                         credentials.chain.back().subject.text + ")");
    }
    if (!parse_iso8601_utc(options.signed_date).has_value()) {
        return fault(SignatureError::kBadContentSignatureDate,
                     "signed date \"" + options.signed_date + "\" is not YYYY-MM-DDTHH:MM:SS");
    }
    if (!options.nonce.empty() && options.nonce.size() != kSignerNonceBytes) {
        return fault(SignatureError::kBadOrMissingSignature,
                     "the signer nonce is " + std::to_string(options.nonce.size()) +
                         " bytes; CS spec v2.5 section 5.3 asks for " +
                         std::to_string(kSignerNonceBytes));
    }
    // The UUID of the publisher the signature names, when one is named, must be a UUID and must not
    // contradict the signing certificate (CS spec v2.5 section 5.3). An empty one is written as it
    // is: whether that is acceptable is the operator's, and the verifier's, to say.
    if (!options.metadata.publisher_uuid.empty()) {
        const PublisherBindingResult binding =
            check_publisher_binding(options.metadata.publisher_uuid, credentials.chain.back());
        if (binding.binding == PublisherBinding::kNotAUuid) {
            return fault(SignatureError::kBadOrMissingSignature, binding.detail);
        }
        if (binding.binding == PublisherBinding::kMismatch) {
            return fault(SignatureError::kBadCredentialsForSigningContent, binding.detail);
        }
    }
    const ChainReport chain = validate_chain(credentials.chain, options.chain_policy);
    if (!chain.ok()) {
        return fault(error_for(chain.failure), chain.detail);
    }

    // The chain's own order matters to a reader, so it is written root first as it was checked.
    const Bytes signer_info = write_signer_info(make_nonce(options), options.metadata,
                                                options.signed_date, credentials.chain);
    const std::array<std::byte, 32> signer_digest = sha256(signer_info);
    const Bytes signed_info = write_signed_info(signer_digest, content_digest);
    const auto signature = ecdsa_p256_sign(credentials.key, sha256(signed_info));
    if (!signature.has_value()) {
        return fault(SignatureError::kCertifierNoPrivateKey, "the private key is not usable");
    }

    tb::Writer signature_data;  // SignatureDataWire (§3.13)
    signature_data.field_i32(1, kSupportedVersion);
    signature_data.field_i32(2, kSupportedVersion);
    signature_data.field_string(3, kContentSignatureUuid);
    signature_data.field(tb::Type::kMap, 4);
    signature_data.map_header(tb::Type::kString, tb::Type::kList, 2);
    signature_data.string("ecc_r_value");
    signature_data.byte_list(signature->r);
    signature_data.string("ecc_s_value");
    signature_data.byte_list(signature->s);
    signature_data.stop();

    tb::Writer entry;  // SignatureEntryWire (§3.8)
    entry.field_struct(1);
    entry.raw(signed_info);
    entry.field_struct(2);
    entry.raw(signature_data.bytes());
    entry.stop();

    tb::Writer top;  // ContentSignature (§3.1)
    top.field_i32(1, kSupportedVersion);
    top.field_i32(2, kSupportedVersion);
    top.field_struct(3);
    top.raw(signer_info);
    top.field(tb::Type::kList, 4);
    top.list_header(tb::Type::kStruct, 1);
    top.raw(entry.bytes());
    top.stop();
    return top.take();
}

std::expected<Bytes, SignatureFault> sign_content(std::span<const std::byte> content,
                                                  const SigningCredentials& credentials,
                                                  const SignOptions& options) {
    return sign_content_digest(sha256(content), credentials, options);
}

// ---- Reading ------------------------------------------------------------------------------

namespace {

std::string string_of(std::span<const tb::Field> fields, std::int16_t id) {
    const tb::Field* f = tb::find(fields, id, tb::Type::kString);
    if (f == nullptr) {
        return {};
    }
    return tb::as_string(*f).value_or(std::string());
}

std::int32_t i32_of(std::span<const tb::Field> fields, std::int16_t id) {
    const tb::Field* f = tb::find(fields, id, tb::Type::kI32);
    if (f == nullptr) {
        return 0;
    }
    return tb::as_i32(*f).value_or(0);
}

std::optional<std::vector<tb::Field>> struct_of(std::span<const tb::Field> fields,
                                                std::int16_t id) {
    const tb::Field* f = tb::find(fields, id, tb::Type::kStruct);
    if (f == nullptr) {
        return std::nullopt;
    }
    return tb::as_struct(*f);
}

std::size_t offset_in(std::span<const std::byte> blob, std::span<const std::byte> part) {
    return static_cast<std::size_t>(part.data() - blob.data());
}

std::optional<DigestEntry> read_digest(std::span<const std::byte> struct_bytes) {
    const auto fields = tb::read_struct(struct_bytes);
    if (!fields) {
        return std::nullopt;
    }
    DigestEntry d;
    d.version = i32_of(*fields, 1);
    d.schema_version = i32_of(*fields, 2);
    d.reference = string_of(*fields, 21);
    if (const auto method = struct_of(*fields, 31)) {
        d.canonical_method = string_of(*method, 1);
        d.hash_algorithm = string_of(*method, 2);
    }
    if (const tb::Field* value = tb::find(*fields, 32, tb::Type::kList)) {
        if (const auto bytes = tb::as_byte_list(*value)) {
            d.value.assign(bytes->begin(), bytes->end());
        }
    }
    return d;
}

// A signature names one content digest (and the signer-information digest beside it); a few more
// are read, and a list longer than that is refused before it is copied.
constexpr std::size_t kMaxContentDigests = 8;

// CertificateChainWire.certificates -> the certificates, or why not.
std::vector<Certificate> read_chain(std::span<const std::byte> field, std::string& error) {
    std::vector<Certificate> chain;
    std::span<const std::byte> body = field;
    if (!body.empty() && body[0] == std::byte{0x40}) {
        std::span<const std::byte> in = body;
        const auto wrapper = der::read(in);
        if (!wrapper || !in.empty()) {
            error = "the certificate list's wrapper is damaged";
            return chain;
        }
        body = wrapper->content;
    }
    while (!body.empty()) {
        if (chain.size() == kMaxChainLength) {
            error = "the certificate list holds more than " + std::to_string(kMaxChainLength) +
                    " certificates";
            chain.clear();
            return chain;
        }
        const auto cert = der::read_expecting(body, der::kSequence);
        if (!cert) {
            error = "the certificate list does not hold DER certificates";
            chain.clear();
            return chain;
        }
        auto parsed = parse_certificate(cert->encoded);
        if (!parsed.has_value()) {
            error =
                "certificate " + std::to_string(chain.size()) + ": " + to_string(parsed.error());
            chain.clear();
            return chain;
        }
        chain.push_back(std::move(*parsed));
    }
    if (chain.empty()) {
        error = "the certificate list is empty";
    }
    return chain;
}

}  // namespace

std::expected<ParsedContentSignature, SignatureFault> parse_content_signature(
    std::span<const std::byte> blob) {
    const auto bad = [](std::string message) {
        return fault(SignatureError::kBadOrMissingSignature, std::move(message));
    };
    if (blob.empty()) {
        return bad("there is no content signature");
    }
    ParsedContentSignature out;
    out.blob.assign(blob.begin(), blob.end());
    const std::span<const std::byte> b = out.blob;

    std::size_t consumed = 0;
    const auto top = tb::read_struct(b, &consumed);
    if (!top) {
        return bad("the content signature is not a well-formed Thrift structure");
    }
    if (consumed != b.size()) {
        return bad("there are bytes after the content signature structure");
    }
    out.version = i32_of(*top, 1);
    out.schema_version = i32_of(*top, 2);

    const tb::Field* signer_field = tb::find(*top, 3, tb::Type::kStruct);
    const tb::Field* values_field = tb::find(*top, 4, tb::Type::kList);
    if (signer_field == nullptr || values_field == nullptr) {
        return bad("the content signature lacks its signer information or its signature values");
    }
    out.signer_info_offset = offset_in(b, signer_field->value);
    out.signer_info_size = signer_field->value.size();
    const auto signer = tb::as_struct(*signer_field);
    if (!signer) {
        return bad("the signer information is damaged");
    }
    if (const tb::Field* nonce = tb::find(*signer, 1, tb::Type::kList)) {
        if (const auto bytes = tb::as_byte_list(*nonce)) {
            out.signer_nonce.assign(bytes->begin(), bytes->end());
        }
    }
    out.signer_version = i32_of(*signer, 2);
    out.signer_schema_version = i32_of(*signer, 3);
    SignerMetadata& m = out.signer;
    if (const auto publisher = struct_of(*signer, 4)) {
        m.publisher_type = i32_of(*publisher, 1);
        m.publisher_uuid = string_of(*publisher, 2);
        m.publisher_name = string_of(*publisher, 3);
        m.publisher_role = string_of(*publisher, 4);
    }
    if (const auto product = struct_of(*signer, 5)) {
        m.product_id = i32_of(*product, 1);
        m.product_name = string_of(*product, 2);
        m.wrapper_name = string_of(*product, 3);
        m.product_role = string_of(*product, 4);
    }
    if (const auto platform = struct_of(*signer, 6)) {
        m.platform_id = string_of(*platform, 1);
        m.os_name = string_of(*platform, 2);
        m.os_version = string_of(*platform, 3);
        m.os_description = string_of(*platform, 4);
    }
    if (const auto application = struct_of(*signer, 7)) {
        if (const auto info = struct_of(*application, 1)) {
            m.application_id = i32_of(*info, 1);
            m.application_name = string_of(*info, 3);
            m.application_description = string_of(*info, 4);
        }
        m.copyright = string_of(*application, 2);
        m.application_version = string_of(*application, 3);
        m.build_hash = string_of(*application, 4);
    }
    out.signed_date = string_of(*signer, 8);
    const auto chain_info = struct_of(*signer, 10);
    if (!chain_info) {
        return bad("the signer information carries no certificate chain");
    }
    out.chain_format_version = i32_of(*chain_info, 1);
    out.signing_method = string_of(*chain_info, 2);
    out.encoding_format = string_of(*chain_info, 3);
    if (const tb::Field* certs = tb::find(*chain_info, 4, tb::Type::kList)) {
        if (const auto bytes = tb::as_byte_list(*certs)) {
            out.chain = read_chain(*bytes, out.chain_error);
        } else {
            out.chain_error = "the certificates are not a list of bytes";
        }
    } else {
        out.chain_error = "the certificate chain carries no certificates";
    }

    // Exactly one signature entry: this verifier checks one, so a blob that carries another is
    // refused and not read as if the first stood for the whole (libs/ac4/ERRATA.md, open question
    // 6). The count is read from the list header, before any element is.
    const std::uint32_t entry_count =
        values_field->value.size() >= 5
            ? (std::to_integer<std::uint32_t>(values_field->value[1]) << 24) |
                  (std::to_integer<std::uint32_t>(values_field->value[2]) << 16) |
                  (std::to_integer<std::uint32_t>(values_field->value[3]) << 8) |
                  std::to_integer<std::uint32_t>(values_field->value[4])
            : 0U;
    if (entry_count != 1) {
        return bad(entry_count == 0 ? "the content signature holds no signature entry"
                                    : "the content signature holds " + std::to_string(entry_count) +
                                          " signature entries and exactly one is accepted");
    }
    const auto entries = tb::as_struct_list(*values_field);
    if (!entries || entries->size() != 1) {
        return bad("the signature entries are damaged");
    }
    const auto entry = tb::read_struct(entries->front());
    if (!entry) {
        return bad("the signature entry is damaged");
    }
    const tb::Field* signed_info_field = tb::find(*entry, 1, tb::Type::kStruct);
    const tb::Field* signature_data_field = tb::find(*entry, 2, tb::Type::kStruct);
    if (signed_info_field == nullptr || signature_data_field == nullptr) {
        return bad("the signature entry lacks its signed information or its signature");
    }
    out.signed_info_offset = offset_in(b, signed_info_field->value);
    out.signed_info_size = signed_info_field->value.size();
    const auto signed_info = tb::as_struct(*signed_info_field);
    const auto signature_data = tb::as_struct(*signature_data_field);
    if (!signed_info || !signature_data) {
        return bad("the signed information or the signature is damaged");
    }
    out.canonical_version = i32_of(*signed_info, 1);
    out.signed_info_version = i32_of(*signed_info, 2);
    out.signature_id = string_of(*signed_info, 3);
    if (const auto canonical = struct_of(*signed_info, 4)) {
        out.canonical_method = string_of(*canonical, 1);
        out.canonical_hash = string_of(*canonical, 2);
    }
    if (const auto method = struct_of(*signed_info, 5)) {
        out.signature_algorithm = string_of(*method, 1);
        out.signature_curve = string_of(*method, 2);
    }
    const tb::Field* signer_digest = tb::find(*signed_info, 6, tb::Type::kStruct);
    if (signer_digest == nullptr) {
        return bad("the signed information carries no signer-information digest");
    }
    const auto signer_entry = read_digest(signer_digest->value);
    if (!signer_entry) {
        return bad("the signer-information digest is damaged");
    }
    out.signer_info_digest = *signer_entry;
    if (const tb::Field* contents = tb::find(*signed_info, 7, tb::Type::kList)) {
        const auto list = tb::as_struct_list(*contents);
        if (!list) {
            return bad("the content digests are damaged");
        }
        if (list->size() > kMaxContentDigests) {
            return bad("the signed information lists " + std::to_string(list->size()) +
                       " content digests and at most " + std::to_string(kMaxContentDigests) +
                       " are accepted");
        }
        for (const auto& bytes : *list) {
            const auto digest = read_digest(bytes);
            if (!digest) {
                return bad("a content digest is damaged");
            }
            out.content_digests.push_back(*digest);
        }
    }
    out.signature_data_version = i32_of(*signature_data, 1);
    out.signature_type = i32_of(*signature_data, 2);
    out.signature_data_id = string_of(*signature_data, 3);
    const tb::Field* map = tb::find(*signature_data, 4, tb::Type::kMap);
    if (map == nullptr) {
        return bad("the signature carries no r and s values");
    }
    const auto values = tb::as_string_to_byte_list_map(*map);
    if (!values) {
        return bad("the signature values are damaged");
    }
    bool have_r = false;
    bool have_s = false;
    for (const tb::ByteListEntry& e : *values) {
        if (e.value.size() != 32) {
            return bad("the signature value \"" + escape_text(e.key) + "\" is not 32 bytes");
        }
        if (e.key == "ecc_r_value" && !have_r) {
            std::copy(e.value.begin(), e.value.end(), out.signature.r.begin());
            have_r = true;
        } else if (e.key == "ecc_s_value" && !have_s) {
            std::copy(e.value.begin(), e.value.end(), out.signature.s.begin());
            have_s = true;
        }
    }
    if (!have_r || !have_s) {
        return bad("the signature lacks ecc_r_value or ecc_s_value");
    }
    return out;
}

// ---- Verifying ----------------------------------------------------------------------------

namespace {

bool bytes_equal(std::span<const std::byte> a, std::span<const std::byte> b) {
    if (a.size() != b.size()) {
        return false;
    }
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff = static_cast<std::uint8_t>(diff | std::to_integer<std::uint8_t>(a[i] ^ b[i]));
    }
    return diff == 0;
}

// What the signer says about itself, as lines a person reads: each part escaped, the empty ones
// left out. Checked no further than the findings on the nonce and the publisher UUID.
void fill_claims(VerifyReport& report, const SignerMetadata& m) {
    const auto join = [](std::initializer_list<std::string> parts) {
        std::string out;
        for (const std::string& part : parts) {
            if (!part.empty()) {
                out += out.empty() ? "" : " ";
                out += part;
            }
        }
        return out;
    };
    const auto text = [](std::string_view value) { return escape_text(value); };
    const auto labelled = [&](std::string_view label, std::string_view value) {
        return value.empty() ? std::string() : std::string(label) + escape_text(value) + ")";
    };
    report.publisher = join({text(m.publisher_name), labelled("(UUID ", m.publisher_uuid)});
    report.product = join({text(m.product_name), labelled("(wrapper ", m.wrapper_name)});
    report.platform = join({text(m.os_name), text(m.os_version), labelled("(", m.os_description)});
    report.application = join(
        {text(m.application_name), text(m.application_version), labelled("(build ", m.build_hash)});
}

const DigestEntry* find_content_digest(const ParsedContentSignature& p) {
    for (const DigestEntry& d : p.content_digests) {
        if (d.reference == kContentReference) {
            return &d;
        }
    }
    return nullptr;
}

}  // namespace

VerifyReport verify_content_signature(std::span<const std::byte> blob,
                                      std::span<const std::byte, 32> content_digest,
                                      const VerifyPolicy& policy) {
    VerifyReport report;
    const auto record = [&report](std::string name, bool ok, std::string detail,
                                  SignatureError code) {
        if (!ok && !report.fault.has_value()) {
            report.fault = SignatureFault{code, detail};
        }
        report.checks.push_back(CheckResult{std::move(name), ok, std::move(detail)});
    };

    auto parsed = parse_content_signature(blob);
    if (!parsed.has_value()) {
        record("structure", false, parsed.error().message, parsed.error().code);
        return report;
    }
    const ParsedContentSignature& p = *parsed;
    record("structure", true, "the Thrift structure of CS spec v2.5 §3 reads",
           SignatureError::kBadOrMissingSignature);
    report.signed_date = escape_text(p.signed_date);
    fill_claims(report, p.signer);

    // Versions, scheme and methods.
    const bool versions_ok =
        p.version == kSupportedVersion && p.schema_version == kSupportedVersion &&
        p.signer_version == kSupportedVersion && p.signer_schema_version == kSupportedVersion &&
        p.chain_format_version == kSupportedVersion && p.canonical_version == kSupportedVersion &&
        p.signed_info_version == kSupportedVersion &&
        p.signature_data_version == kSupportedVersion && p.signature_type == 1 &&
        p.signer_info_digest.version == kSupportedVersion &&
        p.signer_info_digest.schema_version == kSupportedVersion;
    record("version markers", versions_ok,
           versions_ok ? "all version markers are 1"
                       : "a version marker is not 1, the only one this reader knows",
           SignatureError::kLibDoesntSupportContentSignature);
    const bool scheme_ok =
        p.signing_method == kSigningMethod && p.encoding_format == "der" &&
        p.canonical_method == kCanonicalThriftBinary && p.canonical_hash == "sha256" &&
        p.signature_id == kContentSignatureUuid && p.signature_data_id == p.signature_id;
    record("scheme", scheme_ok,
           scheme_ok
               ? "pace-ccjf1, DER certificates, Thrift-canonical signed information"
               : "the signing method \"" + escape_text(p.signing_method) + "\", encoding \"" +
                     escape_text(p.encoding_format) + "\" or canonical method \"" +
                     escape_text(p.canonical_method) + "\" is not the one this reader supports",
           SignatureError::kLibDoesntSupportContentSignature);
    const bool curve_ok = p.signature_algorithm == "ecdsa" && p.signature_curve == "secp256r1";
    record("signature method", curve_ok,
           curve_ok
               ? "ecdsa on secp256r1"
               : "the signature method is \"" + escape_text(p.signature_algorithm) + "\" on \"" +
                     escape_text(p.signature_curve) + "\", not ecdsa on secp256r1",
           SignatureError::kBadContentSignaturePkStandard);

    // The chain.
    const bool chain_read = p.chain_error.empty() && !p.chain.empty();
    record("certificates", chain_read,
           chain_read ? std::to_string(p.chain.size()) + " certificate(s) read" : p.chain_error,
           SignatureError::kBadCredentialsForSigningContent);

    const std::optional<std::int64_t> signed_epoch = parse_iso8601_utc(p.signed_date);
    ChainReport chain_report;
    if (chain_read) {
        report.signer = p.chain.back().subject.text;
        report.signer_serial = p.chain.back().serial_hex;
        ChainPolicy chain_policy;
        chain_policy.trust_anchors = policy.trust_anchors;
        chain_policy.pins = policy.pins;
        chain_policy.required_leaf_title = policy.required_leaf_title;
        chain_policy.require_digital_signature = policy.require_digital_signature;
        bool time_known = true;
        switch (policy.time_basis) {
            case TimeBasis::kNow:
                chain_policy.valid_at = policy.now;
                break;
            case TimeBasis::kSignedDate:
                time_known = signed_epoch.has_value();
                chain_policy.valid_at = signed_epoch;
                break;
            case TimeBasis::kInstant:
                chain_policy.valid_at = policy.instant;
                break;
            case TimeBasis::kIgnore:
                break;
        }
        if (chain_policy.valid_at.has_value()) {
            report.verified_at = *chain_policy.valid_at;
        }
        if (!time_known) {
            record("chain", false, "the signed date is needed to judge validity and is not a date",
                   SignatureError::kBadContentSignatureDate);
        } else {
            chain_report = validate_chain(p.chain, chain_policy);
            record("chain", chain_report.ok(),
                   chain_report.ok()
                       ? (chain_report.anchored
                              ? "well formed and anchored: " + chain_report.anchor_description
                              : std::string("well formed, not anchored"))
                       : chain_report.detail,
                   chain_report.ok() ? SignatureError::kBadCredentialsForSigningContent
                                     : error_for(chain_report.failure));
        }
    }

    // signedDate: a date, not after now (when judged against now), inside the leaf's validity.
    {
        bool ok = signed_epoch.has_value();
        std::string detail =
            ok ? escape_text(p.signed_date)
               : "\"" + escape_text(p.signed_date) + "\" is not YYYY-MM-DDTHH:MM:SS";
        if (ok && policy.time_basis == TimeBasis::kNow &&
            *signed_epoch > policy.now + policy.max_future_skew_seconds) {
            ok = false;
            detail = escape_text(p.signed_date) + " is in the future";
        }
        if (ok && chain_read && policy.time_basis != TimeBasis::kIgnore &&
            (*signed_epoch < p.chain.back().not_before ||
             *signed_epoch > p.chain.back().not_after)) {
            ok = false;
            detail = escape_text(p.signed_date) + " lies outside the signer certificate's validity";
        }
        record("signed date", ok, detail, SignatureError::kBadContentSignatureDate);
    }

    // The signature itself, under the leaf's key, over the SignedInfoWire bytes as received. CS
    // spec v2.5 §6.1: it comes before the digests it holds, which carry weight only once it
    // verifies.
    bool signature_ok = false;
    if (chain_read) {
        signature_ok = ecdsa_p256_verify(p.chain.back().public_key, sha256(p.signed_info_bytes()),
                                         p.signature);
        record("signature", signature_ok,
               signature_ok
                   ? "ECDSA over the signed information verifies under the signer certificate's key"
                   : "the ECDSA signature does not verify under the signer certificate's key",
               SignatureError::kSignatureInvalid);
    } else {
        record("signature", false, "no signer certificate to verify under",
               SignatureError::kBadCredentialsForSigningContent);
    }
    // What a digest check says when the signature under it did not verify: it still agrees or not
    // with the digest stored, but the stored digest is the signer's word and nothing more.
    const std::string unauthenticated =
        signature_ok ? std::string()
                     : " (the signature over this digest did not verify, so it authenticates "
                       "nothing)";

    // The two digests, against the bytes as received.
    {
        const std::array<std::byte, 32> actual = sha256(p.signer_info_bytes());
        const DigestEntry& stored = p.signer_info_digest;
        const bool shape = stored.reference == kSignerInfoReference &&
                           stored.canonical_method == kCanonicalThriftBinary &&
                           stored.hash_algorithm == "sha256" && stored.value.size() == 32;
        const bool ok = shape && bytes_equal(actual, stored.value);
        record("signer-information digest", ok,
               (ok ? "SHA-256 of the signer information matches"
                   : (shape ? "the SHA-256 of the signer information differs from the stored digest"
                            : "the signer-information digest entry is not the expected shape")) +
                   unauthenticated,
               shape ? SignatureError::kDigestMismatch : SignatureError::kBadOrMissingSignature);
    }
    {
        const DigestEntry* stored = find_content_digest(p);
        bool ok = false;
        std::string detail;
        SignatureError code = SignatureError::kBadOrMissingSignature;
        if (stored == nullptr) {
            detail = "no content digest entry (" + std::string(kContentReference) + ")";
        } else if (stored->canonical_method != kCanonicalRaw ||
                   stored->hash_algorithm != "sha256" || stored->value.size() != 32) {
            detail = "the content digest entry is not a raw SHA-256";
            code = SignatureError::kLibDoesntSupportContentSignature;
        } else if (!bytes_equal(content_digest, stored->value)) {
            detail = "the SHA-256 of the content differs from the digest the signature covers";
            code = SignatureError::kDigestMismatch;
        } else {
            ok = true;
            detail = "SHA-256 of the content matches";
        }
        record("content digest", ok, detail + unauthenticated, code);
    }

    // The operator metadata of CS spec v2.5 §5.3. The nonce is 128 bytes; the publisher UUID is a
    // UUID that the signer certificate does not contradict. The specification lets a verifier
    // reject on these and does not require it, so they are findings first (`notes`) and part of the
    // verdict only when the policy says so. That the nonce is fresh, and that the platform and
    // application fields are accurate, a verifier cannot know.
    {
        const bool require = policy.operator_metadata == OperatorMetadata::kRequire;
        const auto finding = [&](std::string name, bool ok, bool decisive, std::string detail,
                                 SignatureError code) {
            if (require && decisive) {
                record(std::move(name), ok, std::move(detail), code);
            } else {
                report.notes.push_back(CheckResult{std::move(name), ok, std::move(detail)});
            }
        };
        const bool nonce_ok = p.signer_nonce.size() == kSignerNonceBytes;
        finding("signer nonce", nonce_ok, true,
                nonce_ok ? std::to_string(kSignerNonceBytes) + " bytes"
                         : std::to_string(p.signer_nonce.size()) +
                               " bytes; CS spec v2.5 section 5.3 asks for " +
                               std::to_string(kSignerNonceBytes),
                SignatureError::kBadOrMissingSignature);
        if (chain_read) {
            const PublisherBindingResult binding =
                check_publisher_binding(p.signer.publisher_uuid, p.chain.back());
            finding("publisher UUID", binding.binding == PublisherBinding::kMatches,
                    binding.binding != PublisherBinding::kUnbound, binding.detail,
                    binding.binding == PublisherBinding::kMismatch
                        ? SignatureError::kBadCredentialsForSigningContent
                        : SignatureError::kBadOrMissingSignature);
        }
    }

    if (report.fault.has_value()) {
        report.verdict = Verdict::kInvalid;
        return report;
    }
    report.verdict = chain_report.anchored ? Verdict::kValid : Verdict::kChecksChainUnanchored;
    report.anchor = chain_report.anchor_description;
    return report;
}

}  // namespace iclforge::base::crypto
