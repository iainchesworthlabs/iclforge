#include "iclforge/base/crypto/content_signature.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "iclforge/base/crypto/ecdsa_der.hpp"
#include "iclforge/base/crypto/thrift_binary.hpp"
#include "iclforge/base/crypto/x509.hpp"

namespace iclforge::base::crypto {
namespace {

using thrift::FieldHeader;
using thrift::Reader;
using thrift::Type;
using thrift::Writer;

// --- The field ids ---------------------------------------------------------------------------
// The specification lists each structure's fields (section 3) and describes how an id is written
// (section 7.3) and gives no id (register item 1 of planning/ac4-content-signing.md). These are the
// numbers 1 to n in the order of the listing, which is what an IDL file with sequential ids would
// give. They are the only place the numbering lives: if a vendor reader shows otherwise, this
// table changes and nothing else does.
namespace id {
// ContentSignature
constexpr std::int16_t kCertificateChain = 1;
constexpr std::int16_t kSignerInfo = 2;
constexpr std::int16_t kSignatureValues = 3;
// CertificateChainWire
constexpr std::int16_t kChainStructure = 1;
constexpr std::int16_t kFormat = 2;
constexpr std::int16_t kChainPkStandardName = 3;
constexpr std::int16_t kCertificates = 4;
// SignerInfoWire
constexpr std::int16_t kSignerSerialNumber = 1;
constexpr std::int16_t kSignatureValue = 2;
// LsSignedInfoWire
constexpr std::int16_t kCanonicalVersion = 1;
constexpr std::int16_t kSignedDate = 2;
constexpr std::int16_t kDigests = 3;
// LsSignedDigestWire
constexpr std::int16_t kDigestValue = 1;
constexpr std::int16_t kReference = 2;
// SignatureValuesWire
constexpr std::int16_t kSignedInfo = 1;
constexpr std::int16_t kSignatureValueDetails = 2;
// SignatureValueDetailsWire
constexpr std::int16_t kDigestDataRef = 1;
constexpr std::int16_t kCanonicalMethod = 2;
constexpr std::int16_t kHash = 3;
constexpr std::int16_t kSignatureMethod = 4;
constexpr std::int16_t kDetailsPkStandardName = 5;
constexpr std::int16_t kDigestMethod = 6;
}  // namespace id

// --- Writing ---------------------------------------------------------------------------------

void write_chain(Writer& w, const CertificateChainWire& chain) {
    w.string_field(id::kChainStructure, chain.chain_structure);
    w.string_field(id::kFormat, chain.format);
    w.string_field(id::kChainPkStandardName, chain.pk_standard_name);
    w.list_field(id::kCertificates, Type::kString, chain.certificates.size());
    for (const auto& certificate : chain.certificates) {
        w.binary(certificate);
    }
    w.stop();
}

void write_signer(Writer& w, const SignerInfoWire& signer) {
    w.string_field(id::kSignerSerialNumber, signer.signer_serial_number);
    w.binary_field(id::kSignatureValue, signer.signature_value);
    w.stop();
}

void write_signed_info(Writer& w, const LsSignedInfoWire& info) {
    w.i32_field(id::kCanonicalVersion, info.canonical_version);
    w.string_field(id::kSignedDate, info.signed_date);
    w.list_field(id::kDigests, Type::kStruct, info.digests.size());
    for (const auto& digest : info.digests) {
        w.binary_field(id::kDigestValue, digest.digest_value);
        w.string_field(id::kReference, digest.reference);
        w.stop();
    }
    w.stop();
}

void write_details(Writer& w, const SignatureValueDetailsWire& details) {
    w.string_field(id::kDigestDataRef, details.digest_data_ref);
    w.string_field(id::kCanonicalMethod, details.canonical_method);
    w.string_field(id::kHash, details.hash);
    w.string_field(id::kSignatureMethod, details.signature_method);
    w.string_field(id::kDetailsPkStandardName, details.pk_standard_name);
    w.string_field(id::kDigestMethod, details.digest_method);
    w.stop();
}

void write_values(Writer& w, const SignatureValuesWire& values) {
    w.struct_field(id::kSignedInfo);
    write_signed_info(w, values.signed_info);
    w.struct_field(id::kSignatureValueDetails);
    write_details(w, values.signature_values);
    w.stop();
}

// --- Reading ---------------------------------------------------------------------------------

// Which of a structure's fields the reader has met: a bit per id from 1. A field met twice is an
// error (a second value for the same field is a way to show two readers two different payloads),
// and a field never met is an error too, because every field of the specification's structures is
// required.
class Seen {
   public:
    explicit Seen(unsigned fields) noexcept : all_((1u << fields) - 1u) {}
    // False when `field` (1-based) was already marked.
    [[nodiscard]] bool mark(std::int16_t field) noexcept {
        const unsigned bit = 1u << (static_cast<unsigned>(field) - 1u);
        if ((seen_ & bit) != 0) {
            return false;
        }
        seen_ |= bit;
        return true;
    }
    [[nodiscard]] bool complete() const noexcept { return seen_ == all_; }

   private:
    unsigned all_;
    unsigned seen_ = 0;
};

using ReadResult = std::expected<bool, thrift::Error>;

// The state of one parse: the reader, and the name of the innermost structure an error occurred
// in, which the failure message gives so that a reader of a log knows where a payload went wrong.
struct Parser {
    Reader reader;
    std::string_view where;

    thrift::Error fail(std::string_view structure, thrift::Error error) {
        if (where.empty()) {
            where = structure;
        }
        return error;
    }
    // A field the structure knows: the type must be `want`, and the id must not repeat.
    std::optional<thrift::Error> accept(std::string_view structure, const FieldHeader& header,
                                        Type want, Seen& seen) {
        if (header.type != want) {
            return fail(structure, thrift::Error::kTypeMismatch);
        }
        if (!seen.mark(header.id)) {
            return fail(structure, thrift::Error::kDuplicateField);
        }
        return std::nullopt;
    }

    // A string or binary field into `out`.
    template <class Out>
    ReadResult read_bytes_field(std::string_view structure, const FieldHeader& header, Seen& seen,
                                Out& out) {
        if (const auto error = accept(structure, header, Type::kString, seen)) {
            return std::unexpected(*error);
        }
        const auto bytes = reader.read_binary();
        if (!bytes) {
            return std::unexpected(fail(structure, bytes.error()));
        }
        if constexpr (std::is_same_v<Out, std::string>) {
            out.assign(reinterpret_cast<const char*>(bytes->data()), bytes->size());
        } else {
            out.assign(bytes->begin(), bytes->end());
        }
        return true;
    }

    std::expected<void, thrift::Error> finish(std::string_view structure,
                                              const std::expected<void, thrift::Error>& result,
                                              const Seen& seen) {
        if (!result) {
            return std::unexpected(fail(structure, result.error()));
        }
        if (!seen.complete()) {
            return std::unexpected(fail(structure, thrift::Error::kMissingField));
        }
        return {};
    }
};

std::expected<void, thrift::Error> parse_chain(Parser& p, CertificateChainWire& out) {
    constexpr std::string_view kName = "CertificateChainWire";
    Seen seen(4);
    const auto result = p.reader.read_struct([&](const FieldHeader& header) -> ReadResult {
        switch (header.id) {
            case id::kChainStructure:
                return p.read_bytes_field(kName, header, seen, out.chain_structure);
            case id::kFormat:
                return p.read_bytes_field(kName, header, seen, out.format);
            case id::kChainPkStandardName:
                return p.read_bytes_field(kName, header, seen, out.pk_standard_name);
            case id::kCertificates: {
                if (const auto error = p.accept(kName, header, Type::kList, seen)) {
                    return std::unexpected(*error);
                }
                const auto list = p.reader.read_list_header();
                if (!list) {
                    return std::unexpected(p.fail(kName, list.error()));
                }
                if (list->element != Type::kString) {
                    return std::unexpected(p.fail(kName, thrift::Error::kTypeMismatch));
                }
                out.certificates.clear();
                out.certificates.reserve(std::min<std::size_t>(list->count, kMaxChainLength));
                for (std::size_t i = 0; i < list->count; ++i) {
                    const auto bytes = p.reader.read_binary();
                    if (!bytes) {
                        return std::unexpected(p.fail(kName, bytes.error()));
                    }
                    out.certificates.emplace_back(bytes->begin(), bytes->end());
                }
                return true;
            }
            default:
                return false;
        }
    });
    return p.finish(kName, result, seen);
}

std::expected<void, thrift::Error> parse_signer(Parser& p, SignerInfoWire& out) {
    constexpr std::string_view kName = "SignerInfoWire";
    Seen seen(2);
    const auto result = p.reader.read_struct([&](const FieldHeader& header) -> ReadResult {
        switch (header.id) {
            case id::kSignerSerialNumber:
                return p.read_bytes_field(kName, header, seen, out.signer_serial_number);
            case id::kSignatureValue:
                return p.read_bytes_field(kName, header, seen, out.signature_value);
            default:
                return false;
        }
    });
    return p.finish(kName, result, seen);
}

std::expected<void, thrift::Error> parse_digest(Parser& p, LsSignedDigestWire& out) {
    constexpr std::string_view kName = "LsSignedDigestWire";
    Seen seen(2);
    const auto result = p.reader.read_struct([&](const FieldHeader& header) -> ReadResult {
        switch (header.id) {
            case id::kDigestValue:
                return p.read_bytes_field(kName, header, seen, out.digest_value);
            case id::kReference:
                return p.read_bytes_field(kName, header, seen, out.reference);
            default:
                return false;
        }
    });
    return p.finish(kName, result, seen);
}

std::expected<void, thrift::Error> parse_signed_info(Parser& p, LsSignedInfoWire& out) {
    constexpr std::string_view kName = "LsSignedInfoWire";
    Seen seen(3);
    const auto result = p.reader.read_struct([&](const FieldHeader& header) -> ReadResult {
        switch (header.id) {
            case id::kCanonicalVersion: {
                if (const auto error = p.accept(kName, header, Type::kI32, seen)) {
                    return std::unexpected(*error);
                }
                const auto value = p.reader.read_i32();
                if (!value) {
                    return std::unexpected(p.fail(kName, value.error()));
                }
                out.canonical_version = *value;
                return true;
            }
            case id::kSignedDate:
                return p.read_bytes_field(kName, header, seen, out.signed_date);
            case id::kDigests: {
                if (const auto error = p.accept(kName, header, Type::kList, seen)) {
                    return std::unexpected(*error);
                }
                const auto list = p.reader.read_list_header();
                if (!list) {
                    return std::unexpected(p.fail(kName, list.error()));
                }
                if (list->element != Type::kStruct) {
                    return std::unexpected(p.fail(kName, thrift::Error::kTypeMismatch));
                }
                out.digests.clear();
                out.digests.reserve(std::min<std::size_t>(list->count, 16));
                for (std::size_t i = 0; i < list->count; ++i) {
                    LsSignedDigestWire digest;
                    if (const auto parsed = parse_digest(p, digest); !parsed) {
                        return std::unexpected(parsed.error());
                    }
                    out.digests.push_back(std::move(digest));
                }
                return true;
            }
            default:
                return false;
        }
    });
    return p.finish(kName, result, seen);
}

std::expected<void, thrift::Error> parse_details(Parser& p, SignatureValueDetailsWire& out) {
    constexpr std::string_view kName = "SignatureValueDetailsWire";
    Seen seen(6);
    const auto result = p.reader.read_struct([&](const FieldHeader& header) -> ReadResult {
        switch (header.id) {
            case id::kDigestDataRef:
                return p.read_bytes_field(kName, header, seen, out.digest_data_ref);
            case id::kCanonicalMethod:
                return p.read_bytes_field(kName, header, seen, out.canonical_method);
            case id::kHash:
                return p.read_bytes_field(kName, header, seen, out.hash);
            case id::kSignatureMethod:
                return p.read_bytes_field(kName, header, seen, out.signature_method);
            case id::kDetailsPkStandardName:
                return p.read_bytes_field(kName, header, seen, out.pk_standard_name);
            case id::kDigestMethod:
                return p.read_bytes_field(kName, header, seen, out.digest_method);
            default:
                return false;
        }
    });
    return p.finish(kName, result, seen);
}

std::expected<void, thrift::Error> parse_values(Parser& p, SignatureValuesWire& out) {
    constexpr std::string_view kName = "SignatureValuesWire";
    Seen seen(2);
    const auto result = p.reader.read_struct([&](const FieldHeader& header) -> ReadResult {
        switch (header.id) {
            case id::kSignedInfo: {
                if (const auto error = p.accept(kName, header, Type::kStruct, seen)) {
                    return std::unexpected(*error);
                }
                if (const auto parsed = parse_signed_info(p, out.signed_info); !parsed) {
                    return std::unexpected(parsed.error());
                }
                return true;
            }
            case id::kSignatureValueDetails: {
                if (const auto error = p.accept(kName, header, Type::kStruct, seen)) {
                    return std::unexpected(*error);
                }
                if (const auto parsed = parse_details(p, out.signature_values); !parsed) {
                    return std::unexpected(parsed.error());
                }
                return true;
            }
            default:
                return false;
        }
    });
    return p.finish(kName, result, seen);
}

std::expected<void, thrift::Error> parse_top(Parser& p, ContentSignature& out) {
    constexpr std::string_view kName = "ContentSignature";
    Seen seen(3);
    const auto result = p.reader.read_struct([&](const FieldHeader& header) -> ReadResult {
        switch (header.id) {
            case id::kCertificateChain: {
                if (const auto error = p.accept(kName, header, Type::kStruct, seen)) {
                    return std::unexpected(*error);
                }
                if (const auto parsed = parse_chain(p, out.certificate_chain); !parsed) {
                    return std::unexpected(parsed.error());
                }
                return true;
            }
            case id::kSignerInfo: {
                if (const auto error = p.accept(kName, header, Type::kStruct, seen)) {
                    return std::unexpected(*error);
                }
                if (const auto parsed = parse_signer(p, out.signer_info); !parsed) {
                    return std::unexpected(parsed.error());
                }
                return true;
            }
            case id::kSignatureValues: {
                if (const auto error = p.accept(kName, header, Type::kStruct, seen)) {
                    return std::unexpected(*error);
                }
                if (const auto parsed = parse_values(p, out.signature_values); !parsed) {
                    return std::unexpected(parsed.error());
                }
                return true;
            }
            default:
                return false;
        }
    });
    if (!result) {
        return std::unexpected(p.fail(kName, result.error()));
    }
    if (!seen.complete()) {
        return std::unexpected(p.fail(kName, thrift::Error::kMissingField));
    }
    if (const auto end = p.reader.expect_end(); !end) {
        return std::unexpected(p.fail(kName, end.error()));
    }
    return {};
}

// --- Small helpers -----------------------------------------------------------------------------

// A string from a payload, made safe to put in a message: printable ASCII only, and short.
std::string printable(std::string_view text) {
    constexpr std::size_t kLimit = 48;
    std::string out;
    for (const char c : text) {
        if (out.size() == kLimit) {
            out += "...";
            break;
        }
        out.push_back(c >= 0x20 && c < 0x7F ? c : '?');
    }
    return out;
}

ContentSignatureFailure failure(ContentSignatureError error, std::string detail) {
    ContentSignatureFailure f;
    f.error = error;
    f.detail = std::move(detail);
    return f;
}

std::expected<ContentSignature, ContentSignatureFailure> bad_signature(std::string detail) {
    return std::unexpected(
        failure(ContentSignatureError::kBadOrMissingSignature, std::move(detail)));
}

// Hexadecimal serial text in a form two spellings of one number share: uppercase, no leading
// zeros ("01" and "1" are the same serial; "0" stays "0"). nullopt for text that is not
// hexadecimal.
std::optional<std::string> normalise_serial(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c >= '0' && c <= '9') {
            out.push_back(c);
        } else if (c >= 'A' && c <= 'F') {
            out.push_back(c);
        } else if (c >= 'a' && c <= 'f') {
            out.push_back(static_cast<char>(c - 'a' + 'A'));
        } else {
            return std::nullopt;
        }
    }
    if (out.empty()) {
        return std::nullopt;
    }
    const auto first = out.find_first_not_of('0');
    return first == std::string::npos ? std::string{"0"} : out.substr(first);
}

bool same_serial(std::string_view a, std::string_view b) {
    const auto na = normalise_serial(a);
    const auto nb = normalise_serial(b);
    return na.has_value() && nb.has_value() && *na == *nb;
}

bool bytes_equal(std::span<const std::byte> a, std::span<const std::byte> b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

std::string chain_failure_text(const ChainFailure& failure, std::size_t chain_size) {
    std::string text = std::string{describe(failure.error)};
    switch (failure.error) {
        case ChainError::kBrokenLink:
        case ChainError::kBadSignature:
        case ChainError::kNotCa:
        case ChainError::kNotYetValid:
        case ChainError::kExpired:
            text += " (certificate " + std::to_string(failure.index + 1) + " of " +
                    std::to_string(chain_size) + ", counting from the root end)";
            break;
        default:
            break;
    }
    return text;
}

ContentSignatureError map_chain_error(ChainError error) {
    switch (error) {
        case ChainError::kNoClock:
            return ContentSignatureError::kNoClock;
        case ChainError::kNoTrust:
            return ContentSignatureError::kNoTrustAnchors;
        default:
            return ContentSignatureError::kBadCredentialsForSigningContent;
    }
}

// Latest time the content signature's date can name: 9999-12-31T23:59:59Z.
constexpr std::int64_t kLatestSignedUnix = 253402300799;

}  // namespace

// --- Names -------------------------------------------------------------------------------------

std::string_view error_name(ContentSignatureError error) {
    switch (error) {
        case ContentSignatureError::kBadOrMissingSignature:
            return "BadOrMissingSignature";
        case ContentSignatureError::kBadContentSignaturePKStandard:
            return "BadContentSignaturePKStandard";
        case ContentSignatureError::kBadContentSignatureDate:
            return "BadContentSignatureDate";
        case ContentSignatureError::kCertNotPrivilegedToSignContent:
            return "CertNotPrivilegedToSignContent";
        case ContentSignatureError::kBadCredentialsForSigningContent:
            return "BadCredentialsForSigningContent";
        case ContentSignatureError::kLibDoesntSupportContentSignature:
            return "LibDoesntSupportContentSignature";
        case ContentSignatureError::kCertifierNoPrivateKey:
            return "CertifierNoPrivateKey";
        case ContentSignatureError::kSignatureInvalid:
            return "SignatureInvalid";
        case ContentSignatureError::kDigestMismatch:
            return "DigestMismatch";
        case ContentSignatureError::kSignerSerialMismatch:
            return "SignerSerialMismatch";
        case ContentSignatureError::kNoTrustAnchors:
            return "NoTrustAnchors";
        case ContentSignatureError::kNoClock:
            return "NoClock";
        case ContentSignatureError::kKeyChainMismatch:
            return "KeyChainMismatch";
        case ContentSignatureError::kNoContent:
            return "NoContent";
    }
    return "Unknown";
}

std::string_view describe(ContentSignatureError error) {
    switch (error) {
        case ContentSignatureError::kBadOrMissingSignature:
            return "there is no content signature, or it cannot be decoded";
        case ContentSignatureError::kBadContentSignaturePKStandard:
            return "the signature names a curve other than secp256r1";
        case ContentSignatureError::kBadContentSignatureDate:
            return "the signing date is invalid or outside the acceptable range";
        case ContentSignatureError::kCertNotPrivilegedToSignContent:
            return "the signer's certificate is not one that may sign content";
        case ContentSignatureError::kBadCredentialsForSigningContent:
            return "the certificate chain is not valid, or does not lead to a trusted certificate";
        case ContentSignatureError::kLibDoesntSupportContentSignature:
            return "the signature uses a version or method this library does not read";
        case ContentSignatureError::kCertifierNoPrivateKey:
            return "no private key was given to sign with";
        case ContentSignatureError::kSignatureInvalid:
            return "the signature does not verify under the signer's key for this content";
        case ContentSignatureError::kDigestMismatch:
            return "the digest recorded in the signature is not the content's digest";
        case ContentSignatureError::kSignerSerialMismatch:
            return "the signer serial number is not the serial number of the signer's certificate";
        case ContentSignatureError::kNoTrustAnchors:
            return "no trust anchor or trusted key was given to verify against";
        case ContentSignatureError::kNoClock:
            return "no verification time was given, so certificate validity cannot be judged";
        case ContentSignatureError::kKeyChainMismatch:
            return "the signer's certificate does not hold the public key of the private key";
        case ContentSignatureError::kNoContent:
            return "there is no content to sign or to verify";
    }
    return "unknown error";
}

bool in_specification(ContentSignatureError error) noexcept {
    switch (error) {
        case ContentSignatureError::kBadOrMissingSignature:
        case ContentSignatureError::kBadContentSignaturePKStandard:
        case ContentSignatureError::kBadContentSignatureDate:
        case ContentSignatureError::kCertNotPrivilegedToSignContent:
        case ContentSignatureError::kBadCredentialsForSigningContent:
        case ContentSignatureError::kLibDoesntSupportContentSignature:
        case ContentSignatureError::kCertifierNoPrivateKey:
            return true;
        default:
            return false;
    }
}

// --- Serialisation -----------------------------------------------------------------------------

std::vector<std::byte> serialize_content_signature(const ContentSignature& signature) {
    std::vector<std::byte> out;
    Writer w(out);
    w.struct_field(id::kCertificateChain);
    write_chain(w, signature.certificate_chain);
    w.struct_field(id::kSignerInfo);
    write_signer(w, signature.signer_info);
    w.struct_field(id::kSignatureValues);
    write_values(w, signature.signature_values);
    w.stop();
    if (w.overflowed()) {
        out.clear();
    }
    return out;
}

std::expected<ContentSignature, ContentSignatureFailure> parse_content_signature(
    std::span<const std::byte> payload) {
    if (payload.empty()) {
        return bad_signature("there is no content signature payload");
    }
    if (payload.size() > kMaxContentSignatureBytes) {
        return bad_signature("the payload is " + std::to_string(payload.size()) +
                             " bytes, more than the " + std::to_string(kMaxContentSignatureBytes) +
                             " this reader accepts");
    }
    Parser parser{Reader{payload}, {}};
    ContentSignature out;
    if (const auto parsed = parse_top(parser, out); !parsed) {
        std::string detail = "Thrift decoding failed";
        if (!parser.where.empty()) {
            detail += " in " + std::string{parser.where};
        }
        detail += ": " + std::string{thrift::describe(parsed.error())};
        return bad_signature(std::move(detail));
    }
    return out;
}

// --- Signing -----------------------------------------------------------------------------------

std::expected<ContentSignature, ContentSignatureFailure> build_content_signature(
    const Digest256& digest, const SigningCredentials& credentials, std::int64_t signed_unix) {
    if (credentials.key.empty()) {
        return std::unexpected(
            failure(ContentSignatureError::kCertifierNoPrivateKey, "the private key is empty"));
    }
    if (credentials.chain.empty() || credentials.chain.size() > kMaxChainLength) {
        return std::unexpected(failure(ContentSignatureError::kBadCredentialsForSigningContent,
                                       "the chain has " + std::to_string(credentials.chain.size()) +
                                           " certificates; a chain of 1 to " +
                                           std::to_string(kMaxChainLength) + " is carried"));
    }
    const Certificate& leaf = credentials.chain.back();
    const auto public_key = credentials.key.public_key();
    if (!public_key.has_value()) {
        return std::unexpected(failure(ContentSignatureError::kCertifierNoPrivateKey,
                                       "the public key of the private key cannot be derived"));
    }
    if (*public_key != leaf.public_key) {
        return std::unexpected(failure(ContentSignatureError::kKeyChainMismatch,
                                       "the last certificate of the chain (" +
                                           leaf.subject.to_string() +
                                           ") holds a different public key from the private key; "
                                           "the chain is for another key, "
                                           "or is not in root-to-leaf order"));
    }
    if (signed_unix < 0 || signed_unix > kLatestSignedUnix) {
        return std::unexpected(failure(ContentSignatureError::kBadContentSignatureDate,
                                       "the signing time is outside years 1970 to 9999"));
    }
    const auto signature = ecdsa_sign(credentials.key, digest);
    if (!signature.has_value()) {
        return std::unexpected(failure(ContentSignatureError::kCertifierNoPrivateKey,
                                       "the private key could not sign"));
    }

    ContentSignature out;
    out.certificate_chain.chain_structure = std::string{kChainStructure};
    out.certificate_chain.format = std::string{kChainFormat};
    out.certificate_chain.pk_standard_name = std::string{kCurveName};
    out.certificate_chain.certificates.reserve(credentials.chain.size());
    for (const Certificate& certificate : credentials.chain) {
        out.certificate_chain.certificates.push_back(certificate.der);
    }
    out.signer_info.signer_serial_number = leaf.serial_hex();
    out.signer_info.signature_value = encode_ecdsa_der(*signature);

    LsSignedDigestWire recorded;
    recorded.digest_value.assign(digest.begin(), digest.end());
    recorded.reference = std::string{kDigestDataRef};
    out.signature_values.signed_info.canonical_version = kContentSignatureVersion;
    out.signature_values.signed_info.signed_date = format_iso8601_utc(signed_unix);
    out.signature_values.signed_info.digests.push_back(std::move(recorded));

    auto& details = out.signature_values.signature_values;
    details.digest_data_ref = std::string{kDigestDataRef};
    details.canonical_method = std::string{kCanonicalMethod};
    details.hash = std::string{kHashName};
    details.signature_method = std::string{kSignatureMethodName};
    details.pk_standard_name = std::string{kCurveName};
    details.digest_method = std::string{kDigestMethodName};
    return out;
}

std::expected<std::vector<std::byte>, ContentSignatureFailure> sign_digest(
    const Digest256& digest, const SigningCredentials& credentials, std::int64_t signed_unix) {
    auto built = build_content_signature(digest, credentials, signed_unix);
    if (!built) {
        return std::unexpected(std::move(built.error()));
    }
    return serialize_content_signature(*built);
}

// --- Verification ------------------------------------------------------------------------------

namespace {

// The scheme checks: the version, the curve names and the method names, each against the one
// value this code reads. Returns the failure, or nullopt.
std::optional<ContentSignatureFailure> check_scheme(const ContentSignature& cs) {
    const auto& info = cs.signature_values.signed_info;
    const auto& details = cs.signature_values.signature_values;
    const auto& chain = cs.certificate_chain;

    if (info.canonical_version != kContentSignatureVersion) {
        return failure(ContentSignatureError::kLibDoesntSupportContentSignature,
                       "canonicalVersion is " + std::to_string(info.canonical_version) +
                           "; this library reads version " +
                           std::to_string(kContentSignatureVersion));
    }
    if (chain.pk_standard_name != kCurveName) {
        return failure(ContentSignatureError::kBadContentSignaturePKStandard,
                       "the certificate chain names the curve '" +
                           printable(chain.pk_standard_name) + "', not '" +
                           std::string{kCurveName} + "'");
    }
    if (details.pk_standard_name != kCurveName) {
        return failure(ContentSignatureError::kBadContentSignaturePKStandard,
                       "the signature values name the curve '" +
                           printable(details.pk_standard_name) + "', not '" +
                           std::string{kCurveName} + "'");
    }
    struct Expected {
        const std::string& actual;
        std::string_view want;
        std::string_view field;
    };
    const Expected checks[] = {
        {chain.chain_structure, kChainStructure, "chainStructure"},
        {chain.format, kChainFormat, "format"},
        {details.canonical_method, kCanonicalMethod, "canonical_method"},
        {details.hash, kHashName, "hash"},
        {details.signature_method, kSignatureMethodName, "signatureMethod"},
        {details.digest_method, kDigestMethodName, "digestMethod"},
        {details.digest_data_ref, kDigestDataRef, "digest_data_ref"},
    };
    for (const Expected& check : checks) {
        if (check.actual != check.want) {
            return failure(ContentSignatureError::kLibDoesntSupportContentSignature,
                           std::string{check.field} + " is '" + printable(check.actual) +
                               "', not '" + std::string{check.want} + "'");
        }
    }
    return std::nullopt;
}

// The signer checks on the leaf (specification section 6.1 step 4).
std::optional<ContentSignatureFailure> check_signer(const ContentSignature& cs,
                                                    const Certificate& leaf,
                                                    const LeafProfile& profile) {
    if (!same_serial(cs.signer_info.signer_serial_number, leaf.serial_hex())) {
        return failure(ContentSignatureError::kSignerSerialMismatch,
                       "signerSerialNumber is '" + printable(cs.signer_info.signer_serial_number) +
                           "' and the signer's certificate has serial " + leaf.serial_hex());
    }
    if (profile.require_digital_signature) {
        if (!leaf.key_usage.has_value()) {
            return failure(ContentSignatureError::kCertNotPrivilegedToSignContent,
                           "the signer's certificate has no keyUsage extension; digitalSignature "
                           "is required");
        }
        if ((*leaf.key_usage & kKeyUsageDigitalSignature) == 0) {
            return failure(ContentSignatureError::kCertNotPrivilegedToSignContent,
                           "the signer's certificate keyUsage lacks digitalSignature");
        }
    }
    if (profile.title.has_value()) {
        const auto title = leaf.subject.find(kOidTitle);
        if (!title.has_value() || *title != *profile.title) {
            return failure(ContentSignatureError::kCertNotPrivilegedToSignContent,
                           "the signer's title is '" +
                               (title.has_value() ? printable(*title) : std::string{"(none)"}) +
                               "', the profile requires '" + printable(*profile.title) + "'");
        }
    }
    if (profile.common_name.has_value()) {
        const auto common_name = leaf.subject.find(kOidCommonName);
        if (!common_name.has_value() || *common_name != *profile.common_name) {
            return failure(
                ContentSignatureError::kCertNotPrivilegedToSignContent,
                "the signer's common name is '" +
                    (common_name.has_value() ? printable(*common_name) : std::string{"(none)"}) +
                    "', the profile requires '" + printable(*profile.common_name) + "'");
        }
    }
    if (profile.serial_hex.has_value() && !same_serial(*profile.serial_hex, leaf.serial_hex())) {
        return failure(ContentSignatureError::kCertNotPrivilegedToSignContent,
                       "the signer's certificate serial is " + leaf.serial_hex() +
                           ", the profile requires " + printable(*profile.serial_hex));
    }
    return std::nullopt;
}

}  // namespace

std::expected<VerifiedSignature, ContentSignatureFailure> verify_digest(
    std::span<const std::byte> payload, const Digest256& digest, const TrustStore& trust,
    const VerifyOptions& options) {
    const auto fail = [](ContentSignatureError error, std::string detail) {
        return std::unexpected(failure(error, std::move(detail)));
    };

    if (trust.empty()) {
        return fail(ContentSignatureError::kNoTrustAnchors,
                    "give a trust anchor or a trusted key to verify against");
    }
    if (!options.chain.now_unix.has_value()) {
        return fail(ContentSignatureError::kNoClock,
                    "give the time to verify at; the library has no clock");
    }
    const std::int64_t now = *options.chain.now_unix;

    // Structure: Thrift, the DER signature, and the lists that must not be empty.
    auto parsed = parse_content_signature(payload);
    if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    const ContentSignature& cs = *parsed;
    const auto signature = decode_ecdsa_der(cs.signer_info.signature_value);
    if (!signature.has_value()) {
        return fail(ContentSignatureError::kBadOrMissingSignature,
                    "signatureValue is not a DER ECDSA signature with r and s in range");
    }
    if (cs.signature_values.signed_info.digests.empty()) {
        return fail(ContentSignatureError::kBadOrMissingSignature,
                    "the signed info lists no digest");
    }
    if (cs.certificate_chain.certificates.empty()) {
        return fail(ContentSignatureError::kBadOrMissingSignature,
                    "the certificate chain is empty");
    }

    // Scheme.
    if (auto problem = check_scheme(cs)) {
        return std::unexpected(std::move(*problem));
    }

    // Chain.
    const auto& wire_chain = cs.certificate_chain.certificates;
    if (wire_chain.size() > kMaxChainLength) {
        return fail(ContentSignatureError::kBadCredentialsForSigningContent,
                    "the chain has " + std::to_string(wire_chain.size()) +
                        " certificates; at most " + std::to_string(kMaxChainLength) + " are read");
    }
    VerifiedSignature result;
    result.chain.reserve(wire_chain.size());
    for (std::size_t i = 0; i < wire_chain.size(); ++i) {
        auto certificate = parse_certificate(wire_chain[i]);
        if (!certificate) {
            return fail(ContentSignatureError::kBadCredentialsForSigningContent,
                        "certificate " + std::to_string(i + 1) + " of " +
                            std::to_string(wire_chain.size()) +
                            " cannot be read: " + std::string{describe(certificate.error())});
        }
        result.chain.push_back(std::move(*certificate));
    }
    auto accepted = validate_chain(result.chain, trust, options.chain);
    if (!accepted) {
        const ChainFailure& chain_failure = accepted.error();
        ContentSignatureFailure f;
        f.error = map_chain_error(chain_failure.error);
        f.detail = chain_failure_text(chain_failure, result.chain.size());
        f.chain = chain_failure;
        return std::unexpected(std::move(f));
    }
    result.acceptance = std::move(*accepted);
    const Certificate& leaf = result.chain.back();

    // Signer.
    if (auto problem = check_signer(cs, leaf, options.profile)) {
        // The specification's verification text reads the chain "from leaf to root" while its
        // structure lists it root first (register item 7). A payload in the other order has the
        // signer's certificate first, which is worth saying when that is what the serial matches.
        if (problem->error == ContentSignatureError::kSignerSerialMismatch &&
            result.chain.size() > 1 &&
            same_serial(cs.signer_info.signer_serial_number, result.chain.front().serial_hex())) {
            problem->detail +=
                "; the first certificate of the chain has that serial, so the chain may be in leaf "
                "to root order, and the wire order is root first";
        }
        return std::unexpected(std::move(*problem));
    }

    // Signature, over the digest the caller computed from the content.
    if (!ecdsa_verify(leaf.public_key, digest, *signature)) {
        // The usual reason is that the content is not what was signed. The payload records the
        // digest it signed, so say which of the two it looks like; the code is the same.
        std::string detail = "the signature does not verify under the signer's key";
        for (const auto& recorded : cs.signature_values.signed_info.digests) {
            if (recorded.reference == kDigestDataRef &&
                !bytes_equal(recorded.digest_value, digest)) {
                detail =
                    "the content's SHA-256 is not the digest the signature was made over: the "
                    "content changed after it was signed, or this is another stream's signature";
                break;
            }
        }
        return fail(ContentSignatureError::kSignatureInvalid, std::move(detail));
    }

    // Metadata: the date, then the digest list.
    const auto& info = cs.signature_values.signed_info;
    const auto signed_unix = parse_iso8601_utc(info.signed_date);
    if (!signed_unix.has_value()) {
        return fail(ContentSignatureError::kBadContentSignatureDate,
                    "signedDate '" + printable(info.signed_date) +
                        "' is not an ISO 8601 UTC time (YYYY-MM-DDThh:mm:ssZ)");
    }
    if (*signed_unix < leaf.not_before) {
        return fail(ContentSignatureError::kBadContentSignatureDate,
                    "signedDate " + info.signed_date +
                        " is before the signer's certificate is valid (" +
                        format_iso8601_utc(leaf.not_before) + ")");
    }
    if (*signed_unix > leaf.not_after) {
        return fail(ContentSignatureError::kBadContentSignatureDate,
                    "signedDate " + info.signed_date +
                        " is after the signer's certificate expires (" +
                        format_iso8601_utc(leaf.not_after) + ")");
    }
    if (*signed_unix > now + options.max_future_skew_seconds) {
        return fail(ContentSignatureError::kBadContentSignatureDate,
                    "signedDate " + info.signed_date + " is later than the verification time (" +
                        format_iso8601_utc(now) + ") by more than the allowed " +
                        std::to_string(options.max_future_skew_seconds) + " seconds");
    }

    bool found_reference = false;
    for (const auto& recorded : info.digests) {
        if (recorded.reference != kDigestDataRef) {
            continue;  // a digest of something else; this reader signs and checks the content's
        }
        found_reference = true;
        if (!bytes_equal(recorded.digest_value, digest)) {
            return fail(ContentSignatureError::kDigestMismatch,
                        "the digest recorded for " + std::string{kDigestDataRef} +
                            " is not the content's SHA-256");
        }
    }
    if (!found_reference) {
        return fail(ContentSignatureError::kDigestMismatch,
                    "the signed info lists no digest named " + std::string{kDigestDataRef});
    }

    result.canonical_version = info.canonical_version;
    result.signed_date = info.signed_date;
    result.signed_unix = *signed_unix;
    return result;
}

}  // namespace iclforge::base::crypto
