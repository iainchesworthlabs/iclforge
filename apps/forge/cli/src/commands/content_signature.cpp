#include "content_signature.hpp"

#include <fmt/base.h>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "../exit_codes.hpp"
#include "../support.hpp"
#include "ac4_encode_core.hpp"
#include "iclforge/ac4/io/content_signature.hpp"
#include "iclforge/base/crypto/chain.hpp"
#include "iclforge/base/crypto/content_signature.hpp"
#include "iclforge/base/crypto/credentials.hpp"
#include "iclforge/base/crypto/x509.hpp"
#include "iclforge/base/version.hpp"

namespace forge_cli::commands {
namespace {

namespace crypto = iclforge::base::crypto;

// signed-date= within this of the clock is taken for the time of signing and not warned about.
constexpr std::int64_t kSignedDateToleranceSeconds = 300;

std::int64_t now_epoch() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// The leaf title policy: unset is the spec's ("Code/Content Signer", CS spec v2.3 §6.1 step 4),
// "any" checks none.
std::string required_title(const Options& meta) {
    if (!meta.leaf_title.has_value()) {
        return std::string(crypto::kDefaultLeafTitle);
    }
    return *meta.leaf_title == "any" ? std::string() : *meta.leaf_title;
}

std::optional<std::vector<std::byte>> hex_to_bytes(std::string_view text) {
    std::vector<std::byte> out;
    int pending = -1;
    for (const char c : text) {
        int v = -1;
        if (c >= '0' && c <= '9') {
            v = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            v = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            v = c - 'A' + 10;
        } else if (c == ':') {
            continue;
        } else {
            return std::nullopt;
        }
        if (pending < 0) {
            pending = v;
        } else {
            out.push_back(static_cast<std::byte>((pending << 4) | v));
            pending = -1;
        }
    }
    if (pending >= 0) {
        return std::nullopt;
    }
    return out;
}

struct SigningMaterial {
    crypto::SigningCredentials credentials;
    crypto::SignOptions options;
};

// The operator's key and chain, or why not. `option` names what asked, for the message. A missing
// key or chain is a usage error, and anything wrong with one that was named is an input error.
std::expected<SigningMaterial, int> load_signing_material(const Options& meta,
                                                          std::string_view option) {
    auto key = crypto::load_content_key(meta.content_key);
    if (!key.has_value()) {
        if (key.error().kind == crypto::CredentialErrorKind::kAbsent) {
            fmt::println(stderr,
                         "error: {} needs a private key: pass content-key=<path>, or set "
                         "ICLFORGE_CONTENT_KEY_FILE / ICLFORGE_CONTENT_KEY",
                         option);
            return std::unexpected(kExitUsage);
        }
        fmt::println(stderr, "error: {}", key.error().message);
        return std::unexpected(kExitInput);
    }
    auto certificates = crypto::load_content_chain(meta.content_chains);
    if (!certificates.has_value()) {
        if (certificates.error().kind == crypto::CredentialErrorKind::kAbsent) {
            fmt::println(stderr,
                         "error: {} needs the certificates that vouch for the key: pass "
                         "content-chain=<path> (a bundle, or repeat it), or set "
                         "ICLFORGE_CONTENT_CHAIN_FILE",
                         option);
            return std::unexpected(kExitUsage);
        }
        fmt::println(stderr, "error: {}", certificates.error().message);
        return std::unexpected(kExitInput);
    }
    auto credentials = crypto::make_credentials(std::move(*key), std::move(*certificates));
    if (!credentials.has_value()) {
        fmt::println(stderr, "error: {}", credentials.error().message);
        return std::unexpected(kExitInput);
    }
    SigningMaterial material;
    material.credentials = std::move(*credentials);
    material.options.signed_date =
        meta.signed_date.value_or(crypto::format_iso8601_utc(now_epoch()));
    // The chain must be valid on the day it signs, and the leaf must be allowed to.
    material.options.chain_policy.valid_at =
        crypto::parse_iso8601_utc(material.options.signed_date);
    material.options.chain_policy.required_leaf_title = required_title(meta);

    // The signer information of CS spec v2.6 section 5.3. The application is this program, named
    // and numbered as it is built; the publisher, product and platform are the operator's to give.
    crypto::SignerMetadata& info = material.options.metadata;
    info.application_name = "ICL Forge forge";
    info.application_version = std::string(iclforge::base::version_string);
    info.build_hash = std::string(iclforge::base::git_commit);
    info.publisher_uuid = meta.publisher_uuid;
    info.publisher_name = meta.publisher_name;
    info.product_id = meta.product_id.value_or(0);
    info.product_name = meta.product_name;
    info.wrapper_name = meta.wrapper_name;
    info.os_name = meta.os_name;
    info.os_version = meta.os_version;
    info.os_description = meta.os_description;
    return material;
}

// What the operator should know before a signature is made, said once and not refused: the
// specification's "shall" about the publisher UUID and the signing time, where the signature can be
// made and a strict verifier may not take it.
void warn_about_signer_information(const Options& meta, std::string_view option,
                                   std::string_view signed_date) {
    if (meta.publisher_uuid.empty()) {
        fmt::println(stderr,
                     "warning: {} has no publisher-uuid=: CS spec v2.6 section 5.3 asks for the "
                     "UUID of your publisher account, and a verifier that requires it (forge's "
                     "verify-content does) refuses a signature without one",
                     option);
    }
    if (meta.signed_date.has_value()) {
        const auto given = crypto::parse_iso8601_utc(signed_date);
        const std::int64_t now = now_epoch();
        if (!given.has_value() || *given > now + kSignedDateToleranceSeconds ||
            *given < now - kSignedDateToleranceSeconds) {
            fmt::println(stderr,
                         "warning: signed-date={} is not the time of signing ({} UTC now): CS spec "
                         "v2.6 section 5.3 says signedDate shall be the actual signing time, and a "
                         "verifier may refuse a signature whose date it does not accept",
                         signed_date, crypto::format_iso8601_utc(now));
        }
    }
}

// Everything verification needs from the command line, loaded once. The policy borrows from the
// vectors here, so it is built where it is used and not copied.
//
// Two ways of checking, as the owner decided for the time: the strict one, which refuses what does
// not check (ac4-verify, and verify-content on decode), judges the certificates at the time of the
// check, as CS spec v2.6 section 6.1 steps 3 and 4 say; check-content, which reports what it finds
// and carries on, judges them at the date the signature records unless verify-time= says otherwise.
// verify-content is also the strict reading of the operator metadata of section 5.3 (the signer
// nonce and the publisher UUID decide the verdict); everywhere else they are reported. The publisher
// UUID is not in the signing certificate (v2.6), so it is compared only with the accounts
// expect-publisher-uuid= names, and with none it is reported.
struct VerifyMaterial {
    std::vector<crypto::Certificate> anchors;
    std::vector<crypto::CertificatePin> pins;
    std::vector<std::string> expected_publishers;  // the UUIDs of the accounts expect-publisher-uuid= names
    crypto::TimeBasis basis = crypto::TimeBasis::kNow;
    std::int64_t instant = 0;
    std::string title;
    std::string time_name = "now";  // what the report calls the time it judged at
    bool require_metadata = false;

    [[nodiscard]] crypto::VerifyPolicy policy() const {
        crypto::VerifyPolicy p;
        p.trust_anchors = anchors;
        p.pins = pins;
        p.time_basis = basis;
        p.now = now_epoch();
        p.instant = instant;
        p.required_leaf_title = title;
        p.operator_metadata = require_metadata ? crypto::OperatorMetadata::kRequire
                                               : crypto::OperatorMetadata::kReport;
        p.expected_publisher_uuids = expected_publishers;
        return p;
    }
};

std::expected<VerifyMaterial, int> load_verify_material(const Options& meta) {
    VerifyMaterial m;
    if (!meta.trust_anchors.empty()) {
        auto anchors = crypto::load_certificate_files(meta.trust_anchors);
        if (!anchors.has_value()) {
            fmt::println(stderr, "error: {}", anchors.error().message);
            return std::unexpected(kExitInput);
        }
        m.anchors = std::move(*anchors);
    }
    for (const std::string& pin : meta.pins) {
        auto bytes = hex_to_bytes(pin);
        if (!bytes.has_value()) {
            fmt::println(stderr, "error: pin={} is not a fingerprint", pin);
            return std::unexpected(kExitUsage);
        }
        m.pins.push_back(crypto::CertificatePin{std::move(*bytes)});
    }
    const std::string time = meta.verify_time.value_or(meta.check_content ? "signed" : "now");
    m.time_name = meta.verify_time.has_value() ? time
                  : meta.check_content         ? "signed, the default of check-content"
                                               : "now";
    if (time == "now") {
        m.basis = crypto::TimeBasis::kNow;
    } else if (time == "signed") {
        m.basis = crypto::TimeBasis::kSignedDate;
    } else if (time == "ignore") {
        m.basis = crypto::TimeBasis::kIgnore;
    } else {
        m.basis = crypto::TimeBasis::kInstant;
        m.instant = crypto::parse_iso8601_utc(time).value_or(0);
    }
    m.title = required_title(meta);
    m.require_metadata = meta.verify_content;
    m.expected_publishers = meta.expect_publisher_uuids;
    return m;
}

std::string_view verdict_text(crypto::Verdict verdict) {
    switch (verdict) {
        case crypto::Verdict::kValid:
            return "valid";
        case crypto::Verdict::kChecksChainUnanchored:
            return "signature checks out; chain unanchored";
        case crypto::Verdict::kInvalid:
            return "INVALID";
    }
    return "?";
}

void print_report(FILE* out, std::string_view name, const crypto::VerifyReport& report,
                  std::string_view time_basis) {
    status_println(out, "content signature of {}: {}", name, verdict_text(report.verdict));
    if (!report.signer.empty()) {
        status_println(out, "  signer      {} (serial {})", report.signer, report.signer_serial);
    }
    if (!report.signed_date.empty()) {
        status_println(out, "  signed      {} (UTC)", report.signed_date);
    }
    // What the signature says about whoever made it: bound by the signature, and no more
    // trustworthy than the signer. The findings on it are the notes below.
    if (!report.publisher.empty()) {
        status_println(out, "  publisher   {}", report.publisher);
    }
    if (!report.product.empty()) {
        status_println(out, "  product     {}", report.product);
    }
    if (!report.platform.empty()) {
        status_println(out, "  platform    {}", report.platform);
    }
    if (!report.application.empty()) {
        status_println(out, "  application {}", report.application);
    }
    if (report.verified_at != 0) {
        status_println(out, "  validity    judged at {} ({})",
                       crypto::format_iso8601_utc(report.verified_at), time_basis);
    }
    if (!report.anchor.empty()) {
        status_println(out, "  anchored by {}", report.anchor);
    }
    for (const crypto::CheckResult& check : report.checks) {
        status_println(out, "  {:<4} {}: {}", check.ok ? "ok" : "FAIL", check.name, check.detail);
    }
    // Findings that did not decide the verdict (verify-content makes them decide it).
    for (const crypto::CheckResult& note : report.notes) {
        status_println(out, "  {:<4} {}: {}", note.ok ? "ok" : "note", note.name, note.detail);
    }
    if (report.verdict == crypto::Verdict::kChecksChainUnanchored) {
        status_println(out,
                       "  note: nothing the operator trusts vouches for the certificates in the "
                       "file, so this shows the file agrees with its own signature, not who made "
                       "it; trust-anchor= or pin= say whom to trust");
    }
    if (report.fault.has_value()) {
        status_println(out, "  fault: {}: {}", to_string(report.fault->code),
                       report.fault->message);
    }
}

bool accepted(const crypto::VerifyReport& report, const Options& meta) {
    return report.verdict == crypto::Verdict::kValid ||
           (report.verdict == crypto::Verdict::kChecksChainUnanchored && meta.unanchored_ok);
}

// Writes `bytes` over `path` by way of a neighbour, so that a failure part way does not leave the
// signed file half written or lose the unsigned one.
bool replace_file(const std::string& path, std::span<const std::byte> bytes) {
    const std::string temporary = path + ".signing";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            fmt::println(stderr, "error: cannot open {} for writing", temporary);
            return false;
        }
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) {
            fmt::println(stderr, "error: cannot write {}", temporary);
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        fmt::println(stderr, "error: cannot replace {}: {}", path, ec.message());
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

}  // namespace

int run_ac4_sign(std::string_view in_path, std::string_view out_path, const Options& meta) {
    if (!is_stdio_path(in_path) && !is_stdio_path(out_path)) {
        std::error_code ec;
        if (std::filesystem::exists(std::string(out_path), ec) &&
            std::filesystem::equivalent(std::string(in_path), std::string(out_path), ec)) {
            fmt::println(stderr,
                         "error: ac4-sign writes a signed copy; {} is the file it reads, so name "
                         "another output",
                         in_path);
            return kExitUsage;
        }
    }
    const auto stream = read_all(in_path);
    if (stream.empty()) {
        fmt::println(stderr, "error: cannot read {}", in_path);
        return kExitInput;
    }
    auto material = load_signing_material(meta, "ac4-sign");
    if (!material.has_value()) {
        return material.error();
    }
    warn_about_signer_information(meta, "ac4-sign", material->options.signed_date);
    auto signed_stream =
        iclforge::ac4::sign_stream(stream, material->credentials, material->options);
    if (!signed_stream.has_value()) {
        fmt::println(stderr, "error: {}: {}", in_path, signed_stream.error().message);
        switch (signed_stream.error().kind) {
            case iclforge::ac4::StreamSignatureErrorKind::kAlreadySigned:
            case iclforge::ac4::StreamSignatureErrorKind::kContainerNotSupported:
                return kExitUsage;
            default:
                return kExitInput;
        }
    }
    EncodedStreamSink sink;
    if (!sink.open(out_path, /*keep_partial=*/false)) {
        return kExitOutput;
    }
    if (!sink.push(std::span<const std::byte>(*signed_stream)) || !sink.close()) {
        sink.abort();
        return kExitOutput;
    }
    FILE* status = status_stream(out_path);
    const auto summary = iclforge::ac4::summarise_signature(*signed_stream);
    status_println(status, "signed {} -> {}: {} bytes of AC-4, then a {} byte content signature",
                   in_path, out_path, signed_stream->size() - summary.blob_size, summary.blob_size);
    status_println(status, "  signer {} (serial {}), dated {} (UTC), {} certificate(s)",
                   summary.signer, summary.signer_serial, summary.signed_date,
                   summary.certificates);
    return kExitOk;
}

int run_ac4_verify(std::string_view in_path, const Options& meta) {
    const auto stream = read_all(in_path);
    if (stream.empty()) {
        fmt::println(stderr, "error: cannot read {}", in_path);
        return kExitInput;
    }
    const auto located = iclforge::ac4::locate_signature_trailer(stream);
    if (!located.has_value()) {
        fmt::println(stderr, "error: {}: {}", in_path, located.error().message);
        return kExitInput;
    }
    auto material = load_verify_material(meta);
    if (!material.has_value()) {
        return material.error();
    }
    const crypto::VerifyReport report = iclforge::ac4::verify_stream(stream, material->policy());
    FILE* out = stdout;
    print_report(out, in_path, report, material->time_name);
    if (meta.check_content) {
        return kExitOk;
    }
    if (accepted(report, meta)) {
        return kExitOk;
    }
    if (report.verdict == crypto::Verdict::kChecksChainUnanchored) {
        fmt::println(stderr,
                     "error: {}: the signature checks out but no trust-anchor= or pin= vouches for "
                     "its certificates; unanchored-ok accepts that, or check-content only reports",
                     in_path);
    } else {
        fmt::println(stderr, "error: {}: the content signature is not acceptable", in_path);
    }
    return kExitQcGate;
}

int check_sign_content_request(std::string_view out_path, const Options& meta, bool output_is_ac4) {
    if (!meta.sign_content) {
        return kExitOk;
    }
    if (!output_is_ac4) {
        fmt::println(stderr,
                     "error: sign-content signs an AC-4 stream; this command writes E-AC-3 unless "
                     "codec=ac4 is given");
        return kExitUsage;
    }
    if (is_stdio_path(out_path)) {
        fmt::println(
            stderr,
            "error: sign-content signs a file once it is written; '-' is not one (sign the "
            "output afterwards with ac4-sign)");
        return kExitUsage;
    }
    if (iclforge::apps::ac4_output_names_mp4(out_path)) {
        fmt::println(stderr,
                     "error: sign-content does not sign an MP4: CS spec v2.6 section 8.3.2 puts "
                     "that out of scope, because it names no box that carries a content "
                     "signature. Write a raw .ac4 file (name the output *.ac4), whose signature "
                     "is a trailer after its last sync frame, and sign that");
        return kExitUsage;
    }
    auto material = load_signing_material(meta, "sign-content");
    if (!material.has_value()) {
        return material.error();
    }
    warn_about_signer_information(meta, "sign-content", material->options.signed_date);
    // Sign a throwaway digest, so that a key that does not belong to its chain, or a signed-date
    // outside the certificates' validity, is found now and not after the encode has run.
    const std::array<std::byte, 32> trial_digest{};
    const auto trial =
        crypto::sign_content_digest(trial_digest, material->credentials, material->options);
    if (!trial.has_value()) {
        fmt::println(stderr, "error: sign-content: {}: {}", crypto::to_string(trial.error().code),
                     trial.error().message);
        return kExitInput;
    }
    return kExitOk;
}

int sign_encoded_file(std::string_view out_path, const Options& meta) {
    if (!meta.sign_content) {
        return kExitOk;
    }
    const auto stream = read_all(out_path);
    if (stream.empty()) {
        fmt::println(stderr, "error: cannot read back {} to sign it", out_path);
        return kExitOutput;
    }
    auto material = load_signing_material(meta, "sign-content");
    if (!material.has_value()) {
        return material.error();
    }
    auto signed_stream =
        iclforge::ac4::sign_stream(stream, material->credentials, material->options);
    if (!signed_stream.has_value()) {
        fmt::println(stderr, "error: {}: {}", out_path, signed_stream.error().message);
        return kExitRuntime;
    }
    if (!replace_file(std::string(out_path), *signed_stream)) {
        return kExitOutput;
    }
    FILE* status = status_stream(out_path);
    const auto summary = iclforge::ac4::summarise_signature(*signed_stream);
    status_println(status, "  content signature: {} bytes, signer {}, dated {} (UTC)",
                   summary.blob_size, summary.signer, summary.signed_date);
    return kExitOk;
}

int check_content_before_decode(std::span<const std::byte> stream, std::string_view in_path,
                                const Options& meta, FILE* status) {
    if (!meta.verify_content && !meta.check_content) {
        return kExitOk;
    }
    auto material = load_verify_material(meta);
    if (!material.has_value()) {
        return material.error();
    }
    const crypto::VerifyReport report = iclforge::ac4::verify_stream(stream, material->policy());
    print_report(status, in_path, report, material->time_name);
    if (meta.check_content || accepted(report, meta)) {
        return kExitOk;
    }
    if (report.verdict == crypto::Verdict::kChecksChainUnanchored) {
        fmt::println(
            stderr,
            "error: verify-content: the signature of {} checks out but no trust-anchor= or "
            "pin= vouches for its certificates (unanchored-ok accepts that)",
            in_path);
    } else {
        fmt::println(stderr, "error: verify-content: the content signature of {} is not acceptable",
                     in_path);
    }
    return kExitInput;
}

}  // namespace forge_cli::commands
