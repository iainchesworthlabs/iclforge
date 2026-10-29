// forge's AC-4 content signing (ac4-sign, ac4-verify, sign-content, verify-content, check-content
// and their key, chain and trust options), each run against the real binary on a certificate chain
// the test builds from fixed labels (tests/support/test_pki.hpp: public keys that protect nothing).
// The signing and verifying themselves are tested in libs/base and libs/ac4; what is checked here
// is what the command line does with them: that nothing signs or checks unless asked, that the
// stream's frames are untouched, which exit code each outcome has, and what it refuses.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
    REQUIRE(run_cli("ac4-sign " + quoted(plain) + " " + quoted(signed_file) +
                        key_chain_options(files) + " signed-date=" + std::string{kWhenAllValid},
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
    SECTION("an MP4 has no trailer to carry it") {
        const fs::path out = dir / "encode_signed.mp4";
        fs::remove(out);
        CHECK(run_cli(encode + quoted(out) + " 192 sign-content" + key_chain_options(files), log) ==
              kExitUsage);
        CHECK(contains(read_log(log), "MP4"));
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
