#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "platform/process.hpp"

#include "iclforge/base/crypto/signing_key.hpp"

// Internal crypto headers - on the include path for this target only (see
// libs/base/tests/CMakeLists.txt), the same way the alsa backend's internal header is.
#include "iclforge/base/crypto/hmac_sha256.hpp"
#include "iclforge/base/crypto/sha256.hpp"
#include "iclforge/base/detail/sha256_impl.hpp"

namespace {

// See apps/forge/cli/tests/test_cli.cpp's own scratch_dir comment for why the
// TEST_CASE below folds this into its scratch leaf, on top of
// ICLFORGE_TEST_SCRATCH_DIR's build-tree rooting.
std::string scratch_pid_suffix() { return iclforge::test::platform::process_id(); }

std::span<const std::byte> as_bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

std::string to_hex(std::span<const std::byte> b) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(b.size() * 2);
    for (const std::byte x : b) {
        const auto v = std::to_integer<unsigned>(x);
        out.push_back(kDigits[v >> 4]);
        out.push_back(kDigits[v & 0xF]);
    }
    return out;
}

// A fixed pseudo-random sequence for message and state material: the same bytes every run, so a
// failure names the same input every time.
struct Lcg {
    std::uint32_t state = 0x9e3779b9U;
    std::uint32_t next() {
        state = state * 1664525U + 1013904223U;
        return state;
    }
    std::uint8_t byte() { return static_cast<std::uint8_t>(next() >> 24); }
};

constexpr std::array<std::uint32_t, 8> kSha256Initial = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                                         0xa54ff53a, 0x510e527f, 0x9b05688c,
                                                         0x1f83d9ab, 0x5be0cd19};

// SHA-256 of `message` from the portable compression function and FIPS 180-4 section 5.1.1's
// padding written out here, so it shares no code with Sha256's update() and finish() or with the
// hardware kernel: the oracle the dispatching hash is held to.
std::array<std::byte, 32> reference_sha256(std::span<const std::uint8_t> message) {
    std::vector<std::uint8_t> framed(message.begin(), message.end());
    framed.push_back(0x80);
    while (framed.size() % 64 != 56) {
        framed.push_back(0);
    }
    const std::uint64_t bits = static_cast<std::uint64_t>(message.size()) * 8;
    for (int shift = 56; shift >= 0; shift -= 8) {
        framed.push_back(static_cast<std::uint8_t>(bits >> shift));
    }

    std::array<std::uint32_t, 8> h = kSha256Initial;
    iclforge::internal::crypto::sha256_compress_portable(h.data(), framed.data(),
                                                         framed.size() / 64);
    std::array<std::byte, 32> digest{};
    for (std::size_t i = 0; i < 8; ++i) {
        for (std::size_t j = 0; j < 4; ++j) {
            digest[i * 4 + j] = static_cast<std::byte>(h[i] >> (24 - 8 * j));
        }
    }
    return digest;
}

}  // namespace

// --- Crypto known-answer vectors (FIPS 180-4 / RFC 4231) -------------------
// These lock the primitives against a spec, so a future refactor of the
// self-contained implementation can't silently change the tag it produces.
TEST_CASE("SHA-256 matches FIPS test vectors", "[signing][sha256]") {
    CHECK(to_hex(iclforge::base::crypto::sha256(as_bytes("abc"))) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(to_hex(iclforge::base::crypto::sha256(as_bytes(""))) ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    // A 56-byte message, exercising the pad-into-a-second-block boundary.
    CHECK(to_hex(iclforge::base::crypto::sha256(
              as_bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // FIPS 180-4's long message, a million 'a': 15,625 blocks through the multi-block path.
    const std::string million(1000000, 'a');
    CHECK(to_hex(iclforge::base::crypto::sha256(as_bytes(million))) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

// The CPU's SHA instructions are checked against the code that needs none. Skipped on a build or a
// CPU without a kernel - which the portable-vs-reference case below still covers.
TEST_CASE("SHA-256 hardware compression equals the portable one", "[signing][sha256][hardware]") {
    if (!iclforge::internal::crypto::sha256_hardware_supported()) {
        SKIP("no SHA-256 hardware kernel in this build, or this CPU lacks the instructions");
    }
    Lcg rng;
    for (int trial = 0; trial < 2000; ++trial) {
        const std::size_t blocks = 1 + static_cast<std::size_t>(trial % 9);
        std::vector<std::uint8_t> data(blocks * 64);
        std::ranges::generate(data, [&] { return rng.byte(); });
        std::array<std::uint32_t, 8> portable{};
        std::ranges::generate(portable, [&] { return rng.next(); });
        std::array<std::uint32_t, 8> hardware = portable;

        iclforge::internal::crypto::sha256_compress_portable(portable.data(), data.data(), blocks);
        iclforge::internal::crypto::sha256_compress_hardware(hardware.data(), data.data(), blocks);

        CAPTURE(trial, blocks);
        REQUIRE(hardware == portable);
    }
}

// Sha256 itself - whichever compression function it dispatches to - at every length across the
// padding boundaries (55, 56, 63, 64, 119, 120, 128 ...) and for any way of splitting the input.
TEST_CASE("SHA-256 matches an independent framing at every length", "[signing][sha256]") {
    Lcg rng;
    for (std::size_t length = 0; length <= 600; ++length) {
        std::vector<std::uint8_t> message(length);
        std::ranges::generate(message, [&] { return rng.byte(); });
        const std::array<std::byte, 32> expected = reference_sha256(message);
        const std::span<const std::byte> bytes = std::as_bytes(std::span(message));

        CAPTURE(length);
        CHECK(iclforge::base::crypto::sha256(bytes) == expected);

        // Three updates that cut the message at awkward places, including mid-block.
        iclforge::base::crypto::Sha256 split;
        const std::size_t first = length / 3;
        const std::size_t second = length / 3;
        split.update(bytes.first(first));
        split.update(bytes.subspan(first, second));
        split.update(bytes.subspan(first + second));
        std::array<std::byte, 32> split_digest{};
        split.finish(split_digest);
        CHECK(split_digest == expected);
    }
}

TEST_CASE("Sha256 is reusable after finish", "[signing][sha256]") {
    iclforge::base::crypto::Sha256 hash;
    std::array<std::byte, 32> digest{};
    hash.update(as_bytes("a message that is not the next one"));
    hash.finish(digest);
    hash.update(as_bytes("abc"));
    hash.finish(digest);
    CHECK(to_hex(digest) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("HMAC-SHA-256 matches RFC 4231 test vectors", "[signing][hmac]") {
    SECTION("test case 1") {
        const std::vector<std::byte> key(20, std::byte{0x0b});
        CHECK(to_hex(iclforge::base::crypto::hmac_sha256(key, as_bytes("Hi There"))) ==
              "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    }
    SECTION("test case 2") {
        CHECK(to_hex(iclforge::base::crypto::hmac_sha256(as_bytes("Jefe"),
                                               as_bytes("what do ya want for nothing?"))) ==
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    }
    SECTION("test case 6 - key longer than the block size") {
        const std::vector<std::byte> key(131, std::byte{0xaa});
        CHECK(to_hex(iclforge::base::crypto::hmac_sha256(
                  key, as_bytes("Test Using Larger Than Block-Size Key - Hash Key First"))) ==
              "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
    }
}

// --- Runtime key loading ----------------------------------------------------
TEST_CASE("load_signing_key reads a key file", "[signing][key]") {
    namespace fs = std::filesystem;
    // ICLFORGE_TEST_SCRATCH_DIR rather than fs::temp_directory_path(), for the
    // reason apps/forge/cli/tests/test_cli.cpp's own scratch_dir explains - the key
    // filenames below are fixed, so a machine-global directory is one two
    // concurrently running iclforge-tests binaries would collide in. The leaf also
    // carries this process's own PID, since ICLFORGE_TEST_SCRATCH_DIR's
    // build-tree rooting alone does not separate two such binaries pointed at
    // the same build tree.
    const fs::path dir = fs::path{ICLFORGE_TEST_SCRATCH_DIR} / ("signing_" + scratch_pid_suffix());
    fs::create_directories(dir);

    SECTION("base64 contents decode to the raw key, whitespace ignored") {
        const fs::path p = dir / "iclforge_test_key_b64.txt";
        {
            std::ofstream out{p};
            out << "AAECA/8=\n";  // base64 of {00,01,02,03,ff}
        }
        const auto key = iclforge::base::crypto::load_signing_key(p.string());
        REQUIRE(key.has_value());
        REQUIRE(key->bytes().size() == 5);
        CHECK(std::to_integer<int>(key->bytes()[0]) == 0x00);
        CHECK(std::to_integer<int>(key->bytes()[4]) == 0xff);
        fs::remove(p);
    }

    SECTION("non-base64 contents are taken as raw bytes") {
        const fs::path p = dir / "iclforge_test_key_raw.bin";
        {
            std::ofstream out{p, std::ios::binary};
            // 5 bytes: length not a multiple of 4 and '!'/0x01 aren't base64,
            // so this is unambiguously raw.
            const char raw[] = {'k', 'e', 'y', '!', '\x01'};
            out.write(raw, sizeof raw);
        }
        const auto key = iclforge::base::crypto::load_signing_key(p.string());
        REQUIRE(key.has_value());
        CHECK(key->bytes().size() == 5);
        fs::remove(p);
    }

    SECTION("a C-array hex export decodes to the raw key") {
        const fs::path p = dir / "iclforge_test_key_hexarray.txt";
        {
            std::ofstream out{p};
            out << "0x00, 0x01, 0x02, 0x03, 0xff\n";
        }
        const auto key = iclforge::base::crypto::load_signing_key(p.string());
        REQUIRE(key.has_value());
        REQUIRE(key->bytes().size() == 5);
        CHECK(std::to_integer<int>(key->bytes()[0]) == 0x00);
        CHECK(std::to_integer<int>(key->bytes()[4]) == 0xff);
        fs::remove(p);
    }

    SECTION("a botched hex export is rejected rather than signed with the wrong bytes") {
        const fs::path p = dir / "iclforge_test_key_botched.txt";
        {
            // Missing every "0x" prefix - exactly the shape a hand-edited or
            // half-converted export can end up in. All-hex-digit-and-comma,
            // so it must not silently fall through to "raw key bytes".
            std::ofstream out{p};
            out << "56, 6c, ef, 66\n";
        }
        const auto key = iclforge::base::crypto::load_signing_key(p.string());
        REQUIRE_FALSE(key.has_value());
        CHECK(key.error().kind == iclforge::base::crypto::KeyErrorKind::kMalformed);
        fs::remove(p);
    }

    SECTION("a missing path is an error, not an absent key") {
        const auto key = iclforge::base::crypto::load_signing_key(
            (dir / "definitely_not_here_iclforge.key").string());
        REQUIRE_FALSE(key.has_value());
        CHECK(key.error().kind == iclforge::base::crypto::KeyErrorKind::kUnreadable);
    }

    SECTION("an empty file resolves but yields no key") {
        const fs::path p = dir / "iclforge_test_key_empty.txt";
        { std::ofstream out{p}; }
        const auto key = iclforge::base::crypto::load_signing_key(p.string());
        REQUIRE_FALSE(key.has_value());
        CHECK(key.error().kind == iclforge::base::crypto::KeyErrorKind::kEmpty);
        fs::remove(p);
    }
}

TEST_CASE("decode_signing_key accepts base64 or raw, and they agree", "[signing][key]") {
    const std::vector<std::byte> expected = {std::byte{0x00}, std::byte{0x01}, std::byte{0x02},
                                             std::byte{0x03}, std::byte{0xff}};

    SECTION("base64 decodes to the key bytes") {
        const std::string b64 = "AAECA/8=";
        const auto key = iclforge::base::crypto::decode_signing_key(as_bytes(b64));
        REQUIRE(key.has_value());
        REQUIRE(key->bytes().size() == expected.size());
        CHECK(std::equal(key->bytes().begin(), key->bytes().end(), expected.begin()));
    }

    SECTION("a raw binary key (non-base64 bytes) is taken verbatim") {
        // 32 bytes of 0xAB - 0xAB is not in the base64 alphabet, so unambiguous.
        const std::vector<std::byte> raw(32, std::byte{0xAB});
        const auto key = iclforge::base::crypto::decode_signing_key(raw);
        REQUIRE(key.has_value());
        REQUIRE(key->bytes().size() == 32);
        CHECK(std::to_integer<int>(key->bytes()[0]) == 0xAB);
    }

    SECTION("the base64 form and the raw form of one key produce the same key") {
        // Raw 32-byte key that contains a non-base64 byte so the raw form can't
        // be misread as base64; its base64 encoding must decode back to it.
        std::vector<std::byte> raw(32, std::byte{0xAB});
        raw[7] = std::byte{0x00};
        // Encode raw -> base64 here rather than hardcode, so the test can't drift.
        static constexpr char kAlphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string enc;
        for (std::size_t i = 0; i < raw.size(); i += 3) {
            const unsigned b0 = std::to_integer<unsigned>(raw[i]);
            const unsigned b1 = i + 1 < raw.size() ? std::to_integer<unsigned>(raw[i + 1]) : 0;
            const unsigned b2 = i + 2 < raw.size() ? std::to_integer<unsigned>(raw[i + 2]) : 0;
            enc.push_back(kAlphabet[b0 >> 2]);
            enc.push_back(kAlphabet[((b0 & 0x3) << 4) | (b1 >> 4)]);
            enc.push_back(i + 1 < raw.size() ? kAlphabet[((b1 & 0xf) << 2) | (b2 >> 6)] : '=');
            enc.push_back(i + 2 < raw.size() ? kAlphabet[b2 & 0x3f] : '=');
        }
        const auto from_raw = iclforge::base::crypto::decode_signing_key(raw);
        const auto from_b64 = iclforge::base::crypto::decode_signing_key(as_bytes(enc));
        REQUIRE(from_raw.has_value());
        REQUIRE(from_b64.has_value());
        CHECK(std::equal(from_raw->bytes().begin(), from_raw->bytes().end(),
                         from_b64->bytes().begin(), from_b64->bytes().end()));
    }

    SECTION("empty content yields no key") {
        CHECK_FALSE(iclforge::base::crypto::decode_signing_key({}).has_value());
    }

    SECTION("a comma-separated 0xHH byte array decodes to the key bytes") {
        const std::string arr = "0x00, 0x01, 0x02, 0x03, 0xff";
        const auto key = iclforge::base::crypto::decode_signing_key(as_bytes(arr));
        REQUIRE(key.has_value());
        REQUIRE(key->bytes().size() == expected.size());
        CHECK(std::equal(key->bytes().begin(), key->bytes().end(), expected.begin()));
    }

    SECTION("a hex array without whitespace between tokens still decodes") {
        const std::string arr = "0x00,0x01,0x02,0x03,0xff";
        const auto key = iclforge::base::crypto::decode_signing_key(as_bytes(arr));
        REQUIRE(key.has_value());
        REQUIRE(key->bytes().size() == expected.size());
        CHECK(std::equal(key->bytes().begin(), key->bytes().end(), expected.begin()));
    }

    SECTION("hex-shaped text that isn't a valid array is refused, not taken as raw bytes") {
        // No "0x" prefixes: all hex digits and commas, so it must be refused
        // rather than silently treated as five bytes of ASCII '5','6',',', etc.
        CHECK_FALSE(iclforge::base::crypto::decode_signing_key(as_bytes("56, 6c, ef, 66")).has_value());
        // A truncated array (odd trailing nibble) is equally refused.
        CHECK_FALSE(iclforge::base::crypto::decode_signing_key(as_bytes("0x00, 0x01, 0x0")).has_value());
    }
}
