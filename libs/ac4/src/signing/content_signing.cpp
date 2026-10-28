#include "iclforge/ac4/signing/content_signing.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>
#include <vector>

#include "iclforge/base/crypto/content_signature.hpp"
#include "iclforge/base/crypto/sha256.hpp"

namespace iclforge::ac4::signing {

namespace {

namespace crypto = iclforge::base::crypto;

[[nodiscard]] crypto::ContentSignatureFailure no_content() {
    return crypto::ContentSignatureFailure{
        .error = crypto::ContentSignatureError::kNoContent,
        .detail = "the stream has no frames, so there is nothing to sign or to verify",
        .chain = std::nullopt,
    };
}

}  // namespace

void ContentDigest::add_frame(std::span<const std::byte> raw_ac4_frame) {
    hash_.update(raw_ac4_frame);
    bytes_ += raw_ac4_frame.size();
    ++frames_;
}

crypto::Digest256 ContentDigest::finish() {
    crypto::Digest256 digest{};
    hash_.finish(digest);
    frames_ = 0;
    bytes_ = 0;
    return digest;
}

crypto::Digest256 digest_of_frames(std::span<const std::span<const std::byte>> frames) {
    ContentDigest digest;
    for (const std::span<const std::byte> frame : frames) {
        digest.add_frame(frame);
    }
    return digest.finish();
}

std::expected<std::vector<std::byte>, crypto::ContentSignatureFailure> sign_frames(
    std::span<const std::span<const std::byte>> frames,
    const crypto::SigningCredentials& credentials, std::int64_t signed_unix) {
    if (frames.empty()) {
        return std::unexpected(no_content());
    }
    return crypto::sign_digest(digest_of_frames(frames), credentials, signed_unix);
}

std::expected<crypto::VerifiedSignature, crypto::ContentSignatureFailure> verify_frames(
    const CarriedContent& content, const crypto::TrustStore& trust,
    const crypto::VerifyOptions& options) {
    if (content.frames.empty()) {
        return std::unexpected(no_content());
    }
    // The digest is the verifier's own, from the frames as they are now. Nothing the payload says
    // about the content is used to decide what the content is.
    const crypto::Digest256 digest = digest_of_frames(content.frames);

    if (!content.signature_present) {
        // verify_digest() on no payload reports the operator's inputs first (an empty trust
        // store, no clock) and then that there is no signature, so a command with no trust
        // configured says that and not "unsigned". Its answer is replaced only in words.
        auto verdict = crypto::verify_digest({}, digest, trust, options);
        if (!verdict.has_value() &&
            verdict.error().error == crypto::ContentSignatureError::kBadOrMissingSignature) {
            verdict.error().detail =
                "the stream carries no content signature (no sidecar file or no signature box)";
        }
        return verdict;
    }
    return crypto::verify_digest(content.signature, digest, trust, options);
}

DecoderConfig soft_mode_config(DecoderConfig base, bool verified) noexcept {
    base.skip_objects = base.skip_objects || !verified;
    return base;
}

}  // namespace iclforge::ac4::signing
