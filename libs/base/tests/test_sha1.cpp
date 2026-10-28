// SHA-1 is in the tree for one purpose, the certificate fingerprints the AC-4 content signing
// specification publishes (libs/base/include/iclforge/base/crypto/sha1.hpp). These are FIPS 180-4's
// own examples and the three NIST gives in the SHA-1 test vectors.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/base/crypto/sha1.hpp"

namespace {

std::string hex_of(const std::array<std::byte, 20>& digest) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (const std::byte b : digest) {
        out.push_back(kDigits[std::to_integer<unsigned>(b) >> 4]);
        out.push_back(kDigits[std::to_integer<unsigned>(b) & 0xF]);
    }
    return out;
}

std::vector<std::byte> bytes(std::string_view text) {
    std::vector<std::byte> out;
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    return out;
}

}  // namespace

TEST_CASE("SHA-1 known answers", "[crypto][sha1]") {
    using iclforge::base::crypto::sha1;
    CHECK(hex_of(sha1(bytes(""))) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(hex_of(sha1(bytes("abc"))) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(hex_of(sha1(bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))) ==
          "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    CHECK(hex_of(sha1(bytes(std::string(1000000, 'a')))) ==
          "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

TEST_CASE("SHA-1 padding at the block boundaries", "[crypto][sha1]") {
    // Lengths 55, 56, 63, 64 and 65 are where the length field does and does not fit in the last
    // block. The expected values are Python's hashlib.sha1 of N letters 'a'.
    using iclforge::base::crypto::sha1;
    CHECK(hex_of(sha1(bytes(std::string(55, 'a')))) == "c1c8bbdc22796e28c0e15163d20899b65621d65a");
    CHECK(hex_of(sha1(bytes(std::string(56, 'a')))) == "c2db330f6083854c99d4b5bfb6e8f29f201be699");
    CHECK(hex_of(sha1(bytes(std::string(63, 'a')))) == "03f09f5b158a7a8cdad920bddc29b81c18a551f5");
    CHECK(hex_of(sha1(bytes(std::string(64, 'a')))) == "0098ba824b5c16427bd7a1122a5a442a25ec644d");
    CHECK(hex_of(sha1(bytes(std::string(65, 'a')))) == "11655326c708d70319be2610e8a57d9a5b959d3b");
}
