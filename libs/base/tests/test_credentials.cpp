// Loading the operator's key, chain and trust anchors (credentials.hpp): PEM and DER, the formats
// OpenSSL writes, the refusals, the order of the chain, and the path / environment order.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/base/crypto/credentials.hpp"
#include "iclforge/base/crypto/p256.hpp"
#include "iclforge/base/crypto/x509.hpp"
#include "platform/process.hpp"
#include "test_pki.hpp"

namespace {

using namespace iclforge::base::crypto;
namespace pki = iclforge::test::pki;
namespace fs = std::filesystem;
using pki::Bytes;

std::span<const std::byte> as_bytes(const std::string& text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

CredentialErrorKind kind_of(const std::expected<P256PrivateKey, CredentialError>& r) {
    REQUIRE_FALSE(r.has_value());
    return r.error().kind;
}

fs::path scratch(const std::string& leaf) {
    const fs::path dir = fs::path{ICLFORGE_TEST_SCRATCH_DIR} /
                         ("credentials_" + iclforge::test::platform::process_id());
    fs::create_directories(dir);
    return dir / leaf;
}

std::string write_file(const std::string& leaf, const std::string& content) {
    const fs::path path = scratch(leaf);
    std::ofstream out{path, std::ios::binary};
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return path.string();
}

std::string pem_certificate(const Bytes& der) {
    return encode_pem("CERTIFICATE", der);
}

// Clears the variables the loaders read, so a test starts from the same environment whatever the
// shell it was run from had, and again when it ends.
struct CleanEnvironment {
    static void clear() {
        for (const auto name : {kEnvContentSigningKeyFile, kEnvContentSigningKey,
                                kEnvContentSigningChainFile, kEnvContentTrustFile}) {
            iclforge::test::platform::unset_environment(name);
        }
    }
    CleanEnvironment() { clear(); }
    ~CleanEnvironment() { clear(); }
};

}  // namespace

TEST_CASE("PEM blocks are decoded and encoded", "[crypto][credentials][pem]") {
    SECTION("round trip at every length around the base64 groups") {
        for (std::size_t length = 0; length < 200; ++length) {
            Bytes data;
            for (std::size_t i = 0; i < length; ++i) {
                data.push_back(static_cast<std::byte>((i * 37 + length) & 0xFF));
            }
            const std::string pem = encode_pem("TEST DATA", data);
            const auto blocks = decode_pem(pem);
            REQUIRE(blocks.has_value());
            REQUIRE(blocks->size() == 1);
            CHECK((*blocks)[0].label == "TEST DATA");
            CHECK((*blocks)[0].data == data);
            CHECK_FALSE((*blocks)[0].has_headers);
        }
    }
    SECTION("lines are 64 characters, with LF endings") {
        const Bytes data(100, std::byte{0xAB});
        const std::string pem = encode_pem("X", data);
        CHECK(pem.starts_with("-----BEGIN X-----\n"));
        CHECK(pem.ends_with("-----END X-----\n"));
        CHECK(pem.find('\r') == std::string::npos);
        std::size_t position = pem.find('\n') + 1;
        const std::size_t second = pem.find('\n', position);
        CHECK(second - position == 64);
    }
    SECTION("CRLF endings, indentation, text around the blocks and several blocks") {
        const Bytes one{std::byte{1}, std::byte{2}, std::byte{3}};
        const Bytes two{std::byte{4}};
        std::string text = "Bag Attributes\n    friendlyName: x\nsubject=/CN=test\n";
        text += encode_pem("CERTIFICATE", one);
        text += "some commentary between blocks\n";
        std::string second = encode_pem("PUBLIC KEY", two);
        for (std::size_t i = 0; (i = second.find('\n', i)) != std::string::npos; i += 2) {
            second.insert(i, "\r");
        }
        text += "  " + second;
        text += "trailing text";
        const auto blocks = decode_pem(text);
        REQUIRE(blocks.has_value());
        REQUIRE(blocks->size() == 2);
        CHECK((*blocks)[0].label == "CERTIFICATE");
        CHECK((*blocks)[0].data == one);
        CHECK((*blocks)[1].label == "PUBLIC KEY");
        CHECK((*blocks)[1].data == two);
    }
    SECTION("no block at all is an empty list, not an error") {
        const auto blocks = decode_pem("nothing to see\n");
        REQUIRE(blocks.has_value());
        CHECK(blocks->empty());
    }
    SECTION("a block that is not well-formed makes the whole text unreadable") {
        const std::string good = encode_pem("CERTIFICATE", Bytes(10, std::byte{7}));
        CHECK_FALSE(decode_pem(good.substr(0, good.find("-----END"))).has_value());  // no END
        std::string mismatch = good;
        mismatch.replace(mismatch.find("-----END CERTIFICATE"), 20, "-----END PUBLIC KEY-");
        CHECK_FALSE(decode_pem(mismatch).has_value());
        std::string bad_char = good;
        bad_char[bad_char.find('\n') + 2] = '!';
        CHECK_FALSE(decode_pem(bad_char).has_value());
        std::string short_body = "-----BEGIN X-----\nQUJD\nQUI\n-----END X-----\n";
        CHECK_FALSE(decode_pem(short_body).has_value());  // length not a multiple of four
        std::string padding_inside = "-----BEGIN X-----\nQQ==QUJD\n-----END X-----\n";
        CHECK_FALSE(decode_pem(padding_inside).has_value());
    }
    SECTION("an empty block decodes to no bytes") {
        const auto blocks = decode_pem("-----BEGIN X-----\n-----END X-----\n");
        REQUIRE(blocks.has_value());
        REQUIRE(blocks->size() == 1);
        CHECK((*blocks)[0].data.empty());
    }
    SECTION("RFC 1421 headers are noted, which is how an encrypted legacy key shows") {
        const std::string text =
            "-----BEGIN EC PRIVATE KEY-----\nProc-Type: 4,ENCRYPTED\nDEK-Info: "
            "AES-128-CBC,00\n\nQUJD\n"
            "-----END EC PRIVATE KEY-----\n";
        const auto blocks = decode_pem(text);
        REQUIRE(blocks.has_value());
        REQUIRE(blocks->size() == 1);
        CHECK((*blocks)[0].has_headers);
    }
}

TEST_CASE("private keys are read from PKCS#8 and SEC 1, in PEM and DER", "[crypto][credentials]") {
    const auto pair = pki::key_from_seed("loader");
    const std::string source = "test key";

    const auto matches = [&](const std::expected<P256PrivateKey, CredentialError>& r) {
        REQUIRE(r.has_value());
        CHECK(r->scalar()[31] == pair.key.scalar()[31]);
        CHECK(std::equal(r->scalar().begin(), r->scalar().end(), pair.key.scalar().begin()));
        CHECK(r->public_key() == pair.pub);
    };

    SECTION("PKCS#8 PEM") {
        matches(
            parse_private_key(as_bytes(encode_pem("PRIVATE KEY", pki::pkcs8_der(pair))), source));
    }
    SECTION("SEC 1 PEM, with and without the curve and the public key") {
        matches(parse_private_key(
            as_bytes(encode_pem("EC PRIVATE KEY", pki::sec1_der(pair, true, true))), source));
        matches(parse_private_key(
            as_bytes(encode_pem("EC PRIVATE KEY", pki::sec1_der(pair, false, false))), source));
        matches(parse_private_key(
            as_bytes(encode_pem("EC PRIVATE KEY", pki::sec1_der(pair, true, false))), source));
        matches(parse_private_key(
            as_bytes(encode_pem("EC PRIVATE KEY", pki::sec1_der(pair, false, true))), source));
    }
    SECTION("DER of either") {
        const Bytes pkcs8 = pki::pkcs8_der(pair);
        matches(parse_private_key(pkcs8, source));
        const Bytes sec1 = pki::sec1_der(pair);
        matches(parse_private_key(sec1, source));
    }
    SECTION("the EC PARAMETERS block that `openssl ecparam -genkey` writes first is skipped") {
        const std::string text = encode_pem("EC PARAMETERS", pki::oid("1.2.840.10045.3.1.7")) +
                                 encode_pem("EC PRIVATE KEY", pki::sec1_der(pair));
        matches(parse_private_key(as_bytes(text), source));
    }
    SECTION("a key and its certificate in one file") {
        const auto authority = pki::make_authority();
        const std::string text = encode_pem("PRIVATE KEY", pki::pkcs8_der(authority.leaf.keys)) +
                                 pem_certificate(authority.leaf.der);
        const auto key = parse_private_key(as_bytes(text), source);
        REQUIRE(key.has_value());
        CHECK(key->public_key() == authority.leaf.keys.pub);
    }
    SECTION("empty") {
        CHECK(kind_of(parse_private_key({}, source)) == CredentialErrorKind::kEmpty);
    }
    SECTION("text that is not a key") {
        const std::string text = "this is not a key";
        CHECK(kind_of(parse_private_key(as_bytes(text), source)) ==
              CredentialErrorKind::kMalformed);
        const Bytes noise(40, std::byte{0x5A});
        CHECK(kind_of(parse_private_key(noise, source)) == CredentialErrorKind::kMalformed);
    }
    SECTION("a PEM file with no private key in it") {
        const auto authority = pki::make_authority();
        CHECK(kind_of(parse_private_key(as_bytes(pem_certificate(authority.leaf.der)), source)) ==
              CredentialErrorKind::kMalformed);
    }
    SECTION("two private keys are ambiguous") {
        const std::string text = encode_pem("PRIVATE KEY", pki::pkcs8_der(pair)) +
                                 encode_pem("PRIVATE KEY", pki::pkcs8_der(pki::key_from_seed("b")));
        CHECK(kind_of(parse_private_key(as_bytes(text), source)) ==
              CredentialErrorKind::kMalformed);
    }
    SECTION("an encrypted key is refused with its own error") {
        const std::string encrypted_pkcs8 =
            encode_pem("ENCRYPTED PRIVATE KEY", Bytes(60, std::byte{1}));
        CHECK(kind_of(parse_private_key(as_bytes(encrypted_pkcs8), source)) ==
              CredentialErrorKind::kEncrypted);
        const std::string legacy =
            "-----BEGIN EC PRIVATE KEY-----\nProc-Type: 4,ENCRYPTED\nDEK-Info: "
            "AES-128-CBC,0011\n\nQUJD\n"
            "-----END EC PRIVATE KEY-----\n";
        CHECK(kind_of(parse_private_key(as_bytes(legacy), source)) ==
              CredentialErrorKind::kEncrypted);
        // DER of EncryptedPrivateKeyInfo: SEQUENCE { SEQUENCE {...}, OCTET STRING }
        const Bytes der = pki::sequence({pki::sequence({pki::oid("1.2.840.113549.1.5.13")}),
                                         pki::octet_string(Bytes(30, std::byte{9}))});
        CHECK(kind_of(parse_private_key(der, source)) == CredentialErrorKind::kEncrypted);
    }
    SECTION("an RSA key") {
        const std::string rsa = encode_pem("RSA PRIVATE KEY", Bytes(30, std::byte{2}));
        CHECK(kind_of(parse_private_key(as_bytes(rsa), source)) ==
              CredentialErrorKind::kUnsupported);
        const Bytes pkcs8_rsa =
            pki::sequence({pki::small_integer(0), pki::sequence({pki::oid("1.2.840.113549.1.1.1")}),
                           pki::octet_string(Bytes(20, std::byte{3}))});
        CHECK(kind_of(parse_private_key(pkcs8_rsa, source)) == CredentialErrorKind::kUnsupported);
    }
    SECTION("an EC key on another curve") {
        const Bytes other_curve = pki::sequence(
            {pki::small_integer(0),
             pki::sequence({pki::oid("1.2.840.10045.2.1"), pki::oid("1.3.132.0.34")}),  // secp384r1
             pki::octet_string(pki::sec1_der(pair, false, false))});
        CHECK(kind_of(parse_private_key(other_curve, source)) == CredentialErrorKind::kUnsupported);
        Bytes sec1 = pki::concat({pki::small_integer(1), pki::octet_string(pair.key.scalar()),
                                  pki::tlv(0xA0, pki::oid("1.3.132.0.34"))});
        CHECK(kind_of(parse_private_key(pki::tlv(0x30, sec1), source)) ==
              CredentialErrorKind::kUnsupported);
    }
    SECTION("a scalar that is the wrong size or out of range") {
        const auto build = [&](const Bytes& scalar) {
            return pki::tlv(0x30, pki::concat({pki::small_integer(1), pki::octet_string(scalar)}));
        };
        CHECK(kind_of(parse_private_key(build(Bytes(31, std::byte{1})), source)) ==
              CredentialErrorKind::kMalformed);
        CHECK(kind_of(parse_private_key(build(Bytes(33, std::byte{1})), source)) ==
              CredentialErrorKind::kMalformed);
        CHECK(kind_of(parse_private_key(build(Bytes(32, std::byte{0})), source)) ==
              CredentialErrorKind::kMalformed);
        CHECK(kind_of(parse_private_key(build(Bytes(32, std::byte{0xFF})), source)) ==
              CredentialErrorKind::kMalformed);
    }
    SECTION("a public key that is not the private key's") {
        const auto other = pki::key_from_seed("another");
        Bytes sec1 = pki::concat({pki::small_integer(1), pki::octet_string(pair.key.scalar()),
                                  pki::tlv(0xA1, pki::bit_string(other.pub.uncompressed()))});
        CHECK(kind_of(parse_private_key(pki::tlv(0x30, sec1), source)) ==
              CredentialErrorKind::kMismatch);
    }
    SECTION("truncation at every length is refused") {
        const Bytes der = pki::pkcs8_der(pair);
        for (std::size_t length = 0; length < der.size(); ++length) {
            CHECK_FALSE(parse_private_key(std::span{der}.first(length), source).has_value());
        }
    }
    SECTION("errors name the source") {
        const auto result = parse_private_key(Bytes(40, std::byte{0x5A}), "file 'my.key'");
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().message.find("my.key") != std::string::npos);
    }
}

TEST_CASE("certificates and public keys are read from PEM and DER", "[crypto][credentials]") {
    const auto authority = pki::make_authority();
    const std::string source = "test";

    SECTION("a bundle in PEM") {
        const std::string text = "# the bundle\n" + pem_certificate(authority.root.der) +
                                 pem_certificate(authority.intermediate.der) +
                                 pem_certificate(authority.leaf.der);
        const auto certs = parse_certificates(as_bytes(text), source);
        REQUIRE(certs.has_value());
        REQUIRE(certs->size() == 3);
        CHECK((*certs)[0].der == authority.root.der);
        CHECK((*certs)[2].der == authority.leaf.der);
    }
    SECTION("one certificate in DER") {
        const auto certs = parse_certificates(authority.leaf.der, source);
        REQUIRE(certs.has_value());
        CHECK(certs->size() == 1);
    }
    SECTION("refusals") {
        CHECK(parse_certificates({}, source).error().kind == CredentialErrorKind::kEmpty);
        const std::string none = "-----BEGIN PUBLIC KEY-----\nQUJD\n-----END PUBLIC KEY-----\n";
        CHECK(parse_certificates(as_bytes(none), source).error().kind ==
              CredentialErrorKind::kMalformed);
        Bytes trailing = authority.leaf.der;
        trailing.push_back(std::byte{0});
        CHECK(parse_certificates(trailing, source).error().kind == CredentialErrorKind::kMalformed);
        // the second certificate is the bad one, and the message says which
        const std::string text = pem_certificate(authority.root.der) +
                                 encode_pem("CERTIFICATE", Bytes(20, std::byte{0x30}));
        const auto result = parse_certificates(as_bytes(text), source);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().message.find("certificate 2") != std::string::npos);
    }
    SECTION("public keys from SPKI and from certificates") {
        const auto key = authority.leaf.keys.pub;
        const auto from_pem =
            parse_public_keys(as_bytes(encode_pem("PUBLIC KEY", pki::spki_der(key))), source);
        REQUIRE(from_pem.has_value());
        REQUIRE(from_pem->size() == 1);
        CHECK((*from_pem)[0] == key);

        const auto from_der = parse_public_keys(pki::spki_der(key), source);
        REQUIRE(from_der.has_value());
        CHECK((*from_der)[0] == key);

        const auto from_cert =
            parse_public_keys(as_bytes(pem_certificate(authority.leaf.der)), source);
        REQUIRE(from_cert.has_value());
        CHECK((*from_cert)[0] == key);

        const auto from_cert_der = parse_public_keys(authority.leaf.der, source);
        REQUIRE(from_cert_der.has_value());
        CHECK((*from_cert_der)[0] == key);

        const std::string two = encode_pem("PUBLIC KEY", pki::spki_der(key)) +
                                encode_pem("PUBLIC KEY", pki::spki_der(authority.root.keys.pub));
        const auto both = parse_public_keys(as_bytes(two), source);
        REQUIRE(both.has_value());
        CHECK(both->size() == 2);
    }
    SECTION("public key refusals") {
        CHECK(parse_public_keys({}, source).error().kind == CredentialErrorKind::kEmpty);
        CHECK(parse_public_keys(Bytes(10, std::byte{1}), source).error().kind ==
              CredentialErrorKind::kMalformed);
        Bytes other_curve = pki::spki_der(authority.leaf.keys.pub);
        const Bytes from = pki::oid("1.2.840.10045.3.1.7");
        const Bytes to = pki::oid("1.2.840.10045.3.1.1");
        const auto it =
            std::search(other_curve.begin(), other_curve.end(), from.begin(), from.end());
        REQUIRE(it != other_curve.end());
        std::copy(to.begin(), to.end(), it);
        CHECK(parse_public_keys(other_curve, source).error().kind ==
              CredentialErrorKind::kUnsupported);
        const std::string none = pem_certificate(Bytes{});
        CHECK_FALSE(parse_public_keys(as_bytes(none), source).has_value());
    }
}

TEST_CASE("a chain is put in order from any order", "[crypto][credentials][chain]") {
    const auto a = pki::make_authority();
    const std::vector<Certificate> expected = a.chain();
    const auto same_order = [&](const std::vector<Certificate>& got) {
        REQUIRE(got.size() == expected.size());
        for (std::size_t i = 0; i < got.size(); ++i) {
            CHECK(got[i].der == expected[i].der);
        }
    };

    std::array<int, 3> permutation = {0, 1, 2};
    do {
        std::vector<Certificate> shuffled;
        for (const int i : permutation) shuffled.push_back(expected[static_cast<std::size_t>(i)]);
        INFO("permutation " << permutation[0] << permutation[1] << permutation[2]);

        const auto with_key = order_chain(shuffled, &a.leaf.keys.pub, "chain");
        REQUIRE(with_key.has_value());
        same_order(*with_key);

        const auto without_key = order_chain(shuffled, nullptr, "chain");
        REQUIRE(without_key.has_value());
        same_order(*without_key);
    } while (std::next_permutation(permutation.begin(), permutation.end()));

    SECTION("repeated certificates count once") {
        std::vector<Certificate> repeated = {a.leaf.cert, a.root.cert, a.root.cert,
                                             a.intermediate.cert, a.leaf.cert};
        const auto result = order_chain(repeated, &a.leaf.keys.pub, "chain");
        REQUIRE(result.has_value());
        same_order(*result);
    }
    SECTION("certificates that are not on the path are dropped") {
        const auto stranger = pki::make_authority({.seed = "stranger"});
        std::vector<Certificate> mixed = {stranger.root.cert, a.leaf.cert,
                                          stranger.intermediate.cert, a.intermediate.cert,
                                          a.root.cert};
        const auto result = order_chain(mixed, &a.leaf.keys.pub, "chain");
        REQUIRE(result.has_value());
        same_order(*result);
    }
    SECTION("a chain that stops short of a root is returned as far as it goes") {
        const std::vector<Certificate> partial = {a.leaf.cert, a.intermediate.cert};
        const auto result = order_chain(partial, &a.leaf.keys.pub, "chain");
        REQUIRE(result.has_value());
        REQUIRE(result->size() == 2);
        CHECK((*result)[0].der == a.intermediate.der);
        CHECK((*result)[1].der == a.leaf.der);
    }
    SECTION("the leaf alone") {
        const std::vector<Certificate> alone = {a.leaf.cert};
        const auto result = order_chain(alone, &a.leaf.keys.pub, "chain");
        REQUIRE(result.has_value());
        CHECK(result->size() == 1);
    }
    SECTION("no certificate holds the key") {
        const auto other = pki::key_from_seed("not in the chain");
        const auto result = order_chain(expected, &other.pub, "chain");
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == CredentialErrorKind::kMismatch);
    }
    SECTION("without the key, two possible leaves are ambiguous") {
        const auto b = pki::make_authority({.seed = "second"});
        std::vector<Certificate> two = {a.leaf.cert, b.leaf.cert};
        const auto result = order_chain(two, nullptr, "chain");
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == CredentialErrorKind::kMismatch);
    }
    SECTION("no certificates") {
        CHECK(order_chain({}, nullptr, "chain").error().kind == CredentialErrorKind::kEmpty);
    }
    SECTION("a certificate with the right name and the wrong key is not preferred") {
        // Two intermediates with the leaf's issuer name; only one verifies the leaf's signature.
        const auto impostor = pki::make_authority({.seed = "impostor"});
        std::vector<Certificate> both = {a.leaf.cert, impostor.intermediate.cert,
                                         a.intermediate.cert, a.root.cert};
        const auto result = order_chain(both, &a.leaf.keys.pub, "chain");
        REQUIRE(result.has_value());
        same_order(*result);
    }
}

TEST_CASE("signing credentials load from paths", "[crypto][credentials][files]") {
    const CleanEnvironment environment;
    const auto a = pki::make_authority();
    const std::string key_path =
        write_file("leaf.key", encode_pem("PRIVATE KEY", pki::pkcs8_der(a.leaf.keys)));
    // leaf first, then the root, then the intermediate: not the wire order
    const std::string chain_path =
        write_file("chain.pem", pem_certificate(a.leaf.der) + pem_certificate(a.root.der) +
                                    pem_certificate(a.intermediate.der));

    SECTION("key and chain, with the chain ordered") {
        const auto credentials = load_signing_credentials(key_path, chain_path);
        REQUIRE(credentials.has_value());
        REQUIRE(credentials->chain.size() == 3);
        CHECK(credentials->chain.front().der == a.root.der);
        CHECK(credentials->chain.back().der == a.leaf.der);
        CHECK(credentials->key.public_key() == a.leaf.keys.pub);
    }
    SECTION("a chain for another key is refused when it is loaded") {
        const std::string other_key = write_file(
            "other.key", encode_pem("PRIVATE KEY", pki::pkcs8_der(pki::key_from_seed("other"))));
        const auto credentials = load_signing_credentials(other_key, chain_path);
        REQUIRE_FALSE(credentials.has_value());
        CHECK(credentials.error().kind == CredentialErrorKind::kMismatch);
    }
    SECTION("unreadable and absent") {
        const auto missing = load_signing_credentials((scratch("nope.key")).string(), chain_path);
        REQUIRE_FALSE(missing.has_value());
        CHECK(missing.error().kind == CredentialErrorKind::kUnreadable);
        CHECK(missing.error().message.find("nope.key") != std::string::npos);

        const auto no_key = load_signing_credentials("", chain_path);
        REQUIRE_FALSE(no_key.has_value());
        CHECK(no_key.error().kind == CredentialErrorKind::kAbsent);

        const auto no_chain = load_signing_credentials(key_path, "");
        REQUIRE_FALSE(no_chain.has_value());
        CHECK(no_chain.error().kind == CredentialErrorKind::kAbsent);
    }
    SECTION("an empty file") {
        const std::string empty = write_file("empty.key", "");
        const auto result = load_content_signing_key(empty);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == CredentialErrorKind::kEmpty);
    }
    SECTION("a file past the size cap is not read into memory") {
        const std::string big = write_file("big.pem", std::string((5u << 20), 'x'));
        const auto result = load_content_signing_chain(big);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == CredentialErrorKind::kMalformed);
    }
}

TEST_CASE("key and chain resolution order: path, then the environment",
          "[crypto][credentials][files]") {
    const CleanEnvironment environment;
    const auto a = pki::make_authority();
    const auto b = pki::make_authority({.seed = "b"});
    const std::string key_a =
        write_file("a.key", encode_pem("PRIVATE KEY", pki::pkcs8_der(a.leaf.keys)));
    const std::string key_b =
        write_file("b.key", encode_pem("PRIVATE KEY", pki::pkcs8_der(b.leaf.keys)));
    const std::string chain_a =
        write_file("a.pem", pem_certificate(a.root.der) + pem_certificate(a.intermediate.der) +
                                pem_certificate(a.leaf.der));
    const std::string chain_b =
        write_file("b.pem", pem_certificate(b.root.der) + pem_certificate(b.intermediate.der) +
                                pem_certificate(b.leaf.der));

    SECTION("nothing set is absent") {
        CHECK(load_content_signing_key("").error().kind == CredentialErrorKind::kAbsent);
        CHECK(load_content_signing_chain("").error().kind == CredentialErrorKind::kAbsent);
    }
    SECTION("the key file variable") {
        iclforge::test::platform::set_environment(kEnvContentSigningKeyFile, key_a);
        const auto key = load_content_signing_key("");
        REQUIRE(key.has_value());
        CHECK(key->public_key() == a.leaf.keys.pub);
    }
    SECTION("the inline key variable") {
        iclforge::test::platform::set_environment(
            kEnvContentSigningKey, encode_pem("PRIVATE KEY", pki::pkcs8_der(a.leaf.keys)));
        const auto key = load_content_signing_key("");
        REQUIRE(key.has_value());
        CHECK(key->public_key() == a.leaf.keys.pub);
    }
    SECTION("the file variable wins over the inline one") {
        iclforge::test::platform::set_environment(kEnvContentSigningKeyFile, key_a);
        iclforge::test::platform::set_environment(
            kEnvContentSigningKey, encode_pem("PRIVATE KEY", pki::pkcs8_der(b.leaf.keys)));
        CHECK(load_content_signing_key("")->public_key() == a.leaf.keys.pub);
    }
    SECTION("a path wins over both, and the environment is not consulted for that material") {
        iclforge::test::platform::set_environment(kEnvContentSigningKeyFile, key_a);
        iclforge::test::platform::set_environment(
            kEnvContentSigningKey, encode_pem("PRIVATE KEY", pki::pkcs8_der(a.leaf.keys)));
        CHECK(load_content_signing_key(key_b)->public_key() == b.leaf.keys.pub);
        // a bad path is an error, and does not fall back to the variable
        const auto bad = load_content_signing_key(scratch("missing.key").string());
        REQUIRE_FALSE(bad.has_value());
        CHECK(bad.error().kind == CredentialErrorKind::kUnreadable);
    }
    SECTION("a variable that names a missing file is an error and not a fall-through") {
        iclforge::test::platform::set_environment(kEnvContentSigningKeyFile,
                                                  scratch("gone.key").string());
        iclforge::test::platform::set_environment(
            kEnvContentSigningKey, encode_pem("PRIVATE KEY", pki::pkcs8_der(a.leaf.keys)));
        const auto bad = load_content_signing_key("");
        REQUIRE_FALSE(bad.has_value());
        CHECK(bad.error().kind == CredentialErrorKind::kUnreadable);
        CHECK(bad.error().message.find(std::string{kEnvContentSigningKeyFile}) !=
              std::string::npos);
    }
    SECTION("the chain variable and the chain path") {
        iclforge::test::platform::set_environment(kEnvContentSigningChainFile, chain_a);
        const auto from_env = load_signing_credentials(key_a, "");
        REQUIRE(from_env.has_value());
        CHECK(from_env->chain.back().der == a.leaf.der);
        const auto from_path = load_signing_credentials(key_b, chain_b);
        REQUIRE(from_path.has_value());
        CHECK(from_path->chain.back().der == b.leaf.der);
    }
}

TEST_CASE("the trust store is the operator's", "[crypto][credentials][files]") {
    const CleanEnvironment environment;
    const auto a = pki::make_authority();
    const auto b = pki::make_authority({.seed = "b"});
    const std::string root_a = write_file("root_a.pem", pem_certificate(a.root.der));
    const std::string both_roots =
        write_file("both.pem", pem_certificate(a.root.der) + pem_certificate(b.root.der));
    const std::string key_only =
        write_file("leaf_pub.pem", encode_pem("PUBLIC KEY", pki::spki_der(a.leaf.keys.pub)));
    const std::string cert_for_key = write_file("leaf_cert.pem", pem_certificate(a.leaf.der));
    const std::string mixed =
        write_file("mixed.pem", pem_certificate(a.root.der) +
                                    encode_pem("PUBLIC KEY", pki::spki_der(b.leaf.keys.pub)));

    SECTION("nothing offered is an error, never an empty store") {
        const auto store = load_trust_store({}, {});
        REQUIRE_FALSE(store.has_value());
        CHECK(store.error().kind == CredentialErrorKind::kAbsent);
    }
    SECTION("anchors from files, several to a file") {
        const std::vector<std::string> anchors = {both_roots, root_a};
        const auto store = load_trust_store(anchors, {});
        REQUIRE(store.has_value());
        CHECK(store->anchors.size() == 3);
        CHECK(store->keys.empty());
        CHECK(store->anchors[0].label == both_roots);
    }
    SECTION("keys from a public key file and from a certificate") {
        const std::vector<std::string> keys = {key_only, cert_for_key};
        const auto store = load_trust_store({}, keys);
        REQUIRE(store.has_value());
        CHECK(store->anchors.empty());
        REQUIRE(store->keys.size() == 2);
        CHECK(store->keys[0].key == a.leaf.keys.pub);
        CHECK(store->keys[1].key == a.leaf.keys.pub);
    }
    SECTION("the environment file is read only when nothing was named, and takes both kinds") {
        iclforge::test::platform::set_environment(kEnvContentTrustFile, mixed);
        const auto from_env = load_trust_store({}, {});
        REQUIRE(from_env.has_value());
        CHECK(from_env->anchors.size() == 1);
        CHECK(from_env->keys.size() == 1);

        const std::vector<std::string> anchors = {root_a};
        const auto named = load_trust_store(anchors, {});
        REQUIRE(named.has_value());
        CHECK(named->anchors.size() == 1);
        CHECK(named->keys
                  .empty());  // the variable's key was not added to a set the operator spelled out
    }
    SECTION("a bad or missing file is an error that names it") {
        const std::vector<std::string> missing = {scratch("none.pem").string()};
        const auto result = load_trust_store(missing, {});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == CredentialErrorKind::kUnreadable);
        const std::string junk = write_file("junk.pem", "not a certificate");
        const std::vector<std::string> junk_list = {junk};
        const auto bad = load_trust_store(junk_list, {});
        REQUIRE_FALSE(bad.has_value());
        CHECK(bad.error().message.find("junk.pem") != std::string::npos);
        const std::vector<std::string> bad_keys = {junk};
        CHECK_FALSE(load_trust_store({}, bad_keys).has_value());
    }
    SECTION("what was loaded validates a chain") {
        const std::vector<std::string> anchors = {root_a};
        const auto store = load_trust_store(anchors, {});
        REQUIRE(store.has_value());
        ChainPolicy policy;
        policy.now_unix = pki::at(2026, 10, 29);
        CHECK(validate_chain(a.chain(), *store, policy).has_value());
        CHECK_FALSE(validate_chain(b.chain(), *store, policy).has_value());
    }
}
