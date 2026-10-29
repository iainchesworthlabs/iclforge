#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "crypto_test_support.hpp"
#include "iclforge/base/crypto/ecdsa_p256.hpp"
#include "iclforge/base/crypto/sha1.hpp"
#include "iclforge/base/crypto/sha256.hpp"

namespace {

using iclforge::base::crypto::EcdsaSignature;
using iclforge::base::crypto::P256PrivateKey;
using iclforge::base::crypto::P256PublicKey;
using iclforge::test::crypto::array_from_hex;
using iclforge::test::crypto::from_hex;
using iclforge::test::crypto::text_bytes;
using iclforge::test::crypto::to_hex;

P256PrivateKey private_key(std::string_view hex) {
    const auto key = P256PrivateKey::from_scalar(from_hex(hex));
    REQUIRE(key.has_value());
    return *key;
}

P256PublicKey public_key(std::string_view x, std::string_view y) {
    P256PublicKey key;
    key.x = array_from_hex<32>(x);
    key.y = array_from_hex<32>(y);
    return key;
}

std::array<std::byte, 32> digest_of(std::string_view message) {
    return iclforge::base::crypto::sha256(text_bytes(message));
}

}  // namespace

// --- SHA-1 (FIPS 180-4), used only for fingerprint pins ----------------------
TEST_CASE("SHA-1 matches the FIPS test vectors", "[crypto][sha1]") {
    using iclforge::base::crypto::sha1;
    CHECK(to_hex(sha1(text_bytes("abc"))) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(to_hex(sha1(text_bytes(""))) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(to_hex(sha1(text_bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))) ==
          "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    // One million 'a', the multi-block case.
    const std::string million(1000000, 'a');
    CHECK(to_hex(sha1(text_bytes(million))) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

// --- The group law, through the public key -----------------------------------
TEST_CASE("P-256 public keys match the group law at the ends of the scalar range",
          "[crypto][ecdsa]") {
    // d = 1 is the generator, d = 2 its double, d = n - 1 its negation (same x, y -> p - y).
    // The expected points are from Python's `cryptography` (OpenSSL), computed independently.
    const auto one = iclforge::base::crypto::p256_public_key(
        private_key("0000000000000000000000000000000000000000000000000000000000000001"));
    REQUIRE(one.has_value());
    CHECK(*one == public_key("6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296",
                             "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5"));

    const auto two = iclforge::base::crypto::p256_public_key(
        private_key("0000000000000000000000000000000000000000000000000000000000000002"));
    REQUIRE(two.has_value());
    CHECK(*two == public_key("7cf27b188d034f7e8a52380304b51ac3c08969e277f21b35a60b48fc47669978",
                             "07775510db8ed040293d9ac69f7430dbba7dade63ce982299e04b79d227873d1"));

    const auto minus_one = iclforge::base::crypto::p256_public_key(
        private_key("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632550"));
    REQUIRE(minus_one.has_value());
    CHECK(*minus_one ==
          public_key("6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296",
                     "b01cbd1c01e58065711814b583f061e9d431cca994cea1313449bf97c840ae0a"));
}

TEST_CASE("P-256 private keys outside 1..n-1 are refused", "[crypto][ecdsa]") {
    CHECK_FALSE(P256PrivateKey::from_scalar(
                    from_hex("0000000000000000000000000000000000000000000000000000000000000000"))
                    .has_value());
    CHECK_FALSE(P256PrivateKey::from_scalar(
                    from_hex("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551"))
                    .has_value());  // n itself
    CHECK_FALSE(P256PrivateKey::from_scalar(
                    from_hex("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"))
                    .has_value());
    CHECK_FALSE(P256PrivateKey::from_scalar(from_hex("0102")).has_value());
    CHECK(P256PrivateKey::from_scalar(
              from_hex("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632550"))
              .has_value());  // n - 1
}

// --- RFC 6979 Appendix A.2.5 (P-256, SHA-256) ---------------------------------
TEST_CASE("ECDSA P-256 signatures match RFC 6979 A.2.5", "[crypto][ecdsa][rfc6979]") {
    const P256PrivateKey key =
        private_key("c9afa9d845ba75166b5c215767b1d6934e50c3db36e89b127b8a622b120f6721");
    const auto pub = iclforge::base::crypto::p256_public_key(key);
    REQUIRE(pub.has_value());
    CHECK(*pub == public_key("60fed4ba255a9d31c961eb74c6356d68c049b8923b61fa6ce669622e60f29fb6",
                             "7903fe1008b8bc99a41ae9e95628bc64f2f1b20c2d7e9f5177a3c294d4462299"));

    const auto sample = iclforge::base::crypto::ecdsa_p256_sign(key, digest_of("sample"));
    REQUIRE(sample.has_value());
    CHECK(to_hex(sample->r) == "efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf3716");
    CHECK(to_hex(sample->s) == "f7cb1c942d657c41d436c7a1b6e29f65f3e900dbb9aff4064dc4ab2f843acda8");

    const auto test = iclforge::base::crypto::ecdsa_p256_sign(key, digest_of("test"));
    REQUIRE(test.has_value());
    CHECK(to_hex(test->r) == "f1abb023518351cd71d881567b1ea663ed3efcf6c5132b354f28d3b0b7d38367");
    CHECK(to_hex(test->s) == "019f4113742a2b14bd25926b49c649155f267e60d3814b4c0cc84250e46f0083");

    CHECK(iclforge::base::crypto::ecdsa_p256_verify(*pub, digest_of("sample"), *sample));
    CHECK(iclforge::base::crypto::ecdsa_p256_verify(*pub, digest_of("test"), *test));
    // A signature is for its own message only.
    CHECK_FALSE(iclforge::base::crypto::ecdsa_p256_verify(*pub, digest_of("test"), *sample));
}

// --- Signatures made by OpenSSL (random nonces) must verify here ---------------
TEST_CASE("ECDSA P-256 verifies signatures made by OpenSSL", "[crypto][ecdsa][oracle]") {
    struct Case {
        const char* message;
        const char* x;
        const char* y;
        const char* r;
        const char* s;
    };
    // Made with Python `cryptography` 50 / OpenSSL 3 from fixed private scalars, over the message's
    // SHA-256 (`key.sign(message, ec.ECDSA(hashes.SHA256()))`), then split into r and s.
    const Case cases[] = {
        {"ICL Forge ECDSA cross-check 0",
         "efdb2a7de656542db086fada0fe3845c803a9d20dd27af69fc567f5f67df3661",
         "fa807fad1bf67354d52b1ec970dfb7e2b8092fc7a07473c33216d68434463ea7",
         "ee1dcedd50938532068e583f6f8d1c675d5aeec6c36a92bef00f5f251319e867",
         "a321fef73e8f303ad1a8f73bcbeecc57f42cb0ab621a41d07f2cedd572f175fa"},
        {"ICL Forge ECDSA cross-check 1",
         "43e77afd40b2c07acfb39863339a5ece6514057eacfc0962f6a8c37712340024",
         "4796fa2921d656d8a9a356f18b387da9b752063442c5c62a179e34a152abe7bb",
         "8874e001627026f77e6ebaf1dcaf0d79e377108db213a3a6a9c97d35990aee5b",
         "cc5a5d62647d38ce5c74accd62cf27dcce4c8b1d4a5f026b00c5bd71670d2371"},
    };
    for (const Case& c : cases) {
        const P256PublicKey pub = public_key(c.x, c.y);
        EcdsaSignature sig;
        sig.r = array_from_hex<32>(c.r);
        sig.s = array_from_hex<32>(c.s);
        CHECK(iclforge::base::crypto::ecdsa_p256_verify(pub, digest_of(c.message), sig));

        // Every kind of damage is refused, each tried separately.
        EcdsaSignature bad_r = sig;
        bad_r.r[31] ^= std::byte{0x01};
        CHECK_FALSE(iclforge::base::crypto::ecdsa_p256_verify(pub, digest_of(c.message), bad_r));
        EcdsaSignature bad_s = sig;
        bad_s.s[0] ^= std::byte{0x80};
        CHECK_FALSE(iclforge::base::crypto::ecdsa_p256_verify(pub, digest_of(c.message), bad_s));
        CHECK_FALSE(
            iclforge::base::crypto::ecdsa_p256_verify(pub, digest_of("another message"), sig));
        P256PublicKey wrong_key = pub;
        wrong_key.x[31] ^= std::byte{0x01};
        CHECK_FALSE(
            iclforge::base::crypto::ecdsa_p256_verify(wrong_key, digest_of(c.message), sig));
    }
}

TEST_CASE("ECDSA P-256 verify refuses r or s outside 1..n-1", "[crypto][ecdsa]") {
    const P256PrivateKey key =
        private_key("c9afa9d845ba75166b5c215767b1d6934e50c3db36e89b127b8a622b120f6721");
    const auto pub = iclforge::base::crypto::p256_public_key(key);
    REQUIRE(pub.has_value());
    const auto good = iclforge::base::crypto::ecdsa_p256_sign(key, digest_of("sample"));
    REQUIRE(good.has_value());

    EcdsaSignature zero_r = *good;
    zero_r.r.fill(std::byte{0});
    CHECK_FALSE(iclforge::base::crypto::ecdsa_p256_verify(*pub, digest_of("sample"), zero_r));
    EcdsaSignature zero_s = *good;
    zero_s.s.fill(std::byte{0});
    CHECK_FALSE(iclforge::base::crypto::ecdsa_p256_verify(*pub, digest_of("sample"), zero_s));
    EcdsaSignature order_r = *good;
    order_r.r =
        array_from_hex<32>("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551");
    CHECK_FALSE(iclforge::base::crypto::ecdsa_p256_verify(*pub, digest_of("sample"), order_r));
    EcdsaSignature high_s = *good;
    high_s.s =
        array_from_hex<32>("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    CHECK_FALSE(iclforge::base::crypto::ecdsa_p256_verify(*pub, digest_of("sample"), high_s));
}

// --- Public key checks --------------------------------------------------------
TEST_CASE("P-256 public keys must be on the curve", "[crypto][ecdsa]") {
    using iclforge::base::crypto::p256_public_key_from_uncompressed;
    using iclforge::base::crypto::p256_public_key_uncompressed;
    using iclforge::base::crypto::p256_public_key_valid;
    const P256PublicKey g =
        public_key("6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296",
                   "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5");
    CHECK(p256_public_key_valid(g));

    P256PublicKey off_curve = g;
    off_curve.y[31] ^= std::byte{0x01};
    CHECK_FALSE(p256_public_key_valid(off_curve));

    // A coordinate at or above p is refused even where its residue would be on the curve.
    P256PublicKey beyond_p = g;
    beyond_p.x =
        array_from_hex<32>("ffffffff00000001000000000000000000000000ffffffffffffffffffffffff");
    CHECK_FALSE(p256_public_key_valid(beyond_p));

    const auto encoded = p256_public_key_uncompressed(g);
    CHECK(encoded[0] == std::byte{0x04});
    const auto round = p256_public_key_from_uncompressed(encoded);
    REQUIRE(round.has_value());
    CHECK(*round == g);
    auto compressed = std::vector<std::byte>(encoded.begin(), encoded.end());
    compressed[0] = std::byte{0x02};
    CHECK_FALSE(p256_public_key_from_uncompressed(compressed).has_value());
    compressed.resize(33);
    CHECK_FALSE(p256_public_key_from_uncompressed(compressed).has_value());

    // A verify with a key off the curve says no, whatever the signature.
    EcdsaSignature any;
    any.r = array_from_hex<32>("0000000000000000000000000000000000000000000000000000000000000001");
    any.s = any.r;
    CHECK_FALSE(iclforge::base::crypto::ecdsa_p256_verify(off_curve, digest_of("x"), any));
}

// --- DER ----------------------------------------------------------------------
TEST_CASE("ECDSA signatures convert to and from DER strictly", "[crypto][ecdsa][der]") {
    using iclforge::base::crypto::ecdsa_signature_from_der;
    using iclforge::base::crypto::ecdsa_signature_to_der;

    // r has its high bit set (so a 0x00 pad, 33 bytes); s has two leading zero bytes (so a shorter,
    // 30-byte INTEGER).
    EcdsaSignature sig;
    sig.r = array_from_hex<32>("efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf3716");
    sig.s = array_from_hex<32>("000012a1c3b8a5f10c3e5f9a2b4d6e8f10325476981a3c5e7f90b2d4f6081a3c");
    const std::string expected =
        "3043"
        "022100efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf3716"
        "021e12a1c3b8a5f10c3e5f9a2b4d6e8f10325476981a3c5e7f90b2d4f6081a3c";
    const std::vector<std::byte> der = ecdsa_signature_to_der(sig);
    CHECK(to_hex(der) == expected);
    const auto back = ecdsa_signature_from_der(der);
    REQUIRE(back.has_value());
    CHECK(*back == sig);

    // Trailing byte, long-form length, non-minimal INTEGER, negative INTEGER, truncation.
    std::vector<std::byte> trailing = der;
    trailing.push_back(std::byte{0});
    CHECK_FALSE(ecdsa_signature_from_der(trailing).has_value());
    CHECK_FALSE(ecdsa_signature_from_der(from_hex("308143" + expected.substr(4))).has_value());
    // r written with a needless 0x00 before a byte whose high bit is clear
    CHECK_FALSE(
        ecdsa_signature_from_der(
            from_hex("3044"
                     "022100efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf3716"
                     "021f0012a1c3b8a5f10c3e5f9a2b4d6e8f10325476981a3c5e7f90b2d4f6081a3c"))
            .has_value());
    // r with its high bit set and no pad would be negative
    CHECK_FALSE(ecdsa_signature_from_der(
                    from_hex("3025"
                             "0220efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf3716"
                             "020101"))
                    .has_value());
    CHECK_FALSE(ecdsa_signature_from_der(std::span<const std::byte>(der).first(der.size() - 1))
                    .has_value());
    // r = 0
    CHECK_FALSE(ecdsa_signature_from_der(from_hex("3006020100020101")).has_value());
}
