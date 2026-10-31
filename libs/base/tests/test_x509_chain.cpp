#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "crypto_test_support.hpp"
#include "iclforge/base/crypto/chain.hpp"
#include "iclforge/base/crypto/der.hpp"
#include "iclforge/base/crypto/ecdsa_p256.hpp"
#include "iclforge/base/crypto/pem.hpp"
#include "iclforge/base/crypto/sha256.hpp"
#include "iclforge/base/crypto/x509.hpp"
#include "test_pki.hpp"

namespace {

namespace crypto = iclforge::base::crypto;
namespace pki = iclforge::test::pki;
using iclforge::test::crypto::from_hex;
using iclforge::test::crypto::to_hex;

std::vector<crypto::Certificate> parse_all(const pki::TestChain& chain) {
    std::vector<crypto::Certificate> out;
    for (const pki::Bytes& der : chain.certificates) {
        auto cert = crypto::parse_certificate(der);
        REQUIRE(cert.has_value());
        out.push_back(*cert);
    }
    return out;
}

}  // namespace

// --- DER ----------------------------------------------------------------------
TEST_CASE("DER reader accepts the canonical form and refuses the rest", "[crypto][der]") {
    namespace der = crypto::der;
    const auto read_one = [](const std::string& hex) {
        const std::vector<std::byte> bytes = from_hex(hex);
        std::span<const std::byte> in = bytes;
        return der::read(in).has_value() && in.empty();
    };
    CHECK(read_one("0201 05"));
    CHECK(read_one("3003020105"));
    CHECK(read_one("0481 80" + std::string(256, '0')));  // 128 bytes, long form is required
    CHECK_FALSE(
        read_one("0481 7f" + std::string(254, '0')));  // 127 bytes in long form: not minimal
    CHECK_FALSE(read_one("0482 0080" + std::string(256, '0')));  // leading zero length octet
    CHECK_FALSE(read_one("0280"));                               // indefinite length
    CHECK_FALSE(read_one("0205 0102"));                          // longer than what is there
    CHECK_FALSE(read_one("02"));                                 // no length
    CHECK_FALSE(read_one("1f 01 01 00"));                        // multi-byte tag number
    CHECK_FALSE(read_one("0485ffffffffff"));                     // five length octets

    // Trailing bytes are left in `input`, not swallowed.
    const std::vector<std::byte> two = from_hex("0201050201 06");
    std::span<const std::byte> in = two;
    const auto first = der::read(in);
    REQUIRE(first.has_value());
    CHECK(in.size() == 3);
}

TEST_CASE("DER object identifiers and small integers decode", "[crypto][der]") {
    namespace der = crypto::der;
    CHECK(der::oid_to_string(from_hex("2a8648ce3d0201")) == "1.2.840.10045.2.1");
    CHECK(der::oid_to_string(from_hex("2a8648ce3d030107")) == "1.2.840.10045.3.1.7");
    CHECK(der::oid_to_string(from_hex("55040c")) == "2.5.4.12");
    CHECK(der::oid_to_string(from_hex("2b06010401da6402")) == "1.3.6.1.4.1.11620.2");
    CHECK_FALSE(der::oid_to_string(from_hex("")).has_value());
    CHECK_FALSE(der::oid_to_string(from_hex("2a86")).has_value());     // cut off mid-arc
    CHECK_FALSE(der::oid_to_string(from_hex("2a80 01")).has_value());  // padded arc
    CHECK(der::small_unsigned(from_hex("02")) == std::optional<std::uint64_t>(2));
    CHECK(der::small_unsigned(from_hex("00ff")) == std::optional<std::uint64_t>(255));
    CHECK_FALSE(der::small_unsigned(from_hex("ff")).has_value());    // negative
    CHECK_FALSE(der::small_unsigned(from_hex("0001")).has_value());  // not minimal
    CHECK_FALSE(der::small_unsigned(from_hex("")).has_value());
}

// --- Time -----------------------------------------------------------------------
TEST_CASE("signedDate text converts to and from seconds", "[crypto][x509][time]") {
    CHECK(crypto::parse_iso8601_utc("1970-01-01T00:00:00") == std::optional<std::int64_t>(0));
    // The sample's own value, from the owner's measurement: 2026-04-13T23:38:29.
    CHECK(crypto::parse_iso8601_utc("2026-04-13T23:38:29") ==
          std::optional<std::int64_t>(1776123509));
    CHECK(crypto::parse_iso8601_utc("2026-04-13T23:38:29Z") ==
          std::optional<std::int64_t>(1776123509));
    CHECK(crypto::format_iso8601_utc(1776123509) == "2026-04-13T23:38:29");
    CHECK(crypto::format_iso8601_utc(0) == "1970-01-01T00:00:00");
    CHECK(crypto::format_iso8601_utc(-1) == "1969-12-31T23:59:59");
    CHECK(crypto::parse_iso8601_utc("2024-02-29T12:00:00").has_value());  // a leap day
    CHECK_FALSE(crypto::parse_iso8601_utc("2025-02-29T12:00:00").has_value());
    CHECK_FALSE(crypto::parse_iso8601_utc("2026-13-01T00:00:00").has_value());
    CHECK_FALSE(crypto::parse_iso8601_utc("2026-04-13T24:00:00").has_value());
    CHECK_FALSE(crypto::parse_iso8601_utc("2026-04-13 23:38:29").has_value());
    CHECK_FALSE(crypto::parse_iso8601_utc("2026-04-13T23:38:29+01:00").has_value());
    CHECK_FALSE(crypto::parse_iso8601_utc("").has_value());
    // The round trip over a span of years, including both sides of leap years.
    for (std::int64_t t = -std::int64_t{86400} * 400; t < std::int64_t{86400} * 365 * 80;
         t += std::int64_t{86400} * 97 + 12345) {
        const std::string text = crypto::format_iso8601_utc(t);
        REQUIRE(crypto::parse_iso8601_utc(text) == std::optional<std::int64_t>(t));
    }
}

// --- Certificates ---------------------------------------------------------------
TEST_CASE("certificates parse into their fields", "[crypto][x509]") {
    const pki::TestChain chain = pki::make_chain();
    const auto certs = parse_all(chain);

    const crypto::Certificate& root = certs[0];
    CHECK(root.self_issued());
    CHECK(root.subject.common_name == std::optional<std::string>("ICL Forge Test Root CA"));
    CHECK(root.ca == std::optional<bool>(true));
    CHECK(root.key_usage == std::optional<std::uint16_t>((1U << 0) | (1U << 5) | (1U << 6)));
    CHECK(root.serial_hex == "0A0001");
    CHECK(root.not_before == pki::epoch("2010-04-21T23:59:59"));
    CHECK(root.not_after == pki::epoch("2040-04-15T23:59:59"));
    CHECK(root.signature_algorithm == crypto::SignatureAlgorithm::kEcdsaWithSha256);
    CHECK(root.signature_algorithm_oid == "1.2.840.10045.4.3.2");
    CHECK(root.signature_parsed);

    const crypto::Certificate& leaf = certs[3];
    CHECK_FALSE(leaf.self_issued());
    CHECK(leaf.subject.title == std::optional<std::string>("Code/Content Signer"));
    CHECK(leaf.subject.common_name == std::optional<std::string>("ICL Forge Test Content Signer"));
    CHECK(leaf.serial_hex == "0A0004B5D62000000005");
    CHECK(leaf.ca == std::optional<bool>(true));  // the leaf of CS spec §4.5 is a CA too
    CHECK(leaf.key_usage == std::optional<std::uint16_t>(1U << 0));
    CHECK(leaf.issuer.der == certs[2].subject.der);
    CHECK(leaf.subject.text.find("title=Code/Content Signer") != std::string::npos);
    CHECK(leaf.public_key == *crypto::p256_public_key(chain.leaf_key));
    // The fingerprints are over the whole DER (CS spec §10.1).
    CHECK(leaf.sha256_fingerprint == crypto::sha256(chain.certificates[3]));
}

TEST_CASE("each certificate verifies under its issuer's key and no other", "[crypto][x509]") {
    const pki::TestChain chain = pki::make_chain();
    const auto certs = parse_all(chain);
    CHECK(crypto::certificate_signature_valid(certs[0], certs[0].public_key));
    for (std::size_t i = 1; i < certs.size(); ++i) {
        CHECK(crypto::certificate_signature_valid(certs[i], certs[i - 1].public_key));
        CHECK_FALSE(crypto::certificate_signature_valid(certs[i], certs[(i + 1) % 4].public_key));
    }
    // A byte changed inside the signed part is caught.
    pki::Bytes tampered = chain.certificates[2];
    tampered[tampered.size() / 2] ^= std::byte{0x01};
    if (auto parsed = crypto::parse_certificate(tampered)) {
        CHECK_FALSE(crypto::certificate_signature_valid(*parsed, certs[1].public_key));
    }
}

TEST_CASE("certificate parsing refuses what it cannot vouch for", "[crypto][x509]") {
    pki::CertSpec spec;
    spec.subject = pki::Name{"ICL Forge Test Subject"};
    spec.ca = false;
    spec.key_usage = 1U << 0;
    const auto key = pki::key_for("x509-negative-tests");
    const pki::Name issuer{"ICL Forge Test Issuer"};
    const auto issuer_key = pki::key_for("x509-negative-issuer");

    SECTION("a well-formed certificate parses") {
        CHECK(crypto::parse_certificate(pki::issue(spec, key, issuer, issuer_key)).has_value());
    }
    SECTION("GeneralizedTime validity is read") {
        spec.generalized_time = true;
        const auto parsed = crypto::parse_certificate(pki::issue(spec, key, issuer, issuer_key));
        REQUIRE(parsed.has_value());
        CHECK(parsed->not_before == pki::epoch(spec.not_before));
    }
    SECTION("an unrecognised critical extension is an error") {
        spec.unknown_critical_extension = true;
        const auto parsed = crypto::parse_certificate(pki::issue(spec, key, issuer, issuer_key));
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == crypto::X509Error::kUnsupportedCriticalExtension);
    }
    SECTION("an unrecognised non-critical extension is ignored") {
        spec.unknown_noncritical_extension = true;
        CHECK(crypto::parse_certificate(pki::issue(spec, key, issuer, issuer_key)).has_value());
    }
    SECTION("an extension that appears twice is refused (RFC 5280 section 4.2)") {
        // The second basicConstraints says cA TRUE where the first says FALSE: a reader that took
        // the last would make a CA of a certificate its issuer marked as an end entity.
        spec.ca = false;
        spec.repeat_basic_constraints = true;
        const auto twice_bc = crypto::parse_certificate(pki::issue(spec, key, issuer, issuer_key));
        REQUIRE_FALSE(twice_bc.has_value());
        CHECK(twice_bc.error() == crypto::X509Error::kMalformed);

        spec.ca = std::nullopt;
        spec.repeat_basic_constraints = false;
        spec.unknown_noncritical_extension = true;
        spec.repeat_unknown_extension = true;
        const auto twice_unknown =
            crypto::parse_certificate(pki::issue(spec, key, issuer, issuer_key));
        REQUIRE_FALSE(twice_unknown.has_value());
        CHECK(twice_unknown.error() == crypto::X509Error::kMalformed);
    }
    SECTION("a v1 certificate is refused") {
        spec.version = 0;
        const auto parsed = crypto::parse_certificate(pki::issue(spec, key, issuer, issuer_key));
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == crypto::X509Error::kUnsupportedVersion);
    }
    SECTION("garbage and truncation are refused") {
        const pki::Bytes good = pki::issue(spec, key, issuer, issuer_key);
        CHECK_FALSE(crypto::parse_certificate(from_hex("00")).has_value());
        CHECK_FALSE(
            crypto::parse_certificate(std::span<const std::byte>(good).first(good.size() - 1))
                .has_value());
        pki::Bytes trailing = good;
        trailing.push_back(std::byte{0});
        CHECK_FALSE(crypto::parse_certificate(trailing).has_value());
        for (std::size_t cut = 0; cut < good.size(); cut += 7) {
            // no prefix of a certificate is one, and none may crash the parser
            CHECK_FALSE(
                crypto::parse_certificate(std::span<const std::byte>(good).first(cut)).has_value());
        }
    }
    SECTION("a signature algorithm the reader cannot check is reported, not guessed") {
        pki::Bytes der = pki::issue(spec, key, issuer, issuer_key);
        // ecdsa-with-SHA256 ends ...04 03 02 and ecdsa-with-SHA384 ...04 03 03; the algorithm is
        // written twice and the copies must agree, so change both.
        const pki::Bytes needle = pki::oid("1.2.840.10045.4.3.2");
        pki::Bytes replacement = needle;
        replacement.back() = std::byte{0x03};
        std::size_t changed = 0;
        for (std::size_t i = 0; i + needle.size() <= der.size(); ++i) {
            if (std::equal(needle.begin(), needle.end(),
                           der.begin() + static_cast<std::ptrdiff_t>(i))) {
                std::copy(replacement.begin(), replacement.end(),
                          der.begin() + static_cast<std::ptrdiff_t>(i));
                ++changed;
            }
        }
        REQUIRE(changed == 2);
        const auto parsed = crypto::parse_certificate(der);
        REQUIRE(parsed.has_value());
        CHECK(parsed->signature_algorithm == crypto::SignatureAlgorithm::kEcdsaWithSha384);
        CHECK_FALSE(
            crypto::certificate_signature_valid(*parsed, *crypto::p256_public_key(issuer_key)));
    }
}

// --- PEM ------------------------------------------------------------------------
TEST_CASE("PEM and base64 round trip and refuse damage", "[crypto][pem]") {
    const std::vector<std::byte> data = from_hex("000102030405060708090a0b0c0d0e0f10111213");
    const std::string pem = crypto::pem_encode("CERTIFICATE", data);
    CHECK(pem.starts_with("-----BEGIN CERTIFICATE-----\n"));
    const auto blocks = crypto::pem_decode("some text\n" + pem + "more text\n" + pem);
    REQUIRE(blocks.has_value());
    REQUIRE(blocks->size() == 2);
    CHECK((*blocks)[0].label == "CERTIFICATE");
    CHECK((*blocks)[1].der == data);

    CHECK_FALSE(crypto::pem_decode("-----BEGIN A-----\nAAAA\n-----END B-----\n").has_value());
    CHECK_FALSE(crypto::pem_decode("-----BEGIN A-----\nA*AA\n-----END A-----\n").has_value());
    CHECK_FALSE(crypto::pem_decode("-----BEGIN A-----\nAAAA\n").has_value());
    CHECK(crypto::pem_decode("no blocks here")->empty());

    CHECK(crypto::base64_encode(from_hex("")) == "");
    CHECK(crypto::base64_encode(from_hex("66")) == "Zg==");
    CHECK(crypto::base64_encode(from_hex("666f")) == "Zm8=");
    CHECK(crypto::base64_encode(from_hex("666f6f")) == "Zm9v");
    CHECK(to_hex(*crypto::base64_decode("Zm9vYg==")) == "666f6f62");
    CHECK_FALSE(crypto::base64_decode("Zg=").has_value());
    CHECK_FALSE(crypto::base64_decode("Zg==Zg==").has_value());
    CHECK_FALSE(crypto::base64_decode("Z").has_value());
}

// --- Chain validation ---------------------------------------------------------
TEST_CASE("a well-formed chain is reported unanchored until an anchor is given",
          "[crypto][chain]") {
    const pki::TestChain chain = pki::make_chain();
    const auto certs = parse_all(chain);

    crypto::ChainPolicy policy;
    policy.valid_at = pki::epoch("2024-01-01T00:00:00");
    policy.required_leaf_title = "Code/Content Signer";
    const crypto::ChainReport unanchored = crypto::validate_chain(certs, policy);
    REQUIRE(unanchored.ok());
    CHECK_FALSE(unanchored.anchored);

    const std::vector<crypto::Certificate> anchors = {certs[0]};
    policy.trust_anchors = anchors;
    const crypto::ChainReport anchored = crypto::validate_chain(certs, policy);
    REQUIRE(anchored.ok());
    CHECK(anchored.anchored);
    CHECK(anchored.anchor_index == std::optional<std::size_t>(0));

    // An anchor that is not in the chain but issued its top certificate also anchors it.
    const std::vector<crypto::Certificate> below = {certs[1], certs[2], certs[3]};
    const crypto::ChainReport above = crypto::validate_chain(below, policy);
    REQUIRE(above.ok());
    CHECK(above.anchored);
    CHECK_FALSE(above.anchor_index.has_value());

    // A different root anchors nothing.
    pki::CertSpec stranger_spec;
    stranger_spec.subject = pki::Name{"ICL Forge Test Other Root"};
    stranger_spec.ca = true;
    stranger_spec.key_usage = 1U << 5;
    const std::vector<crypto::Certificate> strangers = {*crypto::parse_certificate(pki::issue(
        stranger_spec, pki::key_for("stranger"), stranger_spec.subject, pki::key_for("stranger")))};
    policy.trust_anchors = strangers;
    CHECK_FALSE(crypto::validate_chain(certs, policy).anchored);
}

TEST_CASE("text that comes out of a certificate is escaped before a person reads it",
          "[crypto][x509][escape]") {
    // What escape_text keeps and what it makes visible.
    CHECK(crypto::escape_text("plain ASCII / with punctuation") ==
          "plain ASCII / with punctuation");
    CHECK(crypto::escape_text("caf\xC3\xA9") == "caf\xC3\xA9");  // valid UTF-8 stays
    CHECK(crypto::escape_text("a\nb\rc\td") == "a\\x0Ab\\x0Dc\\x09d");
    CHECK(crypto::escape_text("\x1b[2J\x1b[31m") == "\\x1B[2J\\x1B[31m");
    CHECK(crypto::escape_text("back\\slash") == "back\\\\slash");
    CHECK(crypto::escape_text("\x7f") == "\\x7F");
    CHECK(crypto::escape_text("\xC3(") == "\\xC3(");                  // invalid UTF-8
    CHECK(crypto::escape_text("\xC0\x80") == "\\xC0\\x80");           // an overlong NUL
    CHECK(crypto::escape_text("\xED\xA0\x80") == "\\xED\\xA0\\x80");  // a surrogate
    CHECK(crypto::escape_text("\xC2\x9B") == "\\u{9B}");              // the C1 control CSI
    CHECK(crypto::escape_text("\xE2\x80\xAE") == "\\u{202E}");        // right-to-left override
    CHECK(crypto::escape_text("\xE2\x80\xA8") == "\\u{2028}");        // line separator
    CHECK(crypto::escape_text("\xEF\xBB\xBF") == "\\u{FEFF}");        // byte order mark
    CHECK(crypto::escape_text("").empty());

    // A certificate whose names try to clear the screen, start a line that reads like a verdict and
    // hide text. Its text reaches every message, so none of that may survive, and the title it
    // carries is not mistaken for the one that is required.
    pki::CertSpec spec;
    spec.subject = pki::Name{"Evil\x1b[2J\nsigner ok \xE2\x80\xAE",
                             std::string("Code/Content Signer\x1b"), std::string("T\n1")};
    spec.ca = false;
    spec.key_usage = 1U;
    const pki::Name issuer{"ICL Forge Test Issuer"};
    const auto cert = crypto::parse_certificate(
        pki::issue(spec, pki::key_for("hostile"), issuer, pki::key_for("hostile issuer")));
    REQUIRE(cert.has_value());
    for (const std::string& text :
         {cert->subject.text, cert->subject.common_name.value_or(""),
          cert->subject.title.value_or(""), crypto::describe_certificate(*cert, 0)}) {
        INFO(text);
        CHECK(std::none_of(text.begin(), text.end(), [](char c) {
            const auto u = static_cast<unsigned char>(c);
            return u < 0x20 || u == 0x7F;
        }));
        CHECK(text.find("\xE2\x80\xAE") == std::string::npos);
    }
    CHECK(cert->subject.text.find("\\x1B[2J\\x0Asigner ok \\u{202E}") != std::string::npos);
    CHECK(cert->subject.title != std::optional<std::string>("Code/Content Signer"));
    crypto::ChainPolicy policy;
    policy.required_leaf_title = "Code/Content Signer";
    const std::vector<crypto::Certificate> alone = {*cert};
    CHECK(crypto::validate_chain(alone, policy).failure != crypto::ChainFailure::kNone);
}

TEST_CASE("a chain past the cap is refused before any signature in it is checked",
          "[crypto][chain][bounds]") {
    const pki::TestChain chain = pki::make_chain();
    const auto certs = parse_all(chain);
    // Eight is accepted (the cap), nine and fifteen hundred are not. The long one is the same
    // certificate over and over, which fails its issuer check; reaching that check would take the
    // cost of the cap's worth of signatures, so the failure kind says which check refused it.
    const std::vector<crypto::Certificate> eight(8, certs[0]);
    CHECK(crypto::validate_chain(eight, {}).failure != crypto::ChainFailure::kPathLength);
    for (const std::size_t count : {std::size_t{9}, std::size_t{1500}}) {
        const std::vector<crypto::Certificate> long_chain(count, certs[0]);
        const crypto::ChainReport report = crypto::validate_chain(long_chain, {});
        CHECK(report.failure == crypto::ChainFailure::kPathLength);
        CHECK(report.detail.find("at most 8") != std::string::npos);
    }
}

TEST_CASE("an expired intermediate fails at the time asked and is skipped above a pinned anchor",
          "[crypto][chain]") {
    const pki::TestChain chain = pki::make_chain("2025-11-13T01:47:10");
    const auto certs = parse_all(chain);

    // The situation of CS spec §4.3 and the owner's sample: the second certificate expired on
    // 2025-11-13, before the signature's own date of 2026-04-13 and before "now".
    crypto::ChainPolicy at_signing;
    at_signing.valid_at = pki::epoch("2026-04-13T23:38:29");
    const crypto::ChainReport expired = crypto::validate_chain(certs, at_signing);
    REQUIRE_FALSE(expired.ok());
    CHECK(expired.failure == crypto::ChainFailure::kNotValidAtTime);
    CHECK(expired.failed_index == 1);
    CHECK(expired.detail.find("expired at 2025-11-13T01:47:10Z") != std::string::npos);
    CHECK(expired.detail.find("ICL Forge Test Master CA") != std::string::npos);

    crypto::ChainPolicy before_expiry = at_signing;
    before_expiry.valid_at = pki::epoch("2024-01-01T00:00:00");
    CHECK(crypto::validate_chain(certs, before_expiry).ok());

    crypto::ChainPolicy no_time_check = at_signing;
    no_time_check.valid_at.reset();
    CHECK(crypto::validate_chain(certs, no_time_check).ok());

    // Before a certificate's notBefore.
    crypto::ChainPolicy too_early = at_signing;
    too_early.valid_at = pki::epoch("2012-01-01T00:00:00");
    const crypto::ChainReport early = crypto::validate_chain(certs, too_early);
    REQUIRE_FALSE(early.ok());
    CHECK(early.failed_index == 1);  // the root is valid then, the master (from 2015) is not
    CHECK(early.detail.find("not valid before") != std::string::npos);

    // Pinning the issuing CA (index 2) starts the path below the expired master.
    const crypto::CertificatePin pin{
        std::vector<std::byte>(certs[2].sha1_fingerprint.begin(), certs[2].sha1_fingerprint.end())};
    const std::vector<crypto::CertificatePin> pins = {pin};
    crypto::ChainPolicy pinned = at_signing;
    pinned.pins = pins;
    const crypto::ChainReport report = crypto::validate_chain(certs, pinned);
    REQUIRE(report.ok());
    CHECK(report.anchored);
    CHECK(report.anchor_index == std::optional<std::size_t>(2));

    // A SHA-256 pin of the same certificate does the same; a wrong one pins nothing.
    const crypto::CertificatePin pin256{std::vector<std::byte>(certs[2].sha256_fingerprint.begin(),
                                                               certs[2].sha256_fingerprint.end())};
    const std::vector<crypto::CertificatePin> pins256 = {pin256};
    pinned.pins = pins256;
    CHECK(crypto::validate_chain(certs, pinned).anchored);
    crypto::CertificatePin wrong = pin;
    wrong.fingerprint[0] ^= std::byte{0x01};
    const std::vector<crypto::CertificatePin> wrong_pins = {wrong};
    pinned.pins = wrong_pins;
    CHECK(crypto::validate_chain(certs, pinned).failure == crypto::ChainFailure::kNotValidAtTime);
}

TEST_CASE("the leaf must be allowed to sign content", "[crypto][chain]") {
    const pki::TestChain chain = pki::make_chain();
    auto certs = parse_all(chain);
    crypto::ChainPolicy policy;
    policy.required_leaf_title = "Code/Content Signer";
    REQUIRE(crypto::validate_chain(certs, policy).ok());

    policy.required_leaf_title = "Something Else";
    const crypto::ChainReport title = crypto::validate_chain(certs, policy);
    CHECK(title.failure == crypto::ChainFailure::kLeafTitle);
    CHECK(title.failed_index == 3);

    // The title check is optional: an operator whose certificates have none turns it off.
    policy.required_leaf_title.clear();
    CHECK(crypto::validate_chain(certs, policy).ok());

    // A leaf without digitalSignature in its key usage.
    certs[3].key_usage = 1U << 5;
    const crypto::ChainReport usage = crypto::validate_chain(certs, policy);
    CHECK(usage.failure == crypto::ChainFailure::kLeafKeyUsage);
    policy.require_digital_signature = false;
    CHECK(crypto::validate_chain(certs, policy).ok());
}

TEST_CASE("chain structure is checked link by link", "[crypto][chain]") {
    const pki::TestChain chain = pki::make_chain();
    const auto certs = parse_all(chain);
    crypto::ChainPolicy policy;

    SECTION("an empty chain") {
        CHECK(crypto::validate_chain({}, policy).failure == crypto::ChainFailure::kEmpty);
    }
    SECTION("a certificate out of place") {
        std::vector<crypto::Certificate> swapped = {certs[0], certs[2], certs[1], certs[3]};
        const crypto::ChainReport report = crypto::validate_chain(swapped, policy);
        CHECK(report.failure == crypto::ChainFailure::kIssuerMismatch);
        CHECK(report.failed_index == 1);
    }
    SECTION("a link with a signature that does not verify") {
        // Keep the names right but sign the second certificate with the wrong key.
        pki::CertSpec spec;
        spec.subject = chain.names[1];
        spec.ca = true;
        spec.key_usage = (1U << 0) | (1U << 5);
        const pki::Bytes forged = pki::issue(spec, pki::key_for("icl-forge-test-master"),
                                             chain.names[0], pki::key_for("not the root"));
        std::vector<crypto::Certificate> bad = certs;
        bad[1] = *crypto::parse_certificate(forged);
        const crypto::ChainReport report = crypto::validate_chain(bad, policy);
        CHECK(report.failure == crypto::ChainFailure::kBadSignature);
        CHECK(report.failed_index == 1);
    }
    SECTION("a self-issued top certificate with a signature that does not verify") {
        pki::CertSpec spec;
        spec.subject = chain.names[0];
        spec.ca = true;
        spec.key_usage = 1U << 5;
        const pki::Bytes forged = pki::issue(spec, pki::key_for("icl-forge-test-root"),
                                             chain.names[0], pki::key_for("someone else"));
        std::vector<crypto::Certificate> bad = certs;
        bad[0] = *crypto::parse_certificate(forged);
        CHECK(crypto::validate_chain(bad, policy).failure == crypto::ChainFailure::kBadSignature);
    }
    SECTION("an issuer that is not a certificate authority") {
        auto no_ca = certs;
        no_ca[2].ca = false;
        const crypto::ChainReport report = crypto::validate_chain(no_ca, policy);
        CHECK(report.failure == crypto::ChainFailure::kNotCertificateAuthority);
        CHECK(report.failed_index == 2);
        auto no_usage = certs;
        no_usage[1].key_usage = 1U << 0;  // digitalSignature but not keyCertSign
        CHECK(crypto::validate_chain(no_usage, policy).failure ==
              crypto::ChainFailure::kNotCertificateAuthority);
        auto no_basic = certs;
        no_basic[1].ca.reset();
        CHECK(crypto::validate_chain(no_basic, policy).failure ==
              crypto::ChainFailure::kNotCertificateAuthority);
    }
    SECTION("a path length constraint") {
        auto limited = certs;
        limited[0].path_length = 1;  // two intermediates below the root
        CHECK(crypto::validate_chain(limited, policy).failure == crypto::ChainFailure::kPathLength);
        limited[0].path_length = 2;
        CHECK(crypto::validate_chain(limited, policy).ok());
    }
    SECTION("a signature algorithm that cannot be checked") {
        auto other = certs;
        other[2].signature_algorithm = crypto::SignatureAlgorithm::kEcdsaWithSha384;
        other[2].signature_algorithm_oid = "1.2.840.10045.4.3.3";
        const crypto::ChainReport report = crypto::validate_chain(other, policy);
        CHECK(report.failure == crypto::ChainFailure::kUnsupportedAlgorithm);
        CHECK(report.detail.find("1.2.840.10045.4.3.3") != std::string::npos);
    }
}
