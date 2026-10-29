#pragma once

// A content signature carried with an AC-4 elementary stream: the layer between the codec-blind
// signature (iclforge/base/crypto/content_signature.hpp) and a raw `.ac4` file. CS spec v2.2
// §8.3.1: the signed file is the encoder's syncframes, unmodified, followed by the Thrift blob to
// the end of the file; the digest is the SHA-256 of every byte before the blob. The syncframes
// carry no sign of the signature, so a decoder that does not look for one plays a signed file as
// the unsigned one (it stops at the first byte that is not a sync word, which is the blob's first
// byte).
//
// Signing here is opt-in at every call; nothing in the encoder or the decoder reaches this. Only
// the raw elementary stream is handled. Carriage in an MP4 file is described by §8.3.2 in a
// sentence and is not implemented: libs/ac4/ERRATA.md records the reading.

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "iclforge/ac4/export.hpp"
#include "iclforge/base/crypto/content_signature.hpp"

namespace iclforge::ac4 {

enum class StreamSignatureErrorKind : std::uint8_t {
    kNotAnAc4Stream,  // does not begin with an AC-4 sync frame
    kTruncated,       // the last sync frame is cut off, or bytes too few to be a trailer follow
    kAlreadySigned,   // there is a trailer already; strip it or sign the unsigned stream
    kSigning,         // the signature could not be made; `fault` says why
};

struct StreamSignatureError {
    StreamSignatureErrorKind kind = StreamSignatureErrorKind::kNotAnAc4Stream;
    std::string message;
    std::optional<base::crypto::SignatureFault> fault;
};

// Where a stream's signature trailer starts, found as CS spec v2.2 §8.5 describes: walk the
// syncframes forward by their sizes; the bytes from the first position that is not a sync word
// (0xAC40 or 0xAC41) to the end are the trailer.
struct TrailerLocation {
    bool present = false;
    std::size_t content_size = 0;  // the stream's length without the trailer: the signed prefix
    std::span<const std::byte> blob;
};

[[nodiscard]] ICLFORGE_AC4_EXPORT std::expected<TrailerLocation, StreamSignatureError>
locate_signature_trailer(std::span<const std::byte> stream);

// The SHA-256 a signature over this stream covers: every byte before the trailer, or the whole
// stream when there is none.
[[nodiscard]] ICLFORGE_AC4_EXPORT std::expected<std::array<std::byte, 32>, StreamSignatureError>
signed_content_digest(std::span<const std::byte> stream);

// `stream` with a signature made under `credentials` appended. A stream that already has a trailer
// is refused (kAlreadySigned) rather than signed over the old signature.
[[nodiscard]] ICLFORGE_AC4_EXPORT std::expected<std::vector<std::byte>, StreamSignatureError>
sign_stream(std::span<const std::byte> stream, const base::crypto::SigningCredentials& credentials,
            const base::crypto::SignOptions& options);

// Verifies the trailer against the stream before it. Always returns a report: a stream with no
// trailer, or that is not an AC-4 stream, is an invalid report whose fault says so (BadOrMissing-
// Signature), so a caller deciding between "refuse" and "report" has one path.
[[nodiscard]] ICLFORGE_AC4_EXPORT base::crypto::VerifyReport verify_stream(
    std::span<const std::byte> stream, const base::crypto::VerifyPolicy& policy);

// The stream without its trailer (the same bytes when there is none).
[[nodiscard]] ICLFORGE_AC4_EXPORT std::expected<std::vector<std::byte>, StreamSignatureError>
strip_signature(std::span<const std::byte> stream);

// What `forge probe` shows without a key: whether a trailer is there, and what it says about
// itself. Nothing here has been verified.
struct SignatureSummary {
    bool present = false;
    bool readable = false;  // the trailer parsed as a content signature
    std::string problem;    // why not, when present and not readable
    std::size_t blob_size = 0;
    std::string signer;  // the leaf certificate's subject
    std::string signer_serial;
    std::string signed_date;
    std::string signing_method;
    std::size_t certificates = 0;
    std::string publisher;
    std::string application;
};

[[nodiscard]] ICLFORGE_AC4_EXPORT SignatureSummary
summarise_signature(std::span<const std::byte> stream);

}  // namespace iclforge::ac4
