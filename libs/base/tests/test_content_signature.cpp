#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
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
