#include "content_signing_cli.hpp"

#include <fmt/base.h>
#include <fmt/format.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "ac4_content_signing.hpp"
#include "exit_codes.hpp"
#include "iclforge/base/crypto/content_signature.hpp"
#include "mp4_signature_carrier.hpp"
#include "support.hpp"

namespace forge_cli {

namespace {

namespace crypto = iclforge::base::crypto;

// The system clock in Unix seconds. The only place in forge that reads the clock for content
// signing: the helpers it calls take the time as an argument, so the tests do not depend on it.
std::int64_t clock_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool is_one_of(std::string_view command, std::initializer_list<std::string_view> names) {
    for (const std::string_view name : names) {
        if (command == name) {
            return true;
        }
    }
    return false;
}

// "ac4-encode, mp4 and fmp4" for the sentences that list commands.
constexpr std::string_view kSigningCommands =
    "ac4-encode, mp4 and fmp4 (with an AC-4 input), and atmos-encode, atmos-adm and atmos-iab "
    "with codec=ac4";

}  // namespace

// --- The options and the command -------------------------------------------------------------

bool content_options_fit_command(const Options& meta, std::string_view command) {
    const bool signs =
        is_one_of(command, {"ac4-encode", "mp4", "fmp4", "atmos-encode", "atmos-adm", "atmos-iab"});
    const bool writes_ac4 =
        signs && (!is_one_of(command, {"atmos-encode", "atmos-adm", "atmos-iab"}) ||
                  meta.take_codec == iclforge::ac3::plan::Codec::kAc4);
    if (meta.sign_content && !writes_ac4) {
        fmt::println(stderr, "error: sign-content signs AC-4 content and is read by {}; {}",
                     kSigningCommands,
                     signs ? fmt::format("{} writes E-AC-3 unless codec=ac4 is given, and "
                                         "E-AC-3's objects are signed by sign-objects",
                                         command)
                           : fmt::format("{} is not one of them", command));
        return false;
    }
    // The other way round: the E-AC-3 option on the AC-4 command, which would otherwise sign
    // nothing and say so only by the absence of a message.
    if (meta.sign_objects && command == "ac4-encode") {
        fmt::println(stderr,
                     "error: sign-objects signs E-AC-3's EMDF object container; ac4-encode writes "
                     "AC-4, which sign-content signs whole (docs/concepts/content-signing.md)");
        return false;
    }
    if (meta.verify_content && !is_one_of(command, {"decode", "monitor"})) {
        fmt::println(stderr, "error: verify-content is read by decode and monitor, not {}",
                     command);
        return false;
    }
    if (meta.gate_content && command != "decode") {
        fmt::println(stderr,
                     "error: gate-content is read by decode, not {} (monitor takes verify-content)",
                     command);
        return false;
    }
    // Options that only one mode reads. Saying so beats a flag that does nothing, which reads
    // exactly like one that worked.
    const bool verifies = meta.verify_content || meta.gate_content;
    const auto only = [&](bool given, std::string_view what, bool applies, std::string_view modes) {
        if (given && !applies) {
            fmt::println(stderr, "error: {} is read by {}, and none of them was given", what,
                         modes);
            return false;
        }
        return true;
    };
    if (!only(!meta.signing_chain.empty(), "signing-chain=", meta.sign_content, "sign-content") ||
        !only(meta.sign_time.has_value(), "sign-time=", meta.sign_content, "sign-content") ||
        !only(!meta.trust_anchors.empty(), "trust-anchor=", verifies,
              "verify-content and gate-content") ||
        !only(!meta.trust_keys.empty(), "trust-key=", verifies,
              "verify-content and gate-content") ||
        !only(meta.verify_time.has_value(), "verify-time=", verifies,
              "verify-content and gate-content") ||
        !only(meta.allow_expired_intermediates, "allow-expired-intermediates", verifies,
              "verify-content and gate-content") ||
        !only(meta.signer_profile_any || meta.signer_title.has_value() ||
                  meta.signer_cn.has_value() || meta.signer_serial.has_value(),
              "signer-profile=, signer-title=, signer-cn= and signer-serial=", verifies,
              "verify-content and gate-content") ||
        !only(!meta.content_signature.empty(), "content-signature=", verifies || meta.sign_content,
              "sign-content, verify-content and gate-content")) {
        return false;
    }
    return true;
}

// --- Signing ---------------------------------------------------------------------------------

std::expected<ContentSigner, int> prepare_content_signer(const Options& meta,
                                                         std::string_view out_path, bool to_mp4) {
    ContentSigner signer;
    if (!meta.sign_content) {
        return signer;
    }
    // Verifying takes several trust entries; signing is one key, and naming two would leave it
    // unsaid which of them signed.
    if (meta.signing_keys.size() > 1) {
        fmt::println(stderr,
                     "error: sign-content signs with one key, and signing-key= was given {} times",
                     meta.signing_keys.size());
        return std::unexpected(kExitUsage);
    }
    if (to_mp4 && !meta.content_signature.empty()) {
        fmt::println(stderr,
                     "error: content-signature= names the file beside an elementary stream; an "
                     "MP4 carries its signature in a uuid box of the track");
        return std::unexpected(kExitUsage);
    }
    if (!to_mp4) {
        signer.sidecar_ = !meta.content_signature.empty() ? meta.content_signature
                                                          : std::string{out_path} + ".sig";
        if (is_stdio_path(out_path) && meta.content_signature.empty()) {
            fmt::println(stderr,
                         "error: sign-content on a stream written to standard output has no "
                         "file name to put .sig after: give content-signature=<path>");
            return std::unexpected(kExitUsage);
        }
        if (is_stdio_path(signer.sidecar_)) {
            fmt::println(stderr, "error: content-signature= is a file, not standard output");
            return std::unexpected(kExitUsage);
        }
    }
    auto credentials = iclforge::apps::load_content_signer(
        meta.signing_keys.empty() ? std::string_view{}
                                  : std::string_view{meta.signing_keys.front()},
        meta.signing_chain);
    if (!credentials.has_value()) {
        fmt::println(stderr, "error: {}", credentials.error());
        return std::unexpected(kExitInput);
    }
    signer.active_ = true;
    signer.to_mp4_ = to_mp4;
    signer.credentials_ = std::move(*credentials);
    signer.signed_unix_ = meta.sign_time.value_or(clock_unix());
    return signer;
}

bool ContentSigner::sign(std::span<const iclforge::ac4::EncodedFrame> frames) {
    if (!active_) {
        return true;
    }
    auto payload = iclforge::apps::sign_encoded_frames(frames, credentials_, signed_unix_);
    if (!payload.has_value()) {
        fmt::println(stderr, "error: {}", payload.error());
        return false;
    }
    payload_ = std::move(*payload);
    return true;
}

bool ContentSigner::sign(std::span<const std::span<const std::byte>> samples) {
    if (!active_) {
        return true;
    }
    auto payload = iclforge::apps::sign_ac4_samples(samples, credentials_, signed_unix_);
    if (!payload.has_value()) {
        fmt::println(stderr, "error: {}", payload.error());
        return false;
    }
    payload_ = std::move(*payload);
    return true;
}

std::vector<iclforge::containers::mp4::UserBox> ContentSigner::user_boxes() const {
    if (!active_ || !to_mp4_) {
        return {};
    }
    return {iclforge::apps::content_signature_user_box(payload_)};
}

bool ContentSigner::write_sidecar() const {
    if (!active_ || to_mp4_) {
        return true;
    }
    const std::vector<std::vector<std::byte>> chunks{payload_};
    if (!write_frames(sidecar_, chunks)) {
        return false;
    }
    return true;
}

void ContentSigner::report(FILE* status) const {
    if (!active_) {
        return;
    }
    status_println(status, "  content signature: {}",
                   iclforge::apps::describe_signer(credentials_, signed_unix_));
    if (to_mp4_) {
        status_println(status, "                     in a uuid box of the AC-4 track ({} bytes)",
                       payload_.size());
    } else {
        status_println(status,
                       "                     in {} ({} bytes); it has to stay with the stream",
                       sidecar_, payload_.size());
    }
}

// --- Verification ----------------------------------------------------------------------------

iclforge::apps::ContentTrustSettings content_trust_settings(const Options& meta,
                                                            std::string_view option) {
    iclforge::apps::ContentTrustSettings settings;
    settings.option = std::string{option};
    settings.anchor_paths = meta.trust_anchors;
    settings.key_paths = meta.trust_keys;
    settings.verify_unix = meta.verify_time.value_or(clock_unix());
    settings.allow_expired_intermediates = meta.allow_expired_intermediates;
    settings.profile_any = meta.signer_profile_any;
    settings.title = meta.signer_title;
    settings.common_name = meta.signer_cn;
    settings.serial_hex = meta.signer_serial;
    return settings;
}

ContentDecision judge_content(const Options& meta, std::string_view in_path,
                              std::span<const std::byte> file, FILE* status) {
    ContentDecision decision;
    if (!meta.verify_content && !meta.gate_content) {
        return decision;
    }
    const bool gate = meta.gate_content;
    const std::string_view option = gate ? "gate-content" : "verify-content";
    const auto refuse = [&decision](int code) {
        decision.proceed = false;
        decision.exit_code = code;
        return decision;
    };

    // The operator's side first: nothing about the stream is read until there is something to
    // trust, so a missing anchor is said before anything else is.
    const iclforge::apps::ContentTrustSettings settings = content_trust_settings(meta, option);
    const auto trust = iclforge::apps::load_content_trust(settings);
    if (!trust.has_value()) {
        fmt::println(stderr, "error: {}", trust.error());
        return refuse(kExitInput);
    }

    // The sidecar of an elementary stream. Named explicitly it has to exist (a typo is not an
    // unsigned stream); by default it is looked for beside the input, and its absence is the
    // stream being unsigned. An MP4 is read for its own box and the sidecar is not consulted.
    std::vector<std::byte> sidecar;
    const bool named = !meta.content_signature.empty();
    const std::string sidecar_path =
        named ? meta.content_signature
              : (is_stdio_path(in_path) ? std::string{} : std::string{in_path} + ".sig");
    if (!sidecar_path.empty()) {
        std::error_code ec;
        if (std::filesystem::exists(sidecar_path, ec)) {
            sidecar = read_all(sidecar_path);
            if (sidecar.empty()) {
                fmt::println(
                    stderr,
                    "error: {}: cannot read the signature file {} (it is empty or unreadable)",
                    option, sidecar_path);
                return refuse(kExitInput);
            }
        } else if (named) {
            fmt::println(stderr,
                         "error: {}: cannot read the signature file {} (content-signature=)",
                         option, sidecar_path);
            return refuse(kExitInput);
        }
    }

    const iclforge::apps::ContentCheck check = iclforge::apps::check_ac4_content(
        file, sidecar, *trust, iclforge::apps::verify_options_of(settings));
    if (check.outcome == iclforge::apps::ContentOutcome::kVerified) {
        decision.verified = true;
        status_println(status, "  content signature: verified ({}, {} frames)", check.carrier,
                       check.frames);
        for (const std::string& line : iclforge::apps::describe_check(check)) {
            status_println(status, "                     {}", line);
        }
        return decision;
    }

    const std::string why = check.error_name.empty()
                                ? check.message
                                : fmt::format("{}: {}", check.error_name, check.message);
    if (gate) {
        // The licensed policy: carry on, and say what is being given up. Whether anything is,
        // depends on the stream (a presentation without objects loses nothing); the decoder says
        // so when it is asked for objects it was told to leave out.
        fmt::println(stderr,
                     "warning: gate-content: {} did not verify ({}); decoding it without its "
                     "objects",
                     in_path, why);
        return decision;
    }
    const std::string_view kind = check.outcome == iclforge::apps::ContentOutcome::kUnsigned
                                      ? "carries no content signature"
                                  : check.outcome == iclforge::apps::ContentOutcome::kUnreadable
                                      ? "could not be read for a signature"
                                      : "has a content signature that does not verify";
    fmt::println(stderr, "error: verify-content: {} {} ({})", in_path, kind, why);
    return refuse(kExitInput);
}

}  // namespace forge_cli
