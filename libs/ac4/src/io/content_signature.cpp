#include "iclforge/ac4/io/content_signature.hpp"

#include <array>
#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "iclforge/ac4/io/elementary.hpp"
#include "iclforge/base/crypto/content_signature.hpp"
#include "iclforge/base/crypto/sha256.hpp"
#include "iclforge/base/crypto/x509.hpp"

namespace iclforge::ac4 {
namespace {

std::unexpected<StreamSignatureError> failure(StreamSignatureErrorKind kind, std::string message) {
    return std::unexpected(StreamSignatureError{kind, std::move(message), std::nullopt});
}

// A ContentSignature opens with its version (i32 field 1) and schema version (i32 field 2), the two
// fields every measured blob begins with: 08 00 01 <4 bytes> 08 00 02. A byte run that does not
// begin so is not a signature, damaged or not.
bool begins_as_content_signature(std::span<const std::byte> bytes) {
    constexpr std::array<int, 10> kPattern = {0x08, 0x00, 0x01, -1, -1, -1, -1, 0x08, 0x00, 0x02};
    if (bytes.size() < kPattern.size()) {
        return false;
    }
    for (std::size_t i = 0; i < kPattern.size(); ++i) {
        if (kPattern[i] >= 0 && std::to_integer<int>(bytes[i]) != kPattern[i]) {
            return false;
        }
    }
    return true;
}

// An ISO base media file (MP4, fragmented MP4, CMAF) has no magic number: it opens with a box whose
// first four bytes are its length and the next four its type. The types a file or a bare segment
// opens with in practice are the ones listed (ISO/IEC 14496-12 §4.3 'ftyp', 'styp' for a segment,
// and the others tools write when they leave 'ftyp' out). A stream that opens with a sync word,
// 0xAC40 or 0xAC41, is AC-4 whatever its next bytes spell: as a box size that would be 2.8 GB.
bool begins_as_iso_bmff(std::span<const std::byte> bytes) {
    constexpr std::array<std::string_view, 5> kLeadingTypes{"ftyp", "styp", "moov", "moof", "mdat"};
    if (bytes.size() < 8) {
        return false;
    }
    if (bytes[0] == std::byte{0xAC} &&
        (bytes[1] == std::byte{0x40} || bytes[1] == std::byte{0x41})) {
        return false;
    }
    const std::string_view type{reinterpret_cast<const char*>(bytes.data()) + 4, 4};
    for (const std::string_view known : kLeadingTypes) {
        if (type == known) {
            return true;
        }
    }
    return false;
}

// Why the bytes after the framing are not a trailer, for a message.
std::string stray_message(std::span<const std::byte> stream, const TrailerLocation& location) {
    const std::span<const std::byte> rest = stream.subspan(location.framing_ends);
    std::string text = "the sync frames stop at byte " + std::to_string(location.framing_ends) +
                       " and the " + std::to_string(location.stray) +
                       " byte(s) after it are not a content signature";
    if (rest.size() >= 3 && rest[0] == std::byte{'I'} && rest[1] == std::byte{'D'} &&
        rest[2] == std::byte{'3'}) {
        text +=
            " (an ID3v2 tag sits between the frames, as in HLS packed audio; a content signature "
            "covers a stream of sync frames, so the tags have to be taken out first)";
    } else {
        text += " (a damaged frame, or data that is not AC-4)";
    }
    return text;
}

}  // namespace

std::expected<TrailerLocation, StreamSignatureError> locate_signature_trailer(
    std::span<const std::byte> stream) {
    if (begins_as_iso_bmff(stream)) {
        // Said by name, not as "not AC-4": the stream may well be AC-4, in a container that cannot
        // carry a signature.
        return failure(
            StreamSignatureErrorKind::kContainerNotSupported,
            "this is an MP4 (ISO base media) file. A content signature is carried only as a "
            "trailer after the last sync frame of a raw .ac4 stream. CS spec v2.6 section 8.3.2 "
            "puts signing an MP4 out of scope: it names no box that carries one, so no verifier "
            "would know where to look");
    }
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
    location.framing_ends = end;
    if (end == stream.size()) {
        return location;  // no trailer
    }
    // The walk stopped on bytes that are not a sync word (kLostSync), or fewer than the four a sync
    // frame header needs remained. What follows is a trailer only if it begins as a signature does.
    const std::span<const std::byte> rest = stream.subspan(end);
    if (begins_as_content_signature(rest)) {
        location.present = true;
        location.blob = rest;
    } else {
        location.stray = rest.size();
    }
    return location;
}

std::expected<std::array<std::byte, 32>, StreamSignatureError> signed_content_digest(
    std::span<const std::byte> stream) {
    const auto located = locate_signature_trailer(stream);
    if (!located.has_value()) {
        return std::unexpected(located.error());
    }
    if (located->stray != 0) {
        return failure(StreamSignatureErrorKind::kTrailingData, stray_message(stream, *located));
    }
    return base::crypto::sha256(stream.first(located->content_size));
}

std::expected<std::vector<std::byte>, StreamSignatureError> sign_stream(
    std::span<const std::byte> stream, const base::crypto::SigningCredentials& credentials,
    const base::crypto::SignOptions& options) {
    const auto located = locate_signature_trailer(stream);
    if (!located.has_value()) {
        StreamSignatureError error = located.error();
        if (error.kind == StreamSignatureErrorKind::kContainerNotSupported) {
            error.message += "; sign the raw .ac4 elementary stream instead";
        }
        return std::unexpected(std::move(error));
    }
    if (located->present) {
        // Bytes that begin as a signature do: a signature that reads is already there, and one
        // that does not is damage to be removed. Neither is signed over.
        const auto existing = base::crypto::parse_content_signature(located->blob);
        if (existing.has_value()) {
            return failure(StreamSignatureErrorKind::kAlreadySigned,
                           "the stream already carries a content signature; strip it or sign the "
                           "unsigned stream");
        }
        return failure(StreamSignatureErrorKind::kTrailingData,
                       std::to_string(located->blob.size()) +
                           " byte(s) after the last sync frame begin as a content signature but "
                           "do not read as one (" +
                           existing.error().message + "); remove them first");
    }
    if (located->stray != 0) {
        return failure(StreamSignatureErrorKind::kTrailingData, stray_message(stream, *located));
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

    // Read back what was just made, with the signature checks and no trust anchor and no clock: a
    // signature that does not verify is a fault in the signer, and is reported here and not left
    // for whoever checks the file.
    base::crypto::VerifyPolicy own;
    own.time_basis = base::crypto::TimeBasis::kIgnore;
    own.required_leaf_title.clear();
    own.require_digital_signature = false;
    const base::crypto::VerifyReport readback = verify_stream(out, own);
    if (readback.verdict == base::crypto::Verdict::kInvalid) {
        StreamSignatureError error;
        error.kind = StreamSignatureErrorKind::kSigning;
        error.message = "the signature just made does not verify: " +
                        (readback.fault ? readback.fault->message : std::string("no reason given"));
        error.fault = readback.fault;
        return std::unexpected(std::move(error));
    }
    return out;
}

base::crypto::VerifyReport verify_stream(std::span<const std::byte> stream,
                                         const base::crypto::VerifyPolicy& policy) {
    const auto located = locate_signature_trailer(stream);
    if (!located.has_value() || !located->present) {
        base::crypto::VerifyReport report;
        report.verdict = base::crypto::Verdict::kInvalid;
        std::string message = "the stream carries no content signature";
        if (!located.has_value()) {
            message = located.error().message;
        } else if (located->stray != 0) {
            message = "the stream carries no content signature: " + stray_message(stream, *located);
        }
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
    if (located->stray != 0) {
        return failure(StreamSignatureErrorKind::kTrailingData, stray_message(stream, *located));
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
    summary.signed_date = base::crypto::escape_text(parsed->signed_date);
    summary.signing_method = base::crypto::escape_text(parsed->signing_method);
    summary.certificates = parsed->chain.size();
    if (!parsed->chain.empty()) {
        summary.signer = parsed->chain.back().subject.text;
        summary.signer_serial = parsed->chain.back().serial_hex;
    } else {
        summary.problem = parsed->chain_error;
    }
    summary.publisher = base::crypto::escape_text(parsed->signer.publisher_name);
    summary.application = base::crypto::escape_text(parsed->signer.application_name);
    return summary;
}

}  // namespace iclforge::ac4
