// forge's AC-4 content signing (ac4-sign, ac4-verify, sign-content, verify-content, check-content
// and their key, chain and trust options), each run against the real binary on a certificate chain
// the test builds from fixed labels (tests/support/test_pki.hpp: public keys that protect nothing).
// The signing and verifying themselves are tested in libs/base and libs/ac4; what is checked here
// is what the command line does with them: that nothing signs or checks unless asked, that the
// stream's frames are untouched, which exit code each outcome has, and what it refuses.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

#include "platform/process.hpp"
#include "test_pki.hpp"

#include "iclforge/ac3/io/wav.hpp"
#include "iclforge/ac4/io/content_signature.hpp"
#include "iclforge/ac4/io/elementary.hpp"
#include "iclforge/base/crypto/pem.hpp"
#include "iclforge/base/crypto/sha256.hpp"
#include "iclforge/base/crypto/x509.hpp"

namespace fs = std::filesystem;
namespace pki = iclforge::test::pki;

namespace {

fs::path scratch_dir() {
    auto dir = fs::path{ICLFORGE_TEST_SCRATCH_DIR} /
               ("cli_content_signature_" + iclforge::test::platform::process_id());
    fs::create_directories(dir);
    return dir;
}

int run_cli(const std::string& args, const fs::path& log) {
    const std::string command =
        "\"" + std::string(ICLFORGE_CLI_EXE) + "\" " + args + " > \"" + log.string() + "\" 2>&1";
    return iclforge::test::platform::run_shell(command);
}

std::string read_log(const fs::path& log) {
    std::ifstream in{log, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

std::string quoted(const fs::path& path) {
    return "\"" + path.string() + "\"";
}

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

std::vector<std::byte> read_bytes(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    REQUIRE(in.is_open());
    const std::vector<char> raw{std::istreambuf_iterator<char>{in},
                                std::istreambuf_iterator<char>{}};
    std::vector<std::byte> out(raw.size());
    std::ranges::transform(raw, out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}

void write_bytes(const fs::path& path, std::span<const std::byte> bytes) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    REQUIRE(out.is_open());
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

void write_text(const fs::path& path, const std::string& text) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    REQUIRE(out.is_open());
    out << text;
}

constexpr int kRate = 48000;
constexpr int kExitUsage = 1;
constexpr int kExitInput = 2;
constexpr int kQcGate = 6;

// Roughly a quarter of a second of stereo tone: a handful of AC-4 frames, which is all a test of
// the trailer needs.
fs::path stereo_wav(const std::string& name) {
    std::vector<std::vector<float>> channels(2, std::vector<float>(12288));
    for (std::size_t c = 0; c < channels.size(); ++c) {
        for (std::size_t n = 0; n < channels[c].size(); ++n) {
            channels[c][n] = static_cast<float>(
                0.1 * std::sin(2.0 * std::numbers::pi * (331.0 + 157.0 * static_cast<double>(c)) *
                               static_cast<double>(n) / kRate));
        }
    }
    const fs::path path = scratch_dir() / name;
    REQUIRE(iclforge::ac3::io::write_wav_f32(path.string(), channels, kRate).has_value());
    return path;
}

// The test chain on disk the ways an operator would have it: the signer's key as a bare scalar,
// the chain as one PEM bundle, the root alone as the anchor. The chain's second certificate expired
// on 2025-11-13, as the one in CS spec v2.3's example did.
struct Files {
    fs::path key;
    fs::path chain;
    fs::path anchor;
    fs::path other_anchor;
};

Files write_credentials() {
    const fs::path dir = scratch_dir();
    const pki::TestChain chain = pki::make_chain();
    Files files{dir / "signer.key", dir / "chain.pem", dir / "root.pem", dir / "other_root.pem"};
    write_bytes(files.key, chain.leaf_key.scalar());
    std::string bundle;
    for (const pki::Bytes& der : chain.certificates) {
        bundle += iclforge::base::crypto::pem_encode("CERTIFICATE", der);
    }
    write_text(files.chain, bundle);
    write_text(files.anchor,
               iclforge::base::crypto::pem_encode("CERTIFICATE", chain.certificates[0]));
    // A certificate that is not in the chain: trusting it must not anchor anything.
    const auto stranger_key = pki::key_for("icl-forge-test-stranger");
    pki::CertSpec stranger;
    stranger.subject = pki::Name{"ICL Forge Test Stranger CA", std::nullopt, "T0099"};
    stranger.serial_hex = "0A0099";
    stranger.not_before = "2010-01-01T00:00:00";
    stranger.not_after = "2040-01-01T00:00:00";
    stranger.ca = true;
    stranger.key_usage = (1U << 0) | (1U << 5) | (1U << 6);
    write_text(files.other_anchor, iclforge::base::crypto::pem_encode(
                                       "CERTIFICATE", pki::issue(stranger, stranger_key,
                                                                 stranger.subject, stranger_key)));
    return files;
}

std::string key_chain_options(const Files& files) {
    return " content-key=" + quoted(files.key) + " content-chain=" + quoted(files.chain);
}

// The same credentials with the signer certificate issued again, to the same key, carrying `uuid`
// in a subject attribute. CS spec v2.6 section 5.3 says the publisher UUID is not in a signing
// certificate; a test that gives one another UUID shows that it is not held against the signature.
Files write_uuid_credentials(const std::string& uuid) {
    Files files = write_credentials();
    const pki::TestChain chain = pki::make_chain();
    pki::CertSpec leaf;
    leaf.subject = pki::Name{"ICL Forge Test Content Signer", "Code/Content Signer", uuid};
    leaf.serial_hex = "0A0004B5D62000000005";
    leaf.not_before = "2022-06-09T21:56:24";
    leaf.not_after = "2042-06-09T21:56:24";
    leaf.ca = true;
    leaf.key_usage = 1U << 0;
    const pki::Bytes reissued =
        pki::issue(leaf, chain.leaf_key, chain.names[2], pki::key_for("icl-forge-test-issuing"));
    std::string bundle;
    for (std::size_t i = 0; i < 3; ++i) {
        bundle += iclforge::base::crypto::pem_encode("CERTIFICATE", chain.certificates[i]);
    }
    bundle += iclforge::base::crypto::pem_encode("CERTIFICATE", reissued);
    files.chain = scratch_dir() / "uuid_chain.pem";
    write_text(files.chain, bundle);
    return files;
}

constexpr std::string_view kPublisherUuid = "3B9C1F52-7A4E-4D08-B6A1-2F0E9D7C5A84";
constexpr std::string_view kOtherUuid = "0F0E0D0C-0B0A-4908-8706-050403020100";

// An unsigned AC-4 stream, made by the encoder.
fs::path plain_stream(const std::string& name) {
    const fs::path in = stereo_wav(name + ".wav");
    const fs::path out = scratch_dir() / (name + ".ac4");
    REQUIRE(run_cli("ac4-encode " + quoted(in) + " " + quoted(out) + " 192",
                    scratch_dir() / (name + ".log")) == 0);
    return out;
}

// A date every certificate in the chain was valid on, and one the second was not.
constexpr std::string_view kWhenAllValid = "2025-06-01T00:00:00";

}  // namespace

TEST_CASE("ac4-sign leaves the frames alone and ac4-verify accepts the result against its root",
          "[cli][ac4][content-signature]") {
    const auto dir = scratch_dir();
    const auto log = dir / "sign_verify.log";
    const Files files = write_credentials();
    const fs::path plain = plain_stream("sign_verify_plain");
    const fs::path signed_file = dir / "sign_verify_signed.ac4";

    REQUIRE(run_cli("ac4-sign " + quoted(plain) + " " + quoted(signed_file) +
                        key_chain_options(files) + " signed-date=" + std::string{kWhenAllValid},
                    log) == 0);
    CHECK(contains(read_log(log), "content signature"));

    // The signed file is the unsigned one with a trailer on the end, and nothing else.
    const std::vector<std::byte> before = read_bytes(plain);
    const std::vector<std::byte> after = read_bytes(signed_file);
    REQUIRE(after.size() > before.size());
    CHECK(std::equal(before.begin(), before.end(), after.begin()));
    const auto trailer = iclforge::ac4::locate_signature_trailer(after);
    REQUIRE(trailer.has_value());
    CHECK(trailer->present);
    CHECK(trailer->content_size == before.size());

    // Valid against the root, judged at the date it was signed.
    REQUIRE(run_cli("ac4-verify " + quoted(signed_file) + " trust-anchor=" + quoted(files.anchor) +
                        " verify-time=signed",
                    log) == 0);
    const std::string report = read_log(log);
    CHECK(contains(report, ": valid"));
    CHECK(contains(report, "anchored by"));
}

TEST_CASE("ac4-sign says whether a stream already has a signature or has junk after its frames",
          "[cli][ac4][content-signature]") {
    const auto dir = scratch_dir();
    const auto log = dir / "resign.log";
    const Files files = write_credentials();
    const fs::path plain = plain_stream("resign_plain");
    const fs::path signed_file = dir / "resign_signed.ac4";
    REQUIRE(run_cli("ac4-sign " + quoted(plain) + " " + quoted(signed_file) +
                        key_chain_options(files) + " signed-date=" + std::string{kWhenAllValid},
                    log) == 0);

    CHECK(run_cli("ac4-sign " + quoted(signed_file) + " " + quoted(dir / "resign_twice.ac4") +
                      key_chain_options(files) + " signed-date=" + std::string{kWhenAllValid},
                  log) == kExitUsage);
    CHECK(contains(read_log(log), "already carries a content signature"));

    std::vector<std::byte> junk = read_bytes(plain);
    junk.insert(junk.end(), 30, std::byte{0x55});
    const fs::path junk_file = dir / "resign_junk.ac4";
    write_bytes(junk_file, junk);
    CHECK(run_cli("ac4-sign " + quoted(junk_file) + " " + quoted(dir / "resign_junk_out.ac4") +
                      key_chain_options(files) + " signed-date=" + std::string{kWhenAllValid},
                  log) == kExitInput);
    const std::string text = read_log(log);
    CHECK(contains(text, "the 30 byte(s) after it are not a content signature"));
    CHECK_FALSE(contains(text, "already"));
}

TEST_CASE("ac4-verify does not call a signature valid that no trust anchor vouches for",
          "[cli][ac4][content-signature]") {
    const auto dir = scratch_dir();
    const auto log = dir / "unanchored.log";
    const Files files = write_credentials();
    const fs::path signed_file = dir / "unanchored_signed.ac4";
    REQUIRE(run_cli("ac4-sign " + quoted(plain_stream("unanchored_plain")) + " " +
                        quoted(signed_file) + key_chain_options(files) +
                        " signed-date=" + std::string{kWhenAllValid},
                    log) == 0);
    const std::string common = "ac4-verify " + quoted(signed_file) + " verify-time=signed";

    SECTION("no anchor: refused, and said to be unanchored") {
        CHECK(run_cli(common, log) == kQcGate);
        const std::string text = read_log(log);
        CHECK(contains(text, "chain unanchored"));
        CHECK_FALSE(contains(text, ": valid"));
    }
    SECTION("an anchor that is not in the chain does not help") {
        CHECK(run_cli(common + " trust-anchor=" + quoted(files.other_anchor), log) == kQcGate);
    }
    SECTION("unanchored-ok accepts it, still saying what it is") {
        CHECK(run_cli(common + " unanchored-ok", log) == 0);
        const std::string text = read_log(log);
        CHECK(contains(text, "chain unanchored"));
        CHECK_FALSE(contains(text, ": valid"));
    }
    SECTION("check-content reports and exits 0") {
        CHECK(run_cli(common + " check-content", log) == 0);
        CHECK(contains(read_log(log), "chain unanchored"));
    }
    SECTION("a pin on the root's fingerprint anchors it") {
        const pki::TestChain chain = pki::make_chain();
        const auto digest = iclforge::base::crypto::sha256(chain.certificates[0]);
        std::string pin;
        for (const std::byte b : digest) {
            constexpr std::string_view kHex = "0123456789abcdef";
            pin += kHex[std::to_integer<unsigned>(b) >> 4U];
            pin += kHex[std::to_integer<unsigned>(b) & 0xFU];
        }
        CHECK(run_cli(common + " pin=" + pin, log) == 0);
        CHECK(contains(read_log(log), ": valid"));
    }
}

TEST_CASE("ac4-verify judges the certificates now unless told otherwise",
          "[cli][ac4][content-signature]") {
    const auto dir = scratch_dir();
    const auto log = dir / "verify_time.log";
    const Files files = write_credentials();
    const fs::path signed_file = dir / "verify_time_signed.ac4";
    REQUIRE(run_cli("ac4-sign " + quoted(plain_stream("verify_time_plain")) + " " +
                        quoted(signed_file) + key_chain_options(files) +
                        " signed-date=" + std::string{kWhenAllValid},
                    log) == 0);
    const std::string common =
        "ac4-verify " + quoted(signed_file) + " trust-anchor=" + quoted(files.anchor);

    // The chain's second certificate has expired, as the example chain's did: a genuine
    // signature is refused when it is checked, and says why.
    CHECK(run_cli(common, log) == kQcGate);
    CHECK(contains(read_log(log), "expired"));
    CHECK(run_cli(common + " verify-time=signed", log) == 0);
    CHECK(run_cli(common + " verify-time=2025-12-01T00:00:00", log) == kQcGate);
    CHECK(run_cli(common + " verify-time=2025-07-01T00:00:00", log) == 0);
    CHECK(run_cli(common + " verify-time=ignore", log) == 0);
}

TEST_CASE("ac4-verify refuses a stream changed after it was signed, and one never signed",
          "[cli][ac4][content-signature]") {
    const auto dir = scratch_dir();
    const auto log = dir / "tamper.log";
    const Files files = write_credentials();
    const fs::path plain = plain_stream("tamper_plain");
    const fs::path signed_file = dir / "tamper_signed.ac4";
    REQUIRE(run_cli("ac4-sign " + quoted(plain) + " " + quoted(signed_file) +
                        key_chain_options(files) + " signed-date=" + std::string{kWhenAllValid},
                    log) == 0);
    const std::string verify = " trust-anchor=" + quoted(files.anchor) + " verify-time=signed";

    SECTION("a byte of audio") {
        std::vector<std::byte> bytes = read_bytes(signed_file);
        bytes[bytes.size() / 3] ^= std::byte{0x01};
        const fs::path damaged = dir / "tamper_audio.ac4";
        write_bytes(damaged, bytes);
        CHECK(run_cli("ac4-verify " + quoted(damaged) + verify, log) == kQcGate);
        CHECK(contains(read_log(log), "content digest"));
    }
    SECTION("a byte of the signature") {
        std::vector<std::byte> bytes = read_bytes(signed_file);
        bytes[bytes.size() - 20] ^= std::byte{0x01};
        const fs::path damaged = dir / "tamper_signature.ac4";
        write_bytes(damaged, bytes);
        CHECK(run_cli("ac4-verify " + quoted(damaged) + verify, log) == kQcGate);
        CHECK(contains(read_log(log), "INVALID"));
    }
    SECTION("a stream that carries no signature") {
        CHECK(run_cli("ac4-verify " + quoted(plain) + verify, log) == kQcGate);
        CHECK(contains(read_log(log), "no content signature"));
    }
    SECTION("check-content reports the failure and exits 0") {
        CHECK(run_cli("ac4-verify " + quoted(plain) + verify + " check-content", log) == 0);
        CHECK(contains(read_log(log), "INVALID"));
    }
}

TEST_CASE("ac4-verify requires the signing certificate to carry the title unless told not to",
          "[cli][ac4][content-signature]") {
    const auto dir = scratch_dir();
    const auto log = dir / "title.log";
    const Files files = write_credentials();
    const fs::path signed_file = dir / "title_signed.ac4";
    REQUIRE(run_cli("ac4-sign " + quoted(plain_stream("title_plain")) + " " + quoted(signed_file) +
                        key_chain_options(files) + " signed-date=" + std::string{kWhenAllValid},
                    log) == 0);
    const std::string common = "ac4-verify " + quoted(signed_file) +
                               " trust-anchor=" + quoted(files.anchor) + " verify-time=signed";
    CHECK(run_cli(common, log) == 0);
    CHECK(run_cli(common + " leaf-title=\"Release Signer\"", log) == kQcGate);
    CHECK(contains(read_log(log), "Release Signer"));
    CHECK(run_cli(common + " leaf-title=any", log) == 0);
}

TEST_CASE("decode checks a content signature before it plays anything, when asked",
          "[cli][ac4][content-signature]") {
    const auto dir = scratch_dir();
    const auto log = dir / "decode.log";
    const Files files = write_credentials();
    const fs::path plain = plain_stream("decode_plain");
    const fs::path signed_file = dir / "decode_signed.ac4";
    // Signed with a publisher UUID, which the strict check (verify-content) asks of a signature.
    REQUIRE(run_cli("ac4-sign " + quoted(plain) + " " + quoted(signed_file) +
                        key_chain_options(files) + " signed-date=" + std::string{kWhenAllValid} +
                        " publisher-uuid=" + std::string{kPublisherUuid},
                    log) == 0);

    // A signed file decodes as the unsigned one, with no verification asked for and no warning of
    // a lost sync.
    const fs::path from_plain = dir / "decode_plain.wav";
    const fs::path from_signed = dir / "decode_signed.wav";
    REQUIRE(run_cli("decode " + quoted(plain) + " " + quoted(from_plain), log) == 0);
    REQUIRE(run_cli("decode " + quoted(signed_file) + " " + quoted(from_signed), log) == 0);
    const std::string quiet = read_log(log);
    CHECK_FALSE(contains(quiet, "warning"));
    CHECK_FALSE(contains(quiet, "INVALID"));
    CHECK(read_bytes(from_plain) == read_bytes(from_signed));

    const std::string verify =
        " verify-content trust-anchor=" + quoted(files.anchor) + " verify-time=signed";
    SECTION("a valid, anchored signature plays") {
        const fs::path out = dir / "decode_verified.wav";
        REQUIRE(run_cli("decode " + quoted(signed_file) + " " + quoted(out) + verify, log) == 0);
        CHECK(contains(read_log(log), ": valid"));
        CHECK(read_bytes(out) == read_bytes(from_plain));
    }
    SECTION("a stream changed after signing is refused and writes nothing") {
        std::vector<std::byte> bytes = read_bytes(signed_file);
        bytes[bytes.size() / 3] ^= std::byte{0x01};
        const fs::path damaged = dir / "decode_damaged.ac4";
        write_bytes(damaged, bytes);
        const fs::path out = dir / "decode_refused.wav";
        fs::remove(out);
        CHECK(run_cli("decode " + quoted(damaged) + " " + quoted(out) + verify, log) == kExitInput);
        CHECK(contains(read_log(log), "not acceptable"));
        CHECK_FALSE(fs::exists(out));
    }
    SECTION("an unsigned stream is refused") {
        const fs::path out = dir / "decode_unsigned.wav";
        fs::remove(out);
        CHECK(run_cli("decode " + quoted(plain) + " " + quoted(out) + verify, log) == kExitInput);
        CHECK_FALSE(fs::exists(out));
    }
    SECTION("an unanchored signature is refused, and unanchored-ok lets it play") {
        const fs::path out = dir / "decode_unanchored.wav";
        fs::remove(out);
        CHECK(run_cli("decode " + quoted(signed_file) + " " + quoted(out) +
                          " verify-content verify-time=signed",
                      log) == kExitInput);
        CHECK(contains(read_log(log), "unanchored-ok"));
        CHECK_FALSE(fs::exists(out));
        CHECK(run_cli("decode " + quoted(signed_file) + " " + quoted(out) +
                          " verify-content unanchored-ok verify-time=signed",
                      log) == 0);
        CHECK(fs::exists(out));
    }
    SECTION("check-content reports a bad signature and plays on") {
        const fs::path out = dir / "decode_reported.wav";
        CHECK(run_cli("decode " + quoted(plain) + " " + quoted(out) + " check-content", log) == 0);
        CHECK(contains(read_log(log), "INVALID"));
        CHECK(fs::exists(out));
    }
}

TEST_CASE("sign-content signs what an encoder writes, and finds a bad request before encoding",
          "[cli][ac4][content-signature]") {
    const auto dir = scratch_dir();
    const auto log = dir / "encode.log";
    const Files files = write_credentials();
    const fs::path in = stereo_wav("encode_in.wav");
    const std::string encode = "ac4-encode " + quoted(in) + " ";

    SECTION("the signed file verifies, and its frames are the unsigned encode's") {
        const fs::path signed_file = dir / "encode_signed.ac4";
        const fs::path unsigned_file = dir / "encode_unsigned.ac4";
        REQUIRE(run_cli(encode + quoted(signed_file) + " 192 sign-content" +
                            key_chain_options(files) + " signed-date=" + std::string{kWhenAllValid},
                        log) == 0);
        REQUIRE(run_cli(encode + quoted(unsigned_file) + " 192", log) == 0);
        const std::vector<std::byte> unsigned_bytes = read_bytes(unsigned_file);
        const std::vector<std::byte> signed_bytes = read_bytes(signed_file);
        REQUIRE(signed_bytes.size() > unsigned_bytes.size());
        CHECK(std::equal(unsigned_bytes.begin(), unsigned_bytes.end(), signed_bytes.begin()));
        CHECK(run_cli("ac4-verify " + quoted(signed_file) +
                          " trust-anchor=" + quoted(files.anchor) + " verify-time=signed",
                      log) == 0);
    }
    SECTION("without sign-content nothing is signed, key or no key") {
        const fs::path out = dir / "encode_plain.ac4";
        fs::remove(out);
        CHECK(run_cli(encode + quoted(out) + " 192" + key_chain_options(files), log) == kExitUsage);
        CHECK(contains(read_log(log), "unless sign-content asks"));
        CHECK_FALSE(fs::exists(out));
    }
    SECTION("no key: refused before anything is written") {
        const fs::path out = dir / "encode_nokey.ac4";
        fs::remove(out);
        CHECK(run_cli(encode + quoted(out) + " 192 sign-content", log) == kExitUsage);
        CHECK(contains(read_log(log), "needs a private key"));
        CHECK_FALSE(fs::exists(out));
    }
    SECTION(
        "a signed-date outside the certificates' validity: refused before anything is written") {
        const fs::path out = dir / "encode_baddate.ac4";
        fs::remove(out);
        CHECK(run_cli(encode + quoted(out) + " 192 sign-content" + key_chain_options(files) +
                          " signed-date=2001-01-01T00:00:00",
                      log) == kExitInput);
        CHECK(contains(read_log(log), "not valid before"));
        CHECK_FALSE(fs::exists(out));
    }
    SECTION(
        "an MP4 is out of scope (CS spec v2.6 section 8.3.2), and the message says what to do") {
        const fs::path out = dir / "encode_signed.mp4";
        fs::remove(out);
        CHECK(run_cli(encode + quoted(out) + " 192 sign-content" + key_chain_options(files), log) ==
              kExitUsage);
        const std::string text = read_log(log);
        CHECK(contains(text, "sign-content does not sign an MP4"));
        CHECK(contains(text, "out of scope"));
        CHECK(contains(text, "8.3.2"));
        CHECK(contains(text, "raw .ac4"));
        CHECK_FALSE(fs::exists(out));
    }
    SECTION("E-AC-3 output is not signed here") {
        const fs::path out = dir / "encode_atmos.ec3";
        fs::remove(out);
        CHECK(run_cli("atmos-encode " + quoted(in) + " " + quoted(out) + " 448 sign-content" +
                          key_chain_options(files),
                      log) == kExitUsage);
        CHECK(contains(read_log(log), "codec=ac4"));
        CHECK_FALSE(fs::exists(out));
    }
    SECTION("atmos-encode codec=ac4 signs") {
        const fs::path out = dir / "encode_atmos.ac4";
        REQUIRE(run_cli("atmos-encode " + quoted(in) + " " + quoted(out) +
                            " 448 codec=ac4 sign-content" + key_chain_options(files) +
                            " signed-date=" + std::string{kWhenAllValid},
                        log) == 0);
        CHECK(run_cli("ac4-verify " + quoted(out) + " trust-anchor=" + quoted(files.anchor) +
                          " verify-time=signed",
                      log) == 0);
    }
}

TEST_CASE("ac4-verify reports who signed, and verify-content makes the signer information count",
          "[cli][ac4][content-signature][v25]") {
    const auto dir = scratch_dir();
    const auto log = dir / "described.log";
    const Files files = write_credentials();
    const fs::path plain = plain_stream("described_plain");
    const std::string anchor = " trust-anchor=" + quoted(files.anchor);

    // CS spec v2.6 section 5.3: whom the signature names, taken from the options.
    const fs::path described = dir / "described_signed.ac4";
    REQUIRE(run_cli("ac4-sign " + quoted(plain) + " " + quoted(described) +
                        key_chain_options(files) + " signed-date=" + std::string{kWhenAllValid} +
                        " publisher-uuid=" + std::string{kPublisherUuid} +
                        " publisher-name=\"Example Publisher\" product-id=7"
                        " product-name=\"Example Product\" wrapper-name=\"Example Wrapper\""
                        " os-name=\"Example OS\" os-version=2.1 os-description=\"Example OS 2.1\"",
                    log) == 0);
    SECTION("ac4-verify says what the signature claims, and the findings on it") {
        REQUIRE(run_cli("ac4-verify " + quoted(described) + anchor + " verify-time=signed", log) ==
                0);
        const std::string text = read_log(log);
        CHECK(contains(text, "publisher   Example Publisher (UUID " + std::string{kPublisherUuid}));
        CHECK(contains(text, "product     Example Product (wrapper Example Wrapper)"));
        CHECK(contains(text, "platform    Example OS 2.1 (Example OS 2.1)"));
        CHECK(contains(text, "application ICL Forge forge"));
        CHECK(contains(text, "ok   signer nonce: 128 bytes"));
        // The signing certificate does not carry the UUID (CS spec v2.6 section 5.3), so with no
        // expected account it is reported and compared with nothing.
        CHECK(contains(text, "ok   publisher UUID"));
        CHECK(contains(text, "no expected publisher was given"));
    }
    SECTION("verify-content accepts a well formed UUID when no account is expected") {
        CHECK(run_cli(
                  "ac4-verify " + quoted(described) + anchor + " verify-content verify-time=signed",
                  log) == 0);
        CHECK(contains(read_log(log), ": valid"));
    }
    SECTION("a signature without a publisher UUID is warned about when made") {
        const fs::path bare = dir / "described_bare.ac4";
        REQUIRE(run_cli("ac4-sign " + quoted(plain) + " " + quoted(bare) +
                            key_chain_options(files) + " signed-date=" + std::string{kWhenAllValid},
                        log) == 0);
        const std::string made = read_log(log);
        CHECK(contains(made, "warning: ac4-sign has no publisher-uuid="));
        CHECK(contains(made, "verify-content"));

        // ac4-verify reports the gap and still passes; verify-content refuses the same file.
        REQUIRE(run_cli("ac4-verify " + quoted(bare) + anchor + " verify-time=signed", log) == 0);
        const std::string reported = read_log(log);
        CHECK(contains(reported, ": valid"));
        CHECK(contains(reported, "note publisher UUID"));
        CHECK(contains(reported, "publisherUUID is empty"));
        CHECK(run_cli("ac4-verify " + quoted(bare) + anchor + " verify-content verify-time=signed",
                      log) == kQcGate);
        const std::string refused = read_log(log);
        CHECK(contains(refused, "INVALID"));
        CHECK(contains(refused, "FAIL publisher UUID"));
        // On decode, verify-content stops before anything is written; check-content plays on.
        const fs::path out = dir / "described_decoded.wav";
        fs::remove(out);
        CHECK(run_cli("decode " + quoted(bare) + " " + quoted(out) + " verify-content" + anchor +
                          " verify-time=signed",
                      log) == kExitInput);
        CHECK_FALSE(fs::exists(out));
        CHECK(run_cli("decode " + quoted(bare) + " " + quoted(out) + " check-content" + anchor +
                          " verify-time=signed",
                      log) == 0);
        CHECK(fs::exists(out));
    }
}

TEST_CASE("expect-publisher-uuid= names the accounts to accept; the certificate is not asked",
          "[cli][ac4][content-signature][v25]") {
    const auto dir = scratch_dir();
    const auto log = dir / "expected.log";
    const Files files = write_credentials();
    const fs::path plain = plain_stream("expected_plain");
    const std::string when = " signed-date=" + std::string{kWhenAllValid};
    const std::string anchor = " trust-anchor=" + quoted(files.anchor) + " verify-time=signed";
    const std::string account = std::string{kPublisherUuid};
    const std::string other = std::string{kOtherUuid};
    const fs::path signed_file = dir / "expected_signed.ac4";
    REQUIRE(run_cli("ac4-sign " + quoted(plain) + " " + quoted(signed_file) +
                        key_chain_options(files) + when + " publisher-uuid=" + account,
                    log) == 0);
    CHECK_FALSE(contains(read_log(log), "no publisher-uuid="));

    SECTION("the account that signed, in either case, and among several") {
        for (const std::string& option :
             {" expect-publisher-uuid=" + account,
              std::string{" expect-publisher-uuid=3b9c1f52-7a4e-4d08-b6a1-2f0e9d7c5a84"},
              " expect-publisher-uuid=" + other + " expect-publisher-uuid=" + account}) {
            INFO(option);
            CHECK(run_cli("ac4-verify " + quoted(signed_file) + anchor + option, log) == 0);
            CHECK(contains(read_log(log), "ok   publisher UUID"));
            CHECK(contains(read_log(log), "expected publisher account(s)"));
            CHECK(run_cli("ac4-verify " + quoted(signed_file) + anchor + option + " verify-content",
                          log) == 0);
            CHECK(contains(read_log(log), ": valid"));
        }
    }
    SECTION("another account is a finding in ac4-verify and a refusal with verify-content") {
        const std::string option = " expect-publisher-uuid=" + other;
        // ac4-verify reports it and the verdict stands.
        CHECK(run_cli("ac4-verify " + quoted(signed_file) + anchor + option, log) == 0);
        const std::string reported = read_log(log);
        CHECK(contains(reported, ": valid"));
        CHECK(contains(reported, "note publisher UUID"));
        CHECK(contains(reported, "none of the 1 expected publisher account(s)"));
        // verify-content refuses it, on ac4-verify and before decode writes anything.
        CHECK(run_cli("ac4-verify " + quoted(signed_file) + anchor + option + " verify-content",
                      log) == kQcGate);
        const std::string refused = read_log(log);
        CHECK(contains(refused, "INVALID"));
        CHECK(contains(refused, "FAIL publisher UUID"));
        CHECK(contains(refused, "BadCredentialsForSigningContent"));
        const fs::path out = dir / "expected_decoded.wav";
        fs::remove(out);
        CHECK(run_cli("decode " + quoted(signed_file) + " " + quoted(out) + " verify-content" +
                          anchor + option,
                      log) == kExitInput);
        CHECK_FALSE(fs::exists(out));
        // check-content reports it and plays on.
        CHECK(run_cli("decode " + quoted(signed_file) + " " + quoted(out) + " check-content" +
                          anchor + option,
                      log) == 0);
        CHECK(fs::exists(out));
    }
    SECTION("without expect-publisher-uuid= the UUID is reported and never refused") {
        CHECK(run_cli("ac4-verify " + quoted(signed_file) + anchor + " verify-content", log) == 0);
        CHECK(contains(read_log(log), "no expected publisher was given"));
    }
    SECTION("a certificate that carries some other UUID changes nothing") {
        // Signed with a certificate whose subject carries a different UUID than the signature
        // names: CS spec v2.6 section 5.3 withdrew the match, so it signs and verifies, strictly.
        const Files carrying = write_uuid_credentials(other);
        const fs::path carried = dir / "expected_carried.ac4";
        REQUIRE(run_cli("ac4-sign " + quoted(plain) + " " + quoted(carried) +
                            key_chain_options(carrying) + when + " publisher-uuid=" + account,
                        log) == 0);
        CHECK(run_cli("ac4-verify " + quoted(carried) + anchor + " verify-content", log) == 0);
        CHECK(run_cli("ac4-verify " + quoted(carried) + anchor + " verify-content" +
                          " expect-publisher-uuid=" + account,
                      log) == 0);
        CHECK(run_cli("ac4-verify " + quoted(carried) + anchor + " verify-content" +
                          " expect-publisher-uuid=" + other,
                      log) == kQcGate);
    }
}

TEST_CASE("check-content judges the certificates at the signed date, the strict checks now",
          "[cli][ac4][content-signature][v25]") {
    const auto dir = scratch_dir();
    const auto log = dir / "check_time.log";
    const Files files = write_credentials();
    const fs::path signed_file = dir / "check_time_signed.ac4";
    REQUIRE(run_cli("ac4-sign " + quoted(plain_stream("check_time_plain")) + " " +
                        quoted(signed_file) + key_chain_options(files) +
                        " signed-date=" + std::string{kWhenAllValid},
                    log) == 0);
    const std::string common =
        "ac4-verify " + quoted(signed_file) + " trust-anchor=" + quoted(files.anchor);

    // The chain's second certificate expired on 2025-11-13; the signature is dated before that.
    // Strict (the default of ac4-verify, and verify-content): judged now, refused, and said so.
    CHECK(run_cli(common, log) == kQcGate);
    CHECK(contains(read_log(log), "expired"));
    CHECK(contains(read_log(log), "(now)"));
    CHECK(run_cli(common + " verify-content", log) == kQcGate);
    CHECK(contains(read_log(log), "(now)"));

    // check-content reports and carries on, judged at the signed date: the chain checks, and the
    // report names the time and the reason for it.
    CHECK(run_cli(common + " check-content", log) == 0);
    const std::string reported = read_log(log);
    CHECK(contains(reported, "judged at " + std::string{kWhenAllValid}));
    CHECK(contains(reported, "(signed, the default of check-content)"));
    CHECK(contains(reported, ": valid"));
    CHECK_FALSE(contains(reported, "expired"));

    // An explicit verify-time= wins over the default, in either mode.
    CHECK(run_cli(common + " check-content verify-time=now", log) == 0);
    CHECK(contains(read_log(log), "expired"));
    CHECK(contains(read_log(log), "INVALID"));
    // verify-content is the strict reading of the signer information too, and this signature names
    // no publisher UUID: the time is not what refuses it.
    CHECK(run_cli(common + " verify-content verify-time=signed", log) == kQcGate);
    CHECK(contains(read_log(log), "FAIL publisher UUID"));
    CHECK_FALSE(contains(read_log(log), "expired"));

    // decode, the same: check-content plays on and says what it judged by.
    const fs::path out = dir / "check_time.wav";
    CHECK(run_cli("decode " + quoted(signed_file) + " " + quoted(out) + " check-content" +
                      " trust-anchor=" + quoted(files.anchor),
                  log) == 0);
    CHECK(contains(read_log(log), "(signed, the default of check-content)"));
}

TEST_CASE("the signer information options are checked, and refused where nothing signs",
          "[cli][ac4][content-signature][v25]") {
    const auto dir = scratch_dir();
    const auto log = dir / "signer_options.log";
    const Files files = write_credentials();
    const fs::path plain = plain_stream("signer_options_plain");
    const fs::path out = dir / "signer_options_out.ac4";
    const std::string sign =
        "ac4-sign " + quoted(plain) + " " + quoted(out) + key_chain_options(files);

    CHECK(run_cli("ac4-verify " + quoted(plain) + " expect-publisher-uuid=not-a-uuid", log) ==
          kExitUsage);
    CHECK(contains(read_log(log), "expect-publisher-uuid="));
    CHECK(run_cli("ac4-verify " + quoted(plain) + " expect-publisher-uuid=", log) == kExitUsage);
    // It says how to check, so it is refused where nothing signs or checks, and where it would
    // only be dropped.
    CHECK(run_cli(sign + " expect-publisher-uuid=" + std::string{kPublisherUuid}, log) ==
          kExitUsage);
    CHECK(contains(read_log(log), "check an AC-4 content signature"));
    CHECK(run_cli("decode " + quoted(plain) + " " + quoted(dir / "signer_options.wav") +
                      " expect-publisher-uuid=" + std::string{kPublisherUuid},
                  log) == kExitUsage);
    CHECK(contains(read_log(log), "unless verify-content or check-content asks"));
    for (const char* bad :
         {"publisher-uuid=not-a-uuid",
          "publisher-uuid=", "publisher-uuid=3B9C1F527A4E4D08B6A12F0E9D7C5A84", "product-id=-1",
          "product-id=seven", "product-id=", "product-id=99999999999", "publisher-name=",
          "product-name=", "wrapper-name=", "os-name=", "os-version=", "os-description="}) {
        INFO(bad);
        fs::remove(out);
        CHECK(run_cli(sign + " " + bad, log) == kExitUsage);
        CHECK_FALSE(fs::exists(out));
    }
    CHECK(run_cli(sign + " publisher-uuid=not-a-uuid", log) == kExitUsage);
    CHECK(contains(read_log(log), "publisher-uuid="));

    // Where nothing is signed, they are not dropped.
    const fs::path in = stereo_wav("signer_options_in.wav");
    for (const char* option :
         {"publisher-uuid=3B9C1F52-7A4E-4D08-B6A1-2F0E9D7C5A84", "publisher-name=Someone",
          "product-id=3", "product-name=Thing", "wrapper-name=Wrap", "os-name=OS", "os-version=1",
          "os-description=D"}) {
        INFO(option);
        CHECK(run_cli(
                  "encode " + quoted(in) + " " + quoted(dir / "signer_options.ac3") + " " + option,
                  log) == kExitUsage);
        CHECK(contains(read_log(log), "sign AC-4 output"));
        CHECK(run_cli("ac4-verify " + quoted(plain) + " " + option, log) == kExitUsage);
        CHECK(run_cli("ac4-encode " + quoted(in) + " " + quoted(dir / "signer_options_e.ac4") +
                          " 192 " + option,
                      log) == kExitUsage);
        CHECK(contains(read_log(log), "unless sign-content asks"));
    }
}

TEST_CASE("a signed-date that is not the time of signing is warned about, and one that is is not",
          "[cli][ac4][content-signature][v25]") {
    const auto dir = scratch_dir();
    const auto log = dir / "signed_date_warning.log";
    const Files files = write_credentials();
    const fs::path plain = plain_stream("signed_date_warning_plain");
    const fs::path out = dir / "signed_date_warning_out.ac4";

    // A date months away: signed, with the warning that says what section 5.3 asks.
    REQUIRE(run_cli("ac4-sign " + quoted(plain) + " " + quoted(out) + key_chain_options(files) +
                        " signed-date=" + std::string{kWhenAllValid},
                    log) == 0);
    const std::string warned = read_log(log);
    CHECK(contains(warned, "warning: signed-date=" + std::string{kWhenAllValid}));
    CHECK(contains(warned, "not the time of signing"));
    CHECK(contains(warned, "v2.6 section 5.3"));

    // The present moment is the time of signing: no warning. (The test chain's second certificate
    // has expired, so the signature itself is refused, which is after the warning would have been.)
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    run_cli("ac4-sign " + quoted(plain) + " " + quoted(dir / "signed_date_now.ac4") +
                key_chain_options(files) +
                " signed-date=" + iclforge::base::crypto::format_iso8601_utc(now),
            log);
    CHECK_FALSE(contains(read_log(log), "not the time of signing"));
    // Left out: the clock, and no warning either.
    run_cli("ac4-sign " + quoted(plain) + " " + quoted(dir / "signed_date_default.ac4") +
                key_chain_options(files),
            log);
    CHECK_FALSE(contains(read_log(log), "not the time of signing"));
}

TEST_CASE("ac4-sign refuses an MP4 input, names the reason, and writes nothing",
          "[cli][ac4][content-signature]") {
    const auto dir = scratch_dir();
    const auto log = dir / "sign_mp4.log";
    const Files files = write_credentials();
    const fs::path mp4 = dir / "sign_mp4_in.mp4";
    REQUIRE(run_cli("ac4-encode " + quoted(stereo_wav("sign_mp4.wav")) + " " + quoted(mp4) + " 192",
                    log) == 0);
    const fs::path out = dir / "sign_mp4_out.mp4";
    fs::remove(out);

    CHECK(run_cli("ac4-sign " + quoted(mp4) + " " + quoted(out) + key_chain_options(files) +
                      " signed-date=" + std::string{kWhenAllValid},
                  log) == kExitUsage);
    const std::string text = read_log(log);
    CHECK(contains(text, "MP4"));
    CHECK(contains(text, "out of scope"));
    CHECK(contains(text, "8.3.2"));
    CHECK(contains(text, "raw .ac4 elementary stream"));
    CHECK_FALSE(contains(text, "does not begin with an AC-4 sync frame"));
    CHECK_FALSE(fs::exists(out));

    // Checking an MP4 says the same thing and does not call it a damaged AC-4 stream.
    CHECK(run_cli("ac4-verify " + quoted(mp4), log) == kExitInput);
    CHECK(contains(read_log(log), "MP4"));
}

TEST_CASE("the content-signature options are refused on commands they do not belong to",
          "[cli][ac4][content-signature]") {
    const auto dir = scratch_dir();
    const auto log = dir / "misplaced.log";
    const Files files = write_credentials();
    const fs::path in = stereo_wav("misplaced_in.wav");
    const fs::path out = dir / "misplaced.ac3";

    CHECK(run_cli("encode " + quoted(in) + " " + quoted(out) + " sign-content", log) == kExitUsage);
    CHECK(contains(read_log(log), "sign AC-4 output"));
    CHECK(run_cli(
              "encode " + quoted(in) + " " + quoted(out) + " trust-anchor=" + quoted(files.anchor),
              log) == kExitUsage);
    CHECK(contains(read_log(log), "check an AC-4 content signature"));
    CHECK(run_cli("probe " + quoted(in) + " verify-content", log) == kExitUsage);
    CHECK(run_cli("ac4-verify " + quoted(plain_stream("misplaced_plain")) +
                      " verify-content check-content",
                  log) == kExitUsage);
    CHECK(contains(read_log(log), "give one, not both"));
    // Checking an E-AC-3 stream's content signature has no meaning, and is said rather than
    // skipped.
    const fs::path ec3 = dir / "misplaced.ec3";
    REQUIRE(run_cli("atmos-encode " + quoted(in) + " " + quoted(ec3) + " 448", log) == 0);
    CHECK(run_cli("decode " + quoted(ec3) + " " + quoted(dir / "misplaced.wav") + " check-content",
                  log) == kExitUsage);
    CHECK(contains(read_log(log), "AC-3 or E-AC-3"));
}

TEST_CASE("probe describes a content signature without checking it",
          "[cli][ac4][content-signature]") {
    const auto dir = scratch_dir();
    const auto log = dir / "probe.log";
    const Files files = write_credentials();
    const fs::path plain = plain_stream("probe_plain");
    const fs::path signed_file = dir / "probe_signed.ac4";
    REQUIRE(run_cli("ac4-sign " + quoted(plain) + " " + quoted(signed_file) +
                        key_chain_options(files) + " signed-date=" + std::string{kWhenAllValid},
                    log) == 0);

    // The trailer is not a parse error: probe exits 0 and the integrity members are clean.
    REQUIRE(run_cli("probe " + quoted(signed_file), log) == 0);
    const std::string table = read_log(log);
    CHECK(contains(table, "content sig"));
    CHECK(contains(table, "ICL Forge Test Content Signer"));
    CHECK(contains(table, "not verified"));
    CHECK_FALSE(contains(table, "parse error"));

    REQUIRE(run_cli("probe " + quoted(signed_file) + " json=1", log) == 0);
    const std::string json = read_log(log);
    CHECK(contains(json, "\"content_signature\""));
    CHECK(contains(json, "\"verified\": false"));
    CHECK(contains(json, "\"parse_failures\": 0"));

    // Without a signature the member is there and null: the document's members are never omitted.
    REQUIRE(run_cli("probe " + quoted(plain) + " json=1", log) == 0);
    CHECK(contains(read_log(log), "\"content_signature\": null"));
    REQUIRE(run_cli("probe " + quoted(plain), log) == 0);
    CHECK_FALSE(contains(read_log(log), "content sig"));
}
