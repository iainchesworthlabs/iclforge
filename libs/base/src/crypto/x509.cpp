#include "iclforge/base/crypto/x509.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "iclforge/base/crypto/der.hpp"
#include "iclforge/base/crypto/sha1.hpp"
#include "iclforge/base/crypto/sha256.hpp"

namespace iclforge::base::crypto {
namespace {

// Object identifiers, written in dotted form. RFC 5280 Appendix A, RFC 5480 §2.1.1, RFC 5758 §3.2.
constexpr std::string_view kOidEcPublicKey = "1.2.840.10045.2.1";
constexpr std::string_view kOidPrime256v1 = "1.2.840.10045.3.1.7";
constexpr std::string_view kOidEcdsaSha256 = "1.2.840.10045.4.3.2";
constexpr std::string_view kOidEcdsaSha384 = "1.2.840.10045.4.3.3";
constexpr std::string_view kOidEcdsaSha512 = "1.2.840.10045.4.3.4";
constexpr std::string_view kOidKeyUsage = "2.5.29.15";
constexpr std::string_view kOidBasicConstraints = "2.5.29.19";

struct AttributeName {
    std::string_view oid;
    std::string_view name;
};
// RFC 4519 and X.520 short names for the attributes that appear in the names read here; anything
// else is shown by its dotted OID.
constexpr AttributeName kAttributeNames[] = {
    {"2.5.4.3", "CN"},
    {"2.5.4.4", "SN"},
    {"2.5.4.5", "serialNumber"},
    {"2.5.4.6", "C"},
    {"2.5.4.7", "L"},
    {"2.5.4.8", "ST"},
    {"2.5.4.10", "O"},
    {"2.5.4.11", "OU"},
    {"2.5.4.12", "title"},
    {"2.5.4.15", "businessCategory"},
    {"2.5.4.44", "generationQualifier"},
    {"2.5.4.46", "dnQualifier"},
};

std::string_view as_text(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::string hex_upper(std::span<const std::byte> bytes) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out;
    for (const std::byte b : bytes) {
        const auto v = std::to_integer<unsigned>(b);
        out.push_back(kDigits[v >> 4]);
        out.push_back(kDigits[v & 0xFU]);
    }
    return out;
}

// days_from_civil: the number of days from 1970-01-01 to y-m-d in the proleptic Gregorian calendar
// (Howard Hinnant's well-known derivation, in the public domain).
std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

void civil_from_days(std::int64_t z, std::int64_t& y, unsigned& m, unsigned& d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2 ? 1 : 0;
}

bool days_in_month_ok(std::int64_t year, unsigned month, unsigned day) {
    static constexpr unsigned kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12 || day < 1) {
        return false;
    }
    unsigned limit = kDays[month - 1];
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (month == 2 && leap) {
        limit = 29;
    }
    return day <= limit;
}

std::optional<unsigned> two_digits(std::string_view s, std::size_t pos) {
    if (pos + 2 > s.size() || s[pos] < '0' || s[pos] > '9' || s[pos + 1] < '0' ||
        s[pos + 1] > '9') {
        return std::nullopt;
    }
    return static_cast<unsigned>((s[pos] - '0') * 10 + (s[pos + 1] - '0'));
}

// RFC 5280 §4.1.2.5: UTCTime is YYMMDDHHMMSSZ with 50..99 meaning 19xx, GeneralizedTime is
// YYYYMMDDHHMMSSZ; both in Zulu with seconds present and no fraction.
std::optional<std::int64_t> parse_asn1_time(std::uint8_t tag, std::span<const std::byte> content) {
    const std::string_view s = as_text(content);
    std::size_t pos = 0;
    std::int64_t year = 0;
    if (tag == der::kUtcTime) {
        if (s.size() != 13) {
            return std::nullopt;
        }
        const auto yy = two_digits(s, 0);
        if (!yy) {
            return std::nullopt;
        }
        year = *yy >= 50 ? 1900 + *yy : 2000 + *yy;
        pos = 2;
    } else if (tag == der::kGeneralizedTime) {
        if (s.size() != 15) {
            return std::nullopt;
        }
        const auto hi = two_digits(s, 0);
        const auto lo = two_digits(s, 2);
        if (!hi || !lo) {
            return std::nullopt;
        }
        year = static_cast<std::int64_t>(*hi) * 100 + *lo;
        pos = 4;
    } else {
        return std::nullopt;
    }
    const auto month = two_digits(s, pos);
    const auto day = two_digits(s, pos + 2);
    const auto hour = two_digits(s, pos + 4);
    const auto minute = two_digits(s, pos + 6);
    const auto second = two_digits(s, pos + 8);
    if (!month || !day || !hour || !minute || !second || s.back() != 'Z' ||
        !days_in_month_ok(year, *month, *day) || *hour > 23 || *minute > 59 || *second > 59) {
        return std::nullopt;
    }
    return days_from_civil(year, *month, *day) * 86400 + static_cast<std::int64_t>(*hour) * 3600 +
           static_cast<std::int64_t>(*minute) * 60 + static_cast<std::int64_t>(*second);
}

// The text of a string-valued attribute. The 8-bit string types are shown as they are; a BMPString
// is shown when it is plain ASCII; any other type is hex, which is honest if not pretty.
std::string attribute_text(const der::Tlv& value) {
    switch (value.tag) {
        case der::kUtf8String:
        case der::kPrintableString:
        case der::kIa5String:
        case der::kTeletexString:
            return std::string(as_text(value.content));
        case der::kBmpString: {
            std::string out;
            for (std::size_t i = 0; i + 1 < value.content.size(); i += 2) {
                if (value.content[i] != std::byte{0}) {
                    return "#" + hex_upper(value.content);
                }
                out.push_back(static_cast<char>(std::to_integer<unsigned>(value.content[i + 1])));
            }
            return out;
        }
        default:
            return "#" + hex_upper(value.content);
    }
}

// RFC 5280 §4.1.2.4: Name ::= SEQUENCE OF RelativeDistinguishedName (SET OF AttributeTypeAndValue).
std::optional<DistinguishedName> parse_name(const der::Tlv& name) {
    if (name.tag != der::kSequence) {
        return std::nullopt;
    }
    DistinguishedName out;
    out.der.assign(name.encoded.begin(), name.encoded.end());
    std::span<const std::byte> rdns = name.content;
    while (!rdns.empty()) {
        const auto rdn = der::read_expecting(rdns, der::kSet);
        if (!rdn) {
            return std::nullopt;
        }
        std::span<const std::byte> attributes = rdn->content;
        if (attributes.empty()) {
            return std::nullopt;
        }
        while (!attributes.empty()) {
            const auto attribute = der::read_expecting(attributes, der::kSequence);
            if (!attribute) {
                return std::nullopt;
            }
            std::span<const std::byte> body = attribute->content;
            const auto oid = der::read_expecting(body, der::kObjectIdentifier);
            const auto value = der::read(body);
            if (!oid || !value || !body.empty()) {
                return std::nullopt;
            }
            const auto oid_text = der::oid_to_string(oid->content);
            if (!oid_text) {
                return std::nullopt;
            }
            std::string label = *oid_text;
            for (const AttributeName& known : kAttributeNames) {
                if (known.oid == *oid_text) {
                    label = std::string(known.name);
                }
            }
            const std::string text = attribute_text(*value);
            if (!out.text.empty()) {
                out.text += ", ";
            }
            out.text += label + "=" + text;
            if (*oid_text == "2.5.4.3" && !out.common_name) {
                out.common_name = text;
            } else if (*oid_text == "2.5.4.12" && !out.title) {
                out.title = text;
            }
        }
    }
    return out;
}

// SubjectPublicKeyInfo carrying an EC public key on P-256 (RFC 5480 §2.1.1): the
// AlgorithmIdentifier names id-ecPublicKey with the curve as its parameter, and the BIT STRING is
// the uncompressed point.
std::optional<P256PublicKey> parse_public_key_info(const der::Tlv& spki) {
    if (spki.tag != der::kSequence) {
        return std::nullopt;
    }
    std::span<const std::byte> body = spki.content;
    const auto algorithm = der::read_expecting(body, der::kSequence);
    const auto key = der::read_expecting(body, der::kBitString);
    if (!algorithm || !key || !body.empty()) {
        return std::nullopt;
    }
    std::span<const std::byte> algorithm_body = algorithm->content;
    const auto key_oid = der::read_expecting(algorithm_body, der::kObjectIdentifier);
    const auto curve_oid = der::read_expecting(algorithm_body, der::kObjectIdentifier);
    if (!key_oid || !curve_oid || !algorithm_body.empty() ||
        der::oid_to_string(key_oid->content) != kOidEcPublicKey ||
        der::oid_to_string(curve_oid->content) != kOidPrime256v1) {
        return std::nullopt;
    }
    if (key->content.size() != 66 || key->content[0] != std::byte{0}) {
        return std::nullopt;  // the point, and no unused bits
    }
    return p256_public_key_from_uncompressed(key->content.subspan(1));
}

struct ParsedAlgorithm {
    SignatureAlgorithm algorithm = SignatureAlgorithm::kOther;
    std::string oid;
};

std::optional<ParsedAlgorithm> parse_signature_algorithm(const der::Tlv& seq) {
    if (seq.tag != der::kSequence) {
        return std::nullopt;
    }
    std::span<const std::byte> body = seq.content;
    const auto oid = der::read_expecting(body, der::kObjectIdentifier);
    if (!oid) {
        return std::nullopt;
    }
    // Parameters: absent for ECDSA (RFC 5758 §3.2); an explicit NULL is tolerated.
    if (!body.empty()) {
        const auto parameters = der::read(body);
        if (!parameters || parameters->tag != der::kNull || !body.empty()) {
            return std::nullopt;
        }
    }
    const auto text = der::oid_to_string(oid->content);
    if (!text) {
        return std::nullopt;
    }
    ParsedAlgorithm out;
    out.oid = *text;
    if (*text == kOidEcdsaSha256) {
        out.algorithm = SignatureAlgorithm::kEcdsaWithSha256;
    } else if (*text == kOidEcdsaSha384) {
        out.algorithm = SignatureAlgorithm::kEcdsaWithSha384;
    } else if (*text == kOidEcdsaSha512) {
        out.algorithm = SignatureAlgorithm::kEcdsaWithSha512;
    }
    return out;
}

// RFC 5280 §4.2.1.3: a BIT STRING of up to nine named bits, bit 0 first.
std::optional<std::uint16_t> parse_key_usage(std::span<const std::byte> extension_value) {
    std::span<const std::byte> in = extension_value;
    const auto bits = der::read_expecting(in, der::kBitString);
    if (!bits || !in.empty() || bits->content.size() < 2 || bits->content.size() > 3) {
        return std::nullopt;
    }
    std::uint16_t usage = 0;
    for (std::size_t i = 1; i < bits->content.size(); ++i) {
        const unsigned octet = std::to_integer<unsigned>(bits->content[i]);
        for (unsigned bit = 0; bit < 8; ++bit) {
            if ((octet & (0x80U >> bit)) != 0) {
                usage = static_cast<std::uint16_t>(usage | (1U << ((i - 1) * 8 + bit)));
            }
        }
    }
    return usage;
}

// RFC 5280 §4.2.1.9: SEQUENCE { cA BOOLEAN DEFAULT FALSE, pathLenConstraint INTEGER OPTIONAL }.
bool parse_basic_constraints(std::span<const std::byte> extension_value, bool& ca,
                             std::optional<std::uint64_t>& path_length) {
    std::span<const std::byte> in = extension_value;
    const auto seq = der::read_expecting(in, der::kSequence);
    if (!seq || !in.empty()) {
        return false;
    }
    ca = false;
    path_length.reset();
    std::span<const std::byte> body = seq->content;
    if (!body.empty() && body[0] == std::byte{der::kBoolean}) {
        const auto flag = der::read_expecting(body, der::kBoolean);
        if (!flag || flag->content.size() != 1) {
            return false;
        }
        ca = flag->content[0] != std::byte{0};
    }
    if (!body.empty()) {
        const auto length = der::read_expecting(body, der::kInteger);
        if (!length || !body.empty()) {
            return false;
        }
        const auto value = der::small_unsigned(length->content);
        if (!value) {
            return false;
        }
        path_length = *value;
    }
    return true;
}

}  // namespace

const char* to_string(X509Error error) noexcept {
    switch (error) {
        case X509Error::kMalformed:
            return "not a DER X.509 certificate";
        case X509Error::kUnsupportedPublicKey:
            return "the public key is not an EC key on P-256";
        case X509Error::kUnsupportedVersion:
            return "not an X.509 v3 certificate";
        case X509Error::kUnsupportedCriticalExtension:
            return "the certificate has a critical extension this reader does not understand";
    }
    return "unknown certificate error";
}

std::expected<Certificate, X509Error> parse_certificate(std::span<const std::byte> der_bytes) {
    using Unexpected = std::unexpected<X509Error>;
    std::span<const std::byte> input = der_bytes;
    const auto outer = der::read_expecting(input, der::kSequence);
    if (!outer || !input.empty()) {
        return Unexpected(X509Error::kMalformed);
    }
    Certificate cert;
    cert.der.assign(der_bytes.begin(), der_bytes.end());

    std::span<const std::byte> body = outer->content;
    const auto tbs = der::read_expecting(body, der::kSequence);
    const auto outer_algorithm = der::read(body);
    const auto signature_bits = der::read_expecting(body, der::kBitString);
    if (!tbs || !outer_algorithm || !signature_bits || !body.empty()) {
        return Unexpected(X509Error::kMalformed);
    }
    cert.tbs_offset = static_cast<std::size_t>(tbs->encoded.data() - der_bytes.data());
    cert.tbs_size = tbs->encoded.size();

    const auto algorithm = parse_signature_algorithm(*outer_algorithm);
    if (!algorithm) {
        return Unexpected(X509Error::kMalformed);
    }
    cert.signature_algorithm = algorithm->algorithm;
    cert.signature_algorithm_oid = algorithm->oid;
    if (signature_bits->content.size() > 1 && signature_bits->content[0] == std::byte{0}) {
        if (const auto sig = ecdsa_signature_from_der(signature_bits->content.subspan(1))) {
            cert.signature = *sig;
            cert.signature_parsed = true;
        }
    }

    // RFC 5280 §4.1: TBSCertificate ::= SEQUENCE { version [0] EXPLICIT DEFAULT v1, serialNumber,
    // signature, issuer, validity, subject, subjectPublicKeyInfo, issuerUniqueID [1] OPTIONAL,
    // subjectUniqueID [2] OPTIONAL, extensions [3] EXPLICIT OPTIONAL }.
    std::span<const std::byte> fields = tbs->content;
    if (fields.empty() || std::to_integer<std::uint8_t>(fields[0]) != der::kContext0) {
        return Unexpected(X509Error::kUnsupportedVersion);
    }
    const auto version_wrapper = der::read_expecting(fields, der::kContext0);
    if (!version_wrapper) {
        return Unexpected(X509Error::kMalformed);
    }
    std::span<const std::byte> version_body = version_wrapper->content;
    const auto version = der::read_expecting(version_body, der::kInteger);
    if (!version || !version_body.empty()) {
        return Unexpected(X509Error::kMalformed);
    }
    if (der::small_unsigned(version->content) != std::optional<std::uint64_t>(2)) {
        return Unexpected(X509Error::kUnsupportedVersion);
    }

    const auto serial = der::read_expecting(fields, der::kInteger);
    const auto inner_algorithm = der::read(fields);
    const auto issuer = der::read(fields);
    const auto validity = der::read_expecting(fields, der::kSequence);
    const auto subject = der::read(fields);
    const auto spki = der::read(fields);
    if (!serial || !inner_algorithm || !issuer || !validity || !subject || !spki ||
        serial->content.empty()) {
        return Unexpected(X509Error::kMalformed);
    }
    // RFC 5280 §4.1.1.2: the two copies of the algorithm must agree, or the unsigned one could be
    // changed without the signature noticing.
    if (outer_algorithm->encoded.size() != inner_algorithm->encoded.size() ||
        !std::equal(outer_algorithm->encoded.begin(), outer_algorithm->encoded.end(),
                    inner_algorithm->encoded.begin())) {
        return Unexpected(X509Error::kMalformed);
    }

    std::span<const std::byte> serial_octets = serial->content;
    if (serial_octets.size() > 1 && serial_octets[0] == std::byte{0}) {
        serial_octets = serial_octets.subspan(1);
    }
    cert.serial.assign(serial_octets.begin(), serial_octets.end());
    cert.serial_hex = hex_upper(cert.serial);

    const auto issuer_name = parse_name(*issuer);
    const auto subject_name = parse_name(*subject);
    if (!issuer_name || !subject_name) {
        return Unexpected(X509Error::kMalformed);
    }
    cert.issuer = *issuer_name;
    cert.subject = *subject_name;

    std::span<const std::byte> validity_body = validity->content;
    const auto not_before = der::read(validity_body);
    const auto not_after = der::read(validity_body);
    if (!not_before || !not_after || !validity_body.empty()) {
        return Unexpected(X509Error::kMalformed);
    }
    const auto before = parse_asn1_time(not_before->tag, not_before->content);
    const auto after = parse_asn1_time(not_after->tag, not_after->content);
    if (!before || !after) {
        return Unexpected(X509Error::kMalformed);
    }
    cert.not_before = *before;
    cert.not_after = *after;

    const auto key = parse_public_key_info(*spki);
    if (!key) {
        return Unexpected(X509Error::kUnsupportedPublicKey);
    }
    cert.public_key = *key;

    // Unique identifiers are legal in v3 and unused; skip them before the extensions.
    while (!fields.empty() && (std::to_integer<std::uint8_t>(fields[0]) == 0xA1 ||
                               std::to_integer<std::uint8_t>(fields[0]) == 0x81 ||
                               std::to_integer<std::uint8_t>(fields[0]) == 0xA2 ||
                               std::to_integer<std::uint8_t>(fields[0]) == 0x82)) {
        if (!der::read(fields)) {
            return Unexpected(X509Error::kMalformed);
        }
    }
    if (!fields.empty()) {
        const auto wrapper = der::read_expecting(fields, der::kContext3);
        if (!wrapper || !fields.empty()) {
            return Unexpected(X509Error::kMalformed);
        }
        std::span<const std::byte> wrapper_body = wrapper->content;
        const auto list = der::read_expecting(wrapper_body, der::kSequence);
        if (!list || !wrapper_body.empty()) {
            return Unexpected(X509Error::kMalformed);
        }
        std::span<const std::byte> extensions = list->content;
        while (!extensions.empty()) {
            const auto extension = der::read_expecting(extensions, der::kSequence);
            if (!extension) {
                return Unexpected(X509Error::kMalformed);
            }
            std::span<const std::byte> parts = extension->content;
            const auto oid = der::read_expecting(parts, der::kObjectIdentifier);
            bool critical = false;
            if (!parts.empty() && parts[0] == std::byte{der::kBoolean}) {
                const auto flag = der::read_expecting(parts, der::kBoolean);
                if (!flag || flag->content.size() != 1) {
                    return Unexpected(X509Error::kMalformed);
                }
                critical = flag->content[0] != std::byte{0};
            }
            const auto value = der::read_expecting(parts, der::kOctetString);
            if (!oid || !value || !parts.empty()) {
                return Unexpected(X509Error::kMalformed);
            }
            const auto oid_text = der::oid_to_string(oid->content);
            if (!oid_text) {
                return Unexpected(X509Error::kMalformed);
            }
            if (*oid_text == kOidKeyUsage) {
                cert.key_usage = parse_key_usage(value->content);
                if (!cert.key_usage) {
                    return Unexpected(X509Error::kMalformed);
                }
            } else if (*oid_text == kOidBasicConstraints) {
                bool ca = false;
                if (!parse_basic_constraints(value->content, ca, cert.path_length)) {
                    return Unexpected(X509Error::kMalformed);
                }
                cert.ca = ca;
            } else if (critical) {
                return Unexpected(X509Error::kUnsupportedCriticalExtension);
            }
        }
    }

    cert.sha1_fingerprint = sha1(cert.der);
    cert.sha256_fingerprint = sha256(cert.der);
    return cert;
}

bool certificate_signature_valid(const Certificate& cert, const P256PublicKey& issuer) {
    if (cert.signature_algorithm != SignatureAlgorithm::kEcdsaWithSha256 ||
        !cert.signature_parsed) {
        return false;
    }
    return ecdsa_p256_verify(issuer, sha256(cert.tbs()), cert.signature);
}

std::optional<std::int64_t> parse_iso8601_utc(std::string_view text) {
    if (!text.empty() && text.back() == 'Z') {
        text.remove_suffix(1);
    }
    // YYYY-MM-DDTHH:MM:SS
    if (text.size() != 19 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':') {
        return std::nullopt;
    }
    const auto century = two_digits(text, 0);
    const auto year_low = two_digits(text, 2);
    const auto month = two_digits(text, 5);
    const auto day = two_digits(text, 8);
    const auto hour = two_digits(text, 11);
    const auto minute = two_digits(text, 14);
    const auto second = two_digits(text, 17);
    if (!century || !year_low || !month || !day || !hour || !minute || !second) {
        return std::nullopt;
    }
    const std::int64_t year = static_cast<std::int64_t>(*century) * 100 + *year_low;
    if (!days_in_month_ok(year, *month, *day) || *hour > 23 || *minute > 59 || *second > 59) {
        return std::nullopt;
    }
    return days_from_civil(year, *month, *day) * 86400 + static_cast<std::int64_t>(*hour) * 3600 +
           static_cast<std::int64_t>(*minute) * 60 + static_cast<std::int64_t>(*second);
}

std::string format_iso8601_utc(std::int64_t epoch_seconds) {
    std::int64_t days = epoch_seconds / 86400;
    std::int64_t rest = epoch_seconds % 86400;
    if (rest < 0) {
        rest += 86400;
        --days;
    }
    std::int64_t year = 0;
    unsigned month = 0;
    unsigned day = 0;
    civil_from_days(days, year, month, day);
    std::string out(19, '0');
    const auto put = [&out](std::size_t pos, std::int64_t value, std::size_t width) {
        for (std::size_t i = 0; i < width; ++i) {
            out[pos + width - 1 - i] = static_cast<char>('0' + value % 10);
            value /= 10;
        }
    };
    put(0, year, 4);
    out[4] = '-';
    put(5, month, 2);
    out[7] = '-';
    put(8, day, 2);
    out[10] = 'T';
    put(11, rest / 3600, 2);
    out[13] = ':';
    put(14, (rest / 60) % 60, 2);
    out[16] = ':';
    put(17, rest % 60, 2);
    return out;
}

}  // namespace iclforge::base::crypto
