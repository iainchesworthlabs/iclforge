#include "iclforge/ac4/io/content_signature.hpp"

#include <array>
#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "iclforge/ac4/io/elementary.hpp"
#include "iclforge/base/crypto/content_signature.hpp"
#include "iclforge/base/crypto/sha256.hpp"

namespace iclforge::ac4 {
namespace {

std::unexpected<StreamSignatureError> failure(StreamSignatureErrorKind kind, std::string message) {
    return std::unexpected(StreamSignatureError{kind, std::move(message), std::nullopt});
}

}  // namespace

std::expected<TrailerLocation, StreamSignatureError> locate_signature_trailer(
    std::span<const std::byte> stream) {
    const ScanResult scanned = scan(stream);
    if (scanned.frames.empty()) {
        return failure(StreamSignatureErrorKind::kNotAnAc4Stream,
                       "the stream does not begin with an AC-4 sync frame (0xAC40 or 0xAC41)");
    }
    if (scanned.stopped_at == Error::kTruncated) {
        return failure(StreamSignatureErrorKind::kTruncated,
                       "the sync frame at offset " + std::to_string(scanned.stopped_at_offset) +
                           " runs past the end of the stream");
    }
    // Where the last whole frame ends: its payload, and the two CRC bytes of an 0xAC41 frame.
    const SyncFrame& last = scanned.frames.back();
    const std::size_t end = static_cast<std::size_t>(last.raw_ac4_frame.data() - stream.data()) +
                            last.raw_ac4_frame.size() + (last.sync_word == 0xAC41 ? 2U : 0U);
    TrailerLocation location;
    location.content_size = end;
    if (end == stream.size()) {
        return location;  // no trailer
    }
    // The walk stopped on bytes that are not a sync word (kLostSync), or fewer than the four a sync
    // frame header needs remained. A signature is kilobytes; three bytes are damage.
    if (stream.size() - end < 4) {
        return failure(StreamSignatureErrorKind::kTruncated,
                       std::to_string(stream.size() - end) +
                           " byte(s) follow the last sync frame, too few for a signature");
    }
    location.present = true;
    location.blob = stream.subspan(end);
    return location;
}

std::expected<std::array<std::byte, 32>, StreamSignatureError> signed_content_digest(
    std::span<const std::byte> stream) {
    const auto located = locate_signature_trailer(stream);
    if (!located.has_value()) {
        return std::unexpected(located.error());
    }
    return base::crypto::sha256(stream.first(located->content_size));
}

std::expected<std::vector<std::byte>, StreamSignatureError> sign_stream(
    std::span<const std::byte> stream, const base::crypto::SigningCredentials& credentials,
    const base::crypto::SignOptions& options) {
    const auto located = locate_signature_trailer(stream);
    if (!located.has_value()) {
        return std::unexpected(located.error());
    }
    if (located->present) {
        return failure(StreamSignatureErrorKind::kAlreadySigned,
                       "the stream already carries a content signature");
    }
    const std::array<std::byte, 32> digest = base::crypto::sha256(stream);
    auto blob = base::crypto::sign_content_digest(digest, credentials, options);
    if (!blob.has_value()) {
        StreamSignatureError error;
        error.kind = StreamSignatureErrorKind::kSigning;
        error.message = std::string(to_string(blob.error().code)) + ": " + blob.error().message;
        error.fault = blob.error();
        return std::unexpected(std::move(error));
    }
    std::vector<std::byte> out;
    out.reserve(stream.size() + blob->size());
    out.insert(out.end(), stream.begin(), stream.end());
    out.insert(out.end(), blob->begin(), blob->end());
    return out;
}

base::crypto::VerifyReport verify_stream(std::span<const std::byte> stream,
                                         const base::crypto::VerifyPolicy& policy) {
    const auto located = locate_signature_trailer(stream);
    if (!located.has_value() || !located->present) {
        base::crypto::VerifyReport report;
        report.verdict = base::crypto::Verdict::kInvalid;
        const std::string message = located.has_value() ? "the stream carries no content signature"
                                                        : located.error().message;
        report.fault = base::crypto::SignatureFault{
            base::crypto::SignatureError::kBadOrMissingSignature, message};
        report.checks.push_back(base::crypto::CheckResult{"structure", false, message});
        return report;
    }
    const std::array<std::byte, 32> digest =
        base::crypto::sha256(stream.first(located->content_size));
    return base::crypto::verify_content_signature(located->blob, digest, policy);
}

std::expected<std::vector<std::byte>, StreamSignatureError> strip_signature(
    std::span<const std::byte> stream) {
    const auto located = locate_signature_trailer(stream);
    if (!located.has_value()) {
        return std::unexpected(located.error());
    }
    const auto prefix = stream.first(located->content_size);
    return std::vector<std::byte>(prefix.begin(), prefix.end());
}

SignatureSummary summarise_signature(std::span<const std::byte> stream) {
    SignatureSummary summary;
    const auto located = locate_signature_trailer(stream);
    if (!located.has_value() || !located->present) {
        return summary;
    }
    summary.present = true;
    summary.blob_size = located->blob.size();
    const auto parsed = base::crypto::parse_content_signature(located->blob);
    if (!parsed.has_value()) {
        summary.problem = parsed.error().message;
        return summary;
    }
    summary.readable = true;
    summary.signed_date = parsed->signed_date;
    summary.signing_method = parsed->signing_method;
    summary.certificates = parsed->chain.size();
    if (!parsed->chain.empty()) {
        summary.signer = parsed->chain.back().subject.text;
        summary.signer_serial = parsed->chain.back().serial_hex;
    } else {
        summary.problem = parsed->chain_error;
    }
    summary.publisher = parsed->signer.publisher_name;
    summary.application = parsed->signer.application_name;
    return summary;
}

}  // namespace iclforge::ac4
