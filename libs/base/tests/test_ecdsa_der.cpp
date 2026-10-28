// The DER form of an ECDSA signature: SEQUENCE { INTEGER r, INTEGER s } (ecdsa_der.hpp). The
// decoder is strict, so that one (r, s) has one encoding and a signature that only a lenient
// reader accepts is a failure here, not a surprise later.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <vector>

#include "iclforge/base/crypto/ecdsa_der.hpp"
#include "iclforge/base/crypto/p256.hpp"

namespace {

using namespace iclforge::base::crypto;

std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> out;
    for (const unsigned v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

std::array<std::byte, 32> scalar(std::uint8_t fill_low, std::uint8_t high = 0) {
    std::array<std::byte, 32> out{};
    out[0] = static_cast<std::byte>(high);
    out[31] = static_cast<std::byte>(fill_low);
    return out;
}

// n - 1, the largest valid r or s.
std::array<std::byte, 32> order_minus_one() {
    const std::array<std::uint8_t, 32> v = {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
                                            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                            0xBC, 0xE6, 0xFA, 0xAD, 0xA7, 0x17, 0x9E, 0x84,
                                            0xF3, 0xB9, 0xCA, 0xC2, 0xFC, 0x63, 0x25, 0x50};
    std::array<std::byte, 32> out{};
    for (std::size_t i = 0; i < 32; ++i) out[i] = static_cast<std::byte>(v[i]);
    return out;
}

}  // namespace

TEST_CASE("DER encoding of (r, s) is the minimal form", "[crypto][der]") {
    SECTION("r = s = 1") {
        EcdsaSignature s{scalar(1), scalar(1)};
        CHECK(encode_ecdsa_der(s) == bytes({0x30, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x01}));
    }
    SECTION("a high bit gets a leading zero") {
        EcdsaSignature s{scalar(0x80), scalar(0x7F)};
        CHECK(encode_ecdsa_der(s) == bytes({0x30, 0x07, 0x02, 0x02, 0x00, 0x80, 0x02, 0x01, 0x7F}));
    }
    SECTION("leading zero bytes are dropped") {
        std::array<std::byte, 32> r{};
        r[29] = std::byte{0x01};
        r[30] = std::byte{0x02};
        r[31] = std::byte{0x03};
        EcdsaSignature s{r, scalar(0xFF)};
        CHECK(encode_ecdsa_der(s) ==
              bytes({0x30, 0x09, 0x02, 0x03, 0x01, 0x02, 0x03, 0x02, 0x02, 0x00, 0xFF}));
    }
    SECTION("full width values with the high bit set are 33 bytes each") {
        EcdsaSignature s{order_minus_one(), order_minus_one()};
        const auto der = encode_ecdsa_der(s);
        CHECK(der.size() == 2 + 2 * (2 + 33));
        CHECK(der[0] == std::byte{0x30});
        CHECK(der[1] == std::byte{0x46});
        CHECK(der[2] == std::byte{0x02});
        CHECK(der[3] == std::byte{0x21});
        CHECK(der[4] == std::byte{0x00});
    }
    SECTION("full width values with the high bit clear are 32 bytes each, 70 in all") {
        std::array<std::byte, 32> r = order_minus_one();
        r[0] = std::byte{0x7F};
        EcdsaSignature s{r, r};
        CHECK(encode_ecdsa_der(s).size() == 2 + 2 * (2 + 32));
    }
}

TEST_CASE("DER decoding round-trips", "[crypto][der]") {
    for (const unsigned low : {1u, 2u, 0x7Fu, 0x80u, 0xFFu}) {
        for (const unsigned high : {0u, 1u, 0x7Fu, 0x80u, 0xFFu}) {
            EcdsaSignature s{
                scalar(static_cast<std::uint8_t>(low), static_cast<std::uint8_t>(high)),
                scalar(static_cast<std::uint8_t>(low ^ 0x55u), static_cast<std::uint8_t>(high))};
            if (!p256_scalar_in_range(s.r) || !p256_scalar_in_range(s.s)) {
                continue;
            }
            const auto decoded = decode_ecdsa_der(encode_ecdsa_der(s));
            REQUIRE(decoded.has_value());
            CHECK(*decoded == s);
        }
    }
    const EcdsaSignature edge{order_minus_one(), scalar(1)};
    const auto decoded = decode_ecdsa_der(encode_ecdsa_der(edge));
    REQUIRE(decoded.has_value());
    CHECK(*decoded == edge);
}

TEST_CASE("DER decoding refuses every non-canonical or invalid form", "[crypto][der]") {
    SECTION("the good encoding, for reference") {
        CHECK(
            decode_ecdsa_der(bytes({0x30, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x01})).has_value());
    }
    SECTION("empty and tiny inputs") {
        CHECK_FALSE(decode_ecdsa_der({}).has_value());
        CHECK_FALSE(decode_ecdsa_der(bytes({0x30})).has_value());
        CHECK_FALSE(decode_ecdsa_der(bytes({0x30, 0x00})).has_value());
    }
    SECTION("the indefinite length form of BER") {
        CHECK_FALSE(
            decode_ecdsa_der(bytes({0x30, 0x80, 0x02, 0x01, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00}))
                .has_value());
    }
    SECTION("a long-form length where the short form fits") {
        CHECK_FALSE(decode_ecdsa_der(bytes({0x30, 0x81, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x01}))
                        .has_value());
        CHECK_FALSE(decode_ecdsa_der(bytes({0x30, 0x06, 0x02, 0x81, 0x01, 0x01, 0x02, 0x01, 0x01}))
                        .has_value());
    }
    SECTION("a length with a leading zero byte") {
        CHECK_FALSE(
            decode_ecdsa_der(bytes({0x30, 0x82, 0x00, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x01}))
                .has_value());
    }
    SECTION("the wrong tags") {
        CHECK_FALSE(
            decode_ecdsa_der(bytes({0x31, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x01})).has_value());
        CHECK_FALSE(
            decode_ecdsa_der(bytes({0x30, 0x06, 0x04, 0x01, 0x01, 0x02, 0x01, 0x01})).has_value());
        CHECK_FALSE(
            decode_ecdsa_der(bytes({0x30, 0x06, 0x02, 0x01, 0x01, 0x04, 0x01, 0x01})).has_value());
    }
    SECTION("a negative integer") {
        CHECK_FALSE(
            decode_ecdsa_der(bytes({0x30, 0x06, 0x02, 0x01, 0x80, 0x02, 0x01, 0x01})).has_value());
        CHECK_FALSE(
            decode_ecdsa_der(bytes({0x30, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0xFF})).has_value());
    }
    SECTION("a redundant leading zero") {
        CHECK_FALSE(decode_ecdsa_der(bytes({0x30, 0x07, 0x02, 0x02, 0x00, 0x01, 0x02, 0x01, 0x01}))
                        .has_value());
        CHECK_FALSE(decode_ecdsa_der(bytes({0x30, 0x07, 0x02, 0x01, 0x01, 0x02, 0x02, 0x00, 0x7F}))
                        .has_value());
    }
    SECTION("an empty integer") {
        CHECK_FALSE(
            decode_ecdsa_der(bytes({0x30, 0x05, 0x02, 0x00, 0x02, 0x01, 0x01})).has_value());
    }
    SECTION("zero is not a valid r or s") {
        CHECK_FALSE(
            decode_ecdsa_der(bytes({0x30, 0x06, 0x02, 0x01, 0x00, 0x02, 0x01, 0x01})).has_value());
        CHECK_FALSE(
            decode_ecdsa_der(bytes({0x30, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x00})).has_value());
    }
    SECTION("r or s of n or more") {
        // n as an INTEGER is 00 FFFFFFFF 00000000 FFFFFFFF FFFFFFFF BCE6FAAD A7179E84 F3B9CAC2
        // FC632551
        std::vector<std::byte> n =
            bytes({0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF,
                   0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xBC, 0xE6, 0xFA, 0xAD, 0xA7,
                   0x17, 0x9E, 0x84, 0xF3, 0xB9, 0xCA, 0xC2, 0xFC, 0x63, 0x25, 0x51});
        std::vector<std::byte> der{std::byte{0x30}, std::byte{0x26}, std::byte{0x02},
                                   std::byte{0x21}};
        der.insert(der.end(), n.begin(), n.end());
        der.insert(der.end(), {std::byte{0x02}, std::byte{0x01}, std::byte{0x01}});
        CHECK_FALSE(decode_ecdsa_der(der).has_value());
        n.back() = std::byte{0x50};  // n - 1 is fine
        der.assign({std::byte{0x30}, std::byte{0x26}, std::byte{0x02}, std::byte{0x21}});
        der.insert(der.end(), n.begin(), n.end());
        der.insert(der.end(), {std::byte{0x02}, std::byte{0x01}, std::byte{0x01}});
        CHECK(decode_ecdsa_der(der).has_value());
    }
    SECTION("an integer wider than 256 bits") {
        std::vector<std::byte> der{std::byte{0x30}, std::byte{0x26}, std::byte{0x02},
                                   std::byte{0x21}};
        der.push_back(std::byte{0x01});
        der.insert(der.end(), 32, std::byte{0x00});
        der.insert(der.end(), {std::byte{0x02}, std::byte{0x01}, std::byte{0x01}});
        CHECK_FALSE(decode_ecdsa_der(der).has_value());
    }
    SECTION("bytes after the sequence") {
        CHECK_FALSE(decode_ecdsa_der(bytes({0x30, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x01, 0x00}))
                        .has_value());
    }
    SECTION("bytes inside the sequence after s") {
        CHECK_FALSE(decode_ecdsa_der(bytes({0x30, 0x07, 0x02, 0x01, 0x01, 0x02, 0x01, 0x01, 0x00}))
                        .has_value());
    }
    SECTION("a sequence length that disagrees with the contents") {
        CHECK_FALSE(
            decode_ecdsa_der(bytes({0x30, 0x05, 0x02, 0x01, 0x01, 0x02, 0x01, 0x01})).has_value());
        CHECK_FALSE(
            decode_ecdsa_der(bytes({0x30, 0x07, 0x02, 0x01, 0x01, 0x02, 0x01, 0x01})).has_value());
    }
    SECTION("a third integer") {
        CHECK_FALSE(decode_ecdsa_der(
                        bytes({0x30, 0x09, 0x02, 0x01, 0x01, 0x02, 0x01, 0x01, 0x02, 0x01, 0x01}))
                        .has_value());
    }
    SECTION("a multi-byte tag") {
        CHECK_FALSE(decode_ecdsa_der(bytes({0x3F, 0x00, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x01}))
                        .has_value());
    }
}

TEST_CASE("every truncation of a real signature is refused", "[crypto][der]") {
    const EcdsaSignature s{order_minus_one(), order_minus_one()};
    const auto der = encode_ecdsa_der(s);
    REQUIRE(decode_ecdsa_der(der).has_value());
    for (std::size_t length = 0; length < der.size(); ++length) {
        CHECK_FALSE(decode_ecdsa_der(std::span{der}.first(length)).has_value());
    }
}

TEST_CASE("the decoder accepts exactly one encoding of each (r, s)", "[crypto][der]") {
    // For every single-byte change of a valid signature: either it is refused, or what it decodes
    // to encodes back to the changed bytes. A decoder that accepted a second spelling of the same
    // value would fail the second branch.
    EcdsaSignature s{order_minus_one(), scalar(0x42, 0x11)};
    const auto der = encode_ecdsa_der(s);
    std::size_t accepted = 0;
    for (std::size_t i = 0; i < der.size(); ++i) {
        for (unsigned value = 0; value < 256; ++value) {
            auto mutated = der;
            mutated[i] = static_cast<std::byte>(value);
            const auto decoded = decode_ecdsa_der(mutated);
            if (decoded.has_value()) {
                ++accepted;
                CHECK(encode_ecdsa_der(*decoded) == mutated);
            }
        }
    }
    // The original bytes are accepted once per position, and a few changes of a value byte give
    // another valid signature; the point is that the second check above held for all of them.
    CHECK(accepted >= der.size());
}
