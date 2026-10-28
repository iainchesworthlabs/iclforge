// The shared half of forge's sign-content, verify-content and gate-content
// (ac4_content_signing.hpp): loading the operator's key, chain and trust from files and from the
// environment, choosing a carrier by what a file is, the verdict for each way a stream can stand
// (signed, unsigned, altered, signed by someone else, not readable), and the words the commands
// print. The stream is the encoder's (ac4_signing_fixture.hpp); keys and chains are built from a
// seed at run time (tests/support/test_pki.hpp), written to the scratch directory as PEM, and
// read back the way an operator's files are.

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
#include <utility>
#include <vector>

#include "ac4_content_signing.hpp"
#include "ac4_signing_fixture.hpp"
#include "iclforge/base/crypto/content_signature.hpp"
#include "iclforge/base/crypto/credentials.hpp"
#include "iclforge/containers/mp4/mp4.hpp"
#include "iclforge/containers/mp4/reader.hpp"
#include "mp4_signature_carrier.hpp"
#include "platform/process.hpp"
#include "test_pki.hpp"

namespace {

namespace crypto = iclforge::base::crypto;
namespace pki = iclforge::test::pki;
namespace media = iclforge::test::media_signing;
namespace mp4 = iclforge::containers::mp4;
namespace fs = std::filesystem;
using iclforge::apps::ContentOutcome;
using Bytes = std::vector<std::byte>;

constexpr std::int64_t kNow = 1793232000;  // 2026-10-29T00:00:00Z

fs::path scratch(const std::string& leaf) {
    const fs::path dir = fs::path{ICLFORGE_TEST_SCRATCH_DIR} /
                         ("content_signing_" + iclforge::test::platform::process_id());
    fs::create_directories(dir);
    return dir / leaf;
}

std::string write_file(const std::string& leaf, const std::string& content) {
    const fs::path path = scratch(leaf);
    std::ofstream out{path, std::ios::binary};
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return path.string();
}

// The variables the loaders read, cleared at the start and the end of a case, so the machine's own
// environment cannot change what a case sees.
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

// The operator's files: a key, the chain with the leaf first (an order the loader has to sort
// out), and the root as a trust anchor, under names a person would give them.
struct Files {
    pki::Authority authority = pki::make_authority();
    std::string key = write_file(
        "leaf.key", crypto::encode_pem("PRIVATE KEY", pki::pkcs8_der(authority.leaf.keys)));
    std::string chain =
        write_file("chain.pem", crypto::encode_pem("CERTIFICATE", authority.leaf.der) +
                                    crypto::encode_pem("CERTIFICATE", authority.intermediate.der) +
                                    crypto::encode_pem("CERTIFICATE", authority.root.der));
    std::string root =
        write_file("root.pem", crypto::encode_pem("CERTIFICATE", authority.root.der));
    std::string other_root = write_file(
        "other-root.pem",
        crypto::encode_pem("CERTIFICATE", pki::make_authority({.seed = "other"}).root.der));

    [[nodiscard]] iclforge::apps::ContentTrustSettings trust(std::string anchor) const {
        iclforge::apps::ContentTrustSettings settings;
        settings.anchor_paths = {std::move(anchor)};
        settings.verify_unix = kNow;
        return settings;
    }
};

// Signs the shared stream the way sign-content does: load the key and chain from the files, sign
// the frames at a fixed time.
Bytes sign(const Files& files, const media::Encoded& stream) {
    const auto credentials = iclforge::apps::load_content_signer(files.key, files.chain);
    REQUIRE(credentials.has_value());
    const auto payload =
        iclforge::apps::sign_encoded_frames(stream.frames, *credentials, pki::at(2026, 10, 28, 12));
    REQUIRE(payload.has_value());
    return *payload;
}

Bytes mp4_with(const media::Encoded& stream, const Bytes& payload) {
    mp4::AudioTrack track = stream.track;
    track.user_boxes = {iclforge::apps::content_signature_user_box(payload)};
    const auto file = mp4::mux(track, stream.views());
    REQUIRE(file.has_value());
    return *file;
}

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("sign-content's key and chain load from files, in any chain order",
          "[ac4][signing][content_cli]") {
    const CleanEnvironment environment;
    const Files files;
    const auto credentials = iclforge::apps::load_content_signer(files.key, files.chain);
    REQUIRE(credentials.has_value());
    // Root end first, leaf last, whatever order the file had.
    REQUIRE(credentials->chain.size() == 3);
    REQUIRE(credentials->chain.front().der == files.authority.root.der);
    REQUIRE(credentials->chain.back().der == files.authority.leaf.der);

    const std::string said =
        iclforge::apps::describe_signer(*credentials, pki::at(2026, 10, 28, 12));
    REQUIRE(contains(said, "1798A2B5D62000000005"));
    REQUIRE(contains(said, "2026-10-28T12:00:00Z"));
    REQUIRE(contains(said, "3 certificates"));
}

TEST_CASE("sign-content names what is missing or wrong", "[ac4][signing][content_cli]") {
    const CleanEnvironment environment;
    const Files files;

    SECTION("nothing offered: the options and the variables are named") {
        const auto none = iclforge::apps::load_content_signer("", "");
        REQUIRE_FALSE(none.has_value());
        REQUIRE(contains(none.error(), "sign-content"));
        REQUIRE(contains(none.error(), "signing-key=<path>"));
        REQUIRE(contains(none.error(), "ICLFORGE_CONTENT_SIGNING_KEY_FILE"));
        REQUIRE(contains(none.error(), "ICLFORGE_CONTENT_SIGNING_CHAIN_FILE"));
    }
    SECTION("a key and no chain") {
        const auto no_chain = iclforge::apps::load_content_signer(files.key, "");
        REQUIRE_FALSE(no_chain.has_value());
        REQUIRE(contains(no_chain.error(), "chain"));
    }
    SECTION("a path that cannot be read is named, and is not skipped for a variable") {
        iclforge::test::platform::set_environment(crypto::kEnvContentSigningKeyFile, files.key);
        const std::string missing = scratch("missing.key").string();
        const auto bad = iclforge::apps::load_content_signer(missing, files.chain);
        REQUIRE_FALSE(bad.has_value());
        REQUIRE(contains(bad.error(), missing));
    }
    SECTION("a chain for another key is found when signing, not at the first verifier") {
        const pki::Authority other = pki::make_authority({.seed = "other"});
        const std::string other_chain = write_file(
            "other-chain.pem", crypto::encode_pem("CERTIFICATE", other.leaf.der) +
                                   crypto::encode_pem("CERTIFICATE", other.intermediate.der) +
                                   crypto::encode_pem("CERTIFICATE", other.root.der));
        const auto mismatch = iclforge::apps::load_content_signer(files.key, other_chain);
        REQUIRE_FALSE(mismatch.has_value());
    }
    SECTION("the environment supplies what the options leave out") {
        iclforge::test::platform::set_environment(crypto::kEnvContentSigningKeyFile, files.key);
        iclforge::test::platform::set_environment(crypto::kEnvContentSigningChainFile, files.chain);
        REQUIRE(iclforge::apps::load_content_signer("", "").has_value());
    }
    SECTION("signing nothing is refused") {
        const auto credentials = iclforge::apps::load_content_signer(files.key, files.chain);
        REQUIRE(credentials.has_value());
        const auto none = iclforge::apps::sign_encoded_frames({}, *credentials, kNow);
        REQUIRE_FALSE(none.has_value());
        REQUIRE(contains(none.error(), "no content"));
    }
}

TEST_CASE("verify-content's trust loads from paths and never from nothing",
          "[ac4][signing][content_cli]") {
    const CleanEnvironment environment;
    const Files files;

    SECTION("a certificate path is an anchor, labelled with the path") {
        const auto trust = iclforge::apps::load_content_trust(files.trust(files.root));
        REQUIRE(trust.has_value());
        REQUIRE(trust->anchors.size() == 1);
        REQUIRE(contains(trust->anchors[0].label, "root.pem"));
    }
    SECTION("nothing named and nothing in the environment: the operator is told what to pass") {
        iclforge::apps::ContentTrustSettings empty;
        empty.verify_unix = kNow;
        const auto none = iclforge::apps::load_content_trust(empty);
        REQUIRE_FALSE(none.has_value());
        REQUIRE(contains(none.error(), "trust-anchor=<certificate>"));
        REQUIRE(contains(none.error(), "trust-key=<public key>"));
        REQUIRE(contains(none.error(), "ICLFORGE_CONTENT_TRUST_FILE"));
        REQUIRE(contains(none.error(), "never checked against nothing"));
    }
    SECTION("the environment file is read only when no path was named") {
        iclforge::test::platform::set_environment(crypto::kEnvContentTrustFile, files.other_root);
        iclforge::apps::ContentTrustSettings empty;
        empty.verify_unix = kNow;
        const auto from_environment = iclforge::apps::load_content_trust(empty);
        REQUIRE(from_environment.has_value());
        REQUIRE(from_environment->anchors.size() == 1);
        REQUIRE(from_environment->anchors[0].certificate.der !=
                files.authority.root.der);  // the other authority's root

        const auto named = iclforge::apps::load_content_trust(files.trust(files.root));
        REQUIRE(named.has_value());
        REQUIRE(named->anchors.size() == 1);
        REQUIRE(named->anchors[0].certificate.der == files.authority.root.der);
    }
    SECTION("a file that is not a certificate or a key is named") {
        const std::string junk = write_file("junk.pem", "this is not a certificate\n");
        const auto trust = iclforge::apps::load_content_trust(files.trust(junk));
        REQUIRE_FALSE(trust.has_value());
        REQUIRE(contains(trust.error(), "junk.pem"));
    }
}

TEST_CASE("the leaf profile and the clock come from the settings", "[ac4][signing][content_cli]") {
    iclforge::apps::ContentTrustSettings settings;
    settings.verify_unix = kNow;

    crypto::VerifyOptions options = iclforge::apps::verify_options_of(settings);
    REQUIRE(options.chain.now_unix == kNow);
    REQUIRE_FALSE(options.chain.allow_expired_intermediates);
    // The specification's profile by default.
    REQUIRE(options.profile.title == std::string{crypto::kSpecSignerTitle});
    REQUIRE(options.profile.common_name == std::string{crypto::kSpecSignerCommonName});
    REQUIRE(options.profile.serial_hex == std::string{crypto::kSpecSignerSerialHex});

    settings.profile_any = true;
    settings.allow_expired_intermediates = true;
    options = iclforge::apps::verify_options_of(settings);
    REQUIRE(options.chain.allow_expired_intermediates);
    REQUIRE_FALSE(options.profile.title.has_value());
    REQUIRE_FALSE(options.profile.common_name.has_value());
    REQUIRE_FALSE(options.profile.serial_hex.has_value());

    // A replacement applies over either profile.
    settings.title = "Mastering";
    settings.serial_hex = "ABCDEF";
    options = iclforge::apps::verify_options_of(settings);
    REQUIRE(options.profile.title == std::string{"Mastering"});
    REQUIRE(options.profile.serial_hex == std::string{"ABCDEF"});
    REQUIRE_FALSE(options.profile.common_name.has_value());

    settings.profile_any = false;
    settings.common_name = "Studio";
    options = iclforge::apps::verify_options_of(settings);
    REQUIRE(options.profile.common_name == std::string{"Studio"});
    REQUIRE(options.profile.title == std::string{"Mastering"});
}

TEST_CASE("an elementary stream and its sidecar are judged, and the verdict says why",
          "[ac4][signing][content_cli]") {
    const CleanEnvironment environment;
    const Files files;
    const media::Encoded& stream = media::shared();
    const Bytes payload = sign(files, stream);
    const Bytes elementary = stream.elementary();
    const auto trust = iclforge::apps::load_content_trust(files.trust(files.root));
    REQUIRE(trust.has_value());
    const crypto::VerifyOptions options =
        iclforge::apps::verify_options_of(files.trust(files.root));

    SECTION("signed and trusted") {
        const auto check = iclforge::apps::check_ac4_content(elementary, payload, *trust, options);
        REQUIRE(check.outcome == ContentOutcome::kVerified);
        REQUIRE(check.carrier == "elementary stream with sidecar");
        REQUIRE(check.frames == stream.frames.size());
        REQUIRE(check.error_name.empty());
        REQUIRE(contains(check.accepted_by, "anchor 1"));
        REQUIRE(contains(check.accepted_by, "root.pem"));
        const auto lines = iclforge::apps::describe_check(check);
        REQUIRE(lines.size() == 3);
        REQUIRE(contains(lines[0], "1798A2B5D62000000005"));
        REQUIRE(contains(lines[1], "anchor 1"));
        REQUIRE(contains(lines[2], "2026-10-28T12:00:00Z"));
        REQUIRE(contains(lines[2], "does not cover the date"));
    }
    SECTION("no sidecar is an unsigned stream, not a failed check") {
        const auto check = iclforge::apps::check_ac4_content(elementary, {}, *trust, options);
        REQUIRE(check.outcome == ContentOutcome::kUnsigned);
        REQUIRE(check.error_name == "BadOrMissingSignature");
        REQUIRE(contains(check.message, "no sidecar file"));
        REQUIRE(iclforge::apps::describe_check(check).empty());
    }
    SECTION("an altered frame is rejected as an invalid signature") {
        Bytes altered = elementary;
        altered[altered.size() / 2] ^= std::byte{0x01};
        const auto check = iclforge::apps::check_ac4_content(altered, payload, *trust, options);
        // A flipped bit may land in a frame_size field, where the stream stops scanning; both are
        // a refusal, and neither is a pass.
        REQUIRE((check.outcome == ContentOutcome::kRejected ||
                 check.outcome == ContentOutcome::kUnreadable));
        if (check.outcome == ContentOutcome::kRejected) {
            REQUIRE(check.error_name == "SignatureInvalid");
        }
    }
    SECTION("a frame's payload bit altered in a stream without a CRC word is rejected") {
        // Offset into the second frame's body: past its 4-byte sync header.
        const std::size_t first = 4 + stream.frames[0].raw_ac4_frame.size();
        Bytes altered = elementary;
        altered[first + 4 + stream.frames[1].raw_ac4_frame.size() / 2] ^= std::byte{0x08};
        const auto check = iclforge::apps::check_ac4_content(altered, payload, *trust, options);
        REQUIRE(check.outcome == ContentOutcome::kRejected);
        REQUIRE(check.error_name == "SignatureInvalid");
        REQUIRE(contains(check.message, "changed after it was signed"));
    }
    SECTION("a signer the operator does not trust") {
        const auto other = iclforge::apps::load_content_trust(files.trust(files.other_root));
        REQUIRE(other.has_value());
        const auto check = iclforge::apps::check_ac4_content(elementary, payload, *other, options);
        REQUIRE(check.outcome == ContentOutcome::kRejected);
        REQUIRE(check.error_name == "BadCredentialsForSigningContent");
        // The chain failure says which check and which certificate.
        REQUIRE(contains(check.message, "certificate"));
    }
    SECTION("a stream that stops short is not read, and is not called unsigned") {
        Bytes tail = elementary;
        tail.insert(tail.end(), 10, std::byte{0x7E});
        const auto check = iclforge::apps::check_ac4_content(tail, payload, *trust, options);
        REQUIRE(check.outcome == ContentOutcome::kUnreadable);
        REQUIRE(contains(check.message, "loses sync"));
    }
    SECTION("an expired chain, strictly and under the relaxation") {
        const pki::Authority old = pki::make_authority({
            .seed = "old",
            .intermediate_not_after = pki::at(2026, 1, 1),
        });
        const std::string key =
            write_file("old.key", crypto::encode_pem("PRIVATE KEY", pki::pkcs8_der(old.leaf.keys)));
        const std::string chain = write_file(
            "old-chain.pem", crypto::encode_pem("CERTIFICATE", old.leaf.der) +
                                 crypto::encode_pem("CERTIFICATE", old.intermediate.der) +
                                 crypto::encode_pem("CERTIFICATE", old.root.der));
        const std::string root =
            write_file("old-root.pem", crypto::encode_pem("CERTIFICATE", old.root.der));
        const auto credentials = iclforge::apps::load_content_signer(key, chain);
        REQUIRE(credentials.has_value());
        const auto old_payload =
            iclforge::apps::sign_encoded_frames(stream.frames, *credentials, pki::at(2025, 6, 1));
        REQUIRE(old_payload.has_value());

        iclforge::apps::ContentTrustSettings settings = files.trust(root);
        const auto old_trust = iclforge::apps::load_content_trust(settings);
        REQUIRE(old_trust.has_value());

        const auto strict = iclforge::apps::check_ac4_content(
            elementary, *old_payload, *old_trust, iclforge::apps::verify_options_of(settings));
        REQUIRE(strict.outcome == ContentOutcome::kRejected);
        REQUIRE(strict.error_name == "BadCredentialsForSigningContent");

        settings.allow_expired_intermediates = true;
        const auto relaxed = iclforge::apps::check_ac4_content(
            elementary, *old_payload, *old_trust, iclforge::apps::verify_options_of(settings));
        REQUIRE(relaxed.outcome == ContentOutcome::kVerified);
        const auto lines = iclforge::apps::describe_check(relaxed);
        REQUIRE(lines.size() == 4);
        REQUIRE(contains(lines[3], "past its notAfter"));
        REQUIRE(contains(lines[3], "allow-expired-intermediates"));
    }
}

TEST_CASE("an MP4 is judged by its uuid box, and gives the elementary stream's verdict",
          "[ac4][signing][content_cli]") {
    const CleanEnvironment environment;
    const Files files;
    const media::Encoded& stream = media::shared();
    const Bytes payload = sign(files, stream);
    const auto trust = iclforge::apps::load_content_trust(files.trust(files.root));
    REQUIRE(trust.has_value());
    const crypto::VerifyOptions options =
        iclforge::apps::verify_options_of(files.trust(files.root));

    const Bytes signed_mp4 = mp4_with(stream, payload);
    const auto signed_check = iclforge::apps::check_ac4_content(signed_mp4, {}, *trust, options);
    REQUIRE(signed_check.outcome == ContentOutcome::kVerified);
    REQUIRE(signed_check.carrier == "MP4 uuid box");
    REQUIRE(signed_check.frames == stream.frames.size());

    // A sidecar given beside an MP4 is not consulted: the MP4's own box is the signature.
    const Bytes noise(40, std::byte{0x11});
    REQUIRE(iclforge::apps::check_ac4_content(signed_mp4, noise, *trust, options).outcome ==
            ContentOutcome::kVerified);

    mp4::AudioTrack bare = stream.track;
    const auto unsigned_mp4 = mp4::mux(bare, stream.views());
    REQUIRE(unsigned_mp4.has_value());
    const auto unsigned_check =
        iclforge::apps::check_ac4_content(*unsigned_mp4, payload, *trust, options);
    REQUIRE(unsigned_check.outcome == ContentOutcome::kUnsigned);

    // A sample altered inside the MP4.
    const auto demuxed = mp4::demux(signed_mp4);
    REQUIRE(demuxed.has_value());
    Bytes altered = signed_mp4;
    const auto sample = demuxed->samples[1];
    altered[static_cast<std::size_t>(sample.data() - signed_mp4.data()) + sample.size() / 2] ^=
        std::byte{0x02};
    const auto altered_check = iclforge::apps::check_ac4_content(altered, {}, *trust, options);
    REQUIRE(altered_check.outcome == ContentOutcome::kRejected);
    REQUIRE(altered_check.error_name == "SignatureInvalid");

    // The same signature in the sidecar of the elementary stream gives the same verdict.
    const auto elementary_check =
        iclforge::apps::check_ac4_content(stream.elementary(), payload, *trust, options);
    REQUIRE(elementary_check.outcome == signed_check.outcome);
    REQUIRE(elementary_check.accepted_by == signed_check.accepted_by);
}

TEST_CASE("Matroska and MPEG-TS are not read for a signature", "[ac4][signing][content_cli]") {
    const Files files;
    const auto trust = iclforge::apps::load_content_trust(files.trust(files.root));
    REQUIRE(trust.has_value());
    const crypto::VerifyOptions options =
        iclforge::apps::verify_options_of(files.trust(files.root));
    // The EBML magic is what sniffs as Matroska.
    Bytes ebml{std::byte{0x1A}, std::byte{0x45}, std::byte{0xDF}, std::byte{0xA3}};
    ebml.resize(64, std::byte{0});
    const auto check = iclforge::apps::check_ac4_content(ebml, {}, *trust, options);
    REQUIRE(check.outcome == ContentOutcome::kUnreadable);
    REQUIRE(contains(check.message, "Matroska and MPEG-TS carry no AC-4 content signature"));

    const auto inspection = iclforge::apps::inspect_ac4_signature(ebml, {});
    REQUIRE_FALSE(inspection.present);
    REQUIRE(contains(inspection.problem, "Matroska"));
}

TEST_CASE("probe's inspection reads a signature without a key or any trust",
          "[ac4][signing][content_cli]") {
    const CleanEnvironment environment;
    const Files files;
    const media::Encoded& stream = media::shared();
    const Bytes payload = sign(files, stream);

    for (const bool mp4_form : {false, true}) {
        CAPTURE(mp4_form);
        const Bytes file = mp4_form ? mp4_with(stream, payload) : stream.elementary();
        const auto inspection = iclforge::apps::inspect_ac4_signature(
            file, mp4_form ? std::span<const std::byte>{} : std::span<const std::byte>{payload});
        REQUIRE(inspection.present);
        REQUIRE(inspection.problem.empty());
        std::string all;
        for (const std::string& line : inspection.lines) {
            all += line + "\n";
        }
        REQUIRE(contains(all, mp4_form ? "MP4 uuid box" : "elementary stream with sidecar"));
        REQUIRE(contains(all, "signer serial: 1798A2B5D62000000005"));
        REQUIRE(contains(all, "signed: 2026-10-28T12:00:00Z"));
        REQUIRE(contains(all, "version: 3"));
        // Every certificate's SHA-1 fingerprint, as openssl prints it, root end first.
        std::size_t found = 0;
        for (const crypto::Certificate& certificate :
             {files.authority.root.cert, files.authority.intermediate.cert,
              files.authority.leaf.cert}) {
            REQUIRE(contains(all, "SHA-1 " + certificate.sha1_fingerprint()));
            ++found;
        }
        REQUIRE(found == 3);
    }

    SECTION("an unsigned stream is reported as that") {
        const auto inspection = iclforge::apps::inspect_ac4_signature(stream.elementary(), {});
        REQUIRE_FALSE(inspection.present);
        REQUIRE(inspection.problem.empty());
        REQUIRE(inspection.lines.size() == 1);
        REQUIRE(contains(inspection.lines[0], "no content signature"));
    }
    SECTION("a sidecar that does not parse is reported, with the stream still readable") {
        const Bytes junk(100, std::byte{0x42});
        const auto inspection = iclforge::apps::inspect_ac4_signature(stream.elementary(), junk);
        REQUIRE(inspection.present);
        REQUIRE(contains(inspection.problem, "does not parse"));
    }
}
