// The C entry points for AC-4 content signing with the caller's own key
// (iclforge_ac4_sign_stream / iclforge_ac4_verify_stream): a signed stream verifies as VALID against
// the signer's trust anchor or public key, and each way of breaking it has its own outcome. The key
// and certificates are the test chain's (tests/support/test_pki.hpp: public keys that protect
// nothing). The signing and the checks themselves are tested in libs/base and libs/ac4; this is
// what the C layer does with its arguments.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "iclforge/base/crypto/ecdsa_p256.hpp"
#include "iclforge/base/crypto/pem.hpp"
#include "iclforge_c/iclforge.h"
#include "test_pki.hpp"

namespace {

namespace crypto = iclforge::base::crypto;
namespace pki = iclforge::test::pki;

constexpr const char* kSignedDate = "2024-06-01T12:00:00";

// A raw .ac4 stream of sync frames (0xAC40, 16-bit size, payload); the payload is opaque to the
// signature.
std::vector<uint8_t> synthetic_stream() {
    std::vector<uint8_t> out;
    for (int i = 0; i < 5; ++i) {
        const std::size_t payload = 300 + static_cast<std::size_t>(i) * 13;
        out.push_back(0xAC);
        out.push_back(0x40);
        out.push_back(static_cast<uint8_t>(payload >> 8));
        out.push_back(static_cast<uint8_t>(payload & 0xFF));
        for (std::size_t n = 0; n < payload; ++n) {
            out.push_back(static_cast<uint8_t>(static_cast<std::size_t>(i) * 7 + n));
        }
    }
    return out;
}

std::vector<uint8_t> to_u8(const std::string& text) {
    return {text.begin(), text.end()};
}

std::vector<uint8_t> scalar_bytes(const crypto::P256PrivateKey& key) {
    const auto scalar = key.scalar();
    std::vector<uint8_t> out;
    for (const std::byte b : scalar) {
        out.push_back(static_cast<uint8_t>(b));
    }
    return out;
}

// `openssl pkey -pubout`'s PEM for a key: SubjectPublicKeyInfo (id-ecPublicKey, prime256v1, the
// uncompressed point).
std::vector<uint8_t> public_key_pem(const crypto::P256PrivateKey& key) {
    const auto point = crypto::p256_public_key_uncompressed(*crypto::p256_public_key(key));
    constexpr std::array<uint8_t, 26> kPrefix = {0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2A, 0x86, 0x48,
                                                 0xCE, 0x3D, 0x02, 0x01, 0x06, 0x08, 0x2A, 0x86, 0x48,
                                                 0xCE, 0x3D, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00};
    std::vector<std::byte> der;
    for (const uint8_t b : kPrefix) {
        der.push_back(static_cast<std::byte>(b));
    }
    der.insert(der.end(), point.begin(), point.end());
    return to_u8(crypto::pem_encode("PUBLIC KEY", der));
}

struct Credentials {
    pki::TestChain chain = pki::make_chain();
    std::vector<uint8_t> key = scalar_bytes(chain.leaf_key);
    std::vector<uint8_t> chain_pem;
    std::vector<uint8_t> root_pem;
    std::vector<uint8_t> signer_public_key = public_key_pem(chain.leaf_key);

    Credentials() {
        std::string bundle;
        for (const auto& der : chain.certificates) {
            bundle += crypto::pem_encode("CERTIFICATE", der);
        }
        chain_pem = to_u8(bundle);
        root_pem = to_u8(crypto::pem_encode("CERTIFICATE", chain.certificates[0]));
    }

    iclforge_ac4_sign_config_t sign_config() const {
        iclforge_ac4_sign_config_t config{};
        config.private_key = key.data();
        config.private_key_size = key.size();
        config.chain = chain_pem.data();
        config.chain_size = chain_pem.size();
        config.signed_date = kSignedDate;
        return config;
    }
};

std::vector<uint8_t> take(iclforge_bytes_t* bytes) {
    std::vector<uint8_t> out;
    if (bytes != nullptr) {
        out.assign(iclforge_bytes_data(bytes), iclforge_bytes_data(bytes) + iclforge_bytes_size(bytes));
        iclforge_bytes_destroy(bytes);
    }
    return out;
}

std::string text(iclforge_bytes_t* bytes) {
    const std::vector<uint8_t> raw = take(bytes);
    return {raw.begin(), raw.end()};
}

iclforge_ac4_signature_outcome_t verify(const std::vector<uint8_t>& stream,
                                        const iclforge_ac4_verify_config_t& config,
                                        std::string* detail = nullptr) {
    iclforge_ac4_signature_outcome_t outcome = ICLFORGE_AC4_SIGNATURE_VALID;
    iclforge_bytes_t* out_detail = nullptr;
    REQUIRE(iclforge_ac4_verify_stream(stream.data(), stream.size(), &config, &outcome, &out_detail) ==
            ICLFORGE_OK);
    const std::string said = text(out_detail);
    if (detail != nullptr) {
        *detail = said;
    }
    return outcome;
}

iclforge_ac4_verify_config_t verify_config() {
    iclforge_ac4_verify_config_t config{};
    // Every certificate in the test chain was valid on this day (the second expires 2025-11-13).
    config.now_unix = pki::epoch("2024-07-01T00:00:00");
    return config;
}

}  // namespace

TEST_CASE("AC-4 content signing round-trips through the C API with the caller's own key",
          "[capi][ac4][signing]") {
    const Credentials credentials;
    const std::vector<uint8_t> stream = synthetic_stream();
    const iclforge_ac4_sign_config_t sign = credentials.sign_config();

    iclforge_bytes_t* signed_bytes = nullptr;
    REQUIRE(iclforge_ac4_sign_stream(stream.data(), stream.size(), &sign, &signed_bytes, nullptr) ==
            ICLFORGE_OK);
    const std::vector<uint8_t> signed_stream = take(signed_bytes);
    REQUIRE(signed_stream.size() > stream.size());
    // The sync frames are untouched: the signed stream begins with the unsigned one.
    CHECK(std::equal(stream.begin(), stream.end(), signed_stream.begin()));

    iclforge_ac4_verify_config_t anchored = verify_config();
    anchored.trust_anchors = credentials.root_pem.data();
    anchored.trust_anchors_size = credentials.root_pem.size();

    SECTION("valid against the signer's trust anchor, or its public key") {
        CHECK(verify(signed_stream, anchored) == ICLFORGE_AC4_SIGNATURE_VALID);

        iclforge_ac4_verify_config_t by_key = verify_config();
        by_key.trust_key = credentials.signer_public_key.data();
        by_key.trust_key_size = credentials.signer_public_key.size();
        CHECK(verify(signed_stream, by_key) == ICLFORGE_AC4_SIGNATURE_VALID);
    }
    SECTION("untrusted with nothing to trust, or with another signer's key") {
        CHECK(verify(signed_stream, verify_config()) == ICLFORGE_AC4_SIGNATURE_UNTRUSTED);

        const std::vector<uint8_t> stranger = public_key_pem(pki::key_for("icl-forge-test-stranger"));
        iclforge_ac4_verify_config_t by_key = verify_config();
        by_key.trust_key = stranger.data();
        by_key.trust_key_size = stranger.size();
        CHECK(verify(signed_stream, by_key) == ICLFORGE_AC4_SIGNATURE_UNTRUSTED);
    }
    SECTION("a NULL config trusts nothing, so a good signature is never valid") {
        iclforge_ac4_signature_outcome_t outcome = ICLFORGE_AC4_SIGNATURE_VALID;
        REQUIRE(iclforge_ac4_verify_stream(signed_stream.data(), signed_stream.size(), nullptr, &outcome,
                                           nullptr) == ICLFORGE_OK);
        CHECK(outcome != ICLFORGE_AC4_SIGNATURE_VALID);
    }
    SECTION("each way of breaking it has its own outcome") {
        std::vector<uint8_t> audio = signed_stream;
        audio[400] ^= 0x04;
        std::string detail;
        CHECK(verify(audio, anchored, &detail) == ICLFORGE_AC4_SIGNATURE_CONTENT_ALTERED);
        CHECK_FALSE(detail.empty());

        CHECK(verify(stream, anchored) == ICLFORGE_AC4_SIGNATURE_NONE);

        const std::string forge = "ICL Forge";
        std::vector<uint8_t> metadata = signed_stream;
        const auto at = std::search(metadata.begin() + static_cast<std::ptrdiff_t>(stream.size()),
                                    metadata.end(), forge.begin(), forge.end());
        REQUIRE(at != metadata.end());
        *at = 'X';
        CHECK(verify(metadata, anchored) == ICLFORGE_AC4_SIGNATURE_METADATA_ALTERED);

        iclforge_ac4_verify_config_t late = anchored;
        late.now_unix = pki::epoch("2030-01-01T00:00:00");
        CHECK(verify(signed_stream, late) == ICLFORGE_AC4_SIGNATURE_CHAIN_REJECTED);
    }
    SECTION("a trust key that is not a public key is an argument error") {
        iclforge_ac4_verify_config_t config = verify_config();
        config.trust_key = credentials.root_pem.data();  // a certificate is not a public key
        config.trust_key_size = credentials.root_pem.size();
        iclforge_ac4_signature_outcome_t outcome = ICLFORGE_AC4_SIGNATURE_VALID;
        iclforge_bytes_t* detail = nullptr;
        CHECK(iclforge_ac4_verify_stream(signed_stream.data(), signed_stream.size(), &config, &outcome,
                                         &detail) == ICLFORGE_ERROR_INVALID_ARGUMENT);
        CHECK_FALSE(text(detail).empty());
    }
}

TEST_CASE("signing through the C API refuses what it cannot sign", "[capi][ac4][signing]") {
    const Credentials credentials;
    const std::vector<uint8_t> stream = synthetic_stream();
    iclforge_ac4_sign_config_t sign = credentials.sign_config();
    iclforge_bytes_t* out = nullptr;
    iclforge_bytes_t* detail = nullptr;

    SECTION("NULL arguments") {
        CHECK(iclforge_ac4_sign_stream(nullptr, 0, &sign, &out, nullptr) ==
              ICLFORGE_ERROR_INVALID_ARGUMENT);
        CHECK(iclforge_ac4_sign_stream(stream.data(), stream.size(), nullptr, &out, nullptr) ==
              ICLFORGE_ERROR_INVALID_ARGUMENT);
        CHECK(iclforge_ac4_sign_stream(stream.data(), stream.size(), &sign, nullptr, nullptr) ==
              ICLFORGE_ERROR_INVALID_ARGUMENT);
        sign.private_key = nullptr;
        CHECK(iclforge_ac4_sign_stream(stream.data(), stream.size(), &sign, &out, nullptr) ==
              ICLFORGE_ERROR_INVALID_ARGUMENT);
        CHECK(out == nullptr);
    }
    SECTION("a key that is not a key, and a bad date") {
        const std::vector<uint8_t> junk = {1, 2, 3};
        sign.private_key = junk.data();
        sign.private_key_size = junk.size();
        CHECK(iclforge_ac4_sign_stream(stream.data(), stream.size(), &sign, &out, &detail) ==
              ICLFORGE_ERROR_INVALID_ARGUMENT);
        CHECK_FALSE(text(detail).empty());
        sign = credentials.sign_config();
        sign.signed_date = "yesterday";
        CHECK(iclforge_ac4_sign_stream(stream.data(), stream.size(), &sign, &out, nullptr) ==
              ICLFORGE_ERROR_INVALID_ARGUMENT);
    }
    SECTION("a key that is not the chain's") {
        const std::vector<uint8_t> scalar = scalar_bytes(pki::key_for("icl-forge-test-stranger"));
        sign.private_key = scalar.data();
        sign.private_key_size = scalar.size();
        CHECK(iclforge_ac4_sign_stream(stream.data(), stream.size(), &sign, &out, &detail) ==
              ICLFORGE_ERROR_AC4_SIGNING);
        CHECK(out == nullptr);
        CHECK_FALSE(text(detail).empty());
    }
    SECTION("a stream that is signed already, and one that is not AC-4") {
        iclforge_bytes_t* signed_bytes = nullptr;
        REQUIRE(iclforge_ac4_sign_stream(stream.data(), stream.size(), &sign, &signed_bytes, nullptr) ==
                ICLFORGE_OK);
        const std::vector<uint8_t> twice = take(signed_bytes);
        CHECK(iclforge_ac4_sign_stream(twice.data(), twice.size(), &sign, &out, &detail) ==
              ICLFORGE_ERROR_AC4_SIGNING);
        CHECK(text(detail).find("already") != std::string::npos);
        const std::vector<uint8_t> wav = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
        CHECK(iclforge_ac4_sign_stream(wav.data(), wav.size(), &sign, &out, nullptr) ==
              ICLFORGE_ERROR_AC4_SIGNING);
    }
}

TEST_CASE("the outcome names are the tokens forge prints", "[capi][ac4][signing]") {
    CHECK(std::string(iclforge_ac4_signature_outcome_name(ICLFORGE_AC4_SIGNATURE_VALID)) == "valid");
    CHECK(std::string(iclforge_ac4_signature_outcome_name(ICLFORGE_AC4_SIGNATURE_UNTRUSTED)) ==
          "untrusted");
    CHECK(std::string(iclforge_ac4_signature_outcome_name(ICLFORGE_AC4_SIGNATURE_NONE)) ==
          "no-signature");
    CHECK(std::string(iclforge_ac4_signature_outcome_name(ICLFORGE_AC4_SIGNATURE_CONTENT_ALTERED)) ==
          "content-altered");
    CHECK(std::string(iclforge_ac4_signature_outcome_name(ICLFORGE_AC4_SIGNATURE_METADATA_ALTERED)) ==
          "metadata-altered");
    CHECK(std::string(iclforge_ac4_signature_outcome_name(ICLFORGE_AC4_SIGNATURE_INVALID)) ==
          "signature-invalid");
    CHECK(std::string(iclforge_status_message(ICLFORGE_ERROR_AC4_SIGNING)) !=
          std::string(iclforge_status_message(ICLFORGE_ERROR_INTERNAL)));
}
