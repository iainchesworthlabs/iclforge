// Known-answer tests for the in-tree P-256 and ECDSA code (libs/base/src/crypto/p256.cpp and
// p256_math.hpp). The oracles are RFC 6979 Appendix A.2.5, the curve constants of FIPS 186-4, and
// the vector file tools/generators/gen_p256_vectors.py writes (Python integers for the modular
// arithmetic, OpenSSL for the points and for the verdict on every signature). The tests that run
// OpenSSL against this code live are in tests/interop/, which needs Python and skips without it.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/base/crypto/p256.hpp"
#include "iclforge/base/crypto/sha256.hpp"
#include "p256_math.hpp"

namespace {

using namespace iclforge::base::crypto;
namespace math = iclforge::base::crypto::detail;

struct FieldVector {
    int modulus;
    const char* a;
    const char* b;
    const char* mul;
    const char* add;
    const char* sub;
    const char* inverse;
};
struct PointVector {
    const char* k;
    const char* x;
    const char* y;
};
struct OffCurveVector {
    const char* x;
    const char* y;
};
struct SignVector {
    const char* d;
    const char* qx;
    const char* qy;
    const char* digest;
    const char* k;
    const char* r;
    const char* s;
};
struct VerifyVector {
    const char* qx;
    const char* qy;
    const char* digest;
    const char* r;
    const char* s;
    bool valid;
};

#include "p256_vectors.inc"

int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

std::vector<std::byte> hex(std::string_view text) {
    std::vector<std::byte> out;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
        out.push_back(static_cast<std::byte>((nibble(text[i]) << 4) | nibble(text[i + 1])));
    }
    return out;
}

std::array<std::byte, 32> hex32(std::string_view text) {
    REQUIRE(text.size() == 64);
    std::array<std::byte, 32> out{};
    const auto bytes = hex(text);
    std::copy(bytes.begin(), bytes.end(), out.begin());
    return out;
}

math::Limbs limbs(std::string_view text) {
    const auto bytes = hex32(text);
    return math::limbs_from_bytes(bytes.data());
}

std::string to_hex(const math::Limbs& value) {
    std::array<std::byte, 32> bytes{};
    math::limbs_to_bytes(value, bytes.data());
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out;
    for (const std::byte b : bytes) {
        out.push_back(kDigits[std::to_integer<unsigned>(b) >> 4]);
        out.push_back(kDigits[std::to_integer<unsigned>(b) & 0xF]);
    }
    return out;
}

std::string to_hex(std::span<const std::byte> bytes) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out;
    for (const std::byte b : bytes) {
        out.push_back(kDigits[std::to_integer<unsigned>(b) >> 4]);
        out.push_back(kDigits[std::to_integer<unsigned>(b) & 0xF]);
    }
    return out;
}

std::array<std::byte, 65> point_bytes(std::string_view x, std::string_view y) {
    std::array<std::byte, 65> out{};
    out[0] = std::byte{0x04};
    const auto xb = hex32(x);
    const auto yb = hex32(y);
    std::copy(xb.begin(), xb.end(), out.begin() + 1);
    std::copy(yb.begin(), yb.end(), out.begin() + 33);
    return out;
}

constexpr std::string_view kP = "FFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF";
constexpr std::string_view kN = "FFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551";
constexpr std::string_view kGx = "6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296";
constexpr std::string_view kGy = "4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5";

}  // namespace

TEST_CASE("the P-256 constants in the source are FIPS 186-4's", "[crypto][p256]") {
    // Typed from the specification's table (section 2.2), compared with the values the code was
    // given; the vector tests below would fail on a wrong prime, but a wrong b or G would only
    // show as every point being wrong, and this says which constant it was.
    CHECK(to_hex(math::kPrimeLimbs) == kP);
    CHECK(to_hex(math::kOrderLimbs) == kN);
    CHECK(to_hex(math::kGeneratorXLimbs) == kGx);
    CHECK(to_hex(math::kGeneratorYLimbs) == kGy);
    CHECK(to_hex(math::kCurveBLimbs) ==
          "5AC635D8AA3A93E7B3EBBD55769886BC651D06B0CC53B0F63BCE3C3E27D2604B");
    CHECK(math::on_curve(math::kGeneratorXLimbs, math::kGeneratorYLimbs));
}

TEST_CASE("the Montgomery constants are right for p and n", "[crypto][p256]") {
    for (const math::Modulus* mod : {&math::kField, &math::kOrder}) {
        // m * (-m^-1) = -1 (mod 2^32), the defining property of n0
        CHECK(static_cast<std::uint32_t>(mod->m[0] * mod->n0) == 0xFFFFFFFFu);
        // R mod m is 2^256 - m, because m is above 2^255
        math::Limbs sum{};
        CHECK(math::add_with_carry(sum, mod->one, mod->m) == 1u);
        CHECK(sum == math::Limbs{});
    }
}

TEST_CASE("modular arithmetic agrees with Python integers", "[crypto][p256]") {
    REQUIRE(sizeof(kFieldVectors) / sizeof(kFieldVectors[0]) > 100);
    for (const FieldVector& v : kFieldVectors) {
        const math::Modulus& mod = v.modulus == 0 ? math::kField : math::kOrder;
        const math::Limbs a = limbs(v.a);
        const math::Limbs b = limbs(v.b);
        const math::Limbs am = math::to_mont(a, mod);
        const math::Limbs bm = math::to_mont(b, mod);
        INFO("modulus " << (v.modulus == 0 ? "p" : "n") << " a=" << v.a << " b=" << v.b);
        CHECK(to_hex(math::from_mont(am, mod)) == v.a);
        CHECK(to_hex(math::from_mont(math::mont_mul(am, bm, mod), mod)) == v.mul);
        CHECK(to_hex(math::mod_add(a, b, mod.m)) == v.add);
        CHECK(to_hex(math::mod_sub(a, b, mod.m)) == v.sub);
        CHECK(to_hex(math::from_mont(math::mont_inverse(am, mod), mod)) == v.inverse);
    }
}

TEST_CASE("k times G matches OpenSSL", "[crypto][p256]") {
    REQUIRE(sizeof(kPointVectors) / sizeof(kPointVectors[0]) > 80);
    for (const PointVector& v : kPointVectors) {
        const auto key = P256PrivateKey::from_scalar(hex32(v.k));
        REQUIRE(key.has_value());
        const auto pub = key->public_key();
        REQUIRE(pub.has_value());
        INFO("k=" << v.k);
        CHECK(to_hex(pub->x()) == v.x);
        CHECK(to_hex(pub->y()) == v.y);
    }
}

TEST_CASE("the group order and its neighbours", "[crypto][p256]") {
    // n is not a private key; n-1 is, and (n-1)G = -G.
    CHECK_FALSE(P256PrivateKey::from_scalar(hex32(kN)).has_value());
    CHECK_FALSE(P256PrivateKey::from_scalar(
                    hex32("0000000000000000000000000000000000000000000000000000000000000000"))
                    .has_value());
    CHECK_FALSE(P256PrivateKey::from_scalar(
                    hex32("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"))
                    .has_value());
    CHECK_FALSE(P256PrivateKey::from_scalar(
                    hex32("FFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632552"))
                    .has_value());
    CHECK_FALSE(P256PrivateKey::from_scalar(std::span<const std::byte>{}).has_value());
    const std::array<std::byte, 31> short_scalar{};
    CHECK_FALSE(P256PrivateKey::from_scalar(short_scalar).has_value());

    const auto one = P256PrivateKey::from_scalar(
        hex32("0000000000000000000000000000000000000000000000000000000000000001"));
    REQUIRE(one.has_value());
    CHECK(to_hex(one->public_key()->x()) == kGx);
    CHECK(to_hex(one->public_key()->y()) == kGy);

    const auto minus_one = P256PrivateKey::from_scalar(
        hex32("FFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632550"));
    REQUIRE(minus_one.has_value());
    CHECK(to_hex(minus_one->public_key()->x()) == kGx);
    // y(-G) = p - y(G)
    CHECK(to_hex(minus_one->public_key()->y()) ==
          "B01CBD1C01E58065711814B583F061E9D431CCA994CEA1313449BF97C840AE0A");
}

TEST_CASE("public keys: only points of the curve are accepted", "[crypto][p256]") {
    const auto good = P256PublicKey::from_uncompressed(point_bytes(kGx, kGy));
    REQUIRE(good.has_value());
    CHECK(good->uncompressed() == point_bytes(kGx, kGy));

    for (const OffCurveVector& v : kOffCurveVectors) {
        INFO("x=" << v.x << " y=" << v.y);
        CHECK_FALSE(P256PublicKey::from_uncompressed(point_bytes(v.x, v.y)).has_value());
    }

    auto bytes = point_bytes(kGx, kGy);
    SECTION("a prefix other than 04") {
        for (const unsigned prefix : {0x00u, 0x02u, 0x03u, 0x05u, 0x06u, 0x07u, 0xFFu}) {
            bytes[0] = static_cast<std::byte>(prefix);
            CHECK_FALSE(P256PublicKey::from_uncompressed(bytes).has_value());
        }
    }
    SECTION("a wrong length") {
        CHECK_FALSE(P256PublicKey::from_uncompressed(std::span{bytes}.first(64)).has_value());
        CHECK_FALSE(P256PublicKey::from_uncompressed(std::span{bytes}.first(33)).has_value());
        CHECK_FALSE(P256PublicKey::from_uncompressed(std::span<const std::byte>{}).has_value());
        std::vector<std::byte> longer(bytes.begin(), bytes.end());
        longer.push_back(std::byte{0});
        CHECK_FALSE(P256PublicKey::from_uncompressed(longer).has_value());
    }
}

TEST_CASE("RFC 6979 appendix A.2.5: ECDSA P-256 with SHA-256", "[crypto][p256][rfc6979]") {
    const auto key = P256PrivateKey::from_scalar(
        hex32("C9AFA9D845BA75166B5C215767B1D6934E50C3DB36E89B127B8A622B120F6721"));
    REQUIRE(key.has_value());
    const auto pub = key->public_key();
    REQUIRE(pub.has_value());
    CHECK(to_hex(pub->x()) == "60FED4BA255A9D31C961EB74C6356D68C049B8923B61FA6CE669622E60F29FB6");
    CHECK(to_hex(pub->y()) == "7903FE1008B8BC99A41AE9E95628BC64F2F1B20C2D7E9F5177A3C294D4462299");

    struct Case {
        std::string_view message;
        std::string_view k;
        std::string_view r;
        std::string_view s;
    };
    const Case cases[] = {
        {"sample", "A6E3C57DD01ABE90086538398355DD4C3B17AA873382B0F24D6129493D8AAD60",
         "EFD48B2AACB6A8FD1140DD9CD45E81D69D2C877B56AAF991C34D0EA84EAF3716",
         "F7CB1C942D657C41D436C7A1B6E29F65F3E900DBB9AFF4064DC4AB2F843ACDA8"},
        {"test", "D16B6AE827F17175E040871A1C7EC3500192C4C92677336EC2537ACAEE0008E0",
         "F1ABB023518351CD71D881567B1EA663ED3EFCF6C5132B354F28D3B0B7D38367",
         "019F4113742A2B14BD25926B49C649155F267E60D3814B4C0CC84250E46F0083"},
    };
    for (const Case& c : cases) {
        const std::vector<std::byte> message(
            reinterpret_cast<const std::byte*>(c.message.data()),
            reinterpret_cast<const std::byte*>(c.message.data()) + c.message.size());
        const Digest256 digest = sha256(message);
        INFO(c.message);
        const auto k = rfc6979_nonce(*key, digest);
        REQUIRE(k.has_value());
        CHECK(to_hex(*k) == c.k);
        const auto signature = ecdsa_sign(*key, digest);
        REQUIRE(signature.has_value());
        CHECK(to_hex(signature->r) == c.r);
        CHECK(to_hex(signature->s) == c.s);
        CHECK(ecdsa_verify(*pub, digest, *signature));
    }
}

TEST_CASE("ECDSA signing with RFC 6979 nonces matches an independent implementation",
          "[crypto][p256][rfc6979]") {
    REQUIRE(sizeof(kSignVectors) / sizeof(kSignVectors[0]) >= 30);
    for (const SignVector& v : kSignVectors) {
        const auto key = P256PrivateKey::from_scalar(hex32(v.d));
        REQUIRE(key.has_value());
        const auto digest_bytes = hex32(v.digest);
        INFO("d=" << v.d << " digest=" << v.digest);
        const auto pub = key->public_key();
        CHECK(to_hex(pub->x()) == v.qx);
        CHECK(to_hex(pub->y()) == v.qy);
        const auto k = rfc6979_nonce(*key, digest_bytes);
        REQUIRE(k.has_value());
        CHECK(to_hex(*k) == v.k);
        const auto signature = ecdsa_sign(*key, digest_bytes);
        REQUIRE(signature.has_value());
        CHECK(to_hex(signature->r) == v.r);
        CHECK(to_hex(signature->s) == v.s);
    }
}

TEST_CASE("ECDSA verification agrees with OpenSSL on valid and invalid signatures",
          "[crypto][p256]") {
    std::size_t valid = 0;
    std::size_t invalid = 0;
    for (const VerifyVector& v : kVerifyVectors) {
        const auto pub = P256PublicKey::from_uncompressed(point_bytes(v.qx, v.qy));
        REQUIRE(pub.has_value());
        EcdsaSignature signature;
        signature.r = hex32(v.r);
        signature.s = hex32(v.s);
        INFO("Q=" << v.qx << " digest=" << v.digest << " r=" << v.r << " s=" << v.s);
        CHECK(ecdsa_verify(*pub, hex32(v.digest), signature) == v.valid);
        (v.valid ? valid : invalid)++;
    }
    // The file holds both kinds, so that a verifier that accepts everything or nothing fails.
    CHECK(valid >= 20);
    CHECK(invalid >= 100);
}

TEST_CASE("signing is deterministic and changes with the digest and the key", "[crypto][p256]") {
    const auto key = P256PrivateKey::from_scalar(hex32(kGx));
    REQUIRE(key.has_value());
    const Digest256 a = sha256(std::vector<std::byte>{std::byte{'a'}});
    const Digest256 b = sha256(std::vector<std::byte>{std::byte{'b'}});
    const auto sig_a1 = ecdsa_sign(*key, a);
    const auto sig_a2 = ecdsa_sign(*key, a);
    const auto sig_b = ecdsa_sign(*key, b);
    REQUIRE(sig_a1.has_value());
    CHECK(*sig_a1 == *sig_a2);
    CHECK_FALSE(*sig_a1 == *sig_b);

    const auto other = P256PrivateKey::from_scalar(hex32(kGy));
    CHECK_FALSE(*sig_a1 == *ecdsa_sign(*other, a));
    // a signature does not verify under another key, or over another digest
    CHECK(ecdsa_verify(*key->public_key(), a, *sig_a1));
    CHECK_FALSE(ecdsa_verify(*other->public_key(), a, *sig_a1));
    CHECK_FALSE(ecdsa_verify(*key->public_key(), b, *sig_a1));
}

TEST_CASE("an empty private key does not sign", "[crypto][p256]") {
    const P256PrivateKey empty;
    CHECK(empty.empty());
    CHECK_FALSE(empty.public_key().has_value());
    CHECK_FALSE(ecdsa_sign(empty, Digest256{}).has_value());
    CHECK_FALSE(rfc6979_nonce(empty, Digest256{}).has_value());
}

TEST_CASE("verification refuses r and s outside 1..n-1", "[crypto][p256]") {
    const auto key = P256PrivateKey::from_scalar(hex32(kGx));
    const auto pub = *key->public_key();
    const Digest256 digest = sha256(std::vector<std::byte>{std::byte{'x'}});
    const auto good = *ecdsa_sign(*key, digest);
    REQUIRE(ecdsa_verify(pub, digest, good));
    EcdsaSignature s = good;
    s.r = {};
    CHECK_FALSE(ecdsa_verify(pub, digest, s));
    s = good;
    s.s = {};
    CHECK_FALSE(ecdsa_verify(pub, digest, s));
    s = good;
    s.r = hex32(kN);
    CHECK_FALSE(ecdsa_verify(pub, digest, s));
    s = good;
    s.s = hex32(kN);
    CHECK_FALSE(ecdsa_verify(pub, digest, s));
    s = good;
    s.s = hex32("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF");
    CHECK_FALSE(ecdsa_verify(pub, digest, s));
    CHECK_FALSE(p256_scalar_in_range(hex32(kN)));
    CHECK(p256_scalar_in_range(
        hex32("0000000000000000000000000000000000000000000000000000000000000001")));
}

TEST_CASE("the high-s twin of a signature also verifies", "[crypto][p256]") {
    // The specification does not ask for low-s normalisation (register item 18), so a verifier
    // that required it would reject signatures other implementations make.
    const auto key = P256PrivateKey::from_scalar(hex32(kGy));
    const auto pub = *key->public_key();
    const Digest256 digest = sha256(std::vector<std::byte>{std::byte{'t'}});
    const auto good = *ecdsa_sign(*key, digest);
    // n - s, computed with the library's own modular subtraction
    const math::Limbs twin =
        math::mod_sub(math::Limbs{}, math::limbs_from_bytes(good.s.data()), math::kOrder.m);
    EcdsaSignature other = good;
    math::limbs_to_bytes(twin, other.s.data());
    CHECK(ecdsa_verify(pub, digest, other));
}
