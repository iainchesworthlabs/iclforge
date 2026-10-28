#pragma once

// AC-4 content signing: what the pace-ccjf1 signature of iclforge::base::crypto
// (base/crypto/content_signature.hpp) covers when the content is an AC-4 stream, and the two
// operations built on it, sign_frames() and verify_frames(). See docs/concepts/content-signing.md
// and planning/ac4-content-signing.md.
//
// THE SIGNED SPAN. The content that is signed is the concatenation, in stream order, of the
// `raw_ac4_frame` of every frame: the bytes between an elementary stream's frame_size field and its
// optional crc_word (ETSI TS 103 190-2 Annex G.3.1), which are also, byte for byte, the samples of
// an MP4 track (Annex E.4). The digest of a stream is therefore the digest of its MP4, and a
// signature made on one verifies on the other. The sync word, the frame_size field and the CRC word
// are not in the span: a decoder's own CRC check is what catches a flipped CRC bit, and the frame
// boundaries are not authenticated beyond that. The specification names the span only as "AC-4
// bitstream content bytes" (its sections 1.2 and 2.1); this is register item 3 and decision 2 of
// the design document.
//
// WHERE THE SIGNATURE TRAVELS is not decided here. A Carrier (carrier.hpp) takes a stream apart
// into the frames and the serialised signature; sign_frames() returns the serialised signature for
// the writer to put wherever its container keeps one.
//
// NOT IN THE CODEC. Nothing in the encoder or the decoder includes this header (a test scans the
// sources for it), and the minimum-footprint archive does not contain it. Signing is something a
// tool does around a codec.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include "iclforge/ac4/decoder/config.hpp"
#include "iclforge/ac4/export.hpp"
#include "iclforge/ac4/signing/carrier.hpp"
#include "iclforge/base/crypto/content_signature.hpp"
#include "iclforge/base/crypto/sha256.hpp"

namespace iclforge::ac4::signing {

// SHA-256 over the signed span, one frame at a time, so a caller that produces or reads frames in
// sequence does not have to hold the concatenation.
class ICLFORGE_AC4_EXPORT ContentDigest {
   public:
    // Appends one frame's raw_ac4_frame. An empty frame adds nothing to the digest and still
    // counts.
    void add_frame(std::span<const std::byte> raw_ac4_frame);

    [[nodiscard]] std::size_t frame_count() const noexcept { return frames_; }
    // Bytes hashed: the sum of the frames' sizes.
    [[nodiscard]] std::uint64_t byte_count() const noexcept { return bytes_; }

    // The digest of everything added. The object is empty again afterwards.
    [[nodiscard]] base::crypto::Digest256 finish();

   private:
    base::crypto::Sha256 hash_;
    std::size_t frames_ = 0;
    std::uint64_t bytes_ = 0;
};

// The digest of `frames`, which is what ContentDigest gives for the same frames added in order.
[[nodiscard]] ICLFORGE_AC4_EXPORT base::crypto::Digest256 digest_of_frames(
    std::span<const std::span<const std::byte>> frames);

// Signs `frames` with the operator's key and chain (base::crypto::SigningCredentials, from
// load_signing_credentials()) and returns the serialised ContentSignature. `signed_unix` is the
// time to record: the library has no clock, so the caller supplies it.
//
// kNoContent for no frames; otherwise the errors of base::crypto::sign_digest().
[[nodiscard]] ICLFORGE_AC4_EXPORT
    std::expected<std::vector<std::byte>, base::crypto::ContentSignatureFailure>
    sign_frames(std::span<const std::span<const std::byte>> frames,
                const base::crypto::SigningCredentials& credentials, std::int64_t signed_unix);

// Judges what a Carrier produced against the operator's trust: the digest of content.frames is
// computed here, independently of anything the signature says, and handed to
// base::crypto::verify_digest(), whose order of checks and error codes apply.
//
// A stream with no frames is kNoContent, before any other check. A stream with no signature is
// kBadOrMissingSignature, after the checks of the operator's inputs (an empty trust store, no
// clock), which come first in verify_digest().
[[nodiscard]] ICLFORGE_AC4_EXPORT
    std::expected<base::crypto::VerifiedSignature, base::crypto::ContentSignatureFailure>
    verify_frames(const CarriedContent& content, const base::crypto::TrustStore& trust,
                  const base::crypto::VerifyOptions& options);

// The soft mode ("gate-content"): the decoder configuration for content whose verification came
// out as `verified`. Content that did not verify is decoded with its object audio left out
// (DecoderConfig::skip_objects), which gives the same channels bit for bit and is what a licensed
// receiver does with an object stream it cannot authenticate. A configuration that already skips
// objects keeps doing so. The decision is for the whole stream, because the signature is.
//
// A presentation of object audio alone has nothing to play without its objects; Decoder::decode()
// refuses it with DecodeError::kUnsupported, and the caller reports that. An intermediate spatial
// format presentation is rendered into channels by the decoder and is not degraded.
[[nodiscard]] ICLFORGE_AC4_EXPORT DecoderConfig soft_mode_config(DecoderConfig base,
                                                                 bool verified) noexcept;

}  // namespace iclforge::ac4::signing
