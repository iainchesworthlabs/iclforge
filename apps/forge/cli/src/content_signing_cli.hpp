#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac4_content_signing.hpp"
#include "iclforge/ac4/encoder/encoder.hpp"
#include "iclforge/base/crypto/credentials.hpp"
#include "iclforge/containers/mp4/mp4.hpp"
#include "support.hpp"

// The command-line half of AC-4 content signing (pace-ccjf1): what sign-content, verify-content
// and gate-content do with the Options, where each command puts a signature and what it prints.
// The decisions that do not depend on a command line (loading keys and trust, choosing a carrier,
// judging a stream, wording a verdict) are ac4_content_signing.hpp's, which has no Options in it
// so that a test can call it without a command line. The commands that sign or verify are
// commands/ac4_encode.cpp, ac4_encode_objects.cpp, atmos.cpp and containers.cpp (sign) and
// commands/decode.cpp and live_audio.cpp (verify and gate).
//
// The scheme, what it protects and what it does not: docs/concepts/content-signing.md.

namespace forge_cli {

// Checks the content-signing options against the command that was given, once, before any
// command runs, so that an option no command would read is said to be useless and not dropped:
// sign-content on a command that writes no AC-4, verify-content on one that does not decode,
// and the options that only one mode reads (trust-anchor=, signing-chain=, ...) without that
// mode. Prints the reason and returns false for a usage error. `command` is the first positional
// argument. Whether the input of `mp4` or `fmp4` is AC-4 is the command's to check, because only
// the input says.
[[nodiscard]] bool content_options_fit_command(const Options& meta, std::string_view command);

// --- Signing ------------------------------------------------------------------------------------

// sign-content for one output: the operator's credentials, loaded before the command encodes
// anything (a missing key is found in the first second and not after an hour of encoding), the
// time to record, and where the signature goes. An inactive signer (sign-content was not given)
// is a value that does nothing in every call, so a command does not branch on it.
class ContentSigner {
   public:
    [[nodiscard]] bool active() const noexcept { return active_; }

    // Signs the frames that will be written: the raw frames, without sync word, size or CRC, which
    // are also an MP4's samples. Prints the reason and returns false on failure. Call once.
    [[nodiscard]] bool sign(std::span<const iclforge::ac4::EncodedFrame> frames);
    [[nodiscard]] bool sign(std::span<const std::span<const std::byte>> samples);

    // The `uuid` box for an MP4 output (AudioTrack::user_boxes); empty when inactive. Valid after
    // sign().
    [[nodiscard]] std::vector<iclforge::containers::mp4::UserBox> user_boxes() const;

    // Writes the sidecar of an elementary stream that has been written; does nothing for an MP4
    // output or an inactive signer. Prints the reason and returns false on failure. Valid after
    // sign(), and after the stream itself is on disk, so a failed stream write leaves no
    // signature beside nothing.
    [[nodiscard]] bool write_sidecar() const;

    // The status lines after a signed output: who signed, when, and where the signature is.
    void report(FILE* status) const;

   private:
    friend std::expected<ContentSigner, int> prepare_content_signer(const Options& meta,
                                                                    std::string_view out_path,
                                                                    bool to_mp4);
    bool active_ = false;
    bool to_mp4_ = false;
    iclforge::base::crypto::SigningCredentials credentials_;
    std::int64_t signed_unix_ = 0;
    std::string sidecar_;
    std::vector<std::byte> payload_;
};

// Loads what sign-content needs, or returns an inactive signer when it was not given. `to_mp4` is
// whether the output is an MP4 (the signature goes inside) and not an elementary stream (the
// signature goes to content-signature=, else `out_path` + ".sig"). The error is the exit code,
// after the reason has been printed: kExitUsage for options that do not fit and kExitInput for a
// key or chain that cannot be used.
[[nodiscard]] std::expected<ContentSigner, int> prepare_content_signer(const Options& meta,
                                                                       std::string_view out_path,
                                                                       bool to_mp4);

// --- Verification -------------------------------------------------------------------------------

struct ContentDecision {
    // False: the command ends with `exit_code`, and the reason has been printed.
    bool proceed = true;
    int exit_code = 0;
    // The content verified under the operator's trust. For gate-content this decides whether the
    // decoder is given its objects; for verify-content it is true whenever proceed is.
    bool verified = false;
};

// verify-content and gate-content for `file`, the AC-4 input as it lies on disk (an MP4 with its
// `uuid` box, or an elementary stream, whose sidecar is content-signature= or `in_path` + ".sig").
// Neither option given: proceeds and checks nothing. verify-content refuses a stream that is
// unsigned, altered, signed by someone the operator does not trust or unreadable; gate-content
// carries on and says that the stream did not verify. A verified stream says who signed it and
// which trust entry accepted it. Status lines go to `status`, warnings and errors to stderr.
[[nodiscard]] ContentDecision judge_content(const Options& meta, std::string_view in_path,
                                            std::span<const std::byte> file, FILE* status);

// The settings the Options give the helpers in ac4_content_signing.hpp. `option` is the word the
// sentences use for what was asked ("verify-content" or "gate-content").
[[nodiscard]] iclforge::apps::ContentTrustSettings content_trust_settings(const Options& meta,
                                                                          std::string_view option);

}  // namespace forge_cli
