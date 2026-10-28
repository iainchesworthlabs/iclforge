#include "ac4_content_signing.hpp"

#include <fmt/format.h>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "container_input.hpp"
#include "iclforge/ac4/signing/content_signing.hpp"
#include "iclforge/base/crypto/content_signature.hpp"
#include "iclforge/base/crypto/credentials.hpp"
#include "iclforge/base/crypto/x509.hpp"
#include "mp4_signature_carrier.hpp"

namespace iclforge::apps {

namespace {

namespace crypto = iclforge::base::crypto;
namespace signing = iclforge::ac4::signing;

// "sign-content needs ..." for a key or chain that nothing offered, and the loader's own sentence
// (which already names the file or variable) for one that was offered and is bad.
std::string credential_sentence(std::string_view what, const crypto::CredentialError& error,
                                std::string_view hint) {
    if (error.kind == crypto::CredentialErrorKind::kAbsent) {
        return fmt::format("{} needs {}: {}; {}", what, "a key and a chain", error.message, hint);
    }
    return fmt::format("{}: {}", what, error.message);
}

// The title, common name and serial a certificate's subject carries, for a person to recognise it.
std::string subject_of(const crypto::Certificate& certificate) {
    return certificate.subject.to_string();
}

// "anchor 2 (root.pem)" or "trusted key 1 (pin.pem)": 1-based, as the AC-3 keyring reports its
// keys, and the label is the name the operator gave the file.
std::string accepted_by_of(const crypto::ChainAcceptance& acceptance,
                           const crypto::TrustStore& trust) {
    const bool anchor = acceptance.kind == crypto::ChainAcceptance::Kind::kAnchor;
    const std::size_t index = acceptance.trust_index;
    std::string label;
    if (anchor && index < trust.anchors.size()) {
        label = trust.anchors[index].label;
    } else if (!anchor && index < trust.keys.size()) {
        label = trust.keys[index].label;
    }
    return fmt::format("{} {}{}", anchor ? "anchor" : "trusted key", index + 1,
                       label.empty() ? std::string{} : fmt::format(" ({})", label));
}

ContentCheck unreadable(std::string carrier, const signing::CarrierError& error) {
    ContentCheck check;
    check.outcome = ContentOutcome::kUnreadable;
    check.carrier = std::move(carrier);
    check.message =
        error.detail.empty() ? std::string{signing::describe(error.kind)} : error.detail;
    return check;
}

// Which carrier reads a file, by its first bytes (container_input.hpp's sniff, as every command
// that opens a media file does it, and not by its name).
enum class Reading : std::uint8_t { kElementary, kMp4, kNoSignatureContainer };

Reading reading_of(std::span<const std::byte> file) {
    switch (sniff_container(file)) {
        case ContainerKind::kMp4:
            return Reading::kMp4;
        case ContainerKind::kMatroska:
        case ContainerKind::kMpegTs:
            return Reading::kNoSignatureContainer;
        case ContainerKind::kUnknown:
            break;
    }
    return Reading::kElementary;
}

}  // namespace

// --- Signing ------------------------------------------------------------------------------------

std::expected<crypto::SigningCredentials, std::string> load_content_signer(
    std::string_view key_path, std::string_view chain_path) {
    auto credentials = crypto::load_signing_credentials(key_path, chain_path);
    if (!credentials.has_value()) {
        return std::unexpected(credential_sentence(
            "sign-content", credentials.error(),
            "pass signing-key=<path> and signing-chain=<path>, or set "
            "ICLFORGE_CONTENT_SIGNING_KEY_FILE (or ICLFORGE_CONTENT_SIGNING_KEY) and "
            "ICLFORGE_CONTENT_SIGNING_CHAIN_FILE"));
    }
    return std::move(*credentials);
}

std::expected<std::vector<std::byte>, std::string> sign_encoded_frames(
    std::span<const iclforge::ac4::EncodedFrame> frames,
    const crypto::SigningCredentials& credentials, std::int64_t signed_unix) {
    std::vector<std::span<const std::byte>> views;
    views.reserve(frames.size());
    for (const iclforge::ac4::EncodedFrame& frame : frames) {
        views.emplace_back(frame.raw_ac4_frame);
    }
    return sign_ac4_samples(views, credentials, signed_unix);
}

std::expected<std::vector<std::byte>, std::string> sign_ac4_samples(
    std::span<const std::span<const std::byte>> samples,
    const crypto::SigningCredentials& credentials, std::int64_t signed_unix) {
    auto payload = signing::sign_frames(samples, credentials, signed_unix);
    if (!payload.has_value()) {
        return std::unexpected(fmt::format("sign-content: {} ({})",
                                           crypto::describe(payload.error().error),
                                           payload.error().detail));
    }
    return std::move(*payload);
}

std::string describe_signer(const crypto::SigningCredentials& credentials,
                            std::int64_t signed_unix) {
    const crypto::Certificate& leaf = credentials.chain.back();
    return fmt::format("signed as serial {} ({}) at {}, {} certificates in the chain",
                       leaf.serial_hex(), subject_of(leaf), crypto::format_iso8601_utc(signed_unix),
                       credentials.chain.size());
}

// --- Verification -------------------------------------------------------------------------------

std::expected<crypto::TrustStore, std::string> load_content_trust(
    const ContentTrustSettings& settings) {
    auto trust = crypto::load_trust_store(settings.anchor_paths, settings.key_paths);
    if (!trust.has_value()) {
        if (trust.error().kind == crypto::CredentialErrorKind::kAbsent) {
            return std::unexpected(fmt::format(
                "{} needs something to trust: {}; pass trust-anchor=<certificate> or "
                "trust-key=<public key>, or set ICLFORGE_CONTENT_TRUST_FILE. A signature is never "
                "checked against nothing",
                settings.option, trust.error().message));
        }
        return std::unexpected(fmt::format("{}: {}", settings.option, trust.error().message));
    }
    return std::move(*trust);
}

crypto::VerifyOptions verify_options_of(const ContentTrustSettings& settings) {
    crypto::VerifyOptions options;
    options.chain.now_unix = settings.verify_unix;
    options.chain.allow_expired_intermediates = settings.allow_expired_intermediates;
    if (settings.profile_any) {
        options.profile = crypto::LeafProfile::any();
    }
    // A replacement applies on top of either profile: signer-title= under signer-profile=any is
    // "any signer, provided its title is this".
    if (settings.title.has_value()) {
        options.profile.title = *settings.title;
    }
    if (settings.common_name.has_value()) {
        options.profile.common_name = *settings.common_name;
    }
    if (settings.serial_hex.has_value()) {
        options.profile.serial_hex = *settings.serial_hex;
    }
    return options;
}

ContentCheck check_ac4_content(std::span<const std::byte> file, std::span<const std::byte> sidecar,
                               const crypto::TrustStore& trust,
                               const crypto::VerifyOptions& options) {
    const signing::ElementaryCarrier elementary;
    const Mp4UuidCarrier mp4;
    const signing::Carrier* carrier = nullptr;
    switch (reading_of(file)) {
        case Reading::kElementary:
            carrier = &elementary;
            break;
        case Reading::kMp4:
            carrier = &mp4;
            break;
        case Reading::kNoSignatureContainer: {
            ContentCheck check;
            check.outcome = ContentOutcome::kUnreadable;
            check.message =
                "Matroska and MPEG-TS carry no AC-4 content signature; sign the MP4 or the "
                "elementary stream with its sidecar";
            return check;
        }
    }

    const auto content = carrier->read(file, sidecar);
    if (!content.has_value()) {
        return unreadable(std::string{carrier->name()}, content.error());
    }
    ContentCheck check;
    check.carrier = std::string{carrier->name()};
    check.frames = content->frames.size();

    const auto verdict = signing::verify_frames(*content, trust, options);
    if (verdict.has_value()) {
        check.outcome = ContentOutcome::kVerified;
        check.accepted_by = accepted_by_of(verdict->acceptance, trust);
        check.verified = *verdict;
        return check;
    }
    const crypto::ContentSignatureFailure& failure = verdict.error();
    check.error_name = std::string{crypto::error_name(failure.error)};
    check.message =
        failure.detail.empty() ? std::string{crypto::describe(failure.error)} : failure.detail;
    // A chain failure says which check and which certificate, which the detail may not.
    if (failure.chain.has_value()) {
        check.message +=
            fmt::format(" ({}, certificate {} of the chain)",
                        crypto::describe(failure.chain->error), failure.chain->index + 1);
    }
    // "No signature" is the one failure that is not about the signature: the stream is unsigned.
    const bool unsigned_stream =
        !content->signature_present &&
        failure.error == crypto::ContentSignatureError::kBadOrMissingSignature;
    check.outcome = unsigned_stream ? ContentOutcome::kUnsigned : ContentOutcome::kRejected;
    return check;
}

std::vector<std::string> describe_check(const ContentCheck& check) {
    std::vector<std::string> lines;
    if (!check.verified.has_value()) {
        return lines;
    }
    const crypto::VerifiedSignature& verified = *check.verified;
    const crypto::Certificate& leaf = verified.leaf();
    lines.push_back(fmt::format("signer: serial {} ({})", leaf.serial_hex(), subject_of(leaf)));
    lines.push_back(fmt::format("accepted under {}; {} certificates in the chain",
                                check.accepted_by, verified.chain.size()));
    // The date is the signer's statement and the signature does not cover it (register 4 of the
    // design); saying so beside it keeps a log reader from relying on it.
    lines.push_back(
        fmt::format("signed {} (as the signer wrote it; the signature does not cover "
                    "the date)",
                    verified.signed_date));
    for (const std::size_t position : verified.acceptance.expired_intermediates) {
        if (position < verified.chain.size()) {
            lines.push_back(fmt::format(
                "intermediate certificate {} ({}) is past its notAfter and was accepted because "
                "allow-expired-intermediates was given",
                position + 1, subject_of(verified.chain[position])));
        }
    }
    return lines;
}

// --- Inspection ---------------------------------------------------------------------------------

SignatureInspection inspect_ac4_signature(std::span<const std::byte> file,
                                          std::span<const std::byte> sidecar) {
    SignatureInspection out;
    const signing::ElementaryCarrier elementary;
    const Mp4UuidCarrier mp4;
    const signing::Carrier* carrier = nullptr;
    switch (reading_of(file)) {
        case Reading::kElementary:
            carrier = &elementary;
            break;
        case Reading::kMp4:
            carrier = &mp4;
            break;
        case Reading::kNoSignatureContainer:
            out.supported = false;
            out.problem = "Matroska and MPEG-TS carry no AC-4 content signature";
            return out;
    }
    const auto content = carrier->read(file, sidecar);
    if (!content.has_value()) {
        out.problem = content.error().detail;
        return out;
    }
    if (!content->signature_present) {
        out.lines.push_back(fmt::format("carrier: {}; no content signature", carrier->name()));
        return out;
    }
    out.present = true;
    const auto parsed = crypto::parse_content_signature(content->signature);
    if (!parsed.has_value()) {
        out.problem = fmt::format("a content signature is present and does not parse: {}",
                                  parsed.error().detail);
        return out;
    }
    out.carrier = std::string{carrier->name()};
    out.signer_serial = parsed->signer_info.signer_serial_number;
    out.signed_date = parsed->signature_values.signed_info.signed_date;
    out.version = parsed->signature_values.signed_info.canonical_version;
    out.lines.push_back(
        fmt::format("carrier: {}; {} bytes", carrier->name(), content->signature.size()));
    out.lines.push_back(fmt::format("signer serial: {}", parsed->signer_info.signer_serial_number));
    out.lines.push_back(
        fmt::format("signed: {} (the signer's statement; not covered by the "
                    "signature)",
                    parsed->signature_values.signed_info.signed_date));
    out.lines.push_back(
        fmt::format("version: {}", parsed->signature_values.signed_info.canonical_version));
    std::size_t number = 0;
    for (const std::vector<std::byte>& der : parsed->certificate_chain.certificates) {
        ++number;
        const auto certificate = crypto::parse_certificate(der);
        if (!certificate.has_value()) {
            out.lines.push_back(fmt::format("certificate {}: does not parse ({})", number,
                                            crypto::describe(certificate.error())));
            out.certificates.push_back(
                {.subject = {},
                 .serial = {},
                 .not_before = {},
                 .not_after = {},
                 .sha1 = {},
                 .problem = std::string{crypto::describe(certificate.error())}});
            continue;
        }
        out.certificates.push_back(
            {.subject = subject_of(*certificate),
             .serial = certificate->serial_hex(),
             .not_before = crypto::format_iso8601_utc(certificate->not_before),
             .not_after = crypto::format_iso8601_utc(certificate->not_after),
             .sha1 = certificate->sha1_fingerprint(),
             .problem = {}});
        out.lines.push_back(fmt::format("certificate {}: {} serial {}, valid {} to {}", number,
                                        subject_of(*certificate), certificate->serial_hex(),
                                        crypto::format_iso8601_utc(certificate->not_before),
                                        crypto::format_iso8601_utc(certificate->not_after)));
        out.lines.push_back(fmt::format("  SHA-1 {}", certificate->sha1_fingerprint()));
    }
    return out;
}

}  // namespace iclforge::apps
