#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/ac4/encoder/encoder.hpp"
#include "iclforge/ac4/signing/carrier.hpp"
#include "iclforge/base/crypto/content_signature.hpp"
#include "iclforge/base/crypto/credentials.hpp"
#include "iclforge/base/crypto/x509.hpp"

// What forge's `sign-content`, `verify-content` and `gate-content` (and `probe`) share, so that
// the front ends that offer them say the same things and make the same decisions: loading the
// operator's key, chain and trust, choosing the carrier a file needs, running the check, and
// putting the result in words. The cryptography is iclforge::base::crypto's, the signed span and
// the elementary carrier are iclforge::ac4::signing's, and the MP4 carrier is mp4_signature_
// carrier.hpp's. It is in the forge command line's sources, not in apps/shared/media, because it
// joins iclforge::base, iclforge::ac4 and iclforge::containers, and the layering table
// (tools/checks/projects.json) lets an application use all three and the shared media code only
// the codecs and containers.
//
// Nothing here reads a clock or a file by itself: the caller passes the time and the bytes, so the
// decisions are the same under test as in the program, and a command that reads a file once can
// give it to both the check and the decoder.
//
// See docs/concepts/content-signing.md for the scheme and what it does and does not protect.

namespace iclforge::apps {

// --- Signing ------------------------------------------------------------------------------------

// The key and chain for sign-content: `key_path` and `chain_path`, else the environment
// (ICLFORGE_CONTENT_SIGNING_KEY_FILE or _KEY, and ICLFORGE_CONTENT_SIGNING_CHAIN_FILE). The chain
// comes back ordered, root end first, with the leaf checked to hold the key's public key. The
// error is a sentence naming the option or variable that was missing, or the source that was bad.
[[nodiscard]] std::expected<iclforge::base::crypto::SigningCredentials, std::string>
load_content_signer(std::string_view key_path, std::string_view chain_path);

// The serialised ContentSignature for `frames`, signed at `signed_unix` (the caller's clock, or
// sign-time=). The signed span is the frames' raw_ac4_frame in order (ac4/signing/
// content_signing.hpp). The error is a sentence.
[[nodiscard]] std::expected<std::vector<std::byte>, std::string> sign_encoded_frames(
    std::span<const iclforge::ac4::EncodedFrame> frames,
    const iclforge::base::crypto::SigningCredentials& credentials, std::int64_t signed_unix);

// One line saying who signed and when, for the status output of a command that signed:
// "signed as serial 1798A2B5D62000000005 (CN=ILok, ...) at 2026-10-29T00:00:00Z".
[[nodiscard]] std::string describe_signer(
    const iclforge::base::crypto::SigningCredentials& credentials, std::int64_t signed_unix);

// --- Verification -------------------------------------------------------------------------------

// What a verifying command is told by its options. The trust is loaded from the paths, or from
// $ICLFORGE_CONTENT_TRUST_FILE when no path was given (load_trust_store); there is no default
// that trusts anything.
struct ContentTrustSettings {
    std::vector<std::string> anchor_paths;  // trust-anchor=
    std::vector<std::string> key_paths;     // trust-key=
    // The time chain validity is judged at: verify-time=, else the clock.
    std::int64_t verify_unix = 0;
    bool allow_expired_intermediates = false;  // allow-expired-intermediates
    // signer-profile=: false is "spec" (the specification's title, name and serial), true "any".
    bool profile_any = false;
    // signer-title=, signer-cn=, signer-serial=: each replaces one field of the profile.
    std::optional<std::string> title;
    std::optional<std::string> common_name;
    std::optional<std::string> serial_hex;
};

// The trust the settings name, loaded. The error is a sentence naming the option, variable or
// file at fault; it is the operator's to fix, and no stream is read before it.
[[nodiscard]] std::expected<iclforge::base::crypto::TrustStore, std::string> load_content_trust(
    const ContentTrustSettings& settings);

[[nodiscard]] iclforge::base::crypto::VerifyOptions verify_options_of(
    const ContentTrustSettings& settings);

enum class ContentOutcome : std::uint8_t {
    kVerified,    // signed, and the signature verifies under the operator's trust
    kUnsigned,    // the stream carries no content signature
    kRejected,    // a signature is present and does not hold (see error_name)
    kUnreadable,  // the stream could not be taken apart, so nothing was judged
};

struct ContentCheck {
    ContentOutcome outcome = ContentOutcome::kUnreadable;
    // The ContentSignatureError (base/crypto/content_signature.hpp) as error_name() spells it, for
    // kUnsigned and kRejected; empty otherwise. Stable: scripts match it.
    std::string error_name;
    // A sentence for a person: the detail of the failure, or the carrier's reason.
    std::string message;
    // Which carrier read the file ("elementary stream with sidecar", "MP4 uuid box").
    std::string carrier;
    std::size_t frames = 0;
    // Set for kVerified.
    std::optional<iclforge::base::crypto::VerifiedSignature> verified;
    // For kVerified, which anchor or key accepted: "anchor 2 (root.pem)", "trusted key 1
    // (pin.pem)".
    std::string accepted_by;
};

// Judges the AC-4 content in `file`: an MP4 (read through its `uuid` box) or an elementary stream
// (read with `sidecar`, the bytes of its `.sig` file, empty when there is none). Matroska and
// MPEG-TS are kUnreadable: they carry no content signature (planning/ac4-content-signing.md).
// The trust store's labels (the file names the operator gave) are what "accepted_by" names.
[[nodiscard]] ContentCheck check_ac4_content(std::span<const std::byte> file,
                                             std::span<const std::byte> sidecar,
                                             const iclforge::base::crypto::TrustStore& trust,
                                             const iclforge::base::crypto::VerifyOptions& options);

// What the verification found, one sentence a line, for the status output of a command that
// verified: the signer, the signing date (and that the signature does not cover it), the trust
// entry that accepted, and any expired intermediate accepted under the relaxation.
[[nodiscard]] std::vector<std::string> describe_check(const ContentCheck& check);

// --- Inspection (no key, no trust) --------------------------------------------------------------

struct SignatureInspection {
    // False when the stream carries no signature (or none could be found).
    bool present = false;
    // Why nothing could be read, or why the signature does not parse; empty when it does.
    std::string problem;
    // Lines for `probe`: the carrier, the signer's serial number and subject, the signed date, and
    // each certificate's SHA-1 fingerprint, which an operator can compare with the fingerprints a
    // signer publishes. Says nothing about whether the signature is valid: only verify-content,
    // with the operator's trust, does.
    std::vector<std::string> lines;
};

[[nodiscard]] SignatureInspection inspect_ac4_signature(std::span<const std::byte> file,
                                                        std::span<const std::byte> sidecar);

}  // namespace iclforge::apps
