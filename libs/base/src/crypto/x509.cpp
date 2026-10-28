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

#include "der.hpp"
#include "iclforge/base/crypto/ecdsa_der.hpp"
#include "iclforge/base/crypto/sha1.hpp"
#include "iclforge/base/crypto/sha256.hpp"

namespace iclforge::base::crypto {
namespace {

using detail::DerElement;

// ecdsa-with-SHA256, 1.2.840.10045.4.3.2: RFC 5758 section 3.2 says the parameters are absent, so
// the whole AlgorithmIdentifier content is the OID's TLV.
constexpr std::array<std::uint8_t, 10> kEcdsaWithSha256 = {0x06, 0x08, 0x2A, 0x86, 0x48,
                                                           0xCE, 0x3D, 0x04, 0x03, 0x02};
template <std::size_t N>
bool equals_bytes(std::span<const std::byte> a, const std::array<std::uint8_t, N>& b) {
    if (a.size() != N) {
        return false;
    }
    for (std::size_t i = 0; i < N; ++i) {
        if (std::to_integer<std::uint8_t>(a[i]) != b[i]) {
            return false;
        }
    }
    return true;
}

std::string hex_of(std::span<const std::byte> bytes, char separator) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i != 0 && separator != '\0') {
            out.push_back(separator);
        }
        const unsigned v = std::to_integer<unsigned>(bytes[i]);
        out.push_back(kDigits[v >> 4]);
        out.push_back(kDigits[v & 0xF]);
    }
    return out;
}

// --- Time ----------------------------------------------------------------------------------

constexpr bool is_leap(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

constexpr int days_in_month(int year, int month) {
    constexpr std::array<int, 12> kDays = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && is_leap(year) ? 29 : kDays[static_cast<std::size_t>(month - 1)];
}

// Howard Hinnant's days_from_civil: days since 1970-01-01 of a proleptic Gregorian date.
constexpr std::int64_t days_from_civil(int year, int month, int day) {
    const int y = year - (month <= 2 ? 1 : 0);
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const std::int64_t yoe = y - era * 400;
    const std::int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

std::optional<int> digits(std::string_view text, std::size_t offset, std::size_t count) {
    int value = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const char c = text[offset + i];
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = value * 10 + (c - '0');
    }
    return value;
}

// RFC 5280 section 4.1.2.5: UTCTime YYMMDDHHMMSSZ (a year of 50 or more is 19YY) and
// GeneralizedTime YYYYMMDDHHMMSSZ, both in Zulu with seconds present.
std::optional<std::int64_t> parse_asn1_time(const DerElement& element) {
    const std::string_view text{reinterpret_cast<const char*>(element.content.data()),
                                element.content.size()};
    std::size_t offset = 0;
    int year = 0;
    if (element.tag == detail::kTagUtcTime) {
        if (text.size() != 13) {
            return std::nullopt;
        }
        const auto yy = digits(text, 0, 2);
        if (!yy) {
            return std::nullopt;
        }
        year = *yy >= 50 ? 1900 + *yy : 2000 + *yy;
        offset = 2;
    } else if (element.tag == detail::kTagGeneralizedTime) {
        if (text.size() != 15) {
            return std::nullopt;
        }
        const auto yyyy = digits(text, 0, 4);
        if (!yyyy) {
            return std::nullopt;
        }
        year = *yyyy;
        offset = 4;
    } else {
        return std::nullopt;
    }
    if (text.back() != 'Z') {
        return std::nullopt;
    }
    const auto month = digits(text, offset, 2);
    const auto day = digits(text, offset + 2, 2);
    const auto hour = digits(text, offset + 4, 2);
    const auto minute = digits(text, offset + 6, 2);
    const auto second = digits(text, offset + 8, 2);
    if (!month || !day || !hour || !minute || !second) {
        return std::nullopt;
    }
    return unix_time_from_civil(year, *month, *day, *hour, *minute, *second);
}

// --- Names ---------------------------------------------------------------------------------

std::string_view short_name(std::string_view oid) {
    if (oid == "2.5.4.3") return "CN";
    if (oid == "2.5.4.5") return "serialNumber";
    if (oid == "2.5.4.6") return "C";
    if (oid == "2.5.4.7") return "L";
    if (oid == "2.5.4.8") return "ST";
    if (oid == "2.5.4.10") return "O";
    if (oid == "2.5.4.11") return "OU";
    if (oid == "2.5.4.12") return "title";
    if (oid == "1.2.840.113549.1.9.1") return "emailAddress";
    if (oid == "0.9.2342.19200300.100.1.25") return "DC";
    return oid;
}

std::string attribute_string(const DerElement& value) {
    switch (value.tag) {
        case 0x0C:  // UTF8String
        case 0x13:  // PrintableString
        case 0x14:  // TeletexString, taken as bytes
        case 0x16:  // IA5String
        case 0x1A:  // VisibleString
            return std::string{reinterpret_cast<const char*>(value.content.data()),
                               value.content.size()};
        default:
            return "#" + hex_of(value.whole, '\0');
    }
}

std::optional<Name> parse_name(const DerElement& sequence) {
    Name name;
    name.der.assign(sequence.whole.begin(), sequence.whole.end());
    std::span<const std::byte> rdns = sequence.content;
    while (!rdns.empty()) {
        const auto set = detail::read_tagged(rdns, detail::kTagSet);
        if (!set.has_value() || set->content.empty()) {
            return std::nullopt;
        }
        std::span<const std::byte> attributes = set->content;
        while (!attributes.empty()) {
            const auto attribute = detail::read_tagged(attributes, detail::kTagSequence);
            if (!attribute.has_value()) {
                return std::nullopt;
            }
            std::span<const std::byte> parts = attribute->content;
            const auto oid = detail::read_tagged(parts, detail::kTagObjectIdentifier);
            const auto value = detail::read_element(parts);
            if (!oid.has_value() || !value.has_value() || !parts.empty()) {
                return std::nullopt;
            }
            std::string dotted = detail::oid_to_string(oid->content);
            if (dotted.empty()) {
                return std::nullopt;
            }
            name.attributes.push_back({std::move(dotted), attribute_string(*value)});
        }
    }
    return name;
}

// --- Extensions ----------------------------------------------------------------------------

std::optional<std::uint16_t> parse_key_usage(std::span<const std::byte> value) {
    const auto bits = detail::read_tagged(value, detail::kTagBitString);
    if (!bits.has_value() || !value.empty() || bits->content.size() < 2 ||
        std::to_integer<unsigned>(bits->content[0]) > 7) {
        return std::nullopt;
    }
    std::uint16_t mask = 0;
    const std::size_t bit_count =
        (bits->content.size() - 1) * 8 - std::to_integer<unsigned>(bits->content[0]);
    for (std::size_t i = 0; i < bit_count && i < 9; ++i) {
        const unsigned byte = std::to_integer<unsigned>(bits->content[1 + i / 8]);
        if ((byte & (0x80u >> (i % 8))) != 0) {
            mask = static_cast<std::uint16_t>(mask | (1u << i));
        }
    }
    return mask;
}

std::optional<BasicConstraints> parse_basic_constraints(std::span<const std::byte> value) {
    const auto sequence = detail::read_tagged(value, detail::kTagSequence);
    if (!sequence.has_value() || !value.empty()) {
        return std::nullopt;
    }
    BasicConstraints out;
    std::span<const std::byte> body = sequence->content;
    if (const auto flag = detail::read_tagged(body, detail::kTagBoolean)) {
        // DER writes a TRUE boolean as FF and omits a FALSE one that is the default.
        if (flag->content.size() != 1 || flag->content[0] != std::byte{0xFF}) {
            return std::nullopt;
        }
        out.ca = true;
    }
    if (const auto length = detail::read_tagged(body, detail::kTagInteger)) {
        if (length->content.empty() || length->content.size() > 4 ||
            (std::to_integer<unsigned>(length->content[0]) & 0x80u) != 0) {
            return std::nullopt;
        }
        std::uint32_t path = 0;
        for (const std::byte b : length->content) {
            path = (path << 8) | std::to_integer<std::uint32_t>(b);
        }
        out.path_length = path;
    }
    if (!body.empty()) {
        return std::nullopt;
    }
    return out;
}

struct ParsedExtensions {
    std::optional<std::uint16_t> key_usage;
    std::optional<BasicConstraints> basic_constraints;
};

std::expected<ParsedExtensions, X509Error> parse_extensions(std::span<const std::byte> content) {
    std::span<const std::byte> input = content;
    const auto sequence = detail::read_tagged(input, detail::kTagSequence);
    if (!sequence.has_value() || !input.empty()) {
        return std::unexpected(X509Error::kMalformed);
    }
    ParsedExtensions out;
    std::vector<std::string> seen;
    std::span<const std::byte> list = sequence->content;
    if (list.empty()) {
        return std::unexpected(X509Error::kMalformed);  // RFC 5280: at least one extension
    }
    while (!list.empty()) {
        const auto extension = detail::read_tagged(list, detail::kTagSequence);
        if (!extension.has_value()) {
            return std::unexpected(X509Error::kMalformed);
        }
        std::span<const std::byte> parts = extension->content;
        const auto oid_element = detail::read_tagged(parts, detail::kTagObjectIdentifier);
        if (!oid_element.has_value()) {
            return std::unexpected(X509Error::kMalformed);
        }
        bool critical = false;
        if (const auto flag = detail::read_tagged(parts, detail::kTagBoolean)) {
            if (flag->content.size() != 1 || flag->content[0] != std::byte{0xFF}) {
                return std::unexpected(X509Error::kMalformed);
            }
            critical = true;
        }
        const auto value = detail::read_tagged(parts, detail::kTagOctetString);
        if (!value.has_value() || !parts.empty()) {
            return std::unexpected(X509Error::kMalformed);
        }
        const std::string oid = detail::oid_to_string(oid_element->content);
        if (oid.empty()) {
            return std::unexpected(X509Error::kMalformed);
        }
        if (std::ranges::find(seen, oid) != seen.end()) {
            return std::unexpected(X509Error::kDuplicateExtension);
        }
        seen.push_back(oid);
        if (oid == "2.5.29.15") {
            out.key_usage = parse_key_usage(value->content);
            if (!out.key_usage.has_value()) {
                return std::unexpected(X509Error::kMalformed);
            }
        } else if (oid == "2.5.29.19") {
            out.basic_constraints = parse_basic_constraints(value->content);
            if (!out.basic_constraints.has_value()) {
                return std::unexpected(X509Error::kMalformed);
            }
        } else if (critical) {
            // RFC 5280 section 4.2: a certificate with a critical extension the reader does not
            // process must be rejected.
            return std::unexpected(X509Error::kUnknownCriticalExtension);
        }
    }
    return out;
}

}  // namespace

std::string_view describe(X509Error error) {
    switch (error) {
        case X509Error::kMalformed:
            return "the certificate is not well-formed DER";
        case X509Error::kTrailingData:
            return "bytes follow the certificate";
        case X509Error::kUnsupportedVersion:
            return "the certificate is not version 1 or 3";
        case X509Error::kUnsupportedSignature:
            return "the certificate is not signed with ecdsa-with-SHA256";
        case X509Error::kUnsupportedKey:
            return "the certificate's key is not an EC key on prime256v1";
        case X509Error::kKeyNotOnCurve:
            return "the certificate's public key is not a point on P-256";
        case X509Error::kBadTime:
            return "the certificate holds a time that is not a valid UTC time";
        case X509Error::kBadSerial:
            return "the certificate's serial number is negative, empty or over 20 bytes";
        case X509Error::kUnknownCriticalExtension:
            return "the certificate has a critical extension this reader does not implement";
        case X509Error::kDuplicateExtension:
            return "the certificate repeats an extension";
    }
    return "unknown certificate error";
}

std::string_view describe(ChainError error) {
    switch (error) {
        case ChainError::kNoClock:
            return "no verification time was given, so validity periods cannot be judged";
        case ChainError::kNoTrust:
            return "no trust anchor or trusted key was given";
        case ChainError::kEmptyChain:
            return "the certificate chain is empty";
        case ChainError::kBrokenLink:
            return "a certificate's issuer is not the subject of the certificate before it";
        case ChainError::kBadSignature:
            return "a certificate's signature does not verify under its issuer's key";
        case ChainError::kNotCa:
            return "an issuing certificate is not a CA, may not sign certificates, or is past "
                   "its path length";
        case ChainError::kNotYetValid:
            return "a certificate is not valid yet";
        case ChainError::kExpired:
            return "a certificate has expired";
        case ChainError::kUntrusted:
            return "no trust anchor or trusted key vouches for the chain";
    }
    return "unknown chain error";
}

std::optional<std::int64_t> unix_time_from_civil(int year, int month, int day, int hour, int minute,
                                                 int second) {
    if (year < 0 || year > 9999 || month < 1 || month > 12 || day < 1 ||
        day > days_in_month(year, month) || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59) {
        return std::nullopt;
    }
    return days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
}

std::string format_iso8601_utc(std::int64_t unix_seconds) {
    // Howard Hinnant's civil_from_days.
    std::int64_t days = unix_seconds / 86400;
    std::int64_t rest = unix_seconds % 86400;
    if (rest < 0) {
        rest += 86400;
        --days;
    }
    const std::int64_t z = days + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::int64_t doe = z - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const int day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    const int month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    const int year = static_cast<int>(yoe + era * 400 + (month <= 2 ? 1 : 0));
    const auto two = [](int v) {
        std::string s = std::to_string(v);
        return s.size() < 2 ? "0" + s : s;
    };
    std::string y = std::to_string(year);
    while (y.size() < 4) {
        y.insert(y.begin(), '0');
    }
    return y + "-" + two(month) + "-" + two(day) + "T" + two(static_cast<int>(rest / 3600)) + ":" +
           two(static_cast<int>(rest % 3600 / 60)) + ":" + two(static_cast<int>(rest % 60)) + "Z";
}

std::optional<std::int64_t> parse_iso8601_utc(std::string_view text) {
    // YYYY-MM-DDThh:mm:ss, then an optional .fraction, then Z.
    if (text.size() < 20 || text.back() != 'Z' || text[4] != '-' || text[7] != '-' ||
        text[10] != 'T' || text[13] != ':' || text[16] != ':') {
        return std::nullopt;
    }
    if (text.size() > 20) {
        if (text[19] != '.' || text.size() < 22) {
            return std::nullopt;
        }
        for (std::size_t i = 20; i + 1 < text.size(); ++i) {
            if (text[i] < '0' || text[i] > '9') {
                return std::nullopt;
            }
        }
    }
    const auto year = digits(text, 0, 4);
    const auto month = digits(text, 5, 2);
    const auto day = digits(text, 8, 2);
    const auto hour = digits(text, 11, 2);
    const auto minute = digits(text, 14, 2);
    const auto second = digits(text, 17, 2);
    if (!year || !month || !day || !hour || !minute || !second) {
        return std::nullopt;
    }
    return unix_time_from_civil(*year, *month, *day, *hour, *minute, *second);
}

std::optional<std::string_view> Name::find(std::string_view oid) const {
    for (const Attribute& attribute : attributes) {
        if (attribute.oid == oid) {
            return std::string_view{attribute.value};
        }
    }
    return std::nullopt;
}

std::string Name::to_string() const {
    std::string out;
    for (const Attribute& attribute : attributes) {
        if (!out.empty()) {
            out += ", ";
        }
        out += short_name(attribute.oid);
        out += '=';
        out += attribute.value;
    }
    return out;
}

std::string Certificate::serial_hex() const {
    return hex_of(serial, '\0');
}

std::string Certificate::sha1_fingerprint() const {
    return hex_of(sha1(der), ':');
}

std::string Certificate::sha256_fingerprint() const {
    return hex_of(sha256(der), ':');
}

std::expected<P256PublicKey, X509Error> parse_subject_public_key_info(
    std::span<const std::byte> der) {
    std::span<const std::byte> input = der;
    const auto spki = detail::read_tagged(input, detail::kTagSequence);
    if (!spki.has_value()) {
        return std::unexpected(X509Error::kMalformed);
    }
    if (!input.empty()) {
        return std::unexpected(X509Error::kTrailingData);
    }
    std::span<const std::byte> parts = spki->content;
    const auto algorithm = detail::read_tagged(parts, detail::kTagSequence);
    const auto key_bits = detail::read_tagged(parts, detail::kTagBitString);
    if (!algorithm.has_value() || !key_bits.has_value() || !parts.empty()) {
        return std::unexpected(X509Error::kMalformed);
    }
    switch (detail::classify_ec_algorithm(algorithm->content)) {
        case detail::EcAlgorithm::kP256:
            break;
        case detail::EcAlgorithm::kMalformed:
            return std::unexpected(X509Error::kMalformed);
        case detail::EcAlgorithm::kOtherCurve:
        case detail::EcAlgorithm::kNotEc:
            return std::unexpected(X509Error::kUnsupportedKey);
    }
    // A BIT STRING's first content byte counts the unused bits at the end; a point has none.
    if (key_bits->content.empty() || key_bits->content[0] != std::byte{0}) {
        return std::unexpected(X509Error::kMalformed);
    }
    const auto key = P256PublicKey::from_uncompressed(key_bits->content.subspan(1));
    if (!key.has_value()) {
        // The right length for an uncompressed point with a bad value is a point off the curve; any
        // other length is a form (compressed, hybrid) this reader does not take.
        return std::unexpected(key_bits->content.size() == 66 ? X509Error::kKeyNotOnCurve
                                                              : X509Error::kUnsupportedKey);
    }
    return *key;
}

std::expected<Certificate, X509Error> parse_certificate(std::span<const std::byte> der) {
    std::span<const std::byte> input = der;
    const auto outer = detail::read_tagged(input, detail::kTagSequence);
    if (!outer.has_value()) {
        return std::unexpected(X509Error::kMalformed);
    }
    if (!input.empty()) {
        return std::unexpected(X509Error::kTrailingData);
    }
    std::span<const std::byte> body = outer->content;
    const auto tbs = detail::read_tagged(body, detail::kTagSequence);
    const auto outer_algorithm = detail::read_tagged(body, detail::kTagSequence);
    const auto signature_bits = detail::read_tagged(body, detail::kTagBitString);
    if (!tbs.has_value() || !outer_algorithm.has_value() || !signature_bits.has_value() ||
        !body.empty()) {
        return std::unexpected(X509Error::kMalformed);
    }
    if (!equals_bytes(outer_algorithm->content, kEcdsaWithSha256)) {
        return std::unexpected(X509Error::kUnsupportedSignature);
    }
    if (signature_bits->content.empty() || signature_bits->content[0] != std::byte{0}) {
        return std::unexpected(X509Error::kMalformed);
    }

    Certificate cert;
    cert.der.assign(der.begin(), der.end());
    cert.tbs_offset_ = static_cast<std::size_t>(tbs->whole.data() - der.data());
    cert.tbs_size_ = tbs->whole.size();
    const auto signature = decode_ecdsa_der(signature_bits->content.subspan(1));
    if (!signature.has_value()) {
        return std::unexpected(X509Error::kMalformed);
    }
    cert.signature = *signature;

    std::span<const std::byte> fields = tbs->content;
    // version [0] EXPLICIT INTEGER DEFAULT v1; DER does not write the default.
    cert.version = 1;
    if (!fields.empty() && std::to_integer<std::uint8_t>(fields[0]) == 0xA0) {
        const auto wrapper = detail::read_element(fields);
        if (!wrapper.has_value()) {
            return std::unexpected(X509Error::kMalformed);
        }
        std::span<const std::byte> inner = wrapper->content;
        const auto value = detail::read_tagged(inner, detail::kTagInteger);
        if (!value.has_value() || !inner.empty() || value->content.size() != 1) {
            return std::unexpected(X509Error::kMalformed);
        }
        const unsigned v = std::to_integer<unsigned>(value->content[0]);
        if (v != 2) {  // 0 is the default DER omits, 1 is v2, 2 is v3
            return std::unexpected(v == 0 ? X509Error::kMalformed : X509Error::kUnsupportedVersion);
        }
        cert.version = 3;
    }

    const auto serial = detail::read_tagged(fields, detail::kTagInteger);
    const auto inner_algorithm = detail::read_tagged(fields, detail::kTagSequence);
    const auto issuer = detail::read_tagged(fields, detail::kTagSequence);
    const auto validity = detail::read_tagged(fields, detail::kTagSequence);
    const auto subject = detail::read_tagged(fields, detail::kTagSequence);
    const auto spki = detail::read_tagged(fields, detail::kTagSequence);
    if (!serial.has_value() || !inner_algorithm.has_value() || !issuer.has_value() ||
        !validity.has_value() || !subject.has_value() || !spki.has_value()) {
        return std::unexpected(X509Error::kMalformed);
    }

    // RFC 5280 section 4.1.2.2: a positive integer of at most 20 octets, as DER writes it. A
    // leading zero byte is a sign byte and is not part of the serial.
    {
        std::span<const std::byte> s = serial->content;
        if (s.empty() || (std::to_integer<unsigned>(s[0]) & 0x80u) != 0) {
            return std::unexpected(X509Error::kBadSerial);
        }
        if (s[0] == std::byte{0} && s.size() > 1) {
            if ((std::to_integer<unsigned>(s[1]) & 0x80u) == 0) {
                return std::unexpected(X509Error::kMalformed);
            }
            s = s.subspan(1);
        }
        if (s.size() > 20) {
            return std::unexpected(X509Error::kBadSerial);
        }
        cert.serial.assign(s.begin(), s.end());
    }

    if (inner_algorithm->content.size() != outer_algorithm->content.size() ||
        !std::equal(inner_algorithm->content.begin(), inner_algorithm->content.end(),
                    outer_algorithm->content.begin())) {
        return std::unexpected(X509Error::kUnsupportedSignature);
    }

    auto issuer_name = parse_name(*issuer);
    auto subject_name = parse_name(*subject);
    if (!issuer_name.has_value() || !subject_name.has_value()) {
        return std::unexpected(X509Error::kMalformed);
    }
    cert.issuer = std::move(*issuer_name);
    cert.subject = std::move(*subject_name);

    {
        std::span<const std::byte> times = validity->content;
        const auto first = detail::read_element(times);
        const auto second = detail::read_element(times);
        if (!first.has_value() || !second.has_value() || !times.empty()) {
            return std::unexpected(X509Error::kMalformed);
        }
        const auto not_before = parse_asn1_time(*first);
        const auto not_after = parse_asn1_time(*second);
        if (!not_before.has_value() || !not_after.has_value()) {
            return std::unexpected(X509Error::kBadTime);
        }
        cert.not_before = *not_before;
        cert.not_after = *not_after;
    }

    {
        const auto key = parse_subject_public_key_info(spki->whole);
        if (!key.has_value()) {
            return std::unexpected(key.error());
        }
        cert.public_key = *key;
    }

    if (cert.version == 1) {
        if (!fields.empty()) {
            return std::unexpected(X509Error::kMalformed);
        }
    } else if (!fields.empty()) {
        // issuerUniqueID [1] and subjectUniqueID [2] are not read, and RFC 5280 section 4.1.2.8
        // says a conforming CA does not issue them; only extensions [3] may follow.
        const auto wrapper = detail::read_element(fields);
        if (!wrapper.has_value() || std::to_integer<std::uint8_t>(wrapper->whole[0]) != 0xA3 ||
            !fields.empty()) {
            return std::unexpected(X509Error::kMalformed);
        }
        auto extensions = parse_extensions(wrapper->content);
        if (!extensions.has_value()) {
            return std::unexpected(extensions.error());
        }
        cert.key_usage = extensions->key_usage;
        cert.basic_constraints = extensions->basic_constraints;
    }
    return cert;
}

// --- Chains --------------------------------------------------------------------------------

namespace {

bool signed_by(const Certificate& child, const P256PublicKey& issuer_key) {
    return ecdsa_verify(issuer_key, sha256(child.tbs()), child.signature);
}

bool same_anchor(const Certificate& a, const Certificate& b) {
    return a.subject.der == b.subject.der && a.public_key == b.public_key;
}

// Whether `ca` may issue a certificate that has `intermediates_below` CA certificates between it
// and the leaf.
bool may_issue(const Certificate& ca, std::size_t intermediates_below) {
    if (ca.version == 1) {
        // A v1 certificate has no extensions to say it is a CA: acceptable only as a self-issued
        // root, which is what an old trust anchor is.
        return ca.self_issued();
    }
    if (!ca.basic_constraints.has_value() || !ca.basic_constraints->ca) {
        return false;
    }
    if (ca.key_usage.has_value() && (*ca.key_usage & kKeyUsageKeyCertSign) == 0) {
        return false;
    }
    return !ca.basic_constraints->path_length.has_value() ||
           intermediates_below <= *ca.basic_constraints->path_length;
}

// The checks that follow once a place in the chain is trusted. `start` is the first certificate
// that is judged (the anchor itself when it is in the chain); `external` is an anchor outside the
// chain that issued chain[0].
std::expected<ChainAcceptance, ChainFailure> check_from(std::span<const Certificate> chain,
                                                        std::size_t start,
                                                        const Certificate* external,
                                                        const ChainPolicy& policy) {
    const std::size_t last = chain.size() - 1;
    // Structure first: links, signatures and the right to issue.
    if (external != nullptr && !may_issue(*external, chain.size() - 1)) {
        return std::unexpected(ChainFailure{ChainError::kNotCa, 0});
    }
    for (std::size_t i = start + 1; i <= last; ++i) {
        const Certificate& issuer = chain[i - 1];
        if (chain[i].issuer.der != issuer.subject.der) {
            return std::unexpected(ChainFailure{ChainError::kBrokenLink, i});
        }
        if (!signed_by(chain[i], issuer.public_key)) {
            return std::unexpected(ChainFailure{ChainError::kBadSignature, i});
        }
        if (!may_issue(issuer, last - i)) {
            return std::unexpected(ChainFailure{ChainError::kNotCa, i - 1});
        }
    }

    ChainAcceptance accepted;
    const std::int64_t now = *policy.now_unix;
    if (external != nullptr) {
        if (now < external->not_before) {
            return std::unexpected(ChainFailure{ChainError::kNotYetValid, 0});
        }
        if (now > external->not_after) {
            return std::unexpected(ChainFailure{ChainError::kExpired, 0});
        }
    }
    for (std::size_t i = external != nullptr ? 0 : start; i <= last; ++i) {
        const Certificate& cert = chain[i];
        if (now < cert.not_before) {
            return std::unexpected(ChainFailure{ChainError::kNotYetValid, i});
        }
        if (now > cert.not_after) {
            const bool intermediate = i != last && (external != nullptr || i != start);
            if (!(policy.allow_expired_intermediates && intermediate)) {
                return std::unexpected(ChainFailure{ChainError::kExpired, i});
            }
            accepted.expired_intermediates.push_back(i);
        }
    }
    return accepted;
}

}  // namespace

std::expected<ChainAcceptance, ChainFailure> validate_chain(std::span<const Certificate> chain,
                                                            const TrustStore& trust,
                                                            const ChainPolicy& policy) {
    if (!policy.now_unix.has_value()) {
        return std::unexpected(ChainFailure{ChainError::kNoClock, 0});
    }
    if (trust.empty()) {
        return std::unexpected(ChainFailure{ChainError::kNoTrust, 0});
    }
    if (chain.empty()) {
        return std::unexpected(ChainFailure{ChainError::kEmptyChain, 0});
    }

    // The first anchor that vouches for the chain and under which every check passes is the one
    // reported. If an anchor matches and a later check fails, that failure is the answer unless
    // another anchor accepts: the most specific reason is more use to a person than "untrusted".
    std::optional<ChainFailure> first_failure;
    for (std::size_t a = 0; a < trust.anchors.size(); ++a) {
        const Certificate& anchor = trust.anchors[a].certificate;
        std::optional<std::size_t> start;
        const Certificate* external = nullptr;
        for (std::size_t i = 0; i < chain.size() && !start.has_value(); ++i) {
            if (same_anchor(chain[i], anchor)) {
                start = i;
            }
        }
        if (!start.has_value() && chain[0].issuer.der == anchor.subject.der &&
            signed_by(chain[0], anchor.public_key)) {
            start = 0;
            external = &anchor;
        }
        if (!start.has_value()) {
            continue;
        }
        auto result = check_from(chain, *start, external, policy);
        if (result.has_value()) {
            result->kind = ChainAcceptance::Kind::kAnchor;
            result->trust_index = a;
            return result;
        }
        if (!first_failure.has_value()) {
            first_failure = result.error();
        }
    }

    // A trusted key vouches for the leaf that holds it. Only the leaf's own validity is judged:
    // the key is trusted, and the certificates above it are not consulted.
    const Certificate& leaf = chain.back();
    for (std::size_t k = 0; k < trust.keys.size(); ++k) {
        if (!(leaf.public_key == trust.keys[k].key)) {
            continue;
        }
        const std::int64_t now = *policy.now_unix;
        if (now < leaf.not_before) {
            return std::unexpected(ChainFailure{ChainError::kNotYetValid, chain.size() - 1});
        }
        if (now > leaf.not_after) {
            return std::unexpected(ChainFailure{ChainError::kExpired, chain.size() - 1});
        }
        ChainAcceptance accepted;
        accepted.kind = ChainAcceptance::Kind::kTrustedKey;
        accepted.trust_index = k;
        return accepted;
    }

    if (first_failure.has_value()) {
        return std::unexpected(*first_failure);
    }
    return std::unexpected(ChainFailure{ChainError::kUntrusted, 0});
}

}  // namespace iclforge::base::crypto
