#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "crypto_test_support.hpp"
#include "iclforge/base/crypto/credentials.hpp"
#include "iclforge/base/crypto/ecdsa_p256.hpp"
#include "iclforge/base/crypto/pem.hpp"
#include "iclforge/base/crypto/sha256.hpp"
#include "platform/process.hpp"
#include "test_pki.hpp"

namespace {

namespace crypto = iclforge::base::crypto;
namespace pki = iclforge::test::pki;
using iclforge::test::crypto::from_hex;
using iclforge::test::crypto::text_bytes;
using iclforge::test::crypto::to_hex;

// The private scalar of RFC 6979 A.2.5, written by OpenSSL (Python `cryptography` 50) as PKCS#8, as
// SEC 1, and as an encrypted PKCS#8 with the password "pw".
constexpr const char* kPkcs8Pem =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgya+p2EW6dRZrXCFX\n"
    "Z7HWk05Qw9s26JsSe4piKxIPZyGhRANCAARg/tS6JVqdMclh63TGNW1owEm4kjth\n"
    "+mzmaWIuYPKftnkD/hAIuLyZpBrp6VYovGTy8bIMLX6fUXejwpTURiKZ\n"
    "-----END PRIVATE KEY-----\n";
constexpr const char* kSec1Pem =
    "-----BEGIN EC PRIVATE KEY-----\n"
    "MHcCAQEEIMmvqdhFunUWa1whV2ex1pNOUMPbNuibEnuKYisSD2choAoGCCqGSM49\n"
    "AwEHoUQDQgAEYP7UuiVanTHJYet0xjVtaMBJuJI7Yfps5mliLmDyn7Z5A/4QCLi8\n"
    "maQa6elWKLxk8vGyDC1+n1F3o8KU1EYimQ==\n"
    "-----END EC PRIVATE KEY-----\n";
constexpr const char* kEncryptedPem =
    "-----BEGIN ENCRYPTED PRIVATE KEY-----\n"
    "MIH0MF8GCSqGSIb3DQEFDTBSMDEGCSqGSIb3DQEFDDAkBBAjWR/muw6iEq1XEjf3\n"
    "eylsAgIIADAMBggqhkiG9w0CCQUAMB0GCWCGSAFlAwQBKgQQrqJnFIjxBUaFQEGD\n"
    "T1byZQSBkKiBI05XRXvFLtyE1TrBQgOGkGj75kXg2Mwwt6e/MsgQAOtAKwy1qasv\n"
    "0sLIAAxoYra5iUGdoO7rpbMghbzbX4bOS2C1YiFXKQUhILDnp7WNLkSr2Tu/+eA1\n"
    "TopicUxO5xk+lU27rAfQDfDhOAKgcbSa/CKMN7+2jXfTR7d+M8pkTi4fW8PNrxxv\n"
    "DcbUr5ExkA==\n"
    "-----END ENCRYPTED PRIVATE KEY-----\n";
constexpr const char* kScalarHex =
    "c9afa9d845ba75166b5c215767b1d6934e50c3db36e89b127b8a622b120f6721";

std::string scalar_hex_of(const crypto::P256PrivateKey& key) {
    return to_hex(key.scalar());
}

}  // namespace

TEST_CASE("private keys load from every form OpenSSL writes", "[crypto][credentials]") {
    const auto pkcs8 = crypto::parse_p256_private_key(text_bytes(kPkcs8Pem));
    REQUIRE(pkcs8.has_value());
    CHECK(scalar_hex_of(*pkcs8) == kScalarHex);

    const auto sec1 = crypto::parse_p256_private_key(text_bytes(kSec1Pem));
    REQUIRE(sec1.has_value());
    CHECK(scalar_hex_of(*sec1) == kScalarHex);

    // DER of each, extracted from the PEM.
    const auto der8 = crypto::pem_decode(kPkcs8Pem)->front().der;
    const auto der1 = crypto::pem_decode(kSec1Pem)->front().der;
    CHECK(scalar_hex_of(*crypto::parse_p256_private_key(der8)) == kScalarHex);
    CHECK(scalar_hex_of(*crypto::parse_p256_private_key(der1)) == kScalarHex);

    // A bare 32-byte scalar, and base64 of each form (what a CI secret holds).
    CHECK(scalar_hex_of(*crypto::parse_p256_private_key(from_hex(kScalarHex))) == kScalarHex);
    const std::string b64_8 = crypto::base64_encode(der8);
    const std::string b64_1 = crypto::base64_encode(der1);
    CHECK(scalar_hex_of(*crypto::parse_p256_private_key(text_bytes(b64_8))) == kScalarHex);
    CHECK(scalar_hex_of(*crypto::parse_p256_private_key(text_bytes(b64_1 + "\n"))) == kScalarHex);
    CHECK(scalar_hex_of(*crypto::parse_p256_private_key(
              text_bytes(crypto::base64_encode(from_hex(kScalarHex))))) == kScalarHex);
}

TEST_CASE("private keys that cannot be used are refused with a reason", "[crypto][credentials]") {
    const auto encrypted = crypto::parse_p256_private_key(text_bytes(kEncryptedPem));
    REQUIRE_FALSE(encrypted.has_value());
    CHECK(encrypted.error().kind == crypto::CredentialErrorKind::kUnsupported);
    CHECK(encrypted.error().message.find("openssl") != std::string::npos);

    CHECK(crypto::parse_p256_private_key(text_bytes("")).error().kind ==
          crypto::CredentialErrorKind::kEmpty);
    CHECK(crypto::parse_p256_private_key(text_bytes("not a key at all")).error().kind ==
          crypto::CredentialErrorKind::kMalformed);
    CHECK(crypto::parse_p256_private_key(
              text_bytes("-----BEGIN RSA PRIVATE KEY-----\nAAAA\n-----END RSA PRIVATE KEY-----\n"))
              .error()
              .kind == crypto::CredentialErrorKind::kUnsupported);
    // A scalar of zero, and one at the group order.
    CHECK_FALSE(crypto::parse_p256_private_key(from_hex(std::string(64, '0'))).has_value());
    CHECK_FALSE(crypto::parse_p256_private_key(
                    from_hex("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551"))
                    .has_value());

    // A truncated PKCS#8 body is malformed, not a crash.
    auto der = crypto::pem_decode(kPkcs8Pem)->front().der;
    for (std::size_t cut = 1; cut < der.size(); cut += 5) {
        CHECK_FALSE(
            crypto::parse_p256_private_key(std::span<const std::byte>(der).first(cut)).has_value());
    }
    // A SEC 1 key whose stated public key is not its scalar's.
    auto sec1 = crypto::pem_decode(kSec1Pem)->front().der;
    sec1[sec1.size() - 3] ^= std::byte{0x01};
    const auto mismatched = crypto::parse_p256_private_key(sec1);
    REQUIRE_FALSE(mismatched.has_value());
    CHECK(mismatched.error().kind == crypto::CredentialErrorKind::kMismatch);
}

TEST_CASE("certificates load from PEM bundles and DER, in any order", "[crypto][credentials]") {
    const pki::TestChain chain = pki::make_chain();
    std::string bundle = "Certificate chain, leaf first, with notes between the blocks\n";
    for (int i = 3; i >= 0; --i) {
        bundle +=
            crypto::pem_encode("CERTIFICATE", chain.certificates[static_cast<std::size_t>(i)]);
        bundle += "subject=" + chain.names[static_cast<std::size_t>(i)].common_name + "\n";
    }
    const auto parsed = crypto::parse_certificates(text_bytes(bundle));
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->size() == 4);
    CHECK(parsed->front().subject.common_name ==
          std::optional<std::string>("ICL Forge Test Content Signer"));

    const auto ordered = crypto::order_chain(*parsed);
    REQUIRE(ordered.has_value());
    REQUIRE(ordered->size() == 4);
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK((*ordered)[i].der == chain.certificates[i]);
    }

    // DER, one and several back to back.
    CHECK(crypto::parse_certificates(chain.certificates[3])->size() == 1);
    pki::Bytes joined;
    for (const auto& der : chain.certificates) {
        joined.insert(joined.end(), der.begin(), der.end());
    }
    CHECK(crypto::parse_certificates(joined)->size() == 4);

    // Each way a set of certificates fails to be one chain.
    auto missing_middle = *parsed;
    missing_middle.erase(missing_middle.begin() + 1);  // drop the issuing CA
    // leaf, master, root remain: the leaf's issuer is absent, so the walk stops at the leaf and the
    // other two are left over
    CHECK(crypto::order_chain(missing_middle).error().kind ==
          crypto::CredentialErrorKind::kMismatch);
    auto duplicate = *parsed;
    duplicate.push_back(duplicate.front());
    CHECK(crypto::order_chain(duplicate).error().kind == crypto::CredentialErrorKind::kMismatch);
    CHECK(crypto::order_chain({}).error().kind == crypto::CredentialErrorKind::kEmpty);

    CHECK_FALSE(crypto::parse_certificates(
                    text_bytes("-----BEGIN CERTIFICATE-----\n!!!!\n-----END CERTIFICATE-----\n"))
                    .has_value());
    CHECK(crypto::parse_certificates(text_bytes("")).error().kind ==
          crypto::CredentialErrorKind::kEmpty);
    CHECK_FALSE(crypto::parse_certificates(text_bytes("plain text")).has_value());
}

TEST_CASE("a bare 32-byte scalar loads whatever bytes it begins and ends with",
          "[crypto][credentials]") {
    // The bytes that count as white space where a key file is trimmed. A scalar is binary: 62 of
    // 2000 random ones used to be refused because they began or ended with one of these.
    const std::array<unsigned, 4> white = {0x09, 0x0A, 0x0D, 0x20};
    std::vector<std::byte> scalar(32, std::byte{0x11});
    for (const unsigned first : white) {
        for (const unsigned last : white) {
            scalar.front() = static_cast<std::byte>(first);
            scalar.back() = static_cast<std::byte>(last);
            INFO("first " << first << " last " << last);
            const auto key = crypto::parse_p256_private_key(scalar);
            REQUIRE(key.has_value());
            CHECK(std::equal(scalar.begin(), scalar.end(), key->scalar().begin()));
        }
    }
    // Both ends at once with a plain middle, and a spread of scalars with one end in the set.
    for (unsigned n = 0; n < 400; ++n) {
        const pki::Bytes seed = {static_cast<std::byte>(n & 0xFF), static_cast<std::byte>(n >> 8)};
        const auto digest = crypto::sha256(seed);
        std::vector<std::byte> bytes(digest.begin(), digest.end());
        bytes[(n % 2 == 0) ? 0 : 31] = static_cast<std::byte>(white[(n / 2) % 4]);
        const auto key = crypto::parse_p256_private_key(bytes);
        REQUIRE(key.has_value());
        CHECK(std::equal(bytes.begin(), bytes.end(), key->scalar().begin()));
    }
    // Padding around a scalar is still trimmed, as a file with a trailing newline needs.
    std::vector<std::byte> padded(scalar.begin(), scalar.end());
    padded.front() = std::byte{0x42};
    padded.back() = std::byte{0x43};
    padded.insert(padded.begin(), std::byte{'\n'});
    padded.push_back(std::byte{'\n'});
    const auto from_padded = crypto::parse_p256_private_key(padded);
    REQUIRE(from_padded.has_value());
    CHECK(from_padded->scalar()[0] == std::byte{0x42});
}

TEST_CASE("a chain or a certificate file past its cap is refused", "[crypto][credentials]") {
    const pki::TestChain chain = pki::make_chain();
    std::vector<crypto::Certificate> nine;
    for (int i = 0; i < 9; ++i) {
        nine.push_back(*crypto::parse_certificate(chain.certificates[3]));
    }
    const auto too_long = crypto::order_chain(nine);
    REQUIRE_FALSE(too_long.has_value());
    CHECK(too_long.error().message.find("longer than the 8 accepted") != std::string::npos);

    const std::string pem = crypto::pem_encode("CERTIFICATE", chain.certificates[0]);
    std::string bundle;
    for (int i = 0; i < 1024; ++i) {
        bundle += pem;
    }
    CHECK(crypto::parse_certificates(text_bytes(bundle)).has_value());
    bundle += pem;
    const auto too_many = crypto::parse_certificates(text_bytes(bundle));
    REQUIRE_FALSE(too_many.has_value());
    CHECK(too_many.error().message.find("at most 1024") != std::string::npos);
}

TEST_CASE("key and certificate files load whole, at any size", "[crypto][credentials]") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path{ICLFORGE_TEST_SCRATCH_DIR} /
                         ("credentials_files_" + iclforge::test::platform::process_id());
    fs::create_directories(dir);
    const auto write = [&dir](const std::string& name, std::span<const std::byte> bytes) {
        const fs::path path = dir / name;
        std::ofstream out{path, std::ios::binary | std::ios::trunc};
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        return path.string();
    };

    // A bare scalar in a file, 32 bytes exactly, ending in a byte that counts as white space.
    std::vector<std::byte> scalar(32, std::byte{0x23});
    scalar.front() = std::byte{0x20};
    scalar.back() = std::byte{0x0A};
    const auto key = crypto::load_content_key(write("scalar.key", scalar));
    REQUIRE(key.has_value());
    CHECK(std::equal(scalar.begin(), scalar.end(), key->scalar().begin()));

    // A bundle of 20 certificates is several times the 4096 bytes a read takes at once.
    const pki::TestChain chain = pki::make_chain();
    std::string bundle;
    for (std::size_t i = 0; i < 20; ++i) {
        bundle += crypto::pem_encode("CERTIFICATE", chain.certificates[i % 4]);
    }
    REQUIRE(bundle.size() > 3 * 4096);
    const std::vector<std::string> paths = {write("bundle.pem", text_bytes(bundle))};
    const auto certificates = crypto::load_certificate_files(paths);
    REQUIRE(certificates.has_value());
    CHECK(certificates->size() == 20);

    // An empty file is an empty source, and a missing one is unreadable; neither is a key.
    const auto empty = crypto::load_content_key(write("empty.key", {}));
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error().kind == crypto::CredentialErrorKind::kEmpty);
    const auto missing = crypto::load_content_key((dir / "absent.key").string());
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().kind == crypto::CredentialErrorKind::kUnreadable);
}

TEST_CASE("credentials need the key to belong to the leaf", "[crypto][credentials]") {
    const pki::TestChain chain = pki::make_chain();
    std::vector<crypto::Certificate> certs;
    for (const auto& der : chain.certificates) {
        certs.push_back(*crypto::parse_certificate(der));
    }
    const auto good = crypto::make_credentials(chain.leaf_key, certs);
    REQUIRE(good.has_value());
    CHECK(good->chain.size() == 4);
    CHECK(good->chain.back().der == chain.certificates[3]);

    const auto wrong = crypto::make_credentials(pki::key_for("another key"), certs);
    REQUIRE_FALSE(wrong.has_value());
    CHECK(wrong.error().kind == crypto::CredentialErrorKind::kMismatch);
    CHECK(wrong.error().message.find("leaf") != std::string::npos);
    // The message names the certificate and never the key.
    CHECK(wrong.error().message.find(scalar_hex_of(pki::key_for("another key"))) ==
          std::string::npos);
}
