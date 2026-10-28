// Building and judging a pace-ccjf1 content signature (content_signature.hpp). Chains are made by
// tests/support/test_pki.hpp: a root, an intermediate and a leaf shaped like the specification's
// (the leaf is named as the specification's Content Signer is), valid from 2024 to 2034. Every
// negative case starts from a payload that verifies and changes one thing, so a failure names the
// thing that was changed and a case that passed for the wrong reason would show as a different
// error code.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "iclforge/base/crypto/content_signature.hpp"
#include "iclforge/base/crypto/ecdsa_der.hpp"
#include "iclforge/base/crypto/p256.hpp"
#include "iclforge/base/crypto/sha256.hpp"
#include "iclforge/base/crypto/x509.hpp"
#include "test_pki.hpp"

namespace {

using namespace iclforge::base::crypto;
namespace pki = iclforge::test::pki;
using pki::at;
using pki::Bytes;
using E = ContentSignatureError;

constexpr std::int64_t kNow = 1793232000;  // 2026-10-29T00:00:00Z

struct Setup {
    pki::Authority authority;
    SigningCredentials credentials;
    Digest256 digest{};
    std::int64_t signed_unix = at(2026, 10, 28, 12);

    explicit Setup(pki::AuthorityOptions options = {})
        : authority(pki::make_authority(std::move(options))),
          credentials{authority.leaf.keys.key, authority.chain()},
          digest(sha256(pki::bytes_of("the content of the stream"))) {}

    [[nodiscard]] TrustStore trust() const { return trust_of(authority.root.cert); }

    static TrustStore trust_of(const Certificate& anchor) {
        TrustStore store;
        store.anchors.push_back(TrustAnchor{anchor, "test anchor"});
        return store;
    }

    [[nodiscard]] static VerifyOptions options() {
        VerifyOptions o;
        o.chain.now_unix = kNow;
        return o;
    }

    [[nodiscard]] ContentSignature built() const {
        auto cs = build_content_signature(digest, credentials, signed_unix);
        REQUIRE(cs.has_value());
        return std::move(*cs);
    }

    [[nodiscard]] Bytes payload() const {
        auto bytes = sign_digest(digest, credentials, signed_unix);
        REQUIRE(bytes.has_value());
        return std::move(*bytes);
    }

    // The payload of a good signature after `change` has altered its structure.
    [[nodiscard]] Bytes payload_with(const std::function<void(ContentSignature&)>& change) const {
        ContentSignature cs = built();
        change(cs);
        return serialize_content_signature(cs);
    }

    [[nodiscard]] std::expected<VerifiedSignature, ContentSignatureFailure> verify(
        const Bytes& bytes) const {
        return verify_digest(bytes, digest, trust(), options());
    }
};

// The error code a payload is refused with; fails the test if it verifies.
E refused_with(const std::expected<VerifiedSignature, ContentSignatureFailure>& result) {
    INFO("verification was expected to fail");
    REQUIRE_FALSE(result.has_value());
    return result.error().error;
}

bool mentions(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

void flip_bit(std::vector<std::byte>& bytes, std::size_t index, unsigned bit = 0) {
    bytes.at(index) ^= static_cast<std::byte>(1u << bit);
}

// The offset of `needle` in `haystack`; fails the test if it is not there.
std::size_t find_bytes(const Bytes& haystack, std::span<const std::byte> needle) {
    const auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end());
    REQUIRE(it != haystack.end());
    return static_cast<std::size_t>(it - haystack.begin());
}

}  // namespace

TEST_CASE("content signature: a signature made here verifies here", "[content_signature]") {
    const Setup s;
    const Bytes payload = s.payload();
    const auto result = s.verify(payload);
    REQUIRE(result.has_value());
    REQUIRE(result->canonical_version == 3);
    REQUIRE(result->signed_date == "2026-10-28T12:00:00Z");
    REQUIRE(result->signed_unix == s.signed_unix);
    REQUIRE(result->chain.size() == 3);
    REQUIRE(result->leaf().serial_hex() == "1798A2B5D62000000005");
    REQUIRE(result->acceptance.kind == ChainAcceptance::Kind::kAnchor);
    REQUIRE(result->acceptance.trust_index == 0);
    REQUIRE(result->acceptance.expired_intermediates.empty());
}

TEST_CASE("content signature: the structure is the one the specification builds",
          "[content_signature]") {
    // Section 5.2 steps 3 to 8, field by field.
    const Setup s;
    const ContentSignature cs = s.built();

    REQUIRE(cs.certificate_chain.chain_structure == "x509");
    REQUIRE(cs.certificate_chain.format == "der");
    REQUIRE(cs.certificate_chain.pk_standard_name == "secp256r1");
    REQUIRE(cs.certificate_chain.certificates.size() == 3);
    REQUIRE(cs.certificate_chain.certificates[0] == s.authority.root.der);
    REQUIRE(cs.certificate_chain.certificates[1] == s.authority.intermediate.der);
    REQUIRE(cs.certificate_chain.certificates[2] == s.authority.leaf.der);

    // The signer serial is the leaf's, taken from the certificate and not from a constant.
    REQUIRE(cs.signer_info.signer_serial_number == "1798A2B5D62000000005");

    // The signature is DER, and it is the leaf key's signature over the content digest.
    const auto signature = decode_ecdsa_der(cs.signer_info.signature_value);
    REQUIRE(signature.has_value());
    REQUIRE(ecdsa_verify(s.authority.leaf.keys.pub, s.digest, *signature));

    const auto& info = cs.signature_values.signed_info;
    REQUIRE(info.canonical_version == 3);
    REQUIRE(info.signed_date == "2026-10-28T12:00:00Z");
    REQUIRE(info.digests.size() == 1);
    REQUIRE(info.digests[0].reference == "digest_data_ref_signer_info");
    REQUIRE(info.digests[0].digest_value == Bytes(s.digest.begin(), s.digest.end()));

    const auto& details = cs.signature_values.signature_values;
    REQUIRE(details.digest_data_ref == "digest_data_ref_signer_info");
    REQUIRE(details.canonical_method == "thrift_binary");
    REQUIRE(details.hash == "sha256");
    REQUIRE(details.signature_method == "ecdsa");
    REQUIRE(details.pk_standard_name == "secp256r1");
    REQUIRE(details.digest_method == "sha256");
}

TEST_CASE("content signature: signing is deterministic", "[content_signature]") {
    // RFC 6979 nonces, so the same key, chain, digest and time give the same bytes.
    const Setup s;
    REQUIRE(s.payload() == s.payload());
    Setup later;
    later.signed_unix += 1;
    REQUIRE(later.payload() != s.payload());
}

TEST_CASE("content signature: the signer serial comes from the leaf", "[content_signature]") {
    pki::AuthorityOptions options;
    options.leaf_serial = {std::byte{0x0A}, std::byte{0xBC}};
    options.leaf_name = {"Other Signer", "Code/Content Signer", "0ABC"};
    const Setup s(options);
    REQUIRE(s.built().signer_info.signer_serial_number == "0ABC");
    // The default profile names the specification's serial, so this leaf needs its own profile.
    VerifyOptions o = Setup::options();
    o.profile = LeafProfile::any();
    REQUIRE(verify_digest(s.payload(), s.digest, s.trust(), o).has_value());
}

// --- Content, signature and chain damaged one bit at a time -----------------------------------

TEST_CASE("content signature: a changed content digest fails", "[content_signature][tamper]") {
    const Setup s;
    const Bytes payload = s.payload();
    for (std::size_t index = 0; index < 32; index += 7) {
        for (unsigned bit = 0; bit < 8; bit += 3) {
            Digest256 other = s.digest;
            other[index] ^= static_cast<std::byte>(1u << bit);
            const auto result = verify_digest(payload, other, s.trust(), Setup::options());
            INFO("byte " << index << " bit " << bit);
            REQUIRE(refused_with(result) == E::kSignatureInvalid);
            REQUIRE(mentions(result.error().detail, "changed after it was signed"));
        }
    }
}

TEST_CASE("content signature: a damaged signature fails", "[content_signature][tamper]") {
    const Setup s;
    const ContentSignature good = s.built();
    // A flipped bit in the middle of r or of s leaves a valid encoding of another signature.
    const auto der_length = good.signer_info.signature_value.size();
    REQUIRE(der_length >= 68);
    for (const std::size_t index :
         {std::size_t{8}, std::size_t{20}, der_length - 5, der_length - 1}) {
        const Bytes payload = s.payload_with(
            [&](ContentSignature& cs) { flip_bit(cs.signer_info.signature_value, index); });
        INFO("flipped bit 0 of signature byte " << index);
        REQUIRE(refused_with(s.verify(payload)) == E::kSignatureInvalid);
    }
    // A flip in the DER framing is a decoding failure.
    const Bytes framing = s.payload_with([&](ContentSignature& cs) {
        flip_bit(cs.signer_info.signature_value, 0);  // the SEQUENCE tag
    });
    REQUIRE(refused_with(s.verify(framing)) == E::kBadOrMissingSignature);
    // Truncated, padded, emptied.
    const Bytes truncated =
        s.payload_with([&](ContentSignature& cs) { cs.signer_info.signature_value.pop_back(); });
    REQUIRE(refused_with(s.verify(truncated)) == E::kBadOrMissingSignature);
    const Bytes padded = s.payload_with(
        [&](ContentSignature& cs) { cs.signer_info.signature_value.push_back(std::byte{0}); });
    REQUIRE(refused_with(s.verify(padded)) == E::kBadOrMissingSignature);
    const Bytes empty =
        s.payload_with([&](ContentSignature& cs) { cs.signer_info.signature_value.clear(); });
    REQUIRE(refused_with(s.verify(empty)) == E::kBadOrMissingSignature);
}

TEST_CASE("content signature: a signature by another key fails", "[content_signature][tamper]") {
    const Setup s;
    const auto stranger = pki::key_from_seed("a different signer");
    const auto foreign = ecdsa_sign(stranger.key, s.digest);
    REQUIRE(foreign.has_value());
    const Bytes payload = s.payload_with(
        [&](ContentSignature& cs) { cs.signer_info.signature_value = encode_ecdsa_der(*foreign); });
    REQUIRE(refused_with(s.verify(payload)) == E::kSignatureInvalid);
}

TEST_CASE("content signature: signing with a key the chain is not for is refused",
          "[content_signature]") {
    Setup s;
    s.credentials.key = pki::key_from_seed("not the leaf's key").key;
    const auto built = build_content_signature(s.digest, s.credentials, s.signed_unix);
    REQUIRE_FALSE(built.has_value());
    REQUIRE(built.error().error == E::kKeyChainMismatch);
    REQUIRE_FALSE(sign_digest(s.digest, s.credentials, s.signed_unix).has_value());

    // The same finding when the chain is in the wrong order: the last certificate is not the leaf.
    Setup reversed;
    std::reverse(reversed.credentials.chain.begin(), reversed.credentials.chain.end());
    REQUIRE(build_content_signature(reversed.digest, reversed.credentials, reversed.signed_unix)
                .error()
                .error == E::kKeyChainMismatch);
}

TEST_CASE("content signature: signing needs a key, a chain and a sensible time",
          "[content_signature]") {
    Setup s;
    SECTION("no key") {
        s.credentials.key = P256PrivateKey{};
        REQUIRE(build_content_signature(s.digest, s.credentials, s.signed_unix).error().error ==
                E::kCertifierNoPrivateKey);
    }
    SECTION("no chain") {
        s.credentials.chain.clear();
        REQUIRE(build_content_signature(s.digest, s.credentials, s.signed_unix).error().error ==
                E::kBadCredentialsForSigningContent);
    }
    SECTION("a chain too long to carry") {
        s.credentials.chain.assign(kMaxChainLength + 1, s.authority.leaf.cert);
        REQUIRE(build_content_signature(s.digest, s.credentials, s.signed_unix).error().error ==
                E::kBadCredentialsForSigningContent);
    }
    SECTION("a time before 1970 or after year 9999") {
        REQUIRE(build_content_signature(s.digest, s.credentials, -1).error().error ==
                E::kBadContentSignatureDate);
        REQUIRE(build_content_signature(s.digest, s.credentials, 253402300800).error().error ==
                E::kBadContentSignatureDate);
        REQUIRE(build_content_signature(s.digest, s.credentials, 0).has_value());
        REQUIRE(build_content_signature(s.digest, s.credentials, 253402300799).has_value());
    }
}

TEST_CASE("content signature: a damaged certificate fails the chain",
          "[content_signature][tamper]") {
    const Setup s;
    // The last byte of a certificate is in its signature; the intermediate's and the leaf's must
    // verify under their issuers.
    for (const std::size_t position : {std::size_t{1}, std::size_t{2}}) {
        const Bytes payload = s.payload_with([&](ContentSignature& cs) {
            auto& der = cs.certificate_chain.certificates[position];
            flip_bit(der, der.size() - 1);
        });
        const auto result = s.verify(payload);
        INFO("certificate " << position);
        REQUIRE(refused_with(result) == E::kBadCredentialsForSigningContent);
        REQUIRE(result.error().chain.has_value());
        REQUIRE(result.error().chain->error == ChainError::kBadSignature);
        REQUIRE(result.error().chain->index == position);
    }
    // A bit in each certificate's public key: the root no longer matches the anchor, and the
    // others no longer parse or no longer verify.
    const struct {
        std::size_t position;
        P256PublicKey key;
    } keys[] = {{0, s.authority.root.keys.pub},
                {1, s.authority.intermediate.keys.pub},
                {2, s.authority.leaf.keys.pub}};
    for (const auto& entry : keys) {
        const Bytes payload = s.payload_with([&](ContentSignature& cs) {
            auto& der = cs.certificate_chain.certificates[entry.position];
            const auto point = entry.key.uncompressed();
            flip_bit(der, find_bytes(der, point) + 40, 2);
        });
        INFO("public key of certificate " << entry.position);
        REQUIRE(refused_with(s.verify(payload)) == E::kBadCredentialsForSigningContent);
    }
    // Not a certificate at all, and a certificate with something after it.
    const Bytes garbage = s.payload_with([&](ContentSignature& cs) {
        cs.certificate_chain.certificates[2] = pki::bytes_of("not DER");
    });
    const auto garbage_result = s.verify(garbage);
    REQUIRE(refused_with(garbage_result) == E::kBadCredentialsForSigningContent);
    REQUIRE(mentions(garbage_result.error().detail, "certificate 3 of 3 cannot be read"));
    const Bytes trailing = s.payload_with([&](ContentSignature& cs) {
        cs.certificate_chain.certificates[2].push_back(std::byte{0});
    });
    REQUIRE(refused_with(s.verify(trailing)) == E::kBadCredentialsForSigningContent);
}

TEST_CASE("content signature: a chain out of order, cut, or swapped fails",
          "[content_signature][tamper]") {
    const Setup s;
    SECTION("leaf first, as section 6.1 reads") {
        // The last certificate is taken as the signer's, and here it is the root, which is also
        // the operator's anchor, so the chain checks pass and the signer's serial number does
        // not. The verdict is a refusal either way, and the detail says what is probably wrong.
        const Bytes payload = s.payload_with([](ContentSignature& cs) {
            std::reverse(cs.certificate_chain.certificates.begin(),
                         cs.certificate_chain.certificates.end());
        });
        const auto result = s.verify(payload);
        REQUIRE(refused_with(result) == E::kSignerSerialMismatch);
        REQUIRE(mentions(result.error().detail, "leaf to root"));
    }
    SECTION("the intermediate dropped") {
        const Bytes payload = s.payload_with([](ContentSignature& cs) {
            cs.certificate_chain.certificates.erase(cs.certificate_chain.certificates.begin() + 1);
        });
        REQUIRE(refused_with(s.verify(payload)) == E::kBadCredentialsForSigningContent);
    }
    SECTION("the leaf alone, under a root that did not issue it") {
        const Bytes payload = s.payload_with([](ContentSignature& cs) {
            cs.certificate_chain.certificates.erase(cs.certificate_chain.certificates.begin(),
                                                    cs.certificate_chain.certificates.begin() + 2);
        });
        REQUIRE(refused_with(s.verify(payload)) == E::kBadCredentialsForSigningContent);
    }
    SECTION("a chain of 17") {
        const Bytes payload = s.payload_with([&](ContentSignature& cs) {
            cs.certificate_chain.certificates.assign(kMaxChainLength + 1, s.authority.leaf.der);
        });
        const auto result = s.verify(payload);
        REQUIRE(refused_with(result) == E::kBadCredentialsForSigningContent);
        REQUIRE(mentions(result.error().detail, "at most 16"));
    }
    SECTION("no certificates") {
        const Bytes payload =
            s.payload_with([](ContentSignature& cs) { cs.certificate_chain.certificates.clear(); });
        REQUIRE(refused_with(s.verify(payload)) == E::kBadOrMissingSignature);
    }
}

// --- Trust ------------------------------------------------------------------------------------

TEST_CASE("content signature: the wrong anchor, an empty store, no clock",
          "[content_signature][trust]") {
    const Setup s;
    const Bytes payload = s.payload();
    const Setup other([] {
        pki::AuthorityOptions o;
        o.seed = "another authority";
        return o;
    }());

    SECTION("an anchor that did not issue the chain") {
        const auto result = verify_digest(
            payload, s.digest, Setup::trust_of(other.authority.root.cert), Setup::options());
        REQUIRE(refused_with(result) == E::kBadCredentialsForSigningContent);
        REQUIRE(result.error().chain.has_value());
        REQUIRE(result.error().chain->error == ChainError::kUntrusted);
    }
    SECTION("an empty trust store is reported before anything about the payload") {
        const auto result = verify_digest(payload, s.digest, TrustStore{}, Setup::options());
        REQUIRE(refused_with(result) == E::kNoTrustAnchors);
        REQUIRE(refused_with(verify_digest({}, s.digest, TrustStore{}, Setup::options())) ==
                E::kNoTrustAnchors);
    }
    SECTION("no time is reported before anything about the payload") {
        const auto result = verify_digest(payload, s.digest, s.trust(), VerifyOptions{});
        REQUIRE(refused_with(result) == E::kNoClock);
        REQUIRE(refused_with(verify_digest({}, s.digest, s.trust(), VerifyOptions{})) ==
                E::kNoClock);
    }
    SECTION("several anchors: the one that accepts is named") {
        TrustStore store = Setup::trust_of(other.authority.root.cert);
        store.anchors.push_back(TrustAnchor{s.authority.root.cert, "second"});
        const auto result = verify_digest(payload, s.digest, store, Setup::options());
        REQUIRE(result.has_value());
        REQUIRE(result->acceptance.kind == ChainAcceptance::Kind::kAnchor);
        REQUIRE(result->acceptance.trust_index == 1);
    }
    SECTION("a trusted key is enough, and says so") {
        TrustStore store;
        store.keys.push_back(TrustedKey{other.authority.leaf.keys.pub, "stranger"});
        store.keys.push_back(TrustedKey{s.authority.leaf.keys.pub, "signer"});
        const auto result = verify_digest(payload, s.digest, store, Setup::options());
        REQUIRE(result.has_value());
        REQUIRE(result->acceptance.kind == ChainAcceptance::Kind::kTrustedKey);
        REQUIRE(result->acceptance.trust_index == 1);
    }
    SECTION("a key nobody signed with") {
        TrustStore store;
        store.keys.push_back(TrustedKey{other.authority.leaf.keys.pub, "stranger"});
        REQUIRE(refused_with(verify_digest(payload, s.digest, store, Setup::options())) ==
                E::kBadCredentialsForSigningContent);
    }
    SECTION("the intermediate as the anchor, when the operator holds no root") {
        const auto result = verify_digest(
            payload, s.digest, Setup::trust_of(s.authority.intermediate.cert), Setup::options());
        REQUIRE(result.has_value());
    }
}

TEST_CASE("content signature: validity periods are judged at the time given",
          "[content_signature][time]") {
    SECTION("the situation of the specification: an expired intermediate") {
        // The specification's Master CA ended on 2025-11-13. Strict validation refuses the chain;
        // the opt-in relaxation accepts it and names the certificate it forgave.
        pki::AuthorityOptions o;
        o.intermediate_not_after = at(2025, 11, 13);
        const Setup s(o);
        const Bytes payload = s.payload();

        const auto strict = s.verify(payload);
        REQUIRE(refused_with(strict) == E::kBadCredentialsForSigningContent);
        REQUIRE(strict.error().chain->error == ChainError::kExpired);
        REQUIRE(strict.error().chain->index == 1);
        REQUIRE(mentions(strict.error().detail, "certificate 2 of 3"));

        VerifyOptions relaxed = Setup::options();
        relaxed.chain.allow_expired_intermediates = true;
        const auto result = verify_digest(payload, s.digest, s.trust(), relaxed);
        REQUIRE(result.has_value());
        REQUIRE(result->acceptance.expired_intermediates == std::vector<std::size_t>{1});

        // Before it expired, strict validation accepts it.
        VerifyOptions earlier = Setup::options();
        earlier.chain.now_unix = at(2025, 6, 1);
        Setup before(o);
        before.signed_unix = at(2025, 5, 1);
        REQUIRE(
            verify_digest(before.payload(), before.digest, before.trust(), earlier).has_value());
    }
    SECTION("the workaround: anchor on the issuing CA, whose root has expired") {
        pki::AuthorityOptions o;
        o.root_not_after = at(2025, 11, 13);
        const Setup s(o);
        const Bytes payload = s.payload();
        REQUIRE(refused_with(s.verify(payload)) == E::kBadCredentialsForSigningContent);
        REQUIRE(verify_digest(payload, s.digest, Setup::trust_of(s.authority.intermediate.cert),
                              Setup::options())
                    .has_value());
    }
    SECTION("an expired leaf fails under either policy") {
        pki::AuthorityOptions o;
        o.leaf_not_after = at(2026, 1, 1);
        const Setup s(o);
        const Bytes payload = s.payload();
        VerifyOptions relaxed = Setup::options();
        relaxed.chain.allow_expired_intermediates = true;
        REQUIRE(refused_with(s.verify(payload)) == E::kBadCredentialsForSigningContent);
        REQUIRE(refused_with(verify_digest(payload, s.digest, s.trust(), relaxed)) ==
                E::kBadCredentialsForSigningContent);
    }
    SECTION("an expired anchor fails under either policy") {
        pki::AuthorityOptions o;
        o.root_not_after = at(2025, 11, 13);
        const Setup s(o);
        VerifyOptions relaxed = Setup::options();
        relaxed.chain.allow_expired_intermediates = true;
        REQUIRE(refused_with(verify_digest(s.payload(), s.digest, s.trust(), relaxed)) ==
                E::kBadCredentialsForSigningContent);
    }
    SECTION("a certificate that is not valid yet") {
        pki::AuthorityOptions o;
        o.leaf_not_before = at(2027, 1, 1);
        const Setup s(o);
        const auto result = s.verify(s.payload());
        REQUIRE(refused_with(result) == E::kBadCredentialsForSigningContent);
        REQUIRE(result.error().chain->error == ChainError::kNotYetValid);
    }
}

// --- Leaf --------------------------------------------------------------------------------------

TEST_CASE("content signature: the signer serial must be the leaf's",
          "[content_signature][signer]") {
    const Setup s;
    SECTION("another serial") {
        const Bytes payload = s.payload_with([](ContentSignature& cs) {
            cs.signer_info.signer_serial_number = "1798A2B5D62000000006";
        });
        const auto result = s.verify(payload);
        REQUIRE(refused_with(result) == E::kSignerSerialMismatch);
        REQUIRE(mentions(result.error().detail, "1798A2B5D62000000005"));
    }
    SECTION("not hexadecimal, or empty") {
        for (const std::string_view text : {"", "xyz", "1798A2B5D6200000000G"}) {
            const Bytes payload = s.payload_with([&](ContentSignature& cs) {
                cs.signer_info.signer_serial_number = std::string{text};
            });
            INFO("serial '" << text << "'");
            REQUIRE(refused_with(s.verify(payload)) == E::kSignerSerialMismatch);
        }
    }
    SECTION("case and leading zeros are spelling, not difference") {
        for (const std::string_view text : {"1798a2b5d62000000005", "001798A2B5D62000000005"}) {
            const Bytes payload = s.payload_with([&](ContentSignature& cs) {
                cs.signer_info.signer_serial_number = std::string{text};
            });
            INFO("serial '" << text << "'");
            REQUIRE(s.verify(payload).has_value());
        }
    }
}

TEST_CASE("content signature: the leaf must be one that may sign content",
          "[content_signature][signer]") {
    SECTION("the specification's profile is the default") {
        const LeafProfile profile;
        REQUIRE(profile.title == "Code/Content Signer");
        REQUIRE(profile.common_name == "ILok");
        REQUIRE(profile.serial_hex == "1798A2B5D62000000005");
        REQUIRE(profile.require_digital_signature);
        const LeafProfile any = LeafProfile::any();
        REQUIRE_FALSE(any.title.has_value());
        REQUIRE_FALSE(any.common_name.has_value());
        REQUIRE_FALSE(any.serial_hex.has_value());
        REQUIRE(any.require_digital_signature);
    }
    SECTION("a leaf with another title") {
        pki::AuthorityOptions o;
        o.leaf_name = {"ILok", "Something Else", "1798A2B5D62000000005"};
        const Setup s(o);
        const auto result = s.verify(s.payload());
        REQUIRE(refused_with(result) == E::kCertNotPrivilegedToSignContent);
        REQUIRE(mentions(result.error().detail, "title"));
        VerifyOptions custom = Setup::options();
        custom.profile.title = "Something Else";
        REQUIRE(verify_digest(s.payload(), s.digest, s.trust(), custom).has_value());
        custom.profile = LeafProfile::any();
        REQUIRE(verify_digest(s.payload(), s.digest, s.trust(), custom).has_value());
    }
    SECTION("a leaf with another common name") {
        pki::AuthorityOptions o;
        o.leaf_name = {"Somebody Else", "Code/Content Signer", "1798A2B5D62000000005"};
        const Setup s(o);
        const auto result = s.verify(s.payload());
        REQUIRE(refused_with(result) == E::kCertNotPrivilegedToSignContent);
        REQUIRE(mentions(result.error().detail, "common name"));
        VerifyOptions custom = Setup::options();
        custom.profile.common_name = "Somebody Else";
        REQUIRE(verify_digest(s.payload(), s.digest, s.trust(), custom).has_value());
    }
    SECTION("a leaf with another serial number") {
        pki::AuthorityOptions o;
        o.leaf_serial = {std::byte{0x42}};
        const Setup s(o);
        const auto result = s.verify(s.payload());
        REQUIRE(refused_with(result) == E::kCertNotPrivilegedToSignContent);
        REQUIRE(mentions(result.error().detail, "serial"));
        VerifyOptions custom = Setup::options();
        custom.profile.serial_hex = "42";
        REQUIRE(verify_digest(s.payload(), s.digest, s.trust(), custom).has_value());
    }
    SECTION("a leaf without the title attribute at all") {
        pki::AuthorityOptions o;
        o.leaf_name = {"ILok", "", ""};
        const Setup s(o);
        const auto result = s.verify(s.payload());
        REQUIRE(refused_with(result) == E::kCertNotPrivilegedToSignContent);
        REQUIRE(mentions(result.error().detail, "(none)"));
    }
    SECTION("key usage without digitalSignature") {
        pki::AuthorityOptions o;
        o.leaf_key_usage = {1};  // nonRepudiation only
        const Setup s(o);
        const auto result = s.verify(s.payload());
        REQUIRE(refused_with(result) == E::kCertNotPrivilegedToSignContent);
        REQUIRE(mentions(result.error().detail, "digitalSignature"));
        VerifyOptions custom = Setup::options();
        custom.profile.require_digital_signature = false;
        REQUIRE(verify_digest(s.payload(), s.digest, s.trust(), custom).has_value());
    }
    SECTION("no key usage extension") {
        pki::AuthorityOptions o;
        o.leaf_key_usage = {};
        const Setup s(o);
        REQUIRE(refused_with(s.verify(s.payload())) == E::kCertNotPrivilegedToSignContent);
    }
}

// --- Scheme: versions, names, curve -----------------------------------------------------------

TEST_CASE("content signature: only version 3 is read", "[content_signature][version]") {
    const Setup s;
    for (const std::int32_t version : {0, 1, 2, 4, 5, -1, 255, 65536}) {
        const Bytes payload = s.payload_with([&](ContentSignature& cs) {
            cs.signature_values.signed_info.canonical_version = version;
        });
        const auto result = s.verify(payload);
        INFO("version " << version);
        REQUIRE(refused_with(result) == E::kLibDoesntSupportContentSignature);
        REQUIRE(mentions(result.error().detail, "canonicalVersion is " + std::to_string(version)));
    }
    const Bytes v3 = s.payload_with(
        [](ContentSignature& cs) { cs.signature_values.signed_info.canonical_version = 3; });
    REQUIRE(s.verify(v3).has_value());
}

TEST_CASE("content signature: method and structure names are checked",
          "[content_signature][scheme]") {
    const Setup s;
    struct Case {
        const char* what;
        std::function<void(ContentSignature&)> change;
        E expected;
    };
    const Case cases[] = {
        {"chainStructure",
         [](ContentSignature& cs) { cs.certificate_chain.chain_structure = "pgp"; },
         E::kLibDoesntSupportContentSignature},
        {"format", [](ContentSignature& cs) { cs.certificate_chain.format = "pem"; },
         E::kLibDoesntSupportContentSignature},
        {"canonical_method",
         [](ContentSignature& cs) {
             cs.signature_values.signature_values.canonical_method = "json";
         },
         E::kLibDoesntSupportContentSignature},
        {"hash", [](ContentSignature& cs) { cs.signature_values.signature_values.hash = "sha1"; },
         E::kLibDoesntSupportContentSignature},
        {"signatureMethod",
         [](ContentSignature& cs) {
             cs.signature_values.signature_values.signature_method = "rsa";
         },
         E::kLibDoesntSupportContentSignature},
        {"digestMethod",
         [](ContentSignature& cs) {
             cs.signature_values.signature_values.digest_method = "sha512";
         },
         E::kLibDoesntSupportContentSignature},
        {"digest_data_ref",
         [](ContentSignature& cs) {
             cs.signature_values.signature_values.digest_data_ref = "other";
         },
         E::kLibDoesntSupportContentSignature},
        {"chain pkStandardName",
         [](ContentSignature& cs) { cs.certificate_chain.pk_standard_name = "secp384r1"; },
         E::kBadContentSignaturePKStandard},
        {"details pkStandardName",
         [](ContentSignature& cs) {
             cs.signature_values.signature_values.pk_standard_name = "P-256";
         },
         E::kBadContentSignaturePKStandard},
        {"an empty name",
         [](ContentSignature& cs) { cs.signature_values.signature_values.hash.clear(); },
         E::kLibDoesntSupportContentSignature},
        {"a name with one more character",
         [](ContentSignature& cs) { cs.signature_values.signature_values.hash = "sha256 "; },
         E::kLibDoesntSupportContentSignature},
        {"a different case",
         [](ContentSignature& cs) { cs.certificate_chain.chain_structure = "X509"; },
         E::kLibDoesntSupportContentSignature},
    };
    for (const Case& c : cases) {
        INFO(c.what);
        REQUIRE(refused_with(s.verify(s.payload_with(c.change))) == c.expected);
    }
}

TEST_CASE("content signature: the scheme is checked before the chain, the chain before the signer",
          "[content_signature][order]") {
    // Each pair of faults reports the one that comes first in verify_digest's documented order.
    const Setup s;
    const Setup other([] {
        pki::AuthorityOptions o;
        o.seed = "another authority";
        return o;
    }());
    const TrustStore wrong_anchor = Setup::trust_of(other.authority.root.cert);

    // unsupported version and untrusted chain: the version.
    const Bytes v2 = s.payload_with(
        [](ContentSignature& cs) { cs.signature_values.signed_info.canonical_version = 2; });
    REQUIRE(refused_with(verify_digest(v2, s.digest, wrong_anchor, Setup::options())) ==
            E::kLibDoesntSupportContentSignature);

    // untrusted chain and wrong serial: the chain.
    const Bytes serial =
        s.payload_with([](ContentSignature& cs) { cs.signer_info.signer_serial_number = "01"; });
    REQUIRE(refused_with(verify_digest(serial, s.digest, wrong_anchor, Setup::options())) ==
            E::kBadCredentialsForSigningContent);

    // wrong serial and a changed content: the serial.
    Digest256 changed = s.digest;
    changed[0] ^= std::byte{1};
    REQUIRE(refused_with(verify_digest(serial, changed, s.trust(), Setup::options())) ==
            E::kSignerSerialMismatch);

    // a changed content and a bad date: the signature.
    const Bytes bad_date = s.payload_with(
        [](ContentSignature& cs) { cs.signature_values.signed_info.signed_date = "not a date"; });
    REQUIRE(refused_with(verify_digest(bad_date, changed, s.trust(), Setup::options())) ==
            E::kSignatureInvalid);

    // a bad date and a wrongly recorded digest: the date.
    const Bytes both = s.payload_with([](ContentSignature& cs) {
        cs.signature_values.signed_info.signed_date = "not a date";
        cs.signature_values.signed_info.digests[0].digest_value.assign(32, std::byte{0});
    });
    REQUIRE(refused_with(s.verify(both)) == E::kBadContentSignatureDate);
}

// --- Metadata: date and digest list -----------------------------------------------------------

TEST_CASE("content signature: the signing date is bounded", "[content_signature][date]") {
    const Setup s;
    const auto with_date = [&](const std::string& date) {
        return s.payload_with(
            [&](ContentSignature& cs) { cs.signature_values.signed_info.signed_date = date; });
    };

    SECTION("forms that are not an ISO 8601 UTC time") {
        for (const std::string date :
             {"", "yesterday", "2026-10-28", "2026-10-28T12:00:00", "2026-10-28 12:00:00Z",
              "2026-10-28T12:00:00+10:00", "2026-13-28T12:00:00Z", "2026-02-30T12:00:00Z",
              "2026-10-28T25:00:00Z", "20261028T120000Z", "2026-10-28T12:00:00Zjunk"}) {
            INFO("date '" << date << "'");
            REQUIRE(refused_with(s.verify(with_date(date))) == E::kBadContentSignatureDate);
        }
    }
    SECTION("a fraction of a second is accepted") {
        REQUIRE(s.verify(with_date("2026-10-28T12:00:00.250Z")).has_value());
    }
    SECTION("before the leaf was valid") {
        const auto result = s.verify(with_date("2023-12-31T23:59:59Z"));
        REQUIRE(refused_with(result) == E::kBadContentSignatureDate);
        REQUIRE(mentions(result.error().detail, "before the signer's certificate is valid"));
        REQUIRE(s.verify(with_date("2024-01-01T00:00:00Z")).has_value());
    }
    SECTION("after the leaf expires") {
        // The verification time is within validity, so only the date is out of range.
        const auto result = s.verify(with_date("2034-01-01T00:00:01Z"));
        REQUIRE(refused_with(result) == E::kBadContentSignatureDate);
    }
    SECTION("later than the clock, beyond the allowed skew") {
        // The verification time is kNow; a day of skew is allowed.
        REQUIRE(s.verify(with_date("2026-10-29T23:59:59Z")).has_value());
        const auto result = s.verify(with_date("2026-10-30T00:00:01Z"));
        REQUIRE(refused_with(result) == E::kBadContentSignatureDate);
        REQUIRE(mentions(result.error().detail, "later than the verification time"));
        VerifyOptions strict = Setup::options();
        strict.max_future_skew_seconds = 0;
        REQUIRE(verify_digest(with_date("2026-10-29T00:00:01Z"), s.digest, s.trust(), strict)
                    .error()
                    .error == E::kBadContentSignatureDate);
        REQUIRE(verify_digest(with_date("2026-10-29T00:00:00Z"), s.digest, s.trust(), strict)
                    .has_value());
    }
}

TEST_CASE("content signature: the date is not covered by the signature",
          "[content_signature][date]") {
    // Specification section 5.2 signs the content digest and nothing else (register item 4), so a
    // payload with another date inside the allowed range still verifies. This test records that
    // property so that nobody takes the date for authenticated.
    const Setup s;
    const Bytes redated = s.payload_with([](ContentSignature& cs) {
        cs.signature_values.signed_info.signed_date = "2025-01-01T00:00:00Z";
    });
    const auto result = s.verify(redated);
    REQUIRE(result.has_value());
    REQUIRE(result->signed_date == "2025-01-01T00:00:00Z");
}

TEST_CASE("content signature: the digest list", "[content_signature][digest]") {
    const Setup s;
    SECTION("no entries") {
        const Bytes payload = s.payload_with(
            [](ContentSignature& cs) { cs.signature_values.signed_info.digests.clear(); });
        REQUIRE(refused_with(s.verify(payload)) == E::kBadOrMissingSignature);
    }
    SECTION("an entry that records another digest") {
        const Bytes payload = s.payload_with([](ContentSignature& cs) {
            flip_bit(cs.signature_values.signed_info.digests[0].digest_value, 5);
        });
        REQUIRE(refused_with(s.verify(payload)) == E::kDigestMismatch);
    }
    SECTION("an entry of the wrong length") {
        const Bytes payload = s.payload_with([](ContentSignature& cs) {
            cs.signature_values.signed_info.digests[0].digest_value.pop_back();
        });
        REQUIRE(refused_with(s.verify(payload)) == E::kDigestMismatch);
    }
    SECTION("no entry for the content") {
        const Bytes payload = s.payload_with([](ContentSignature& cs) {
            cs.signature_values.signed_info.digests[0].reference = "something_else";
        });
        const auto result = s.verify(payload);
        REQUIRE(refused_with(result) == E::kDigestMismatch);
        REQUIRE(mentions(result.error().detail, "lists no digest named"));
    }
    SECTION("an extra entry for something else is ignored") {
        const Bytes payload = s.payload_with([](ContentSignature& cs) {
            cs.signature_values.signed_info.digests.push_back(
                LsSignedDigestWire{Bytes(32, std::byte{0xEE}), "another_reference"});
        });
        REQUIRE(s.verify(payload).has_value());
    }
    SECTION("an extra entry for the content that disagrees is not") {
        const Bytes payload = s.payload_with([](ContentSignature& cs) {
            cs.signature_values.signed_info.digests.push_back(
                LsSignedDigestWire{Bytes(32, std::byte{0xEE}), std::string{kDigestDataRef}});
        });
        REQUIRE(refused_with(s.verify(payload)) == E::kDigestMismatch);
    }
    SECTION("a second entry that agrees is fine") {
        const Bytes payload = s.payload_with([](ContentSignature& cs) {
            cs.signature_values.signed_info.digests.push_back(
                cs.signature_values.signed_info.digests[0]);
        });
        REQUIRE(s.verify(payload).has_value());
    }
}

// --- Payload damage ---------------------------------------------------------------------------

TEST_CASE("content signature: a truncated payload never verifies", "[content_signature][tamper]") {
    const Setup s;
    const Bytes payload = s.payload();
    REQUIRE(payload.size() > 500);
    for (std::size_t n = 0; n < payload.size(); ++n) {
        const Bytes prefix(payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(n));
        INFO("cut at " << n << " of " << payload.size());
        REQUIRE(refused_with(s.verify(prefix)) == E::kBadOrMissingSignature);
    }
    Bytes extended = payload;
    extended.push_back(std::byte{0});
    REQUIRE(refused_with(s.verify(extended)) == E::kBadOrMissingSignature);
}

TEST_CASE("content signature: a flipped bit anywhere is refused, with three named exceptions",
          "[content_signature][tamper]") {
    // One bit flipped in each byte of a good payload, in turn. A payload is Thrift framing, three
    // certificates, a signature, a digest and some strings, and a flip in any of them must make
    // verification fail, except in:
    //   - the root certificate: the operator's anchor vouches for the chain, matched on subject and
    //     key, and the copy in the payload above it is not consulted (x509.hpp);
    //   - the signing date, which the signature does not cover (register item 4), and which stays
    //     a valid date or becomes an invalid one;
    //   - the signer serial number's spelling: a letter flipped to the other case is the same
    //     number.
    // Those three bounds are found in the payload and skipped; nothing else may be.
    const Setup s;
    const Bytes payload = s.payload();

    const auto root_at = find_bytes(payload, s.authority.root.der);
    const auto root_end = root_at + s.authority.root.der.size();
    const auto date = pki::bytes_of("2026-10-28T12:00:00Z");
    const auto date_at = find_bytes(payload, date);
    const auto serial = pki::bytes_of("1798A2B5D62000000005");
    // The serial text is also in the leaf's subject; the one in the signer info is the last.
    const auto serial_at = static_cast<std::size_t>(
        std::find_end(payload.begin(), payload.end(), serial.begin(), serial.end()) -
        payload.begin());
    REQUIRE(serial_at > date_at - 400);  // after the certificates, in the signer info

    std::size_t checked = 0;
    for (std::size_t i = 0; i < payload.size(); ++i) {
        const bool in_root = i >= root_at && i < root_end;
        const bool in_date = i >= date_at && i < date_at + date.size();
        const bool in_serial = i >= serial_at && i < serial_at + serial.size();
        if (in_root || in_date || in_serial) {
            continue;
        }
        Bytes damaged = payload;
        flip_bit(damaged, i, static_cast<unsigned>(i % 8));
        INFO("bit " << (i % 8) << " of byte " << i << " of " << payload.size());
        REQUIRE_FALSE(s.verify(damaged).has_value());
        ++checked;
    }
    // Most of the payload was exercised.
    REQUIRE(checked > payload.size() - s.authority.root.der.size() - 60);
}

TEST_CASE("content signature: error codes have names and words", "[content_signature]") {
    const E all[] = {E::kBadOrMissingSignature,
                     E::kBadContentSignaturePKStandard,
                     E::kBadContentSignatureDate,
                     E::kCertNotPrivilegedToSignContent,
                     E::kBadCredentialsForSigningContent,
                     E::kLibDoesntSupportContentSignature,
                     E::kCertifierNoPrivateKey,
                     E::kSignatureInvalid,
                     E::kDigestMismatch,
                     E::kSignerSerialMismatch,
                     E::kNoTrustAnchors,
                     E::kNoClock,
                     E::kKeyChainMismatch};
    std::vector<std::string_view> names;
    for (const E e : all) {
        REQUIRE_FALSE(error_name(e).empty());
        REQUIRE(error_name(e) != "Unknown");
        REQUIRE_FALSE(describe(e).empty());
        REQUIRE(describe(e) != "unknown error");
        names.push_back(error_name(e));
    }
    std::sort(names.begin(), names.end());
    REQUIRE(std::adjacent_find(names.begin(), names.end()) == names.end());

    // The seven of section 6.2 under the specification's own spelling.
    REQUIRE(error_name(E::kBadOrMissingSignature) == "BadOrMissingSignature");
    REQUIRE(error_name(E::kBadContentSignaturePKStandard) == "BadContentSignaturePKStandard");
    REQUIRE(error_name(E::kBadContentSignatureDate) == "BadContentSignatureDate");
    REQUIRE(error_name(E::kCertNotPrivilegedToSignContent) == "CertNotPrivilegedToSignContent");
    REQUIRE(error_name(E::kBadCredentialsForSigningContent) == "BadCredentialsForSigningContent");
    REQUIRE(error_name(E::kLibDoesntSupportContentSignature) == "LibDoesntSupportContentSignature");
    REQUIRE(error_name(E::kCertifierNoPrivateKey) == "CertifierNoPrivateKey");
    for (std::size_t i = 0; i < 7; ++i) {
        REQUIRE(in_specification(all[i]));
    }
    for (std::size_t i = 7; i < std::size(all); ++i) {
        REQUIRE_FALSE(in_specification(all[i]));
    }
}

TEST_CASE("content signature: the constants are the specification's", "[content_signature]") {
    REQUIRE(kContentSignatureScheme == "pace-ccjf1");
    REQUIRE(kContentSignatureUuid == "b715f9db-6b41-403d-8bbe-429f3209ca2e");
    REQUIRE(kContentSignatureVersion == 3);
    REQUIRE(kCurveName == "secp256r1");
    REQUIRE(kDigestDataRef == "digest_data_ref_signer_info");
    REQUIRE(kCanonicalMethod == "thrift_binary");
    REQUIRE(kHashName == "sha256");
    REQUIRE(kSignatureMethodName == "ecdsa");
    REQUIRE(kDigestMethodName == "sha256");
    REQUIRE(kSpecSignerTitle == "Code/Content Signer");
    REQUIRE(kSpecSignerCommonName == "ILok");
    REQUIRE(kSpecSignerSerialHex == "1798A2B5D62000000005");
}
