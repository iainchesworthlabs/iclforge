#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "crypto_test_support.hpp"
#include "iclforge/base/crypto/chain.hpp"
#include "iclforge/base/crypto/content_signature.hpp"
#include "iclforge/base/crypto/credentials.hpp"
#include "iclforge/base/crypto/ecdsa_p256.hpp"
#include "iclforge/base/crypto/sha256.hpp"
#include "iclforge/base/crypto/thrift_binary.hpp"
#include "iclforge/base/crypto/x509.hpp"
#include "test_pki.hpp"

namespace {

namespace crypto = iclforge::base::crypto;
namespace tb = iclforge::base::crypto::thrift;
namespace pki = iclforge::test::pki;
using iclforge::test::crypto::text_bytes;

constexpr const char* kSignedDate = "2024-06-01T12:00:00";  // the master CA is still valid then

struct Fixture {
    pki::TestChain chain = pki::make_chain();
    std::vector<crypto::Certificate> certs;
    crypto::SigningCredentials credentials;

    Fixture() {
        for (const pki::Bytes& der : chain.certificates) {
            certs.push_back(*crypto::parse_certificate(der));
        }
        credentials = *crypto::make_credentials(chain.leaf_key, certs);
    }

    crypto::SignOptions options() const {
        crypto::SignOptions o;
        o.signed_date = kSignedDate;
        o.chain_policy.valid_at = pki::epoch(kSignedDate);
        o.chain_policy.required_leaf_title = std::string(crypto::kDefaultLeafTitle);
        o.metadata.application_version = "test";
        // fixed nonce so that two runs of a test give the same bytes
        o.nonce.assign(128, std::byte{0x5A});
        return o;
    }

    crypto::VerifyPolicy policy(std::span<const crypto::Certificate> anchors = {}) const {
        crypto::VerifyPolicy p;
        p.trust_anchors = anchors;
        p.time_basis = crypto::TimeBasis::kInstant;
        p.instant = pki::epoch("2024-07-01T00:00:00");
        return p;
    }
};

std::vector<std::byte> content_of(std::size_t n) {
    std::vector<std::byte> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<std::byte>((i * 31 + 7) & 0xFF);
    }
    return out;
}

bool has_check(const crypto::VerifyReport& r, const std::string& name, bool ok) {
    return std::any_of(r.checks.begin(), r.checks.end(),
                       [&](const crypto::CheckResult& c) { return c.name == name && c.ok == ok; });
}

std::int16_t id_of(const tb::Field& f) {
    return f.id;
}

}  // namespace

TEST_CASE("a content signature has the field layout of the measured blob",
          "[crypto][content-signature]") {
    const Fixture f;
    const auto content = content_of(5000);
    const auto blob = crypto::sign_content(content, f.credentials, f.options());
    REQUIRE(blob.has_value());

    // Top level: version, schemaVersion, signerInfo, signatureValues, in ascending id order.
    const auto top = tb::read_struct(*blob);
    REQUIRE(top.has_value());
    REQUIRE(top->size() == 4);
    CHECK(std::vector<std::int16_t>{id_of((*top)[0]), id_of((*top)[1]), id_of((*top)[2]),
                                    id_of((*top)[3])} == std::vector<std::int16_t>{1, 2, 3, 4});

    const auto signer = tb::as_struct((*top)[2]);
    REQUIRE(signer.has_value());
    std::vector<std::int16_t> signer_ids;
    for (const tb::Field& field : *signer) {
        signer_ids.push_back(field.id);
    }
    // The measured SignerInfoWire: nonce, two versions, publisher, product, platform, application,
    // signedDate, then certificateInfo at id 10 (there is no 9).
    CHECK(signer_ids == std::vector<std::int16_t>{1, 2, 3, 4, 5, 6, 7, 8, 10});
    CHECK(tb::as_byte_list(*tb::find(*signer, 1, tb::Type::kList))->size() == 128);
    CHECK(tb::as_string(*tb::find(*signer, 8, tb::Type::kString)) ==
          std::optional<std::string>(kSignedDate));

    // The certificates are a list<byte> opening 40 82 LL LL, as in the measured blob.
    const auto certificate_info = tb::as_struct(*tb::find(*signer, 10, tb::Type::kStruct));
    REQUIRE(certificate_info.has_value());
    CHECK(tb::as_string(*tb::find(*certificate_info, 2, tb::Type::kString)) ==
          std::optional<std::string>("pace-ccjf1"));
    CHECK(tb::as_string(*tb::find(*certificate_info, 3, tb::Type::kString)) ==
          std::optional<std::string>("der"));
    const auto certificates = tb::as_byte_list(*tb::find(*certificate_info, 4, tb::Type::kList));
    REQUIRE(certificates.has_value());
    CHECK((*certificates)[0] == std::byte{0x40});
    CHECK((*certificates)[1] == std::byte{0x82});
    const std::size_t wrapped = (std::to_integer<std::size_t>((*certificates)[2]) << 8) |
                                std::to_integer<std::size_t>((*certificates)[3]);
    CHECK(wrapped == certificates->size() - 4);

    // One signature entry: signedInfo (1) and signatureData (2).
    const auto entries = tb::as_struct_list((*top)[3]);
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 1);
    const auto entry = tb::read_struct(entries->front());
    REQUIRE(entry.has_value());
    CHECK(std::vector<std::int16_t>{id_of((*entry)[0]), id_of((*entry)[1])} ==
          std::vector<std::int16_t>{1, 2});
    const auto signed_info = tb::as_struct((*entry)[0]);
    REQUIRE(signed_info.has_value());
    std::vector<std::int16_t> info_ids;
    for (const tb::Field& field : *signed_info) {
        info_ids.push_back(field.id);
    }
    CHECK(info_ids == std::vector<std::int16_t>{1, 2, 3, 4, 5, 6, 7});
    const auto digest = tb::as_struct(*tb::find(*signed_info, 6, tb::Type::kStruct));
    REQUIRE(digest.has_value());
    std::vector<std::int16_t> digest_ids;
    for (const tb::Field& field : *digest) {
        digest_ids.push_back(field.id);
    }
    CHECK(digest_ids ==
          std::vector<std::int16_t>{1, 2, 21, 31, 32});  // non-contiguous, as measured
    const auto data = tb::as_struct((*entry)[1]);
    REQUIRE(data.has_value());
    const auto map = tb::as_string_to_byte_list_map(*tb::find(*data, 4, tb::Type::kMap));
    REQUIRE(map.has_value());
    REQUIRE(map->size() == 2);
    CHECK((*map)[0].key == "ecc_r_value");
    CHECK((*map)[1].key == "ecc_s_value");
    CHECK((*map)[0].value.size() == 32);
}

TEST_CASE("the signature is over the signed information, which holds the content digest",
          "[crypto][content-signature]") {
    const Fixture f;
    const auto content = content_of(777);
    const auto blob = crypto::sign_content(content, f.credentials, f.options());
    REQUIRE(blob.has_value());
    const auto parsed = crypto::parse_content_signature(*blob);
    REQUIRE(parsed.has_value());

    // Verified here with the primitives directly, not through verify_content_signature().
    CHECK(crypto::ecdsa_p256_verify(f.credentials.chain.back().public_key,
                                    crypto::sha256(parsed->signed_info_bytes()),
                                    parsed->signature));
    // The content digest the signature covers is the SHA-256 of the content, inside SignedInfoWire.
    REQUIRE(parsed->content_digests.size() == 1);
    CHECK(parsed->content_digests[0].reference == "digest_data_ref_win_signature_data");
    CHECK(parsed->content_digests[0].canonical_method == "canonical_method_raw");
    const auto expected = crypto::sha256(content);
    CHECK(std::equal(expected.begin(), expected.end(), parsed->content_digests[0].value.begin()));
    // The signer-information digest is the SHA-256 of the signer information as written.
    const auto signer_digest = crypto::sha256(parsed->signer_info_bytes());
    CHECK(std::equal(signer_digest.begin(), signer_digest.end(),
                     parsed->signer_info_digest.value.begin()));
    CHECK(parsed->signer_info_digest.reference == "digest_data_ref_signer_info");
    CHECK(parsed->signed_date == kSignedDate);
    CHECK(parsed->chain.size() == 4);
    CHECK(parsed->chain.back().der == f.chain.certificates[3]);
    CHECK(parsed->signature_id == crypto::kContentSignatureUuid);
    // A signature over the content digest alone, which is what the spec's text says, does not
    // verify.
    CHECK_FALSE(crypto::ecdsa_p256_verify(f.credentials.chain.back().public_key, expected,
                                          parsed->signature));
}

TEST_CASE("signing is deterministic given a nonce, and the metadata round trips",
          "[crypto][content-signature]") {
    const Fixture f;
    const auto content = content_of(100);
    crypto::SignOptions options = f.options();
    options.metadata.publisher_name = "Example Publisher";
    options.metadata.publisher_uuid = "11111111-2222-3333-4444-555555555555";
    options.metadata.product_name = "Example Product";
    options.metadata.os_name = "TestOS";
    options.metadata.copyright = "Copyright the test";
    options.metadata.build_hash = "abc123";
    const auto a = crypto::sign_content(content, f.credentials, options);
    const auto b = crypto::sign_content(content, f.credentials, options);
    REQUIRE(a.has_value());
    CHECK(*a == *b);  // RFC 6979: same key and message, same signature

    const auto parsed = crypto::parse_content_signature(*a);
    REQUIRE(parsed.has_value());
    CHECK(parsed->signer == options.metadata);
    CHECK(parsed->signer_nonce == options.nonce);

    // Without a nonce supplied, each signature gets its own, 128 bytes.
    crypto::SignOptions fresh = f.options();
    fresh.nonce.clear();
    const auto c = crypto::sign_content(content, f.credentials, fresh);
    const auto d = crypto::sign_content(content, f.credentials, fresh);
    REQUIRE(c.has_value());
    CHECK(*c != *d);
    CHECK(crypto::parse_content_signature(*c)->signer_nonce.size() == 128);
}

TEST_CASE("a signed blob verifies, unanchored until the operator anchors it",
          "[crypto][content-signature]") {
    const Fixture f;
    const auto content = content_of(4096);
    const auto blob = crypto::sign_content(content, f.credentials, f.options());
    REQUIRE(blob.has_value());
    const auto digest = crypto::sha256(content);

    const crypto::VerifyReport unanchored =
        crypto::verify_content_signature(*blob, digest, f.policy());
    REQUIRE_FALSE(unanchored.fault.has_value());
    CHECK(unanchored.verdict == crypto::Verdict::kChecksChainUnanchored);
    CHECK(unanchored.signer.find("ICL Forge Test Content Signer") != std::string::npos);
    CHECK(unanchored.signer_serial == "0A0004B5D62000000005");
    CHECK(unanchored.signed_date == kSignedDate);
    for (const crypto::CheckResult& c : unanchored.checks) {
        INFO(c.name << ": " << c.detail);
        CHECK(c.ok);
    }

    const std::vector<crypto::Certificate> anchors = {f.certs[0]};
    const crypto::VerifyReport anchored =
        crypto::verify_content_signature(*blob, digest, f.policy(anchors));
    CHECK(anchored.verdict == crypto::Verdict::kValid);
    CHECK(anchored.anchor.find("trust anchor") != std::string::npos);

    // A pin of the issuing CA anchors it the same way.
    const std::vector<crypto::CertificatePin> pins = {crypto::CertificatePin{std::vector<std::byte>(
        f.certs[2].sha256_fingerprint.begin(), f.certs[2].sha256_fingerprint.end())}};
    crypto::VerifyPolicy pinned = f.policy();
    pinned.pins = pins;
    CHECK(crypto::verify_content_signature(*blob, digest, pinned).verdict ==
          crypto::Verdict::kValid);

    // A trust anchor that is a different root anchors nothing.
    pki::CertSpec other_spec;
    other_spec.subject = pki::Name{"ICL Forge Test Unrelated Root"};
    other_spec.ca = true;
    other_spec.key_usage = 1U << 5;
    const std::vector<crypto::Certificate> others = {*crypto::parse_certificate(pki::issue(
        other_spec, pki::key_for("unrelated"), other_spec.subject, pki::key_for("unrelated")))};
    CHECK(crypto::verify_content_signature(*blob, digest, f.policy(others)).verdict ==
          crypto::Verdict::kChecksChainUnanchored);
}

TEST_CASE("wrong content is refused with a digest mismatch", "[crypto][content-signature]") {
    const Fixture f;
    auto content = content_of(2048);
    const auto blob = crypto::sign_content(content, f.credentials, f.options());
    REQUIRE(blob.has_value());
    content[1000] ^= std::byte{0x01};
    const crypto::VerifyReport report =
        crypto::verify_content_signature(*blob, crypto::sha256(content), f.policy());
    CHECK(report.verdict == crypto::Verdict::kInvalid);
    REQUIRE(report.fault.has_value());
    CHECK(report.fault->code == crypto::SignatureError::kDigestMismatch);
    CHECK(has_check(report, "content digest", false));
    CHECK(has_check(report, "signature", true));  // the signature is fine; the content is not
}

TEST_CASE("validity is judged at the time the policy names", "[crypto][content-signature]") {
    // The shape of the owner's measurement: an intermediate that expired before 'now' and before
    // the signature's own date.
    const Fixture f;
    const auto content = content_of(64);
    crypto::SignOptions options = f.options();
    const auto blob = crypto::sign_content(content, f.credentials, options);
    REQUIRE(blob.has_value());
    const auto digest = crypto::sha256(content);

    crypto::VerifyPolicy policy = f.policy();
    policy.time_basis = crypto::TimeBasis::kNow;
    policy.now = pki::epoch("2026-10-29T00:00:00");
    const crypto::VerifyReport at_now = crypto::verify_content_signature(*blob, digest, policy);
    CHECK(at_now.verdict == crypto::Verdict::kInvalid);
    REQUIRE(at_now.fault.has_value());
    CHECK(at_now.fault->code == crypto::SignatureError::kBadCredentialsForSigningContent);
    CHECK(at_now.fault->message.find("expired at 2025-11-13T01:47:10Z") != std::string::npos);
    CHECK(has_check(at_now, "chain", false));
    CHECK(has_check(at_now, "signature", true));  // all the other checks still ran and passed

    // The signed date, 2024-06-01, is before the expiry.
    policy.time_basis = crypto::TimeBasis::kSignedDate;
    CHECK(crypto::verify_content_signature(*blob, digest, policy).verdict ==
          crypto::Verdict::kChecksChainUnanchored);
    policy.time_basis = crypto::TimeBasis::kInstant;
    policy.instant = pki::epoch("2026-04-13T23:38:29");
    CHECK(crypto::verify_content_signature(*blob, digest, policy).verdict ==
          crypto::Verdict::kInvalid);
    policy.time_basis = crypto::TimeBasis::kIgnore;
    CHECK(crypto::verify_content_signature(*blob, digest, policy).verdict ==
          crypto::Verdict::kChecksChainUnanchored);
}

TEST_CASE("a signed date that cannot be right is refused", "[crypto][content-signature]") {
    const Fixture f;
    const auto content = content_of(64);
    const auto digest = crypto::sha256(content);

    // Signed after 'now' (more than a day).
    crypto::SignOptions future = f.options();
    future.signed_date = "2024-09-01T00:00:00";
    future.chain_policy.valid_at = pki::epoch(future.signed_date);
    const auto blob = crypto::sign_content(content, f.credentials, future);
    REQUIRE(blob.has_value());
    crypto::VerifyPolicy policy = f.policy();
    policy.time_basis = crypto::TimeBasis::kNow;
    policy.now = pki::epoch("2024-07-01T00:00:00");
    const auto report = crypto::verify_content_signature(*blob, digest, policy);
    REQUIRE(report.fault.has_value());
    CHECK(report.fault->code == crypto::SignatureError::kBadContentSignatureDate);

    // Signed before the signer certificate existed (the leaf is valid from 2022-06-09).
    crypto::SignOptions early = f.options();
    early.signed_date = "2021-01-01T00:00:00";
    early.chain_policy.valid_at.reset();
    const auto early_blob = crypto::sign_content(content, f.credentials, early);
    REQUIRE(early_blob.has_value());
    const auto early_report = crypto::verify_content_signature(*early_blob, digest, f.policy());
    REQUIRE(early_report.fault.has_value());
    CHECK(early_report.fault->code == crypto::SignatureError::kBadContentSignatureDate);

    // Not a date at all cannot be signed.
    crypto::SignOptions junk = f.options();
    junk.signed_date = "yesterday";
    const auto refused = crypto::sign_content(content, f.credentials, junk);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == crypto::SignatureError::kBadContentSignatureDate);
}

TEST_CASE("the leaf must carry the title and the key usage the policy asks for",
          "[crypto][content-signature]") {
    const Fixture f;
    const auto content = content_of(64);
    const auto blob = crypto::sign_content(content, f.credentials, f.options());
    REQUIRE(blob.has_value());
    const auto digest = crypto::sha256(content);

    crypto::VerifyPolicy policy = f.policy();
    policy.required_leaf_title = "Some Other Title";
    auto report = crypto::verify_content_signature(*blob, digest, policy);
    REQUIRE(report.fault.has_value());
    CHECK(report.fault->code == crypto::SignatureError::kCertNotPrivilegedToSignContent);

    policy.required_leaf_title.clear();  // an operator whose certificates have no title
    CHECK(crypto::verify_content_signature(*blob, digest, policy).verdict ==
          crypto::Verdict::kChecksChainUnanchored);
}

TEST_CASE("signing refuses credentials that do not hold together", "[crypto][content-signature]") {
    const Fixture f;
    const auto content = content_of(64);

    SECTION("a chain that is not valid at the signed date") {
        crypto::SignOptions options = f.options();
        options.signed_date = "2026-04-13T23:38:29";
        options.chain_policy.valid_at = pki::epoch(options.signed_date);
        const auto refused = crypto::sign_content(content, f.credentials, options);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == crypto::SignatureError::kBadCredentialsForSigningContent);
        CHECK(refused.error().message.find("expired") != std::string::npos);
    }
    SECTION("a leaf without the required title") {
        crypto::SignOptions options = f.options();
        options.chain_policy.required_leaf_title = "No Such Title";
        const auto refused = crypto::sign_content(content, f.credentials, options);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == crypto::SignatureError::kCertNotPrivilegedToSignContent);
    }
    SECTION("no key") {
        crypto::SigningCredentials empty;
        const auto refused = crypto::sign_content(content, empty, f.options());
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == crypto::SignatureError::kCertifierNoPrivateKey);
    }
    SECTION("a key that is not the leaf's") {
        crypto::SigningCredentials wrong = f.credentials;
        wrong.key = pki::key_for("someone else");
        const auto refused = crypto::sign_content(content, wrong, f.options());
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == crypto::SignatureError::kBadCredentialsForSigningContent);
    }
}

TEST_CASE("no single byte of a content signature can be changed unnoticed",
          "[crypto][content-signature][tamper]") {
    const Fixture f;
    const auto content = content_of(1500);
    const auto blob = crypto::sign_content(content, f.credentials, f.options());
    REQUIRE(blob.has_value());
    const auto digest = crypto::sha256(content);
    REQUIRE(crypto::verify_content_signature(*blob, digest, f.policy()).verdict ==
            crypto::Verdict::kChecksChainUnanchored);

    // Every 17th byte, from the structure at the front to the signature at the end, flipped in
    // turn. Each damaged blob must fail one check or another; none may pass for a valid signature.
    std::size_t tried = 0;
    for (std::size_t i = 0; i < blob->size(); i += 17) {
        std::vector<std::byte> damaged = *blob;
        damaged[i] ^= std::byte{0x01};
        const auto report = crypto::verify_content_signature(damaged, digest, f.policy());
        INFO("byte " << i << " of " << blob->size());
        CHECK(report.verdict == crypto::Verdict::kInvalid);
        ++tried;
    }
    CHECK(tried > 100);

    // The last byte, the r and s values and the first byte, specifically.
    for (const std::size_t i : {std::size_t{0}, blob->size() - 1, blob->size() - 2,
                                blob->size() - 40, blob->size() - 80}) {
        std::vector<std::byte> damaged = *blob;
        damaged[i] ^= std::byte{0x80};
        CHECK(crypto::verify_content_signature(damaged, digest, f.policy()).verdict ==
              crypto::Verdict::kInvalid);
    }
}

TEST_CASE("a content signature is parsed or refused, whatever its length",
          "[crypto][content-signature]") {
    const Fixture f;
    const auto blob = crypto::sign_content(content_of(10), f.credentials, f.options());
    REQUIRE(blob.has_value());
    for (std::size_t cut = 0; cut < blob->size(); cut += 11) {
        CHECK_FALSE(crypto::parse_content_signature(std::span<const std::byte>(*blob).first(cut))
                        .has_value());
    }
    std::vector<std::byte> trailing = *blob;
    trailing.push_back(std::byte{0});
    const auto refused = crypto::parse_content_signature(trailing);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == crypto::SignatureError::kBadOrMissingSignature);
    CHECK_FALSE(crypto::parse_content_signature({}).has_value());
    // A blob reordered inside a struct is still read: Thrift readers accept any field order. (The
    // digest would catch it; the point is the reader does not depend on the writer's order.)
    CHECK(crypto::parse_content_signature(*blob).has_value());
}

// ---- Lists a hostile blob can stretch -------------------------------------------------------

namespace {

// `blob` with the bytes of `part`, which must lie inside it, replaced by `with`.
std::vector<std::byte> splice(std::span<const std::byte> blob, std::span<const std::byte> part,
                              std::span<const std::byte> with) {
    const auto at = static_cast<std::size_t>(part.data() - blob.data());
    std::vector<std::byte> out(blob.begin(), blob.begin() + static_cast<std::ptrdiff_t>(at));
    out.insert(out.end(), with.begin(), with.end());
    out.insert(out.end(), blob.begin() + static_cast<std::ptrdiff_t>(at + part.size()), blob.end());
    return out;
}

// A Thrift list header: the element type and a big-endian count.
std::vector<std::byte> list_header(tb::Type element, std::uint32_t count) {
    return {static_cast<std::byte>(element), static_cast<std::byte>(count >> 24),
            static_cast<std::byte>((count >> 16) & 0xFF),
            static_cast<std::byte>((count >> 8) & 0xFF), static_cast<std::byte>(count & 0xFF)};
}

void append(std::vector<std::byte>& to, std::span<const std::byte> bytes) {
    to.insert(to.end(), bytes.begin(), bytes.end());
}

}  // namespace

TEST_CASE("a second signature entry is refused, even one that is damaged",
          "[crypto][content-signature][bounds]") {
    const Fixture f;
    const auto content = content_of(300);
    const auto blob = crypto::sign_content(content, f.credentials, f.options());
    REQUIRE(blob.has_value());
    const auto digest = crypto::sha256(content);
    REQUIRE(crypto::verify_content_signature(*blob, digest, f.policy()).verdict ==
            crypto::Verdict::kChecksChainUnanchored);

    const auto top = tb::read_struct(*blob);
    REQUIRE(top.has_value());
    const tb::Field* list = tb::find(*top, 4, tb::Type::kList);
    REQUIRE(list != nullptr);
    const std::span<const std::byte> one_entry = list->value.subspan(5);

    // The entry again with the last byte of s changed: the first entry is untouched and verifies
    // alone, so a verifier that read only the first would call this blob valid.
    std::vector<std::byte> damaged(one_entry.begin(), one_entry.end());
    damaged[damaged.size() - 3] ^= std::byte{0x01};
    for (const auto& second :
         {std::vector<std::byte>(one_entry.begin(), one_entry.end()), damaged}) {
        std::vector<std::byte> value = list_header(tb::Type::kStruct, 2);
        append(value, one_entry);
        append(value, second);
        const auto two = splice(*blob, list->value, value);
        const auto parsed = crypto::parse_content_signature(two);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().code == crypto::SignatureError::kBadOrMissingSignature);
        CHECK(parsed.error().message.find("2 signature entries") != std::string::npos);
        const crypto::VerifyReport report =
            crypto::verify_content_signature(two, digest, f.policy());
        CHECK(report.verdict == crypto::Verdict::kInvalid);
        CHECK(report.fault.has_value());
    }

    // None at all, and a count that claims millions, are refused without reading the elements.
    const auto none = splice(*blob, list->value, list_header(tb::Type::kStruct, 0));
    CHECK_FALSE(crypto::parse_content_signature(none).has_value());
    const auto huge = splice(*blob, list->value, list_header(tb::Type::kStruct, 4'000'000));
    CHECK_FALSE(crypto::parse_content_signature(huge).has_value());
}

TEST_CASE("a chain or a digest list past its cap is refused",
          "[crypto][content-signature][bounds]") {
    const Fixture f;
    const auto content = content_of(300);
    const auto blob = crypto::sign_content(content, f.credentials, f.options());
    REQUIRE(blob.has_value());
    const auto digest = crypto::sha256(content);
    const auto top = tb::read_struct(*blob);
    REQUIRE(top.has_value());

    // Nine certificates in the list (the four of the chain, then five repeated): the cap is eight.
    const auto signer = tb::as_struct(*tb::find(*top, 3, tb::Type::kStruct));
    REQUIRE(signer.has_value());
    const auto info = tb::as_struct(*tb::find(*signer, 10, tb::Type::kStruct));
    REQUIRE(info.has_value());
    const tb::Field* certificates = tb::find(*info, 4, tb::Type::kList);
    REQUIRE(certificates != nullptr);
    const std::span<const std::byte> wrapped = certificates->value.subspan(5);
    std::vector<std::byte> body(wrapped.begin() + 4, wrapped.end());  // after 40 82 LL LL
    for (int i = 0; i < 5; ++i) {
        append(body, f.chain.certificates[3]);
    }
    std::vector<std::byte> list = {std::byte{0x40}, std::byte{0x82},
                                   static_cast<std::byte>(body.size() >> 8),
                                   static_cast<std::byte>(body.size() & 0xFF)};
    append(list, body);
    std::vector<std::byte> value =
        list_header(tb::Type::kByte, static_cast<std::uint32_t>(list.size()));
    append(value, list);
    const auto long_chain = splice(*blob, certificates->value, value);
    const auto parsed = crypto::parse_content_signature(long_chain);
    REQUIRE(parsed.has_value());
    CHECK(parsed->chain.empty());
    CHECK(parsed->chain_error.find("more than 8 certificates") != std::string::npos);
    CHECK(crypto::verify_content_signature(long_chain, digest, f.policy()).verdict ==
          crypto::Verdict::kInvalid);

    // Nine content digests where one belongs.
    const auto entries = tb::as_struct_list(*tb::find(*top, 4, tb::Type::kList));
    REQUIRE(entries.has_value());
    const auto entry = tb::read_struct(entries->front());
    REQUIRE(entry.has_value());
    const auto signed_info = tb::as_struct(*tb::find(*entry, 1, tb::Type::kStruct));
    REQUIRE(signed_info.has_value());
    const tb::Field* digests = tb::find(*signed_info, 7, tb::Type::kList);
    REQUIRE(digests != nullptr);
    const auto first_digest = tb::as_struct_list(*digests)->front();
    std::vector<std::byte> nine = list_header(tb::Type::kStruct, 9);
    for (int i = 0; i < 9; ++i) {
        append(nine, first_digest);
    }
    const auto many = splice(*blob, digests->value, nine);
    const auto refused = crypto::parse_content_signature(many);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().message.find("at most 8") != std::string::npos);
}

// ---- CS spec v2.6: the order of the checks, and the operator metadata of §5.3 ------------------

namespace {

// A signature written here, by hand, from the field list of CS spec v2.6 §7.6 and the steps of
// §5.2, and not by the library's writer: so that what the verifier is shown is not only what its
// own signer makes, and so that the fields §5.3 speaks of can be given values the signer refuses to
// write (a malformed publisher UUID, a nonce of the wrong length).
struct HandBuilt {
    std::vector<std::byte> nonce = std::vector<std::byte>(128, std::byte{0x33});
    std::string publisher_uuid;
    std::string signed_date = kSignedDate;
    std::vector<std::byte> content_digest = std::vector<std::byte>(32, std::byte{0x44});
};

void hand_digest(tb::Writer& w, std::string_view reference, std::string_view method,
                 std::span<const std::byte> value) {
    w.field_i32(1, 1);
    w.field_i32(2, 1);
    w.field_string(21, reference);
    w.field_struct(31);
    w.field_string(1, method);
    w.field_string(2, "sha256");
    w.stop();
    w.field_byte_list(32, value);
    w.stop();
}

std::vector<std::byte> hand_built(const std::array<pki::Bytes, 4>& certificates,
                                  const crypto::P256PrivateKey& key, const HandBuilt& in) {
    std::vector<std::byte> der;
    for (const pki::Bytes& certificate : certificates) {
        append(der, certificate);
    }
    // 40 82 LL LL around the four certificates, as the measured blob has it.
    std::vector<std::byte> wrapped = {std::byte{0x40}, std::byte{0x82},
                                      static_cast<std::byte>(der.size() >> 8),
                                      static_cast<std::byte>(der.size() & 0xFF)};
    append(wrapped, der);

    tb::Writer signer;
    signer.field_byte_list(1, in.nonce);
    signer.field_i32(2, 1);
    signer.field_i32(3, 1);
    signer.field_struct(4);
    signer.field_i32(1, 1);
    signer.field_string(2, in.publisher_uuid);
    signer.field_string(3, "Hand Built Publisher");
    signer.field_string(4, "publisher");
    signer.stop();
    signer.field_struct(5);
    signer.field_i32(1, 0);
    signer.field_string(2, "");
    signer.field_string(3, "");
    signer.field_string(4, "product");
    signer.stop();
    signer.field_struct(6);
    signer.field_string(1, "");
    signer.field_string(2, "");
    signer.field_string(3, "");
    signer.field_string(4, "");
    signer.stop();
    signer.field_struct(7);
    signer.field_struct(1);
    signer.field_i32(1, 0);
    signer.field_string(3, "hand built");
    signer.field_string(4, "");
    signer.stop();
    signer.field_string(2, "");
    signer.field_string(3, "");
    signer.field_string(4, "");
    signer.stop();
    signer.field_string(8, in.signed_date);
    signer.field_struct(10);
    signer.field_i32(1, 1);
    signer.field_string(2, "pace-ccjf1");
    signer.field_string(3, "der");
    signer.field_byte_list(4, wrapped);
    signer.stop();
    signer.stop();

    tb::Writer info;
    info.field_i32(1, 1);
    info.field_i32(2, 1);
    info.field_string(3, "b715f9db-6b41-403d-8bbe-429f3209ca2e");
    info.field_struct(4);
    info.field_string(1, "canonical_method_thrift_binary");
    info.field_string(2, "sha256");
    info.stop();
    info.field_struct(5);
    info.field_string(1, "ecdsa");
    info.field_string(2, "secp256r1");
    info.stop();
    info.field_struct(6);
    hand_digest(info, "digest_data_ref_signer_info", "canonical_method_thrift_binary",
                crypto::sha256(signer.bytes()));
    info.field(tb::Type::kList, 7);
    info.list_header(tb::Type::kStruct, 1);
    hand_digest(info, "digest_data_ref_win_signature_data", "canonical_method_raw",
                in.content_digest);
    info.stop();

    const auto signature = *crypto::ecdsa_p256_sign(key, crypto::sha256(info.bytes()));
    tb::Writer data;
    data.field_i32(1, 1);
    data.field_i32(2, 1);
    data.field_string(3, "b715f9db-6b41-403d-8bbe-429f3209ca2e");
    data.field(tb::Type::kMap, 4);
    data.map_header(tb::Type::kString, tb::Type::kList, 2);
    data.string("ecc_r_value");
    data.byte_list(signature.r);
    data.string("ecc_s_value");
    data.byte_list(signature.s);
    data.stop();

    tb::Writer top;
    top.field_i32(1, 1);
    top.field_i32(2, 1);
    top.field_struct(3);
    top.raw(signer.bytes());
    top.field(tb::Type::kList, 4);
    top.list_header(tb::Type::kStruct, 1);
    top.field_struct(1);
    top.raw(info.bytes());
    top.field_struct(2);
    top.raw(data.bytes());
    top.stop();
    top.stop();
    return top.take();
}

// The test chain with its signer certificate issued again, to the same key, with a serialNumber
// attribute in its subject (the test chain has the ones the library's own tests give it) set to
// `subject_attribute`: a UUID, to stand for a certificate that carries one. CS spec v2.6 §5.3: the
// publisher UUID is not in a signing certificate and is not compared with one, so a test that
// gives the certificate another UUID shows that it is not.
struct UuidChain {
    std::array<pki::Bytes, 4> certificates;
    std::vector<crypto::Certificate> parsed;
};

UuidChain chain_with_subject_attribute(const pki::TestChain& base,
                                       const std::string& subject_attribute) {
    UuidChain out;
    out.certificates = base.certificates;
    pki::CertSpec leaf;
    leaf.subject =
        pki::Name{"ICL Forge Test Content Signer", "Code/Content Signer", subject_attribute};
    leaf.serial_hex = "0A0004B5D62000000005";
    leaf.not_before = "2022-06-09T21:56:24";
    leaf.not_after = "2042-06-09T21:56:24";
    leaf.ca = true;
    leaf.key_usage = 1U << 0;
    out.certificates[3] =
        pki::issue(leaf, base.leaf_key, base.names[2], pki::key_for("icl-forge-test-issuing"));
    for (const pki::Bytes& der : out.certificates) {
        out.parsed.push_back(*crypto::parse_certificate(der));
    }
    return out;
}

constexpr const char* kPublisherUuid = "3b9c1f52-7a4e-4d08-b6a1-2f0e9d7c5a84";
constexpr const char* kOtherUuid = "0f0e0d0c-0b0a-4908-8706-050403020100";

}  // namespace

TEST_CASE("a signature written by hand reads and verifies as the library's own does",
          "[crypto][content-signature][v25]") {
    const Fixture f;
    const std::vector<std::byte> blob =
        hand_built(f.chain.certificates, f.chain.leaf_key, HandBuilt{});
    std::array<std::byte, 32> digest{};
    digest.fill(std::byte{0x44});
    const auto report = crypto::verify_content_signature(blob, digest, f.policy());
    for (const crypto::CheckResult& c : report.checks) {
        INFO(c.name << ": " << c.detail);
        CHECK(c.ok);
    }
    CHECK(report.verdict == crypto::Verdict::kChecksChainUnanchored);

    // The library reads every field the hand-built blob carries as it was written.
    const auto parsed = crypto::parse_content_signature(blob);
    REQUIRE(parsed.has_value());
    CHECK(parsed->version == 1);
    CHECK(parsed->signature_type == 1);
    CHECK(parsed->signer_nonce.size() == crypto::kSignerNonceBytes);
}

TEST_CASE("the signature is checked before the digests it authenticates (CS spec v2.6 6.1)",
          "[crypto][content-signature][v25]") {
    const Fixture f;
    auto content = content_of(900);
    const auto blob = crypto::sign_content(content, f.credentials, f.options());
    REQUIRE(blob.has_value());
    const auto digest = crypto::sha256(content);

    const auto index_of = [](const crypto::VerifyReport& r, std::string_view name) {
        for (std::size_t i = 0; i < r.checks.size(); ++i) {
            if (r.checks[i].name == name) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };

    // In a good report: the signature, then the signer-information digest, then the content digest.
    const crypto::VerifyReport good = crypto::verify_content_signature(*blob, digest, f.policy());
    REQUIRE(good.verdict == crypto::Verdict::kChecksChainUnanchored);
    REQUIRE(index_of(good, "signature") >= 0);
    CHECK(index_of(good, "signature") < index_of(good, "signer-information digest"));
    CHECK(index_of(good, "signer-information digest") < index_of(good, "content digest"));
    CHECK(index_of(good, "chain") < index_of(good, "signature"));
    // A digest the signature authenticates says nothing more than that it matches.
    for (const crypto::CheckResult& c : good.checks) {
        CHECK(c.detail.find("authenticates nothing") == std::string::npos);
    }

    // A damaged signature and wrong content together: the first fault is the signature, and the
    // digest checks, which still run, say they rest on a signature that did not verify.
    std::vector<std::byte> damaged = *blob;
    damaged[damaged.size() - 10] ^= std::byte{0x01};  // inside s
    content[100] ^= std::byte{0x01};
    const crypto::VerifyReport bad =
        crypto::verify_content_signature(damaged, crypto::sha256(content), f.policy());
    CHECK(bad.verdict == crypto::Verdict::kInvalid);
    REQUIRE(bad.fault.has_value());
    CHECK(bad.fault->code == crypto::SignatureError::kSignatureInvalid);
    CHECK(has_check(bad, "signature", false));
    CHECK(has_check(bad, "content digest", false));
    for (const crypto::CheckResult& c : bad.checks) {
        if (c.name == "content digest" || c.name == "signer-information digest") {
            INFO(c.name << ": " << c.detail);
            CHECK(c.detail.find("did not verify, so it authenticates nothing") !=
                  std::string::npos);
        }
    }

    // A good signature over content that changed: the digest is the fault, and the report does not
    // doubt the digest's authority.
    content[100] ^= std::byte{0x01};
    content[200] ^= std::byte{0x01};
    const crypto::VerifyReport changed =
        crypto::verify_content_signature(*blob, crypto::sha256(content), f.policy());
    REQUIRE(changed.fault.has_value());
    CHECK(changed.fault->code == crypto::SignatureError::kDigestMismatch);
    CHECK(changed.fault->message.find("authenticates nothing") == std::string::npos);
}

TEST_CASE("every wire version field the signer writes is 1, and scheme version 3 is not one",
          "[crypto][content-signature][v25]") {
    // CS spec v2.6 §3.16: "an implementation SHALL write wire version = 1". Read back from the
    // bytes with the Thrift reader, field by field, and not through the library's parser.
    const Fixture f;
    const auto blob = crypto::sign_content(content_of(10), f.credentials, f.options());
    REQUIRE(blob.has_value());
    const auto top = tb::read_struct(*blob);
    REQUIRE(top.has_value());
    const auto version_of = [](std::span<const tb::Field> fields, std::int16_t id) {
        const tb::Field* field = tb::find(fields, id, tb::Type::kI32);
        REQUIRE(field != nullptr);
        return tb::as_i32(*field);
    };
    CHECK(version_of(*top, 1) == std::optional<std::int32_t>(1));  // ContentSignature.version
    CHECK(version_of(*top, 2) == std::optional<std::int32_t>(1));  // .schemaVersion
    const auto signer = tb::as_struct(*tb::find(*top, 3, tb::Type::kStruct));
    REQUIRE(signer.has_value());
    CHECK(version_of(*signer, 2) == std::optional<std::int32_t>(1));
    CHECK(version_of(*signer, 3) == std::optional<std::int32_t>(1));
    const auto chain_info = tb::as_struct(*tb::find(*signer, 10, tb::Type::kStruct));
    REQUIRE(chain_info.has_value());
    CHECK(version_of(*chain_info, 1) == std::optional<std::int32_t>(1));
    const auto entries = tb::as_struct_list(*tb::find(*top, 4, tb::Type::kList));
    REQUIRE(entries.has_value());
    const auto entry = tb::read_struct(entries->front());
    REQUIRE(entry.has_value());
    const auto signed_info = tb::as_struct(*tb::find(*entry, 1, tb::Type::kStruct));
    REQUIRE(signed_info.has_value());
    CHECK(version_of(*signed_info, 1) == std::optional<std::int32_t>(1));  // canonicalVersion
    CHECK(version_of(*signed_info, 2) == std::optional<std::int32_t>(1));  // version
    const auto signer_digest = tb::as_struct(*tb::find(*signed_info, 6, tb::Type::kStruct));
    REQUIRE(signer_digest.has_value());
    CHECK(version_of(*signer_digest, 1) == std::optional<std::int32_t>(1));
    CHECK(version_of(*signer_digest, 2) == std::optional<std::int32_t>(1));
    const auto content_digests = tb::as_struct_list(*tb::find(*signed_info, 7, tb::Type::kList));
    REQUIRE(content_digests.has_value());
    const auto content_digest = tb::read_struct(content_digests->front());
    REQUIRE(content_digest.has_value());
    CHECK(version_of(*content_digest, 1) == std::optional<std::int32_t>(1));
    CHECK(version_of(*content_digest, 2) == std::optional<std::int32_t>(1));
    const auto data = tb::as_struct(*tb::find(*entry, 2, tb::Type::kStruct));
    REQUIRE(data.has_value());
    CHECK(version_of(*data, 1) == std::optional<std::int32_t>(1));
    CHECK(version_of(*data, 2) == std::optional<std::int32_t>(1));

    // A reader given the scheme release id (3) where a wire version belongs does not know the
    // structure and says so, whichever struct carries it: the signature is refused as unsupported.
    const tb::Field* version = tb::find(*top, 1, tb::Type::kI32);
    REQUIRE(version != nullptr);
    const std::vector<std::byte> three = {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{3}};
    const auto v3 = splice(*blob, version->value, three);
    const auto report =
        crypto::verify_content_signature(v3, crypto::sha256(content_of(10)), f.policy());
    CHECK(report.verdict == crypto::Verdict::kInvalid);
    CHECK(has_check(report, "version markers", false));
    REQUIRE(report.fault.has_value());
    CHECK(report.fault->code == crypto::SignatureError::kLibDoesntSupportContentSignature);
}

TEST_CASE("signing refuses a nonce that is not 128 bytes and a publisher UUID that is wrong",
          "[crypto][content-signature][v25]") {
    const Fixture f;
    const auto content = content_of(32);

    SECTION("a nonce of another length") {
        for (const std::size_t size : {std::size_t{1}, std::size_t{16}, std::size_t{127},
                                       std::size_t{129}, std::size_t{256}}) {
            crypto::SignOptions options = f.options();
            options.nonce.assign(size, std::byte{0x77});
            const auto refused = crypto::sign_content(content, f.credentials, options);
            INFO(size);
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().code == crypto::SignatureError::kBadOrMissingSignature);
            CHECK(refused.error().message.find("asks for 128") != std::string::npos);
        }
        crypto::SignOptions exact = f.options();
        exact.nonce.assign(128, std::byte{0x77});
        CHECK(crypto::sign_content(content, f.credentials, exact).has_value());
    }
    SECTION("a publisher UUID that is not a UUID") {
        for (const char* text :
             {"not-a-uuid", "3B9C1F52-7A4E-4D08-B6A1-2F0E9D7C5A8",
              "3B9C1F52-7A4E-4D08-B6A1-2F0E9D7C5A8G", "3B9C1F527A4E4D08B6A12F0E9D7C5A84",
              "{3B9C1F52-7A4E-4D08-B6A1-2F0E9D7C5A84}"}) {
            crypto::SignOptions options = f.options();
            options.metadata.publisher_uuid = text;
            const auto refused = crypto::sign_content(content, f.credentials, options);
            INFO(text);
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().code == crypto::SignatureError::kBadOrMissingSignature);
            CHECK(refused.error().message.find("is not a UUID") != std::string::npos);
        }
    }
    SECTION("a UUID in the signer certificate's subject is not held against the publisher UUID") {
        // CS spec v2.6 §5.3 withdrew v2.5's rule that the publisher UUID match the certificate's:
        // the UUID is the PACE account's, carried out of band. Signing with a certificate that
        // carries another UUID is signing, and the verifier takes the result.
        const UuidChain chain = chain_with_subject_attribute(f.chain, kPublisherUuid);
        const crypto::SigningCredentials credentials =
            *crypto::make_credentials(f.chain.leaf_key, chain.parsed);
        crypto::SignOptions options = f.options();
        options.metadata.publisher_uuid = kOtherUuid;
        const auto blob = crypto::sign_content(content, credentials, options);
        REQUIRE(blob.has_value());
        crypto::VerifyPolicy policy = f.policy();
        policy.operator_metadata = crypto::OperatorMetadata::kRequire;
        const auto report =
            crypto::verify_content_signature(*blob, crypto::sha256(content), policy);
        CHECK_FALSE(report.fault.has_value());
        CHECK(report.verdict == crypto::Verdict::kChecksChainUnanchored);
        CHECK(report.publisher.find(kOtherUuid) != std::string::npos);
    }
    SECTION("no UUID at all is written as it is: the signer's choice, the verifier's finding") {
        crypto::SignOptions options = f.options();
        options.metadata.publisher_uuid.clear();
        CHECK(crypto::sign_content(content, f.credentials, options).has_value());
        // A UUID is written as it is given, in either case.
        options.metadata.publisher_uuid = kPublisherUuid;
        CHECK(crypto::sign_content(content, f.credentials, options).has_value());
    }
}

TEST_CASE("each signature gets a nonce of its own, 128 bytes, not a constant",
          "[crypto][content-signature][v25]") {
    const Fixture f;
    crypto::SignOptions options = f.options();
    options.nonce.clear();
    std::vector<std::vector<std::byte>> nonces;
    for (int i = 0; i < 8; ++i) {
        const auto blob = crypto::sign_content(content_of(16), f.credentials, options);
        REQUIRE(blob.has_value());
        const auto parsed = crypto::parse_content_signature(*blob);
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->signer_nonce.size() == 128);
        // Not all one byte, not the fixed nonce of the tests, and none of the others.
        CHECK(std::any_of(parsed->signer_nonce.begin(), parsed->signer_nonce.end(),
                          [&](std::byte b) { return b != parsed->signer_nonce.front(); }));
        for (const auto& earlier : nonces) {
            CHECK(earlier != parsed->signer_nonce);
        }
        nonces.push_back(parsed->signer_nonce);
    }
}

TEST_CASE("operator metadata is reported, and decides the verdict only when the policy says so",
          "[crypto][content-signature][v25]") {
    const Fixture f;
    std::array<std::byte, 32> digest{};
    digest.fill(std::byte{0x44});
    const std::vector<std::string> expected_one = {kPublisherUuid};
    const std::vector<std::string> expected_two = {kOtherUuid, kPublisherUuid};
    const std::vector<std::string> expected_other = {kOtherUuid};

    struct Case {
        const char* name;
        std::string uuid;
        std::size_t nonce_size;
        const std::vector<std::string>* expected;  // the accounts the verifier expects, or none
        bool finding_ok;                           // the finding that case is about
        const char* finding;
        crypto::SignatureError strict_fault;
    };
    const std::vector<std::string> nobody;
    const Case cases[] = {
        // Without expected accounts the UUID is reported and compared with nothing.
        {"a UUID, nothing to compare it with", kPublisherUuid, 128, &nobody, true, "publisher UUID",
         crypto::SignatureError::kBadOrMissingSignature},
        {"no UUID", "", 128, &nobody, false, "publisher UUID",
         crypto::SignatureError::kBadOrMissingSignature},
        {"a text that is not a UUID", "not-a-uuid", 128, &nobody, false, "publisher UUID",
         crypto::SignatureError::kBadOrMissingSignature},
        {"a nonce of 16 bytes", kPublisherUuid, 16, &nobody, false, "signer nonce",
         crypto::SignatureError::kBadOrMissingSignature},
        {"a nonce of 129 bytes", kPublisherUuid, 129, &nobody, false, "signer nonce",
         crypto::SignatureError::kBadOrMissingSignature},
        // With them it has to be one of them, in either case.
        {"the expected account", kPublisherUuid, 128, &expected_one, true, "publisher UUID",
         crypto::SignatureError::kBadOrMissingSignature},
        {"the expected account, in capitals", "3B9C1F52-7A4E-4D08-B6A1-2F0E9D7C5A84", 128,
         &expected_one, true, "publisher UUID", crypto::SignatureError::kBadOrMissingSignature},
        {"the second of two expected accounts", kPublisherUuid, 128, &expected_two, true,
         "publisher UUID", crypto::SignatureError::kBadOrMissingSignature},
        {"another account than the expected one", kPublisherUuid, 128, &expected_other, false,
         "publisher UUID", crypto::SignatureError::kBadCredentialsForSigningContent},
        {"no UUID, with an expected account", "", 128, &expected_one, false, "publisher UUID",
         crypto::SignatureError::kBadOrMissingSignature},
    };
    for (const Case& c : cases) {
        INFO(c.name);
        HandBuilt in;
        in.publisher_uuid = c.uuid;
        in.nonce.assign(c.nonce_size, std::byte{0x5C});
        const std::vector<std::byte> blob = hand_built(f.chain.certificates, f.chain.leaf_key, in);

        // Reporting: a good signature stays one, and the finding is there to read.
        crypto::VerifyPolicy report_only = f.policy();
        report_only.operator_metadata = crypto::OperatorMetadata::kReport;
        report_only.expected_publisher_uuids = *c.expected;
        const crypto::VerifyReport reported =
            crypto::verify_content_signature(blob, digest, report_only);
        CHECK(reported.verdict == crypto::Verdict::kChecksChainUnanchored);
        CHECK_FALSE(reported.fault.has_value());
        const auto note =
            std::find_if(reported.notes.begin(), reported.notes.end(),
                         [&](const crypto::CheckResult& n) { return n.name == c.finding; });
        REQUIRE(note != reported.notes.end());
        CHECK(note->ok == c.finding_ok);

        // Requiring: the same signature is refused when the finding is a bad one, with the code
        // and the check that says why, and passes when it is a good one.
        crypto::VerifyPolicy strict = f.policy();
        strict.operator_metadata = crypto::OperatorMetadata::kRequire;
        strict.expected_publisher_uuids = *c.expected;
        const crypto::VerifyReport required =
            crypto::verify_content_signature(blob, digest, strict);
        if (c.finding_ok) {
            CHECK(required.verdict == crypto::Verdict::kChecksChainUnanchored);
            CHECK(has_check(required, c.finding, true));
        } else {
            CHECK(required.verdict == crypto::Verdict::kInvalid);
            REQUIRE(required.fault.has_value());
            CHECK(required.fault->code == c.strict_fault);
            CHECK(has_check(required, c.finding, false));
            // Everything else about the signature still checked out.
            CHECK(has_check(required, "signature", true));
            CHECK(has_check(required, "content digest", true));
        }
    }

    // The signing certificate is not asked for the UUID (CS spec v2.6 §5.3): a certificate that
    // carries another UUID in its subject, and one that carries none, make no difference to the
    // verdict, in either mode, with or without the expected accounts.
    const UuidChain other_uuid = chain_with_subject_attribute(f.chain, kOtherUuid);
    const UuidChain no_uuid = chain_with_subject_attribute(f.chain, "T0004");
    for (const UuidChain* chain : {&other_uuid, &no_uuid}) {
        HandBuilt in;
        in.publisher_uuid = kPublisherUuid;
        const std::vector<std::byte> blob = hand_built(chain->certificates, f.chain.leaf_key, in);
        for (const auto mode :
             {crypto::OperatorMetadata::kReport, crypto::OperatorMetadata::kRequire}) {
            for (const auto* expected : {&nobody, &expected_one}) {
                crypto::VerifyPolicy policy = f.policy();
                policy.operator_metadata = mode;
                policy.expected_publisher_uuids = *expected;
                const crypto::VerifyReport report =
                    crypto::verify_content_signature(blob, digest, policy);
                CHECK(report.verdict == crypto::Verdict::kChecksChainUnanchored);
                CHECK_FALSE(report.fault.has_value());
            }
        }
    }
}

TEST_CASE("the report carries what the signer says about itself, escaped",
          "[crypto][content-signature][v25]") {
    const Fixture f;
    crypto::SignOptions options = f.options();
    options.metadata.publisher_name = "Example Publisher";
    options.metadata.publisher_uuid = kPublisherUuid;
    options.metadata.product_name = "Example Product";
    options.metadata.wrapper_name = "Example Wrapper";
    options.metadata.os_name = "TestOS";
    options.metadata.os_version = "1.2";
    options.metadata.os_description = "TestOS 1.2 on a test";
    options.metadata.application_name = "Signer\x1B[2J";
    options.metadata.application_version = "9.9";
    options.metadata.build_hash = "abc123";
    const auto content = content_of(8);
    const auto blob = crypto::sign_content(content, f.credentials, options);
    REQUIRE(blob.has_value());
    const auto report =
        crypto::verify_content_signature(*blob, crypto::sha256(content), f.policy());
    CHECK(report.publisher == "Example Publisher (UUID 3b9c1f52-7a4e-4d08-b6a1-2f0e9d7c5a84)");
    CHECK(report.product == "Example Product (wrapper Example Wrapper)");
    CHECK(report.platform == "TestOS 1.2 (TestOS 1.2 on a test)");
    CHECK(report.application == "Signer\\x1B[2J 9.9 (build abc123)");

    // Nothing given: nothing to show, and no empty brackets.
    crypto::SignOptions bare = f.options();
    bare.metadata.application_name.clear();
    bare.metadata.application_version.clear();
    const auto plain = crypto::sign_content(content, f.credentials, bare);
    REQUIRE(plain.has_value());
    const auto empty =
        crypto::verify_content_signature(*plain, crypto::sha256(content), f.policy());
    CHECK(empty.publisher.empty());
    CHECK(empty.product.empty());
    CHECK(empty.platform.empty());
    CHECK(empty.application.empty());
}

// ---- The owner's sample ------------------------------------------------------------------
// A ContentSignature blob the owner extracted from a signing tool (see libs/ac4/ERRATA.md for the
// provenance). It is not in the repository and these checks need no key: set
// ICLFORGE_CONTENT_SIGNATURE_SAMPLE to its path. They assert the properties the owner's
// measurements established, using only what the sample itself says (its own stored digests, its own
// date), so that nothing derived from it is written down here.
TEST_CASE("the owner's sample parses and verifies the way the measurements say",
          "[crypto][content-signature][sample]") {
    const char* path = std::getenv("ICLFORGE_CONTENT_SIGNATURE_SAMPLE");
    if (path == nullptr || *path == '\0') {
        SKIP(
            "ICLFORGE_CONTENT_SIGNATURE_SAMPLE is not set; the owner's sample is not part of the "
            "repository");
    }
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> blob;
    for (const char c : raw) {
        blob.push_back(static_cast<std::byte>(c));
    }

    // Field ids, asserted on the raw Thrift.
    const auto top = tb::read_struct(blob);
    REQUIRE(top.has_value());
    CHECK(tb::find(*top, 1, tb::Type::kI32) != nullptr);
    CHECK(tb::find(*top, 2, tb::Type::kI32) != nullptr);
    CHECK(tb::find(*top, 3, tb::Type::kStruct) != nullptr);
    CHECK(tb::find(*top, 4, tb::Type::kList) != nullptr);
    const auto signer = tb::as_struct(*tb::find(*top, 3, tb::Type::kStruct));
    REQUIRE(signer.has_value());
    for (const std::int16_t id :
         {std::int16_t{4}, std::int16_t{5}, std::int16_t{6}, std::int16_t{7}, std::int16_t{10}}) {
        CHECK(tb::find(*signer, id, tb::Type::kStruct) != nullptr);
    }
    CHECK(tb::find(*signer, 1, tb::Type::kList) != nullptr);
    CHECK(tb::find(*signer, 8, tb::Type::kString) != nullptr);
    CHECK(tb::find(*signer, 9, tb::Type::kStruct) == nullptr);  // there is no field 9

    const auto parsed = crypto::parse_content_signature(blob);
    REQUIRE(parsed.has_value());
    CHECK(parsed->signer_nonce.size() == 128);
    CHECK(parsed->signing_method == "pace-ccjf1");
    CHECK(parsed->encoding_format == "der");
    CHECK(parsed->signature_algorithm == "ecdsa");
    CHECK(parsed->signature_curve == "secp256r1");
    REQUIRE(parsed->chain_error.empty());
    REQUIRE(parsed->chain.size() == 4);
    CHECK(parsed->chain.front().self_issued());
    CHECK(parsed->chain.back().subject.title == std::optional<std::string>("Code/Content Signer"));
    CHECK(parsed->content_digests.size() == 1);

    // The signer-information digest is over the signer information as received.
    const auto signer_digest = crypto::sha256(parsed->signer_info_bytes());
    CHECK(std::equal(signer_digest.begin(), signer_digest.end(),
                     parsed->signer_info_digest.value.begin()));
    // The ECDSA signature is over SHA-256 of SignedInfoWire's bytes, under the embedded leaf's key,
    // and is not over the content digest.
    CHECK(crypto::ecdsa_p256_verify(parsed->chain.back().public_key,
                                    crypto::sha256(parsed->signed_info_bytes()),
                                    parsed->signature));
    std::array<std::byte, 32> stored_content_digest{};
    std::copy_n(parsed->content_digests[0].value.begin(), 32, stored_content_digest.begin());
    CHECK_FALSE(crypto::ecdsa_p256_verify(parsed->chain.back().public_key, stored_content_digest,
                                          parsed->signature));

    // The chain verifies with validity periods off and at a time before the second certificate
    // expired; it fails at the signature's own date and at the present, because that certificate
    // has expired.
    crypto::VerifyPolicy policy;
    policy.time_basis = crypto::TimeBasis::kIgnore;
    const auto ignoring = crypto::verify_content_signature(blob, stored_content_digest, policy);
    for (const crypto::CheckResult& c : ignoring.checks) {
        INFO(c.name << ": " << c.detail);
        CHECK(c.ok);
    }
    CHECK(ignoring.verdict == crypto::Verdict::kChecksChainUnanchored);

    policy.time_basis = crypto::TimeBasis::kInstant;
    policy.instant = pki::epoch("2024-01-01T00:00:00");
    // The signed date (2026) is after 'now' for this pinned instant only if 'now' were judged; the
    // instant basis does not judge it against the clock. The chain, though, must pass.
    const auto before_expiry =
        crypto::verify_content_signature(blob, stored_content_digest, policy);
    CHECK(has_check(before_expiry, "chain", true));

    policy.time_basis = crypto::TimeBasis::kSignedDate;
    const auto at_signing = crypto::verify_content_signature(blob, stored_content_digest, policy);
    CHECK(has_check(at_signing, "chain", false));
    REQUIRE(at_signing.fault.has_value());
    CHECK(at_signing.fault->message.find("expired") != std::string::npos);

    policy.time_basis = crypto::TimeBasis::kNow;
    policy.now = std::chrono::duration_cast<std::chrono::seconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count();
    CHECK(has_check(crypto::verify_content_signature(blob, stored_content_digest, policy), "chain",
                    false));

    // CS spec v2.6 §3.16 states the sample's wire version fields are all 1, §2.3 and §3.13 that r
    // and s are 32 raw bytes each, §3.2 that the nonce is 128 bytes and §5.3 what the publisher
    // UUID is. Held against the sample itself, with nothing of it written down here.
    CHECK(parsed->version == 1);
    CHECK(parsed->schema_version == 1);
    CHECK(parsed->signer_version == 1);
    CHECK(parsed->signer_schema_version == 1);
    CHECK(parsed->chain_format_version == 1);
    CHECK(parsed->canonical_version == 1);
    CHECK(parsed->signed_info_version == 1);
    CHECK(parsed->signature_data_version == 1);
    CHECK(parsed->signature_type == 1);
    CHECK(parsed->signer_info_digest.version == 1);
    CHECK(parsed->content_digests.front().version == 1);
    CHECK(parsed->signer_nonce.size() == crypto::kSignerNonceBytes);
    CHECK(crypto::is_uuid(parsed->signer.publisher_uuid));
    // CS spec v2.6 §5.3: the publisher UUID is the PACE account's, carried out of band, and not in
    // the signing certificate. In the sample it is in none of the four certificates, in any
    // encoding the library can be asked about (the text in either case, or its 16 bytes, in any
    // part of the certificate), so a verifier that required it there would refuse this one.
    const std::string& uuid = parsed->signer.publisher_uuid;
    const auto contains = [](std::span<const std::byte> haystack,
                             std::span<const std::byte> needle) {
        return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) !=
               haystack.end();
    };
    std::string lower = uuid;
    std::string upper = uuid;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    std::string hex;  // the 16 bytes the hexadecimal digits spell
    for (const char c : uuid) {
        if (c != '-') {
            hex += c;
        }
    }
    std::vector<std::byte> uuid_bytes;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        uuid_bytes.push_back(static_cast<std::byte>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    REQUIRE(uuid_bytes.size() == 16);
    for (const crypto::Certificate& certificate : parsed->chain) {
        CHECK_FALSE(contains(certificate.der, text_bytes(lower)));
        CHECK_FALSE(contains(certificate.der, text_bytes(upper)));
        CHECK_FALSE(contains(certificate.der, uuid_bytes));
    }
    // Required signer information does not turn the genuine signature into an invalid one when
    // there is nothing to compare the UUID with.
    crypto::VerifyPolicy required;
    required.time_basis = crypto::TimeBasis::kIgnore;
    required.operator_metadata = crypto::OperatorMetadata::kRequire;
    const auto strict = crypto::verify_content_signature(blob, stored_content_digest, required);
    CHECK_FALSE(strict.fault.has_value());
    CHECK(strict.verdict == crypto::Verdict::kChecksChainUnanchored);
    CHECK(!strict.publisher.empty());
    CHECK(!strict.application.empty());
    // With the sample's own account as the expected one, in either case, it passes; with another it
    // is a finding when reporting and a refusal when required. (The UUID is read from the sample,
    // not written here.)
    for (const std::string& account : {uuid, lower, upper}) {
        const std::vector<std::string> expected = {"0f0e0d0c-0b0a-4908-8706-050403020100", account};
        required.expected_publisher_uuids = expected;
        const auto listed = crypto::verify_content_signature(blob, stored_content_digest, required);
        CHECK_FALSE(listed.fault.has_value());
        CHECK(listed.verdict == crypto::Verdict::kChecksChainUnanchored);
        CHECK(has_check(listed, "publisher UUID", true));
    }
    const std::vector<std::string> elsewhere = {"0f0e0d0c-0b0a-4908-8706-050403020100"};
    required.expected_publisher_uuids = elsewhere;
    const auto refused = crypto::verify_content_signature(blob, stored_content_digest, required);
    CHECK(refused.verdict == crypto::Verdict::kInvalid);
    REQUIRE(refused.fault.has_value());
    CHECK(refused.fault->code == crypto::SignatureError::kBadCredentialsForSigningContent);
    CHECK(has_check(refused, "publisher UUID", false));
    CHECK(has_check(refused, "signature", true));  // the rest of the signature is as it was
    crypto::VerifyPolicy reporting = required;
    reporting.operator_metadata = crypto::OperatorMetadata::kReport;
    const auto reported = crypto::verify_content_signature(blob, stored_content_digest, reporting);
    CHECK_FALSE(reported.fault.has_value());
    CHECK(reported.verdict == crypto::Verdict::kChecksChainUnanchored);
    const auto note =
        std::find_if(reported.notes.begin(), reported.notes.end(),
                     [](const crypto::CheckResult& n) { return n.name == "publisher UUID"; });
    REQUIRE(note != reported.notes.end());
    CHECK_FALSE(note->ok);
    // The signature is checked before the digests it holds, in the sample's report as in any.
    const auto order_of = [&](std::string_view name) {
        for (std::size_t i = 0; i < strict.checks.size(); ++i) {
            if (strict.checks[i].name == name) {
                return i;
            }
        }
        return strict.checks.size();
    };
    CHECK(order_of("signature") < order_of("signer-information digest"));
    CHECK(order_of("signer-information digest") < order_of("content digest"));

    // Any other content is refused, with everything else still passing.
    const auto wrong =
        crypto::verify_content_signature(blob, crypto::sha256(text_bytes("other")), [&] {
            crypto::VerifyPolicy p;
            p.time_basis = crypto::TimeBasis::kIgnore;
            return p;
        }());
    CHECK(has_check(wrong, "content digest", false));
    CHECK(has_check(wrong, "signature", true));
}
