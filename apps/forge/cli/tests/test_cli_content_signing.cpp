// forge's AC-4 content signing (sign-content, verify-content, gate-content and the signature
// probe reports), each run against the real binary and checked in what it writes and says:
// exit codes, the files that exist afterwards, and the words of each refusal. The keys and chains
// are built at run time from a seed (tests/support/test_pki.hpp) and written to the scratch
// directory as PEM, the way an operator's files are; no key is committed. Times are given with
// sign-time= and verify-time=, so no case depends on the date it runs on. The unit tests of the
// code behind the commands are test_ac4_content_signing.cpp and test_mp4_signature_carrier.cpp.

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
#include "iclforge/ac4/io/elementary.hpp"
#include "iclforge/base/crypto/credentials.hpp"
#include "iclforge/base/crypto/x509.hpp"

namespace fs = std::filesystem;
namespace crypto = iclforge::base::crypto;
namespace pki = iclforge::test::pki;
using Bytes = std::vector<std::byte>;

namespace {

constexpr int kRate = 48000;
constexpr std::string_view kSignTime = "sign-time=2026-05-01T00:00:00Z";
constexpr std::string_view kVerifyTime = "verify-time=2026-06-01T00:00:00Z";

fs::path scratch_dir() {
    auto dir = fs::path{ICLFORGE_TEST_SCRATCH_DIR} /
               ("cli_content_signing_" + iclforge::test::platform::process_id());
    fs::create_directories(dir);
    return dir;
}

std::string quoted(const fs::path& path) {
    return "\"" + path.string() + "\"";
}

struct Run {
    int exit_code = -1;
    std::string log;
};

std::string read_text(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

Bytes read_bytes(const fs::path& path) {
    const std::string text = read_text(path);
    Bytes out(text.size());
    std::ranges::transform(text, out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}

void write_bytes(const fs::path& path, const Bytes& bytes) {
    std::ofstream out{path, std::ios::binary};
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

void write_text(const fs::path& path, const std::string& text) {
    std::ofstream out{path, std::ios::binary};
    out << text;
}

// Runs forge with `args` (already quoted where they need it), the output and errors in one log.
Run forge(const std::string& args, const std::string& redirect_in = {}) {
    static int counter = 0;
    const fs::path log = scratch_dir() / ("run" + std::to_string(++counter) + ".log");
    const std::string command = "\"" + std::string(ICLFORGE_CLI_EXE) + "\" " + args +
                                (redirect_in.empty() ? "" : " < " + redirect_in) + " > \"" +
                                log.string() + "\" 2>&1";
    Run run;
    run.exit_code = iclforge::test::platform::run_shell(command);
    run.log = read_text(log);
    return run;
}

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

// The variables the commands read, cleared around each case so the machine's own environment
// cannot change what a case sees.
struct CleanEnvironment {
    static void clear() {
        for (const auto name :
             {crypto::kEnvContentSigningKeyFile, crypto::kEnvContentSigningKey,
              crypto::kEnvContentSigningChainFile, crypto::kEnvContentTrustFile}) {
            iclforge::test::platform::unset_environment(name);
        }
    }
    CleanEnvironment() { clear(); }
    ~CleanEnvironment() { clear(); }
};

// A signer's files: a key and a chain (leaf first, a deliberate disorder the loader sorts out),
// and the root as a trust anchor.
struct Signer {
    pki::Authority authority;
    fs::path key;
    fs::path chain;
    fs::path root;

    explicit Signer(const std::string& name, pki::AuthorityOptions options = {})
        : authority(pki::make_authority(std::move(options))) {
        key = scratch_dir() / (name + ".key");
        chain = scratch_dir() / (name + "-chain.pem");
        root = scratch_dir() / (name + "-root.pem");
        write_text(key, crypto::encode_pem("PRIVATE KEY", pki::pkcs8_der(authority.leaf.keys)));
        write_text(chain, crypto::encode_pem("CERTIFICATE", authority.leaf.der) +
                              crypto::encode_pem("CERTIFICATE", authority.intermediate.der) +
                              crypto::encode_pem("CERTIFICATE", authority.root.der));
        write_text(root, crypto::encode_pem("CERTIFICATE", authority.root.der));
    }

    [[nodiscard]] std::string sign_args() const {
        return "sign-content signing-key=" + quoted(key) + " signing-chain=" + quoted(chain) + " " +
               std::string{kSignTime};
    }
    [[nodiscard]] std::string verify_args() const {
        return "trust-anchor=" + quoted(root) + " " + std::string{kVerifyTime};
    }
};

// A second of two tones as a stereo WAV.
fs::path tones_wav() {
    static const fs::path path = [] {
        std::vector<std::vector<float>> channels(2, std::vector<float>(kRate));
        for (std::size_t n = 0; n < channels[0].size(); ++n) {
            const double t = static_cast<double>(n) / kRate;
            channels[0][n] = static_cast<float>(0.1 * std::sin(2.0 * std::numbers::pi * 440.0 * t));
            channels[1][n] = static_cast<float>(0.1 * std::sin(2.0 * std::numbers::pi * 660.0 * t));
        }
        const fs::path wav = scratch_dir() / "tones.wav";
        REQUIRE(iclforge::ac3::io::write_wav_f32(wav.string(), channels, kRate).has_value());
        return wav;
    }();
    return path;
}

// The elementary stream and its signature, written by sign-content.
struct SignedStream {
    fs::path stream;
    fs::path sidecar;
};

SignedStream sign_elementary(const Signer& signer, const std::string& name) {
    SignedStream out{scratch_dir() / (name + ".ac4"), scratch_dir() / (name + ".ac4.sig")};
    const Run run = forge("ac4-encode " + quoted(tones_wav()) + " " + quoted(out.stream) + " 128 " +
                          signer.sign_args());
    REQUIRE(run.exit_code == 0);
    REQUIRE(fs::exists(out.sidecar));
    return out;
}

fs::path sign_mp4(const Signer& signer, const std::string& name) {
    const fs::path out = scratch_dir() / (name + ".mp4");
    const Run run = forge("ac4-encode " + quoted(tones_wav()) + " " + quoted(out) + " 128 " +
                          signer.sign_args());
    REQUIRE(run.exit_code == 0);
    return out;
}

fs::path wav_out(const std::string& name) {
    const fs::path path = scratch_dir() / (name + ".wav");
    std::error_code ec;
    fs::remove(path, ec);
    return path;
}

}  // namespace

TEST_CASE("sign-content writes a sidecar beside a raw stream and verify-content accepts it",
          "[cli][content_signing]") {
    const CleanEnvironment environment;
    const Signer signer{"elementary"};
    const SignedStream signed_stream = sign_elementary(signer, "elementary");

    // Signing wrote the stream a plain encode writes, and a signature beside it.
    const fs::path plain = scratch_dir() / "elementary-plain.ac4";
    REQUIRE(forge("ac4-encode " + quoted(tones_wav()) + " " + quoted(plain) + " 128").exit_code ==
            0);
    REQUIRE(read_bytes(plain) == read_bytes(signed_stream.stream));
    REQUIRE_FALSE(fs::exists(scratch_dir() / "elementary-plain.ac4.sig"));

    const fs::path out = wav_out("elementary-verified");
    const Run run = forge("decode " + quoted(signed_stream.stream) + " " + quoted(out) +
                          " verify-content " + signer.verify_args());
    REQUIRE(run.exit_code == 0);
    REQUIRE(fs::exists(out));
    REQUIRE(contains(run.log, "content signature: verified (elementary stream with sidecar"));
    REQUIRE(contains(run.log, "serial 1798A2B5D62000000005"));
    REQUIRE(contains(run.log, "accepted under anchor 1"));
    REQUIRE(contains(run.log, "elementary-root.pem"));
    REQUIRE(contains(run.log, "the signature does not cover the date"));
    REQUIRE(contains(run.log, "2026-05-01T00:00:00Z"));
}

TEST_CASE("verify-content refuses a stream and writes nothing, whatever is wrong with it",
          "[cli][content_signing]") {
    const CleanEnvironment environment;
    const Signer signer{"refusals"};
    const SignedStream signed_stream = sign_elementary(signer, "refusals");

    SECTION("an unsigned stream") {
        const fs::path plain = scratch_dir() / "refusals-plain.ac4";
        REQUIRE(
            forge("ac4-encode " + quoted(tones_wav()) + " " + quoted(plain) + " 128").exit_code ==
            0);
        const fs::path out = wav_out("refusals-unsigned");
        const Run run = forge("decode " + quoted(plain) + " " + quoted(out) + " verify-content " +
                              signer.verify_args());
        REQUIRE(run.exit_code == 2);
        REQUIRE(contains(run.log, "carries no content signature"));
        REQUIRE(contains(run.log, "BadOrMissingSignature"));
        REQUIRE_FALSE(fs::exists(out));
    }
    SECTION("a frame changed after signing") {
        // A bit in the middle of the second frame's payload, found by scanning the stream so the
        // framing stays intact and the change is to what the signature covers.
        Bytes bytes = read_bytes(signed_stream.stream);
        const auto scanned = iclforge::ac4::scan(bytes);
        REQUIRE(scanned.frames.size() > 2);
        REQUIRE_FALSE(scanned.stopped_at.has_value());
        const auto& frame = scanned.frames[1].raw_ac4_frame;
        const auto offset =
            static_cast<std::size_t>(frame.data() - bytes.data()) + frame.size() / 2;
        bytes[offset] ^= std::byte{0x04};
        const fs::path altered = scratch_dir() / "refusals-altered.ac4";
        write_bytes(altered, bytes);
        write_bytes(scratch_dir() / "refusals-altered.ac4.sig", read_bytes(signed_stream.sidecar));

        const fs::path out = wav_out("refusals-altered");
        const Run run = forge("decode " + quoted(altered) + " " + quoted(out) + " verify-content " +
                              signer.verify_args());
        REQUIRE(run.exit_code == 2);
        REQUIRE(contains(run.log, "SignatureInvalid"));
        REQUIRE(contains(run.log, "changed after it was signed"));
        REQUIRE_FALSE(fs::exists(out));
    }
    SECTION("a stream that stops short is not judged by the frames before the bad place") {
        Bytes bytes = read_bytes(signed_stream.stream);
        bytes.insert(bytes.end(), 12, std::byte{0x55});
        const fs::path trailing = scratch_dir() / "refusals-trailing.ac4";
        write_bytes(trailing, bytes);
        write_bytes(scratch_dir() / "refusals-trailing.ac4.sig", read_bytes(signed_stream.sidecar));
        const fs::path out = wav_out("refusals-trailing");
        const Run run = forge("decode " + quoted(trailing) + " " + quoted(out) +
                              " verify-content " + signer.verify_args());
        REQUIRE(run.exit_code == 2);
        REQUIRE(contains(run.log, "loses sync"));
        REQUIRE_FALSE(fs::exists(out));
    }
    SECTION("a signer the operator does not trust") {
        const Signer other{"refusals-other", {.seed = "other authority"}};
        const fs::path out = wav_out("refusals-other");
        const Run run = forge("decode " + quoted(signed_stream.stream) + " " + quoted(out) +
                              " verify-content trust-anchor=" + quoted(other.root) + " " +
                              std::string{kVerifyTime});
        REQUIRE(run.exit_code == 2);
        REQUIRE(contains(run.log, "BadCredentialsForSigningContent"));
        REQUIRE_FALSE(fs::exists(out));
    }
    SECTION("a signature file that is not there when it is named") {
        const fs::path out = wav_out("refusals-missing");
        const Run run = forge("decode " + quoted(signed_stream.stream) + " " + quoted(out) +
                              " verify-content " + signer.verify_args() +
                              " content-signature=" + quoted(scratch_dir() / "nowhere.sig"));
        REQUIRE(run.exit_code == 2);
        REQUIRE(contains(run.log, "cannot read the signature file"));
        REQUIRE_FALSE(fs::exists(out));
    }
    SECTION("nothing to trust: a signature is never checked against nothing") {
        const fs::path out = wav_out("refusals-no-trust");
        const Run run = forge("decode " + quoted(signed_stream.stream) + " " + quoted(out) +
                              " verify-content " + std::string{kVerifyTime});
        REQUIRE(run.exit_code == 2);
        REQUIRE(contains(run.log, "trust-anchor=<certificate>"));
        REQUIRE(contains(run.log, "ICLFORGE_CONTENT_TRUST_FILE"));
        REQUIRE(contains(run.log, "never checked against nothing"));
        REQUIRE_FALSE(fs::exists(out));
    }
}

TEST_CASE("the trust is read from the environment only when no path is named",
          "[cli][content_signing]") {
    const CleanEnvironment environment;
    const Signer signer{"trust-env"};
    const Signer other{"trust-env-other", {.seed = "other authority"}};
    const SignedStream signed_stream = sign_elementary(signer, "trust-env");

    iclforge::test::platform::set_environment(crypto::kEnvContentTrustFile, signer.root.string());
    const fs::path out = wav_out("trust-env");
    REQUIRE(forge("decode " + quoted(signed_stream.stream) + " " + quoted(out) +
                  " verify-content " + std::string{kVerifyTime})
                .exit_code == 0);

    // A named anchor is the whole set: an exported variable cannot widen it.
    const fs::path refused = wav_out("trust-env-refused");
    const Run run = forge("decode " + quoted(signed_stream.stream) + " " + quoted(refused) +
                          " verify-content trust-anchor=" + quoted(other.root) + " " +
                          std::string{kVerifyTime});
    REQUIRE(run.exit_code == 2);
    REQUIRE_FALSE(fs::exists(refused));
}

TEST_CASE("the key and the chain are read from the environment when no path is named",
          "[cli][content_signing]") {
    const CleanEnvironment environment;
    const Signer signer{"sign-env"};

    SECTION("by file") {
        iclforge::test::platform::set_environment(crypto::kEnvContentSigningKeyFile,
                                                  signer.key.string());
        iclforge::test::platform::set_environment(crypto::kEnvContentSigningChainFile,
                                                  signer.chain.string());
        const fs::path stream = scratch_dir() / "sign-env.ac4";
        const Run run = forge("ac4-encode " + quoted(tones_wav()) + " " + quoted(stream) +
                              " 128 sign-content " + std::string{kSignTime});
        REQUIRE(run.exit_code == 0);
        REQUIRE(fs::exists(scratch_dir() / "sign-env.ac4.sig"));
    }
    SECTION("the key as PEM in a variable") {
        iclforge::test::platform::set_environment(crypto::kEnvContentSigningKey,
                                                  read_text(signer.key));
        iclforge::test::platform::set_environment(crypto::kEnvContentSigningChainFile,
                                                  signer.chain.string());
        const fs::path stream = scratch_dir() / "sign-env-inline.ac4";
        const Run run = forge("ac4-encode " + quoted(tones_wav()) + " " + quoted(stream) +
                              " 128 sign-content " + std::string{kSignTime});
        REQUIRE(run.exit_code == 0);
        REQUIRE(fs::exists(scratch_dir() / "sign-env-inline.ac4.sig"));
    }
}

TEST_CASE("sign-content names what is missing before it encodes anything",
          "[cli][content_signing]") {
    const CleanEnvironment environment;
    const Signer signer{"sign-missing"};
    const fs::path stream = scratch_dir() / "sign-missing.ac4";
    std::error_code ec;
    fs::remove(stream, ec);
    fs::remove(scratch_dir() / "sign-missing.ac4.sig", ec);

    SECTION("no key and no chain") {
        const Run run =
            forge("ac4-encode " + quoted(tones_wav()) + " " + quoted(stream) + " 128 sign-content");
        REQUIRE(run.exit_code == 2);
        REQUIRE(contains(run.log, "signing-key=<path>"));
        REQUIRE(contains(run.log, "signing-chain=<path>"));
        REQUIRE(contains(run.log, "ICLFORGE_CONTENT_SIGNING_KEY_FILE"));
        REQUIRE_FALSE(fs::exists(stream));
    }
    SECTION("a key and no chain") {
        const Run run = forge("ac4-encode " + quoted(tones_wav()) + " " + quoted(stream) +
                              " 128 sign-content signing-key=" + quoted(signer.key));
        REQUIRE(run.exit_code == 2);
        REQUIRE_FALSE(fs::exists(stream));
    }
    SECTION("a chain for another key") {
        const Signer other{"sign-missing-other", {.seed = "other authority"}};
        const Run run = forge("ac4-encode " + quoted(tones_wav()) + " " + quoted(stream) +
                              " 128 sign-content signing-key=" + quoted(signer.key) +
                              " signing-chain=" + quoted(other.chain));
        REQUIRE(run.exit_code == 2);
        REQUIRE_FALSE(fs::exists(stream));
    }
    SECTION("two keys") {
        const Run run =
            forge("ac4-encode " + quoted(tones_wav()) + " " + quoted(stream) +
                  " 128 sign-content signing-key=" + quoted(signer.key) +
                  " signing-key=" + quoted(signer.key) + " signing-chain=" + quoted(signer.chain));
        REQUIRE(run.exit_code == 1);
        REQUIRE(contains(run.log, "signs with one key"));
        REQUIRE_FALSE(fs::exists(stream));
    }
}

TEST_CASE("an MP4 carries its signature in the track, from ac4-encode and from mp4",
          "[cli][content_signing]") {
    const CleanEnvironment environment;
    const Signer signer{"mp4"};

    SECTION("ac4-encode to an .mp4") {
        const fs::path mp4 = sign_mp4(signer, "mp4-encoded");
        const fs::path out = wav_out("mp4-encoded");
        const Run run = forge("decode " + quoted(mp4) + " " + quoted(out) + " verify-content " +
                              signer.verify_args());
        REQUIRE(run.exit_code == 0);
        REQUIRE(contains(run.log, "verified (MP4 uuid box"));
        REQUIRE(fs::exists(out));
        // Nothing beside it: the box is the signature.
        REQUIRE_FALSE(fs::exists(scratch_dir() / "mp4-encoded.mp4.sig"));
    }
    SECTION("mp4 of a raw stream, and the raw stream's own sidecar is not needed") {
        const fs::path plain = scratch_dir() / "mp4-wrapped.ac4";
        REQUIRE(
            forge("ac4-encode " + quoted(tones_wav()) + " " + quoted(plain) + " 128").exit_code ==
            0);
        const fs::path mp4 = scratch_dir() / "mp4-wrapped.mp4";
        const Run signed_run =
            forge("mp4 " + quoted(plain) + " " + quoted(mp4) + " " + signer.sign_args());
        REQUIRE(signed_run.exit_code == 0);
        REQUIRE(contains(signed_run.log, "in a uuid box of the AC-4 track"));
        const fs::path out = wav_out("mp4-wrapped");
        REQUIRE(forge("decode " + quoted(mp4) + " " + quoted(out) + " verify-content " +
                      signer.verify_args())
                    .exit_code == 0);
    }
    SECTION("a changed sample is caught") {
        const fs::path mp4 = sign_mp4(signer, "mp4-altered-source");
        Bytes bytes = read_bytes(mp4);
        // The media data box's payload: a bit well inside it.
        const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        const std::size_t mdat = text.find("mdat");
        REQUIRE(mdat != std::string::npos);
        bytes[mdat + 4 + 1500] ^= std::byte{0x20};
        const fs::path altered = scratch_dir() / "mp4-altered.mp4";
        write_bytes(altered, bytes);
        const fs::path out = wav_out("mp4-altered");
        const Run run = forge("decode " + quoted(altered) + " " + quoted(out) + " verify-content " +
                              signer.verify_args());
        REQUIRE(run.exit_code == 2);
        REQUIRE(contains(run.log, "SignatureInvalid"));
        REQUIRE_FALSE(fs::exists(out));
    }
    SECTION("what is in the MP4 around the samples is not covered") {
        const fs::path mp4 = sign_mp4(signer, "mp4-outside-source");
        Bytes bytes = read_bytes(mp4);
        const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        const std::size_t mvhd = text.find("mvhd");
        REQUIRE(mvhd != std::string::npos);
        // The movie header's creation time: 4 bytes of version and flags, then a 4-byte time in
        // a version 0 header.
        bytes[mvhd + 4 + 4 + 3] ^= std::byte{0x01};
        const fs::path changed = scratch_dir() / "mp4-outside-time.mp4";
        write_bytes(changed, bytes);
        const fs::path out = wav_out("mp4-outside-time");
        const Run run = forge("decode " + quoted(changed) + " " + quoted(out) + " verify-content " +
                              signer.verify_args());
        REQUIRE(run.exit_code == 0);
        REQUIRE(contains(run.log, "content signature: verified"));
    }
    SECTION(
        "fmp4 keeps it in the initialization segment, and init plus segments verify as a file") {
        const fs::path plain = scratch_dir() / "fmp4-source.ac4";
        REQUIRE(
            forge("ac4-encode " + quoted(tones_wav()) + " " + quoted(plain) + " 128").exit_code ==
            0);
        const fs::path dir = scratch_dir() / "fmp4-signed";
        const Run run =
            forge("fmp4 " + quoted(plain) + " " + quoted(dir) + " " + signer.sign_args());
        REQUIRE(run.exit_code == 0);
        REQUIRE(contains(run.log, "in a uuid box of the AC-4 track"));

        Bytes joined = read_bytes(dir / "init.mp4");
        for (int n = 1; fs::exists(dir / ("segment" + std::to_string(n) + ".m4s")); ++n) {
            const Bytes segment = read_bytes(dir / ("segment" + std::to_string(n) + ".m4s"));
            joined.insert(joined.end(), segment.begin(), segment.end());
        }
        const fs::path whole = scratch_dir() / "fmp4-joined.mp4";
        write_bytes(whole, joined);
        const fs::path out = wav_out("fmp4-joined");
        const Run verified = forge("decode " + quoted(whole) + " " + quoted(out) +
                                   " verify-content " + signer.verify_args());
        REQUIRE(verified.exit_code == 0);
        REQUIRE(contains(verified.log, "verified (MP4 uuid box"));
    }
}

TEST_CASE("an expired intermediate is refused unless allow-expired-intermediates is given",
          "[cli][content_signing]") {
    const CleanEnvironment environment;
    // The intermediate lapses before the verification time; the root and the leaf do not.
    const Signer signer{"expired",
                        {.seed = "expired", .intermediate_not_after = pki::at(2026, 1, 1)}};
    const SignedStream signed_stream = sign_elementary(signer, "expired");

    const fs::path refused = wav_out("expired-strict");
    const Run strict = forge("decode " + quoted(signed_stream.stream) + " " + quoted(refused) +
                             " verify-content " + signer.verify_args());
    REQUIRE(strict.exit_code == 2);
    REQUIRE(contains(strict.log, "BadCredentialsForSigningContent"));
    REQUIRE(contains(strict.log, "expired"));
    REQUIRE_FALSE(fs::exists(refused));

    const fs::path accepted = wav_out("expired-relaxed");
    const Run relaxed =
        forge("decode " + quoted(signed_stream.stream) + " " + quoted(accepted) +
              " verify-content allow-expired-intermediates " + signer.verify_args());
    REQUIRE(relaxed.exit_code == 0);
    REQUIRE(contains(relaxed.log, "is past its notAfter and was accepted because"));
    REQUIRE(contains(relaxed.log, "allow-expired-intermediates"));
    REQUIRE(fs::exists(accepted));

    // The relaxation is for intermediates: a root past its date is refused under it.
    const Signer old_root{"expired-root",
                          {.seed = "expired root", .root_not_after = pki::at(2026, 1, 1)}};
    const SignedStream old_stream = sign_elementary(old_root, "expired-root");
    const fs::path root_out = wav_out("expired-root");
    const Run root_run =
        forge("decode " + quoted(old_stream.stream) + " " + quoted(root_out) +
              " verify-content allow-expired-intermediates " + old_root.verify_args());
    REQUIRE(root_run.exit_code == 2);
    REQUIRE_FALSE(fs::exists(root_out));
}

TEST_CASE("the leaf profile is the specification's unless the operator changes it",
          "[cli][content_signing]") {
    const CleanEnvironment environment;
    const Signer signer{
        "profile",
        {.seed = "profile",
         .leaf_name = {"Mastering Room", "Mastering Signer", "ABCD1234"},
         .leaf_serial = {std::byte{0xAB}, std::byte{0xCD}, std::byte{0x12}, std::byte{0x34}}}};
    const SignedStream signed_stream = sign_elementary(signer, "profile");
    const std::string base = "decode " + quoted(signed_stream.stream);

    const fs::path refused = wav_out("profile-spec");
    const Run spec =
        forge(base + " " + quoted(refused) + " verify-content " + signer.verify_args());
    REQUIRE(spec.exit_code == 2);
    REQUIRE(contains(spec.log, "CertNotPrivilegedToSignContent"));
    REQUIRE_FALSE(fs::exists(refused));

    const fs::path any = wav_out("profile-any");
    REQUIRE(forge(base + " " + quoted(any) + " verify-content signer-profile=any " +
                  signer.verify_args())
                .exit_code == 0);

    // Naming all three fields is the same as naming none of them, for a leaf that has them.
    const fs::path named = wav_out("profile-named");
    REQUIRE(forge(base + " " + quoted(named) +
                  " verify-content \"signer-title=Mastering Signer\" \"signer-cn=Mastering Room\" "
                  "signer-serial=ABCD1234 " +
                  signer.verify_args())
                .exit_code == 0);

    const fs::path wrong = wav_out("profile-wrong");
    REQUIRE(forge(base + " " + quoted(wrong) +
                  " verify-content \"signer-title=Mastering Signer\" \"signer-cn=Mastering Room\" "
                  "signer-serial=ABCD1235 " +
                  signer.verify_args())
                .exit_code == 2);
}

TEST_CASE("gate-content decodes what does not verify, without its objects, and carries on",
          "[cli][content_signing]") {
    const CleanEnvironment environment;
    const Signer signer{"gate"};
    const SignedStream signed_stream = sign_elementary(signer, "gate");

    SECTION("a stream that verifies is decoded as it is, and says who signed it") {
        const fs::path out = wav_out("gate-verified");
        const Run run = forge("decode " + quoted(signed_stream.stream) + " " + quoted(out) +
                              " gate-content " + signer.verify_args());
        REQUIRE(run.exit_code == 0);
        REQUIRE(contains(run.log, "content signature: verified"));
        REQUIRE_FALSE(contains(run.log, "object audio left out"));
        REQUIRE(fs::exists(out));
    }
    SECTION("an unsigned stream carries on, with a warning and the same channels") {
        const fs::path plain = scratch_dir() / "gate-plain.ac4";
        REQUIRE(
            forge("ac4-encode " + quoted(tones_wav()) + " " + quoted(plain) + " 128").exit_code ==
            0);
        const fs::path gated = wav_out("gate-unsigned");
        const Run run = forge("decode " + quoted(plain) + " " + quoted(gated) + " gate-content " +
                              signer.verify_args());
        REQUIRE(run.exit_code == 0);
        REQUIRE(contains(run.log, "warning: gate-content"));
        REQUIRE(contains(run.log, "did not verify"));
        REQUIRE(contains(run.log, "decoding it without its objects"));
        REQUIRE(fs::exists(gated));

        // A stream with no object audio loses nothing: the same samples as an unchecked decode.
        const fs::path unchecked = wav_out("gate-unchecked");
        REQUIRE(forge("decode " + quoted(plain) + " " + quoted(unchecked)).exit_code == 0);
        REQUIRE(read_bytes(gated) == read_bytes(unchecked));
    }
    SECTION("a signature from someone else carries on too") {
        const Signer other{"gate-other", {.seed = "other authority"}};
        const fs::path out = wav_out("gate-other");
        const Run run = forge("decode " + quoted(signed_stream.stream) + " " + quoted(out) +
                              " gate-content trust-anchor=" + quoted(other.root) + " " +
                              std::string{kVerifyTime});
        REQUIRE(run.exit_code == 0);
        REQUIRE(contains(run.log, "BadCredentialsForSigningContent"));
        REQUIRE(fs::exists(out));
    }
    SECTION("with nothing to trust it is still refused: it has no way to judge") {
        const fs::path out = wav_out("gate-no-trust");
        const Run run = forge("decode " + quoted(signed_stream.stream) + " " + quoted(out) +
                              " gate-content " + std::string{kVerifyTime});
        REQUIRE(run.exit_code == 2);
        REQUIRE(contains(run.log, "gate-content needs something to trust"));
        REQUIRE_FALSE(fs::exists(out));
    }
}

TEST_CASE("the options that do not fit are usage errors that say what to use",
          "[cli][content_signing]") {
    const CleanEnvironment environment;
    const Signer signer{"usage"};
    const fs::path out = wav_out("usage");
    const fs::path stream = scratch_dir() / "usage.ac4";
    REQUIRE(forge("ac4-encode " + quoted(tones_wav()) + " " + quoted(stream) + " 128").exit_code ==
            0);

    SECTION("verify-content and gate-content together") {
        const Run run = forge("decode " + quoted(stream) + " " + quoted(out) +
                              " verify-content gate-content " + signer.verify_args());
        REQUIRE(run.exit_code == 1);
        REQUIRE(contains(run.log, "give one, not both"));
    }
    SECTION("sign-content with sign-objects") {
        const Run run = forge("atmos-encode " + quoted(tones_wav()) + " " + quoted(out) +
                              " 448 codec=ac4 sign-content sign-objects");
        REQUIRE(run.exit_code == 1);
        REQUIRE(contains(run.log, "one command writes one of them"));
    }
    SECTION("sign-content on a command that writes no AC-4") {
        const Run run =
            forge("encode " + quoted(tones_wav()) + " " + quoted(out) + " sign-content");
        REQUIRE(run.exit_code == 1);
        REQUIRE(contains(run.log, "sign-content signs AC-4 content"));
    }
    SECTION("sign-content on atmos-encode without codec=ac4 names sign-objects") {
        const Run run =
            forge("atmos-encode " + quoted(tones_wav()) + " " + quoted(out) + " 448 sign-content");
        REQUIRE(run.exit_code == 1);
        REQUIRE(contains(run.log, "sign-objects"));
    }
    SECTION("sign-content on an E-AC-3 stream wrapped by mp4") {
        const fs::path ec3 = scratch_dir() / "usage.ec3";
        REQUIRE(forge("eac3-silence " + quoted(ec3) + " 1").exit_code == 0);
        const Run run = forge("mp4 " + quoted(ec3) + " " + quoted(scratch_dir() / "usage.mp4") +
                              " " + signer.sign_args());
        REQUIRE(run.exit_code == 1);
        REQUIRE(contains(run.log, "is AC-3 or E-AC-3"));
        REQUIRE(contains(run.log, "sign-objects"));
    }
    SECTION("sign-objects on ac4-encode names sign-content") {
        const Run run =
            forge("ac4-encode " + quoted(tones_wav()) + " " + quoted(out) + " 128 sign-objects");
        REQUIRE(run.exit_code == 1);
        REQUIRE(contains(run.log, "sign-content"));
    }
    SECTION("verify-objects on an AC-4 stream names verify-content") {
        const Run run = forge("decode " + quoted(stream) + " " + quoted(out) + " verify-objects");
        REQUIRE(run.exit_code == 1);
        REQUIRE(contains(run.log, "verify-content"));
    }
    SECTION("verify-content on an E-AC-3 stream names verify-objects") {
        const fs::path ec3 = scratch_dir() / "usage-eac3.ec3";
        REQUIRE(forge("eac3-silence " + quoted(ec3) + " 1").exit_code == 0);
        const Run run = forge("decode " + quoted(ec3) + " " + quoted(out) + " verify-content " +
                              signer.verify_args());
        REQUIRE(run.exit_code == 1);
        REQUIRE(contains(run.log, "verify-objects"));
    }
    SECTION("verify-content on a command that does not decode") {
        const Run run = forge("probe " + quoted(stream) + " verify-content");
        REQUIRE(run.exit_code == 1);
        REQUIRE(contains(run.log, "verify-content is read by decode and monitor"));
    }
    SECTION("an option that only a mode reads, without the mode") {
        const Run run = forge("decode " + quoted(stream) + " " + quoted(out) +
                              " trust-anchor=" + quoted(signer.root));
        REQUIRE(run.exit_code == 1);
        REQUIRE(contains(run.log, "trust-anchor= is read by verify-content and gate-content"));
    }
    SECTION("a time that is not ISO 8601 UTC") {
        const Run run = forge("decode " + quoted(stream) + " " + quoted(out) +
                              " verify-content verify-time=tomorrow");
        REQUIRE(run.exit_code == 1);
        REQUIRE(contains(run.log, "verify-time= needs a UTC time"));
    }
    SECTION("a profile that is neither spec nor any") {
        const Run run = forge("decode " + quoted(stream) + " " + quoted(out) +
                              " verify-content signer-profile=vendor");
        REQUIRE(run.exit_code == 1);
        REQUIRE(contains(run.log, "signer-profile= is spec or any"));
    }
}

TEST_CASE("probe reports a signature without a key or any trust", "[cli][content_signing]") {
    const CleanEnvironment environment;
    const Signer signer{"probe"};

    SECTION("an elementary stream with a sidecar, as a table and as JSON") {
        const SignedStream signed_stream = sign_elementary(signer, "probe");
        const Run table = forge("probe " + quoted(signed_stream.stream));
        REQUIRE(table.exit_code == 0);
        REQUIRE(contains(table.log, "content sig."));
        REQUIRE(contains(table.log, "carrier: elementary stream with sidecar"));
        REQUIRE(contains(table.log, "signer serial: 1798A2B5D62000000005"));
        REQUIRE(contains(table.log, "signed: 2026-05-01T00:00:00Z"));
        REQUIRE(contains(table.log, "not covered by the signature"));
        // Each certificate's SHA-1 fingerprint, as openssl prints it, for comparing with a
        // published one.
        for (const auto* der : {&signer.authority.root.der, &signer.authority.intermediate.der,
                                &signer.authority.leaf.der}) {
            const auto certificate = crypto::parse_certificate(*der);
            REQUIRE(certificate.has_value());
            REQUIRE(contains(table.log, "SHA-1 " + certificate->sha1_fingerprint()));
        }

        const Run json = forge("probe " + quoted(signed_stream.stream) + " json=1");
        REQUIRE(json.exit_code == 0);
        REQUIRE(contains(json.log, "\"content_signature\""));
        REQUIRE(contains(json.log, "\"signer_serial\": \"1798A2B5D62000000005\""));
        REQUIRE(contains(json.log, "\"sha1\""));
    }
    SECTION("an MP4") {
        const fs::path mp4 = sign_mp4(signer, "probe-mp4");
        const Run run = forge("probe " + quoted(mp4));
        REQUIRE(run.exit_code == 0);
        REQUIRE(contains(run.log, "carrier: MP4 uuid box"));
        REQUIRE(contains(run.log, "signer serial: 1798A2B5D62000000005"));
    }
    SECTION("an unsigned stream says nothing of signatures, and JSON has a null") {
        const fs::path plain = scratch_dir() / "probe-plain.ac4";
        REQUIRE(
            forge("ac4-encode " + quoted(tones_wav()) + " " + quoted(plain) + " 128").exit_code ==
            0);
        const Run table = forge("probe " + quoted(plain));
        REQUIRE(table.exit_code == 0);
        REQUIRE_FALSE(contains(table.log, "content sig."));
        const Run json = forge("probe " + quoted(plain) + " json=1");
        REQUIRE(json.exit_code == 0);
        REQUIRE(contains(json.log, "\"content_signature\": null"));
    }
    SECTION("an AC-3 stream's JSON has the member too, null, as the schema promises") {
        const fs::path ac3 = scratch_dir() / "probe-ac3.ac3";
        REQUIRE(forge("encode " + quoted(tones_wav()) + " " + quoted(ac3) + " 192").exit_code == 0);
        const Run json = forge("probe " + quoted(ac3) + " json=1");
        REQUIRE(json.exit_code == 0);
        REQUIRE(contains(json.log, "\"content_signature\": null"));
    }
    SECTION("a sidecar that does not parse is reported, and the stream is still probed") {
        const fs::path stream = scratch_dir() / "probe-junk.ac4";
        REQUIRE(
            forge("ac4-encode " + quoted(tones_wav()) + " " + quoted(stream) + " 128").exit_code ==
            0);
        write_bytes(scratch_dir() / "probe-junk.ac4.sig", Bytes(64, std::byte{0x33}));
        const Run run = forge("probe " + quoted(stream));
        REQUIRE(run.exit_code == 0);
        REQUIRE(contains(run.log, "does not parse"));
    }
}

TEST_CASE("a stream read from standard input is verified with a named signature file",
          "[cli][content_signing]") {
    const CleanEnvironment environment;
    const Signer signer{"stdin"};
    const SignedStream signed_stream = sign_elementary(signer, "stdin");
    const fs::path out = wav_out("stdin");
    const Run run = forge("decode - " + quoted(out) + " verify-content " + signer.verify_args() +
                              " content-signature=" + quoted(signed_stream.sidecar),
                          quoted(signed_stream.stream));
    REQUIRE(run.exit_code == 0);
    REQUIRE(contains(run.log, "content signature: verified"));
    REQUIRE(fs::exists(out));

    // With no name there is nowhere to look, and the stream is unsigned.
    const fs::path refused = wav_out("stdin-unnamed");
    const Run unnamed =
        forge("decode - " + quoted(refused) + " verify-content " + signer.verify_args(),
              quoted(signed_stream.stream));
    REQUIRE(unnamed.exit_code == 2);
    REQUIRE(contains(unnamed.log, "carries no content signature"));
    REQUIRE_FALSE(fs::exists(refused));
}
