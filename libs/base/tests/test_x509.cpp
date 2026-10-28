// The X.509 reader and chain validation (x509.hpp). Certificates are written by tests/support/
// test_pki.hpp, with one fault at a time where a test is about a refusal; the differential tests
// in tests/interop/ check that the same bytes are what OpenSSL reads.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/base/crypto/p256.hpp"
#include "iclforge/base/crypto/sha256.hpp"
#include "iclforge/base/crypto/x509.hpp"
#include "test_pki.hpp"

namespace {

using namespace iclforge::base::crypto;
namespace pki = iclforge::test::pki;
using pki::at;
using pki::Bytes;

constexpr std::int64_t kNow = 1793232000;  // 2026-10-29T00:00:00Z

ChainPolicy at_now() {
    ChainPolicy policy;
    policy.now_unix = kNow;
    return policy;
}

TrustStore trusting(const Certificate& anchor) {
    TrustStore store;
    store.anchors.push_back(TrustAnchor{anchor, "test"});
    return store;
}

// Replaces the first occurrence of `from` in `der` with `to` (the same length), returning whether
// it was found.
bool patch(Bytes& der, const Bytes& from, const Bytes& to) {
    REQUIRE(from.size() == to.size());
    const auto it = std::search(der.begin(), der.end(), from.begin(), from.end());
    if (it == der.end()) {
        return false;
    }
    std::copy(to.begin(), to.end(), it);
    return true;
}

Bytes raw(std::initializer_list<unsigned> values) {
    Bytes out;
    for (const unsigned v : values) out.push_back(static_cast<std::byte>(v));
    return out;
}

// One certificate with a spec of the test's own, signed by `signer`.
Bytes certificate_with(const pki::CertificateSpec& spec, const pki::KeyPair& signer) {
    return pki::build_certificate(spec, signer.key);
}

pki::CertificateSpec basic_leaf(const pki::KeyPair& subject) {
    pki::CertificateSpec spec;
    spec.issuer = pki::name_of("Issuer");
    spec.subject = pki::name_of("Leaf", "Code/Content Signer", "ABC");
    spec.not_before = at(2024, 1, 1);
    spec.not_after = at(2034, 1, 1);
    spec.subject_key = subject.pub;
    spec.key_usage_bits = {0};
    return spec;
}

}  // namespace

TEST_CASE("civil time converts to and from Unix seconds", "[crypto][x509][time]") {
    CHECK(unix_time_from_civil(1970, 1, 1, 0, 0, 0) == 0);
    CHECK(unix_time_from_civil(2000, 1, 1, 0, 0, 0) == 946684800);
    CHECK(unix_time_from_civil(2038, 1, 19, 3, 14, 7) == 2147483647);
    CHECK(unix_time_from_civil(2024, 2, 29, 12, 0, 0) == 1709208000);
    CHECK(unix_time_from_civil(2026, 10, 29, 0, 0, 0) == kNow);
    CHECK(unix_time_from_civil(2100, 3, 1, 0, 0, 0) == 4107542400);
    CHECK(unix_time_from_civil(9999, 12, 31, 23, 59, 59) == 253402300799);
    CHECK(unix_time_from_civil(1969, 12, 31, 23, 59, 59) == -1);
    CHECK(unix_time_from_civil(1950, 1, 1, 0, 0, 0) == -631152000);

    SECTION("impossible dates are refused") {
        CHECK_FALSE(unix_time_from_civil(2023, 2, 29, 0, 0, 0).has_value());  // not a leap year
        CHECK_FALSE(unix_time_from_civil(2100, 2, 29, 0, 0, 0).has_value());  // century, not leap
        CHECK(unix_time_from_civil(2000, 2, 29, 0, 0, 0).has_value());        // 400-year, leap
        CHECK_FALSE(unix_time_from_civil(2024, 13, 1, 0, 0, 0).has_value());
        CHECK_FALSE(unix_time_from_civil(2024, 0, 1, 0, 0, 0).has_value());
        CHECK_FALSE(unix_time_from_civil(2024, 4, 31, 0, 0, 0).has_value());
        CHECK_FALSE(unix_time_from_civil(2024, 1, 0, 0, 0, 0).has_value());
        CHECK_FALSE(unix_time_from_civil(2024, 1, 1, 24, 0, 0).has_value());
        CHECK_FALSE(unix_time_from_civil(2024, 1, 1, 0, 60, 0).has_value());
        CHECK_FALSE(unix_time_from_civil(2024, 1, 1, 0, 0, 60).has_value());
        CHECK_FALSE(unix_time_from_civil(10000, 1, 1, 0, 0, 0).has_value());
        CHECK_FALSE(unix_time_from_civil(-1, 1, 1, 0, 0, 0).has_value());
    }
    SECTION("ISO 8601 formatting and parsing agree") {
        for (const std::int64_t t :
             {std::int64_t{0}, std::int64_t{-1}, kNow, std::int64_t{1709208000},
              std::int64_t{253402300799}, std::int64_t{-631152000}}) {
            const std::string text = format_iso8601_utc(t);
            CHECK(parse_iso8601_utc(text) == t);
        }
        CHECK(format_iso8601_utc(kNow) == "2026-10-29T00:00:00Z");
        CHECK(format_iso8601_utc(0) == "1970-01-01T00:00:00Z");
        CHECK(format_iso8601_utc(-1) == "1969-12-31T23:59:59Z");
        CHECK(format_iso8601_utc(1709208000) == "2024-02-29T12:00:00Z");
    }
    SECTION("ISO 8601 parsing is strict about the form") {
        CHECK(parse_iso8601_utc("2026-10-29T00:00:00Z") == kNow);
        CHECK(parse_iso8601_utc("2026-10-29T00:00:00.123Z") == kNow);  // fraction ignored
        CHECK_FALSE(parse_iso8601_utc("2026-10-29T00:00:00").has_value());
        CHECK_FALSE(parse_iso8601_utc("2026-10-29T00:00:00+00:00").has_value());
        CHECK_FALSE(parse_iso8601_utc("2026-10-29 00:00:00Z").has_value());
        CHECK_FALSE(parse_iso8601_utc("2026-10-29T00:00:00.Z").has_value());
        CHECK_FALSE(parse_iso8601_utc("2026-10-29T00:00:00.1x2Z").has_value());
        CHECK_FALSE(parse_iso8601_utc("2026-13-29T00:00:00Z").has_value());
        CHECK_FALSE(parse_iso8601_utc("2026-10-29T25:00:00Z").has_value());
        CHECK_FALSE(parse_iso8601_utc("20261029T000000Z").has_value());
        CHECK_FALSE(parse_iso8601_utc("").has_value());
        CHECK_FALSE(parse_iso8601_utc("Z").has_value());
        CHECK_FALSE(parse_iso8601_utc("2026-1a-29T00:00:00Z").has_value());
    }
}

TEST_CASE("a certificate is read field by field", "[crypto][x509]") {
    const auto authority = pki::make_authority();
    const Certificate& leaf = authority.leaf.cert;
    const Certificate& root = authority.root.cert;
    const Certificate& mid = authority.intermediate.cert;

    CHECK(leaf.version == 3);
    CHECK(leaf.serial_hex() == "1798A2B5D62000000005");
    CHECK(leaf.serial.size() == 10);
    CHECK(leaf.subject.find(kOidCommonName) == "ILok");
    CHECK(leaf.subject.find(kOidTitle) == "Code/Content Signer");
    CHECK(leaf.subject.find(kOidSerialNumber) == "1798A2B5D62000000005");
    CHECK_FALSE(leaf.subject.find("2.5.4.10").has_value());
    CHECK(leaf.issuer.find(kOidCommonName) == "Test Signing CA");
    CHECK(leaf.not_before == at(2024, 1, 1));
    CHECK(leaf.not_after == at(2034, 1, 1));
    CHECK(leaf.public_key == authority.leaf.keys.pub);
    REQUIRE(leaf.key_usage.has_value());
    CHECK(*leaf.key_usage == kKeyUsageDigitalSignature);
    CHECK_FALSE(leaf.basic_constraints.has_value());
    CHECK_FALSE(leaf.self_issued());

    REQUIRE(mid.basic_constraints.has_value());
    CHECK(mid.basic_constraints->ca);
    CHECK_FALSE(mid.basic_constraints->path_length.has_value());
    REQUIRE(mid.key_usage.has_value());
    CHECK((*mid.key_usage & kKeyUsageKeyCertSign) != 0);
    CHECK((*mid.key_usage & kKeyUsageDigitalSignature) == 0);

    CHECK(root.self_issued());
    CHECK(root.subject.to_string() == "CN=Test Master CA");
    CHECK(leaf.subject.to_string() ==
          "serialNumber=1798A2B5D62000000005, title=Code/Content Signer, CN=ILok");

    SECTION("fingerprints are colon separated uppercase hexadecimal of the DER") {
        const std::string sha1 = leaf.sha1_fingerprint();
        CHECK(sha1.size() == 20 * 3 - 1);
        CHECK(sha1[2] == ':');
        const std::string sha256 = leaf.sha256_fingerprint();
        CHECK(sha256.size() == 32 * 3 - 1);
        CHECK(sha256 != sha1);
        CHECK(root.sha1_fingerprint() != leaf.sha1_fingerprint());
        for (const char c : sha1) {
            CHECK(((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || c == ':'));
        }
    }
    SECTION("the signed bytes are the TBS exactly as written") {
        const auto tbs = leaf.tbs();
        REQUIRE(tbs.size() > 100);
        CHECK(tbs[0] == std::byte{0x30});
        // a slice of the DER itself, not a copy
        CHECK(tbs.data() > leaf.der.data());
        CHECK(tbs.data() + tbs.size() < leaf.der.data() + leaf.der.size());
        CHECK(ecdsa_verify(mid.public_key, sha256(tbs), leaf.signature));
        CHECK_FALSE(ecdsa_verify(root.public_key, sha256(tbs), leaf.signature));
    }
}

TEST_CASE("version 1 and long validity periods", "[crypto][x509]") {
    pki::AuthorityOptions options;
    options.root_version = 1;
    options.root_not_before = at(1950, 1, 1);
    options.root_not_after = at(2100, 1, 1);  // GeneralizedTime
    const auto authority = pki::make_authority(options);
    CHECK(authority.root.cert.version == 1);
    CHECK_FALSE(authority.root.cert.key_usage.has_value());
    CHECK_FALSE(authority.root.cert.basic_constraints.has_value());
    CHECK(authority.root.cert.not_before == at(1950, 1, 1));
    CHECK(authority.root.cert.not_after == at(2100, 1, 1));
    // the two-digit year pivot: 49 is 2049, 50 is 1950
    pki::AuthorityOptions edge;
    edge.leaf_not_before = at(2049, 12, 31, 23, 59, 59);
    edge.leaf_not_after = at(2050, 1, 1);
    const auto e = pki::make_authority(edge);
    CHECK(e.leaf.cert.not_before == at(2049, 12, 31, 23, 59, 59));
    CHECK(e.leaf.cert.not_after == at(2050, 1, 1));
}

TEST_CASE("a certificate that is not well-formed is refused with the reason", "[crypto][x509]") {
    const auto authority = pki::make_authority();
    const Bytes good = authority.leaf.der;
    REQUIRE(parse_certificate(good).has_value());

    const auto reason = [](const Bytes& der) {
        const auto parsed = parse_certificate(der);
        REQUIRE_FALSE(parsed.has_value());
        return parsed.error();
    };

    SECTION("empty and truncated input, at every length") {
        CHECK(reason({}) == X509Error::kMalformed);
        for (std::size_t length = 0; length < good.size(); ++length) {
            CHECK_FALSE(parse_certificate(std::span{good}.first(length)).has_value());
        }
    }
    SECTION("bytes after the certificate") {
        Bytes more = good;
        more.push_back(std::byte{0});
        CHECK(reason(more) == X509Error::kTrailingData);
    }
    SECTION("an indefinite length at the top") {
        Bytes bad = good;
        bad[1] = std::byte{0x80};
        CHECK(reason(bad) == X509Error::kMalformed);
    }
    SECTION("not a SEQUENCE") {
        Bytes bad = good;
        bad[0] = std::byte{0x31};
        CHECK(reason(bad) == X509Error::kMalformed);
    }
    SECTION("a version other than v1 and v3") {
        Bytes bad = good;
        REQUIRE(
            patch(bad, raw({0xA0, 0x03, 0x02, 0x01, 0x02}), raw({0xA0, 0x03, 0x02, 0x01, 0x01})));
        CHECK(reason(bad) == X509Error::kUnsupportedVersion);
        bad = good;
        REQUIRE(
            patch(bad, raw({0xA0, 0x03, 0x02, 0x01, 0x02}), raw({0xA0, 0x03, 0x02, 0x01, 0x00})));
        CHECK(reason(bad) == X509Error::kMalformed);  // v1 is written without the field
    }
    SECTION("a signature algorithm other than ecdsa-with-SHA256") {
        Bytes bad = good;
        // 1.2.840.10045.4.3.2 becomes 1.2.840.10045.4.3.3 (SHA-384) in the first place it appears
        // (inside the TBS); the outer copy then disagrees with it.
        REQUIRE(patch(bad, raw({0x06, 0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02}),
                      raw({0x06, 0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x03})));
        CHECK(reason(bad) == X509Error::kUnsupportedSignature);
    }
    SECTION("a key on another curve") {
        Bytes bad = good;
        REQUIRE(patch(bad, raw({0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07}),
                      raw({0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x06})));
        CHECK(reason(bad) == X509Error::kUnsupportedKey);
    }
    SECTION("a public key that is not on the curve") {
        Bytes bad = good;
        // the last byte of the SPKI point sits before the extensions; find the point by its prefix
        const auto point = authority.leaf.keys.pub.uncompressed();
        Bytes needle(point.begin(), point.end());
        Bytes changed = needle;
        changed.back() = static_cast<std::byte>(std::to_integer<unsigned>(changed.back()) ^ 1u);
        REQUIRE(patch(bad, needle, changed));
        CHECK(reason(bad) == X509Error::kKeyNotOnCurve);
    }
    SECTION("a compressed point is not read") {
        Bytes bad = good;
        const auto point = authority.leaf.keys.pub.uncompressed();
        Bytes needle(point.begin(), point.end());
        Bytes changed = needle;
        changed[0] = std::byte{0x02};
        REQUIRE(patch(bad, needle, changed));
        const auto error = reason(bad);
        CHECK((error == X509Error::kKeyNotOnCurve || error == X509Error::kUnsupportedKey));
    }
    SECTION("a signature BIT STRING with unused bits") {
        Bytes bad = good;
        // The outer BIT STRING is the last element: its unused-bits byte is 00, three bytes
        // before the signature body begins. Find it from the end: tag 03, length, 00, 30.
        for (std::size_t i = bad.size() - 1; i > 3; --i) {
            if (bad[i - 3] == std::byte{0x03} && bad[i - 1] == std::byte{0x00} &&
                bad[i] == std::byte{0x30}) {
                bad[i - 1] = std::byte{0x01};
                break;
            }
        }
        CHECK(reason(bad) == X509Error::kMalformed);
    }
    SECTION("a time that is not a date") {
        Bytes bad = good;
        REQUIRE(patch(bad, raw({'2', '4', '0', '1', '0', '1', '0', '0', '0', '0', '0', '0', 'Z'}),
                      raw({'2', '4', '1', '3', '0', '1', '0', '0', '0', '0', '0', '0', 'Z'})));
        CHECK(reason(bad) == X509Error::kBadTime);
        bad = good;
        REQUIRE(patch(bad, raw({'2', '4', '0', '1', '0', '1', '0', '0', '0', '0', '0', '0', 'Z'}),
                      raw({'2', '4', '0', '1', '0', '1', '0', '0', '0', '0', '0', 'x', 'Z'})));
        CHECK(reason(bad) == X509Error::kBadTime);
        bad = good;
        REQUIRE(patch(bad, raw({'2', '4', '0', '1', '0', '1', '0', '0', '0', '0', '0', '0', 'Z'}),
                      raw({'2', '4', '0', '1', '0', '1', '0', '0', '0', '0', '0', '0', '+'})));
        CHECK(reason(bad) == X509Error::kBadTime);
    }
}

TEST_CASE("serial numbers", "[crypto][x509]") {
    const auto keys = pki::key_from_seed("serial");
    const auto issuer = pki::key_from_seed("serial-issuer");

    SECTION("a high bit in the first byte is written with a sign byte and read without") {
        auto spec = basic_leaf(keys);
        spec.serial = {std::byte{0x80}, std::byte{0x01}};
        const auto cert = parse_certificate(certificate_with(spec, issuer));
        REQUIRE(cert.has_value());
        CHECK(cert->serial_hex() == "8001");
    }
    SECTION("twenty bytes are the most") {
        auto spec = basic_leaf(keys);
        spec.serial.assign(20, std::byte{0x11});
        CHECK(parse_certificate(certificate_with(spec, issuer)).has_value());
        spec.serial.assign(21, std::byte{0x11});
        const auto cert = parse_certificate(certificate_with(spec, issuer));
        REQUIRE_FALSE(cert.has_value());
        CHECK(cert.error() == X509Error::kBadSerial);
    }
    SECTION("a negative serial") {
        auto spec = basic_leaf(keys);
        spec.raw_serial_tlv = raw({0x02, 0x01, 0x80});
        const auto cert = parse_certificate(certificate_with(spec, issuer));
        REQUIRE_FALSE(cert.has_value());
        CHECK(cert.error() == X509Error::kBadSerial);
    }
    SECTION("an empty serial") {
        auto spec = basic_leaf(keys);
        spec.raw_serial_tlv = raw({0x02, 0x00});
        const auto cert = parse_certificate(certificate_with(spec, issuer));
        REQUIRE_FALSE(cert.has_value());
        CHECK(cert.error() == X509Error::kBadSerial);
    }
    SECTION("a redundant leading zero is not DER") {
        auto spec = basic_leaf(keys);
        spec.raw_serial_tlv = raw({0x02, 0x02, 0x00, 0x01});
        const auto cert = parse_certificate(certificate_with(spec, issuer));
        REQUIRE_FALSE(cert.has_value());
        CHECK(cert.error() == X509Error::kMalformed);
    }
    SECTION("zero is a serial that reads as 00") {
        auto spec = basic_leaf(keys);
        spec.raw_serial_tlv = raw({0x02, 0x01, 0x00});
        const auto cert = parse_certificate(certificate_with(spec, issuer));
        REQUIRE(cert.has_value());
        CHECK(cert->serial_hex() == "00");
    }
}

TEST_CASE("extensions", "[crypto][x509]") {
    const auto keys = pki::key_from_seed("extensions");
    const auto issuer = pki::key_from_seed("extensions-issuer");

    SECTION("an unknown extension that is not critical is ignored") {
        auto spec = basic_leaf(keys);
        spec.extra_extensions.push_back({"1.2.3.4.5", false, pki::octet_string(Bytes{})});
        CHECK(parse_certificate(certificate_with(spec, issuer)).has_value());
    }
    SECTION("an unknown critical extension is refused") {
        auto spec = basic_leaf(keys);
        spec.extra_extensions.push_back({"1.2.3.4.5", true, pki::octet_string(Bytes{})});
        const auto cert = parse_certificate(certificate_with(spec, issuer));
        REQUIRE_FALSE(cert.has_value());
        CHECK(cert.error() == X509Error::kUnknownCriticalExtension);
    }
    SECTION("an extension repeated") {
        auto spec = basic_leaf(keys);
        spec.extra_extensions.push_back({"2.5.29.15", true, pki::key_usage_value({0})});
        const auto cert = parse_certificate(certificate_with(spec, issuer));
        REQUIRE_FALSE(cert.has_value());
        CHECK(cert.error() == X509Error::kDuplicateExtension);
    }
    SECTION("key usage bits are read by number") {
        auto spec = basic_leaf(keys);
        spec.key_usage_bits = {0, 1, 5, 6, 8};
        const auto cert = parse_certificate(certificate_with(spec, issuer));
        REQUIRE(cert.has_value());
        REQUIRE(cert->key_usage.has_value());
        CHECK(*cert->key_usage == ((1u << 0) | (1u << 1) | (1u << 5) | (1u << 6) | (1u << 8)));
    }
    SECTION("basicConstraints with a path length") {
        auto spec = basic_leaf(keys);
        spec.ca = true;
        spec.path_length = 3;
        const auto cert = parse_certificate(certificate_with(spec, issuer));
        REQUIRE(cert.has_value());
        REQUIRE(cert->basic_constraints.has_value());
        CHECK(cert->basic_constraints->ca);
        CHECK(cert->basic_constraints->path_length == 3u);
    }
    SECTION("garbage after the extensions inside the TBS") {
        auto spec = basic_leaf(keys);
        spec.trailing_in_tbs = raw({0x05, 0x00});
        const auto cert = parse_certificate(certificate_with(spec, issuer));
        REQUIRE_FALSE(cert.has_value());
        CHECK(cert.error() == X509Error::kMalformed);
    }
    SECTION("a v1 certificate has no extensions, and one that claims some is malformed") {
        auto spec = basic_leaf(keys);
        spec.version = 1;
        spec.key_usage_bits.clear();
        const auto cert = parse_certificate(certificate_with(spec, issuer));
        REQUIRE(cert.has_value());
        CHECK(cert->version == 1);
        spec.trailing_in_tbs = raw({0x05, 0x00});
        CHECK_FALSE(parse_certificate(certificate_with(spec, issuer)).has_value());
    }
}

TEST_CASE("names", "[crypto][x509]") {
    const auto keys = pki::key_from_seed("names");
    const auto issuer = pki::key_from_seed("names-issuer");
    auto spec = basic_leaf(keys);

    SECTION("a printable string and a string type this reader does not decode") {
        spec.subject = {{"2.5.4.3", "Printable", 0x13}, {"2.5.4.10", "AB", 0x1E}};
        const auto cert = parse_certificate(certificate_with(spec, issuer));
        REQUIRE(cert.has_value());
        CHECK(cert->subject.find(kOidCommonName) == "Printable");
        // an undecoded string is shown as '#' and the hexadecimal of its DER, never as text
        const auto organisation = cert->subject.find("2.5.4.10");
        REQUIRE(organisation.has_value());
        CHECK(organisation->substr(0, 1) == "#");
    }
    SECTION("the issuer link compares the bytes of the name, not its text") {
        // 'Same' as a UTF8String and as a PrintableString read the same and are different names.
        pki::CertificateSpec a = basic_leaf(keys);
        a.subject = {{"2.5.4.3", "Same", 0x0C}};
        pki::CertificateSpec b = basic_leaf(keys);
        b.subject = {{"2.5.4.3", "Same", 0x13}};
        const auto ca = parse_certificate(certificate_with(a, issuer));
        const auto cb = parse_certificate(certificate_with(b, issuer));
        REQUIRE(ca.has_value());
        REQUIRE(cb.has_value());
        CHECK(ca->subject.find(kOidCommonName) == cb->subject.find(kOidCommonName));
        CHECK(ca->subject.der != cb->subject.der);
    }
}

TEST_CASE("a SubjectPublicKeyInfo is read", "[crypto][x509]") {
    const auto keys = pki::key_from_seed("spki");
    const Bytes der = pki::spki_der(keys.pub);
    const auto parsed = parse_subject_public_key_info(der);
    REQUIRE(parsed.has_value());
    CHECK(*parsed == keys.pub);

    Bytes more = der;
    more.push_back(std::byte{0});
    CHECK(parse_subject_public_key_info(more).error() == X509Error::kTrailingData);
    for (std::size_t length = 0; length < der.size(); ++length) {
        CHECK_FALSE(parse_subject_public_key_info(std::span{der}.first(length)).has_value());
    }
    Bytes other_curve = der;
    REQUIRE(patch(other_curve, raw({0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07}),
                  raw({0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x01})));
    CHECK(parse_subject_public_key_info(other_curve).error() == X509Error::kUnsupportedKey);
    Bytes rsa = der;
    REQUIRE(patch(rsa, raw({0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01}),
                  raw({0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01})));
    CHECK(parse_subject_public_key_info(rsa).error() == X509Error::kUnsupportedKey);
}

// --- Chains ---------------------------------------------------------------------------------

TEST_CASE("a chain to a trusted root is accepted", "[crypto][x509][chain]") {
    const auto a = pki::make_authority();
    const auto chain = a.chain();

    SECTION("the whole chain, root anchor") {
        const auto result = validate_chain(chain, trusting(a.root.cert), at_now());
        REQUIRE(result.has_value());
        CHECK(result->kind == ChainAcceptance::Kind::kAnchor);
        CHECK(result->trust_index == 0);
        CHECK(result->expired_intermediates.empty());
    }
    SECTION("a chain without its root, the root held as the anchor") {
        const std::vector<Certificate> shorter{a.intermediate.cert, a.leaf.cert};
        CHECK(validate_chain(shorter, trusting(a.root.cert), at_now()).has_value());
    }
    SECTION("an intermediate as the anchor, whether or not the chain carries the root") {
        CHECK(validate_chain(chain, trusting(a.intermediate.cert), at_now()).has_value());
        const std::vector<Certificate> shorter{a.intermediate.cert, a.leaf.cert};
        CHECK(validate_chain(shorter, trusting(a.intermediate.cert), at_now()).has_value());
        const std::vector<Certificate> leaf_only{a.leaf.cert};
        CHECK(validate_chain(leaf_only, trusting(a.intermediate.cert), at_now()).has_value());
    }
    SECTION("the leaf itself as the anchor") {
        const std::vector<Certificate> leaf_only{a.leaf.cert};
        CHECK(validate_chain(leaf_only, trusting(a.leaf.cert), at_now()).has_value());
    }
    SECTION("a version 1 self-signed root") {
        pki::AuthorityOptions options;
        options.root_version = 1;
        const auto v1 = pki::make_authority(options);
        CHECK(validate_chain(v1.chain(), trusting(v1.root.cert), at_now()).has_value());
    }
    SECTION("the first anchor that accepts is the one reported") {
        const auto other = pki::make_authority({.seed = "other"});
        TrustStore store;
        store.anchors.push_back(TrustAnchor{other.root.cert, "other"});
        store.anchors.push_back(TrustAnchor{a.root.cert, "right"});
        const auto result = validate_chain(chain, store, at_now());
        REQUIRE(result.has_value());
        CHECK(result->trust_index == 1);
    }
    SECTION("an anchor is identified by subject and key, not by the bytes of its certificate") {
        // The same CA re-issued: same name, same key, a different serial and validity.
        pki::CertificateSpec reissue;
        reissue.serial = {std::byte{0x09}};
        reissue.issuer = reissue.subject = pki::name_of("Test Master CA");
        reissue.not_before = at(2025, 1, 1);
        reissue.not_after = at(2035, 1, 1);
        reissue.subject_key = a.root.keys.pub;
        reissue.ca = true;
        reissue.key_usage_bits = {5, 6};
        const auto reissued = pki::parse_or_die(pki::build_certificate(reissue, a.root.keys.key));
        CHECK(validate_chain(chain, trusting(reissued), at_now()).has_value());
    }
}

TEST_CASE("a trusted key accepts the signer that holds it", "[crypto][x509][chain]") {
    const auto a = pki::make_authority();
    TrustStore store;
    store.keys.push_back(TrustedKey{a.leaf.keys.pub, "leaf key"});

    SECTION("the chain need not lead anywhere") {
        const std::vector<Certificate> leaf_only{a.leaf.cert};
        const auto result = validate_chain(leaf_only, store, at_now());
        REQUIRE(result.has_value());
        CHECK(result->kind == ChainAcceptance::Kind::kTrustedKey);
        CHECK(result->trust_index == 0);
    }
    SECTION("the leaf's own validity is still judged") {
        const auto expired = pki::make_authority({.leaf_not_after = at(2025, 1, 1)});
        TrustStore s;
        s.keys.push_back(TrustedKey{expired.leaf.keys.pub, "k"});
        const std::vector<Certificate> leaf_only{expired.leaf.cert};
        const auto result = validate_chain(leaf_only, s, at_now());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == ChainError::kExpired);
    }
    SECTION("another key trusts nothing") {
        TrustStore s;
        s.keys.push_back(TrustedKey{a.root.keys.pub, "root key"});
        const std::vector<Certificate> leaf_only{a.leaf.cert};
        const auto result = validate_chain(leaf_only, s, at_now());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == ChainError::kUntrusted);
    }
}

TEST_CASE("a chain that must not be accepted is refused for the right reason",
          "[crypto][x509][chain]") {
    const auto a = pki::make_authority();
    const auto chain = a.chain();
    const auto expect = [&](std::span<const Certificate> c, const TrustStore& t,
                            const ChainPolicy& p, ChainError error, std::size_t index = 0) {
        const auto result = validate_chain(c, t, p);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == error);
        CHECK(result.error().index == index);
    };

    SECTION("no clock, no trust, no chain") {
        expect(chain, trusting(a.root.cert), ChainPolicy{}, ChainError::kNoClock);
        expect(chain, TrustStore{}, at_now(), ChainError::kNoTrust);
        expect({}, trusting(a.root.cert), at_now(), ChainError::kEmptyChain);
    }
    SECTION("the root of another authority") {
        const auto other = pki::make_authority({.seed = "other"});
        expect(chain, trusting(other.root.cert), at_now(), ChainError::kUntrusted);
    }
    SECTION("a CA of the same name with another key") {
        // Same subject as the real root, so the name link holds; the key differs, so the
        // signature under it does not, and the anchor is not the issuer.
        const auto other = pki::make_authority({.seed = "imposter"});
        expect(chain, trusting(other.root.cert), at_now(), ChainError::kUntrusted);
        const std::vector<Certificate> shorter{a.intermediate.cert, a.leaf.cert};
        expect(shorter, trusting(other.root.cert), at_now(), ChainError::kUntrusted);
    }
    SECTION("a leaf without its issuers, the root held as the anchor") {
        const std::vector<Certificate> leaf_only{a.leaf.cert};
        expect(leaf_only, trusting(a.root.cert), at_now(), ChainError::kUntrusted);
    }
    SECTION("a link missing in the middle") {
        const std::vector<Certificate> gap{a.root.cert, a.leaf.cert};
        expect(gap, trusting(a.root.cert), at_now(), ChainError::kBrokenLink, 1);
    }
    SECTION("the chain in the wrong order") {
        const std::vector<Certificate> reversed{a.leaf.cert, a.intermediate.cert, a.root.cert};
        expect(reversed, trusting(a.intermediate.cert), at_now(), ChainError::kBrokenLink, 2);
        // Under the root it ends at the anchor itself, which is then the signer: a chain that
        // is accepted as "the anchor's own key", and the content signature then has to verify
        // under the root's private key, which only its holder can do.
        const auto result = validate_chain(reversed, trusting(a.root.cert), at_now());
        REQUIRE(result.has_value());
    }
    SECTION("a leaf signature that has been altered") {
        auto bad = chain;
        Bytes der = a.leaf.der;
        der[der.size() - 3] ^= std::byte{0x01};  // inside the signature value
        bad[2] = pki::parse_or_die(der);
        expect(bad, trusting(a.root.cert), at_now(), ChainError::kBadSignature, 2);
    }
    SECTION("an intermediate whose serial has been altered, so only its signature is wrong") {
        auto bad = chain;
        Bytes der = a.intermediate.der;
        // serial 02 sits right after the version field: A0 03 02 01 02 | 02 01 02
        REQUIRE(patch(der, raw({0xA0, 0x03, 0x02, 0x01, 0x02, 0x02, 0x01, 0x02}),
                      raw({0xA0, 0x03, 0x02, 0x01, 0x02, 0x02, 0x01, 0x03})));
        bad[1] = pki::parse_or_die(der);
        expect(bad, trusting(a.root.cert), at_now(), ChainError::kBadSignature, 1);
    }
    SECTION("a leaf used as an issuer") {
        const auto extra_keys = pki::key_from_seed("below-the-leaf");
        pki::CertificateSpec spec = basic_leaf(extra_keys);
        spec.issuer = pki::name_of("ILok", "Code/Content Signer", "1798A2B5D62000000005");
        const auto below = pki::parse_or_die(pki::build_certificate(spec, a.leaf.keys.key));
        auto longer = chain;
        longer.push_back(below);
        expect(longer, trusting(a.root.cert), at_now(), ChainError::kNotCa, 2);
    }
    SECTION("an intermediate that is not a CA") {
        const auto weak = pki::make_authority({.intermediate_is_ca = false});
        expect(weak.chain(), trusting(weak.root.cert), at_now(), ChainError::kNotCa, 1);
    }
    SECTION("an intermediate without keyCertSign") {
        const auto weak = pki::make_authority({.intermediate_key_usage = {0, 6}});
        expect(weak.chain(), trusting(weak.root.cert), at_now(), ChainError::kNotCa, 1);
    }
    SECTION("a path length the chain exceeds") {
        const auto limited = pki::make_authority({.root_path_length = 0});
        // root (pathlen 0) -> intermediate CA -> leaf: one CA below the root
        expect(limited.chain(), trusting(limited.root.cert), at_now(), ChainError::kNotCa, 0);
        // pathlen 1 allows it
        const auto allowed = pki::make_authority({.root_path_length = 1});
        CHECK(validate_chain(allowed.chain(), trusting(allowed.root.cert), at_now()).has_value());
        // intermediate pathlen 0 allows the leaf below it
        const auto mid_zero = pki::make_authority({.intermediate_path_length = 0});
        CHECK(validate_chain(mid_zero.chain(), trusting(mid_zero.root.cert), at_now()).has_value());
    }
    SECTION("a version 1 certificate in the middle is not a CA") {
        // A v1 intermediate carries no basicConstraints, so nothing says it may issue.
        pki::CertificateSpec spec;
        spec.version = 1;
        spec.serial = {std::byte{0x07}};
        spec.issuer = pki::name_of("Test Master CA");
        spec.subject = pki::name_of("Test Signing CA");
        spec.not_before = at(2024, 1, 1);
        spec.not_after = at(2034, 1, 1);
        spec.subject_key = a.intermediate.keys.pub;
        auto bad = chain;
        bad[1] = pki::parse_or_die(pki::build_certificate(spec, a.root.keys.key));
        expect(bad, trusting(a.root.cert), at_now(), ChainError::kNotCa, 1);
    }
}

TEST_CASE("validity periods are judged at the time the caller gives", "[crypto][x509][chain]") {
    SECTION("an expired leaf") {
        const auto a = pki::make_authority({.leaf_not_after = at(2025, 1, 1)});
        const auto result = validate_chain(a.chain(), trusting(a.root.cert), at_now());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == ChainError::kExpired);
        CHECK(result.error().index == 2);
        // and the same chain is fine when the clock is set back
        ChainPolicy earlier;
        earlier.now_unix = at(2024, 6, 1);
        CHECK(validate_chain(a.chain(), trusting(a.root.cert), earlier).has_value());
    }
    SECTION("a leaf that is not valid yet") {
        const auto a = pki::make_authority({.leaf_not_before = at(2027, 1, 1)});
        const auto result = validate_chain(a.chain(), trusting(a.root.cert), at_now());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == ChainError::kNotYetValid);
        CHECK(result.error().index == 2);
    }
    SECTION("the first and last second of a validity period are inside it") {
        const auto a = pki::make_authority();
        ChainPolicy first;
        first.now_unix = at(2024, 1, 1);
        ChainPolicy last;
        last.now_unix = at(2034, 1, 1);
        ChainPolicy before;
        before.now_unix = at(2024, 1, 1) - 1;
        ChainPolicy after;
        after.now_unix = at(2034, 1, 1) + 1;
        CHECK(validate_chain(a.chain(), trusting(a.root.cert), first).has_value());
        CHECK(validate_chain(a.chain(), trusting(a.root.cert), last).has_value());
        CHECK_FALSE(validate_chain(a.chain(), trusting(a.root.cert), before).has_value());
        CHECK_FALSE(validate_chain(a.chain(), trusting(a.root.cert), after).has_value());
    }
    SECTION("an expired root as the anchor, as the specification's own chain is today") {
        const auto a = pki::make_authority({.root_not_after = at(2025, 11, 13)});
        const auto strict = validate_chain(a.chain(), trusting(a.root.cert), at_now());
        REQUIRE_FALSE(strict.has_value());
        CHECK(strict.error().error == ChainError::kExpired);
        CHECK(strict.error().index == 0);

        // The relaxation covers an intermediate, not the anchor.
        ChainPolicy relaxed = at_now();
        relaxed.allow_expired_intermediates = true;
        const auto still = validate_chain(a.chain(), trusting(a.root.cert), relaxed);
        REQUIRE_FALSE(still.has_value());
        CHECK(still.error().error == ChainError::kExpired);

        // Anchoring on the intermediate, which has not expired, is what the operator does.
        CHECK(validate_chain(a.chain(), trusting(a.intermediate.cert), at_now()).has_value());
    }
    SECTION("an expired intermediate: strict refuses, the relaxation accepts and says so") {
        const auto a = pki::make_authority({.intermediate_not_after = at(2025, 11, 13)});
        const auto strict = validate_chain(a.chain(), trusting(a.root.cert), at_now());
        REQUIRE_FALSE(strict.has_value());
        CHECK(strict.error().error == ChainError::kExpired);
        CHECK(strict.error().index == 1);

        ChainPolicy relaxed = at_now();
        relaxed.allow_expired_intermediates = true;
        const auto result = validate_chain(a.chain(), trusting(a.root.cert), relaxed);
        REQUIRE(result.has_value());
        CHECK(result->expired_intermediates == std::vector<std::size_t>{1});

        // An intermediate held outside the chain, the anchor's child, is relaxed the same way.
        const std::vector<Certificate> shorter{a.intermediate.cert, a.leaf.cert};
        const auto external = validate_chain(shorter, trusting(a.root.cert), relaxed);
        REQUIRE(external.has_value());
        CHECK(external->expired_intermediates == std::vector<std::size_t>{0});
    }
    SECTION(
        "the relaxation does not excuse a leaf, a not-yet-valid intermediate or a bad signature") {
        ChainPolicy relaxed = at_now();
        relaxed.allow_expired_intermediates = true;

        const auto leaf = pki::make_authority({.leaf_not_after = at(2025, 1, 1)});
        CHECK(validate_chain(leaf.chain(), trusting(leaf.root.cert), relaxed).error().error ==
              ChainError::kExpired);

        const auto early = pki::make_authority({.intermediate_not_before = at(2027, 1, 1)});
        CHECK(validate_chain(early.chain(), trusting(early.root.cert), relaxed).error().error ==
              ChainError::kNotYetValid);

        auto tampered = pki::make_authority({.intermediate_not_after = at(2025, 11, 13)});
        auto chain = tampered.chain();
        Bytes der = tampered.intermediate.der;
        der[der.size() - 3] ^= std::byte{0x01};
        chain[1] = pki::parse_or_die(der);
        CHECK(validate_chain(chain, trusting(tampered.root.cert), relaxed).error().error ==
              ChainError::kBadSignature);
    }
    SECTION("an expired anchor is refused even when the chain is relaxed") {
        const auto a = pki::make_authority({.intermediate_not_after = at(2025, 11, 13)});
        ChainPolicy relaxed = at_now();
        relaxed.allow_expired_intermediates = true;
        // anchor = the expired intermediate itself
        const auto result = validate_chain(a.chain(), trusting(a.intermediate.cert), relaxed);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().error == ChainError::kExpired);
    }
}

TEST_CASE("trusting an intermediate does not trust another authority", "[crypto][x509][chain]") {
    const auto a = pki::make_authority();
    const auto other = pki::make_authority({.seed = "elsewhere"});
    const auto result = validate_chain(other.chain(), trusting(a.intermediate.cert), at_now());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().error == ChainError::kUntrusted);
}
