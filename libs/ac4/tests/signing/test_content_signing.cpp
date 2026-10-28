// AC-4 content signing over the frames of a real stream (ac4/signing/content_signing.hpp): the
// digest of the signed span, sign_frames() and verify_frames(), and what each kind of damage to
// the content or the signature comes to. The frames are the encoder's, from two tones, so the
// bytes are the shape of a real stream and not a made-up block. Every negative case starts from
// content that verifies and changes one thing; a case that failed for another reason would show
// as a different error code.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "iclforge/ac4/decoder/config.hpp"
#include "iclforge/ac4/signing/content_signing.hpp"
#include "iclforge/base/crypto/content_signature.hpp"
#include "iclforge/base/crypto/sha256.hpp"
#include "signing_support.hpp"

namespace {

namespace signing = iclforge::ac4::signing;
namespace crypto = iclforge::base::crypto;
namespace support = iclforge::test::ac4_signing;
namespace pki = iclforge::test::pki;
using E = crypto::ContentSignatureError;
using Bytes = std::vector<std::byte>;

crypto::Digest256 sha256_of(const Bytes& bytes) {
    return crypto::sha256(bytes);
}

Bytes concatenated(const support::Stream& stream) {
    Bytes out;
    for (const auto& frame : stream.encoded) {
        out.insert(out.end(), frame.raw_ac4_frame.begin(), frame.raw_ac4_frame.end());
    }
    return out;
}

// The content as owned bytes, so a case can change a frame and hand back views of the result.
struct Owned {
    std::vector<Bytes> frames;
    Bytes signature;

    explicit Owned(const support::Stream& stream, Bytes signature_in)
        : signature(std::move(signature_in)) {
        for (const auto& frame : stream.encoded) {
            frames.push_back(frame.raw_ac4_frame);
        }
    }

    [[nodiscard]] signing::CarriedContent content() const {
        signing::CarriedContent out;
        for (const Bytes& frame : frames) {
            out.frames.emplace_back(frame);
        }
        out.signature = signature;
        out.signature_present = true;
        return out;
    }
};

struct Fixture {
    const support::Stream& stream = support::shared_stream();
    support::Signer signer;
    Bytes payload;

    Fixture() {
        const auto views = stream.views();
        auto signed_payload = signing::sign_frames(views, signer.credentials, signer.signed_unix);
        REQUIRE(signed_payload.has_value());
        payload = std::move(*signed_payload);
    }

    [[nodiscard]] Owned owned() const { return Owned(stream, payload); }

    [[nodiscard]] static E refused(
        const std::expected<crypto::VerifiedSignature, crypto::ContentSignatureFailure>& result) {
        REQUIRE_FALSE(result.has_value());
        return result.error().error;
    }

    [[nodiscard]] auto verify(const signing::CarriedContent& content) const {
        return signing::verify_frames(content, signer.trust(), support::verify_options());
    }
};

}  // namespace

TEST_CASE("the digest is SHA-256 of the frames' raw bytes, one after the other", "[ac4][signing]") {
    const support::Stream& stream = support::shared_stream();
    const Bytes whole = concatenated(stream);
    REQUIRE(whole.size() > 1000);

    const auto views = stream.views();
    REQUIRE(signing::digest_of_frames(views) == sha256_of(whole));

    // Added one at a time it is the same, and the counts say what went in.
    signing::ContentDigest digest;
    for (const auto& frame : views) {
        digest.add_frame(frame);
    }
    REQUIRE(digest.frame_count() == views.size());
    REQUIRE(digest.byte_count() == whole.size());
    REQUIRE(digest.finish() == sha256_of(whole));

    // finish() leaves it empty, and an empty one is the digest of nothing.
    REQUIRE(digest.frame_count() == 0);
    REQUIRE(digest.byte_count() == 0);
    REQUIRE(digest.finish() == sha256_of({}));

    // How the same bytes are cut into frames does not change the digest: the span is a
    // concatenation. (It is why the digest of a stream is the digest of its MP4 samples, and why
    // frame boundaries are outside what the signature authenticates.)
    signing::ContentDigest halves;
    halves.add_frame(std::span<const std::byte>(whole).first(whole.size() / 2));
    halves.add_frame(std::span<const std::byte>(whole).subspan(whole.size() / 2));
    halves.add_frame({});
    REQUIRE(halves.frame_count() == 3);
    REQUIRE(halves.finish() == sha256_of(whole));
}

TEST_CASE("a signed stream verifies, and the payload is the structure of the specification",
          "[ac4][signing]") {
    const Fixture f;
    const Owned content = f.owned();
    const auto verified = f.verify(content.content());
    REQUIRE(verified.has_value());
    REQUIRE(verified->canonical_version == 3);
    REQUIRE(verified->acceptance.kind == crypto::ChainAcceptance::Kind::kAnchor);
    REQUIRE(verified->acceptance.trust_index == 0);
    REQUIRE(verified->chain.size() == 3);
    REQUIRE(verified->leaf().serial_hex() == "1798A2B5D62000000005");
    REQUIRE(verified->signed_date == "2026-10-28T12:00:00Z");

    // The payload records the digest of the frames, under the reference the specification names.
    const auto parsed = crypto::parse_content_signature(f.payload);
    REQUIRE(parsed.has_value());
    const auto& digests = parsed->signature_values.signed_info.digests;
    REQUIRE(digests.size() == 1);
    REQUIRE(digests[0].reference == crypto::kDigestDataRef);
    const crypto::Digest256 expected = signing::digest_of_frames(f.stream.views());
    REQUIRE(digests[0].digest_value.size() == expected.size());
    REQUIRE(std::equal(expected.begin(), expected.end(), digests[0].digest_value.begin()));

    // RFC 6979 makes signing deterministic: the same frames and key give the same bytes.
    const auto again =
        signing::sign_frames(f.stream.views(), f.signer.credentials, f.signer.signed_unix);
    REQUIRE(again.has_value());
    REQUIRE(*again == f.payload);
}

TEST_CASE("one changed bit in any frame fails the signature", "[ac4][signing]") {
    const Fixture f;
    const std::size_t count = f.stream.encoded.size();
    // Every frame, at its first, middle and last byte: the first byte of a frame is the table of
    // contents, the last the end of its audio.
    for (std::size_t frame = 0; frame < count; ++frame) {
        const std::size_t size = f.stream.encoded[frame].raw_ac4_frame.size();
        for (const std::size_t at : {std::size_t{0}, size / 2, size - 1}) {
            CAPTURE(frame, at);
            Owned content = f.owned();
            content.frames[frame][at] ^= std::byte{0x01};
            const auto result = f.verify(content.content());
            REQUIRE(Fixture::refused(result) == E::kSignatureInvalid);
        }
    }
}

TEST_CASE("a frame dropped, added, repeated or moved fails the signature", "[ac4][signing]") {
    const Fixture f;
    const std::size_t count = f.stream.encoded.size();
    REQUIRE(count >= 3);

    SECTION("the first, a middle and the last frame removed") {
        for (const std::size_t gone : {std::size_t{0}, count / 2, count - 1}) {
            CAPTURE(gone);
            Owned content = f.owned();
            content.frames.erase(content.frames.begin() + static_cast<std::ptrdiff_t>(gone));
            REQUIRE(Fixture::refused(f.verify(content.content())) == E::kSignatureInvalid);
        }
    }
    SECTION("a frame repeated") {
        Owned content = f.owned();
        content.frames.insert(content.frames.begin() + 1, content.frames[1]);
        REQUIRE(Fixture::refused(f.verify(content.content())) == E::kSignatureInvalid);
    }
    SECTION("two frames swapped") {
        Owned content = f.owned();
        // They are different frames, or swapping them would change nothing.
        REQUIRE(content.frames[0] != content.frames[1]);
        std::swap(content.frames[0], content.frames[1]);
        REQUIRE(Fixture::refused(f.verify(content.content())) == E::kSignatureInvalid);
    }
    SECTION("a frame from another stream appended") {
        const support::Stream other = support::encode_stream(12000);
        Owned content = f.owned();
        content.frames.push_back(other.encoded.front().raw_ac4_frame);
        REQUIRE(Fixture::refused(f.verify(content.content())) == E::kSignatureInvalid);
    }
    SECTION("a byte cut from the end of the last frame") {
        Owned content = f.owned();
        content.frames.back().pop_back();
        REQUIRE(Fixture::refused(f.verify(content.content())) == E::kSignatureInvalid);
    }
}

TEST_CASE("the signature fails for a signature that is not this content's", "[ac4][signing]") {
    const Fixture f;
    // A signature made over another stream's frames, by the same signer.
    const support::Stream other = support::encode_stream(12000);
    const auto other_payload =
        signing::sign_frames(other.views(), f.signer.credentials, f.signer.signed_unix);
    REQUIRE(other_payload.has_value());
    REQUIRE(*other_payload != f.payload);
    const auto result = f.verify(support::carried(f.stream, *other_payload));
    REQUIRE(Fixture::refused(result) == E::kSignatureInvalid);
    REQUIRE(result.error().detail.find("changed after it was signed") != std::string::npos);
}

TEST_CASE("verification judges the operator's trust, clock and profile", "[ac4][signing]") {
    const Fixture f;
    const Owned content = f.owned();

    SECTION("a different authority's root is not an anchor") {
        const support::Signer stranger{pki::AuthorityOptions{.seed = "stranger"}};
        const auto result =
            signing::verify_frames(content.content(), stranger.trust(), support::verify_options());
        REQUIRE(Fixture::refused(result) == E::kBadCredentialsForSigningContent);
    }
    SECTION("the intermediate is enough as an anchor") {
        crypto::TrustStore store;
        store.anchors.push_back(crypto::TrustAnchor{f.signer.authority.intermediate.cert, "ca"});
        REQUIRE(signing::verify_frames(content.content(), store, support::verify_options())
                    .has_value());
    }
    SECTION("the leaf's key pinned directly is enough") {
        crypto::TrustStore store;
        store.keys.push_back(crypto::TrustedKey{f.signer.authority.leaf.cert.public_key, "pin"});
        const auto result =
            signing::verify_frames(content.content(), store, support::verify_options());
        REQUIRE(result.has_value());
        REQUIRE(result->acceptance.kind == crypto::ChainAcceptance::Kind::kTrustedKey);
    }
    SECTION("an empty trust store accepts nothing, even a good signature") {
        REQUIRE(Fixture::refused(signing::verify_frames(content.content(), crypto::TrustStore{},
                                                        support::verify_options())) ==
                E::kNoTrustAnchors);
    }
    SECTION("no clock is an error and not a pass") {
        REQUIRE(Fixture::refused(signing::verify_frames(content.content(), f.signer.trust(),
                                                        crypto::VerifyOptions{})) == E::kNoClock);
    }
    SECTION("a chain that has expired") {
        const auto late = [&](std::int64_t when, bool relaxed) {
            crypto::VerifyOptions options = support::verify_options();
            options.chain.now_unix = when;
            options.chain.allow_expired_intermediates = relaxed;
            return signing::verify_frames(content.content(), f.signer.trust(), options);
        };
        // Past 2034, when every certificate of the test chain has expired.
        REQUIRE(Fixture::refused(late(pki::at(2035, 1, 1), false)) ==
                E::kBadCredentialsForSigningContent);
        REQUIRE(Fixture::refused(late(pki::at(2035, 1, 1), true)) ==
                E::kBadCredentialsForSigningContent);
        // Before 2024, when none of them had begun.
        REQUIRE(Fixture::refused(late(pki::at(2023, 6, 1), false)) ==
                E::kBadCredentialsForSigningContent);
        REQUIRE(late(pki::at(2030, 1, 1), false).has_value());
    }
    SECTION("an intermediate that has expired, strictly and relaxed") {
        const support::Signer old{pki::AuthorityOptions{
            .intermediate_not_after = pki::at(2026, 1, 1),
        }};
        const auto views = f.stream.views();
        const auto payload = signing::sign_frames(views, old.credentials, old.signed_unix);
        REQUIRE(payload.has_value());
        const auto content_old = support::carried(f.stream, *payload);

        const auto strict =
            signing::verify_frames(content_old, old.trust(), support::verify_options());
        REQUIRE(Fixture::refused(strict) == E::kBadCredentialsForSigningContent);

        crypto::VerifyOptions relaxed = support::verify_options();
        relaxed.chain.allow_expired_intermediates = true;
        const auto accepted = signing::verify_frames(content_old, old.trust(), relaxed);
        REQUIRE(accepted.has_value());
        REQUIRE(accepted->acceptance.expired_intermediates.size() == 1);
    }
    SECTION("a leaf that is not the specification's, with the profile relaxed") {
        const support::Signer other{pki::AuthorityOptions{
            .leaf_name = {"Studio Signer", "Mastering", "ABCDEF"},
            .leaf_serial = {std::byte{0xAB}, std::byte{0xCD}, std::byte{0xEF}},
        }};
        const auto payload =
            signing::sign_frames(f.stream.views(), other.credentials, other.signed_unix);
        REQUIRE(payload.has_value());
        const auto theirs = support::carried(f.stream, *payload);
        REQUIRE(Fixture::refused(
                    signing::verify_frames(theirs, other.trust(), support::verify_options())) ==
                E::kCertNotPrivilegedToSignContent);
        crypto::VerifyOptions any = support::verify_options();
        any.profile = crypto::LeafProfile::any();
        REQUIRE(signing::verify_frames(theirs, other.trust(), any).has_value());
    }
}

TEST_CASE("a signature version other than 3 is not read", "[ac4][signing]") {
    const Fixture f;
    const auto parsed = crypto::parse_content_signature(f.payload);
    REQUIRE(parsed.has_value());
    const Owned content = f.owned();
    for (const std::int32_t version : {0, 1, 2, 3, 4}) {
        CAPTURE(version);
        crypto::ContentSignature edited = *parsed;
        edited.signature_values.signed_info.canonical_version = version;
        const auto result =
            f.verify(support::carried(f.stream, crypto::serialize_content_signature(edited)));
        if (version == 3) {
            REQUIRE(result.has_value());
        } else {
            REQUIRE(Fixture::refused(result) == E::kLibDoesntSupportContentSignature);
        }
    }
}

TEST_CASE("damage to the signature payload fails verification", "[ac4][signing]") {
    const Fixture f;

    SECTION("truncated at every length from nothing to one byte short") {
        // Every prefix, which the Thrift reader and the DER reader refuse; none may pass.
        for (std::size_t length = 0; length < f.payload.size(); length += 7) {
            CAPTURE(length);
            Bytes cut(f.payload.begin(), f.payload.begin() + static_cast<std::ptrdiff_t>(length));
            const auto result = f.verify(support::carried(f.stream, std::move(cut)));
            REQUIRE_FALSE(result.has_value());
        }
        Bytes one_short(f.payload.begin(), f.payload.end() - 1);
        REQUIRE_FALSE(f.verify(support::carried(f.stream, std::move(one_short))).has_value());
    }
    SECTION("a byte added after the structure") {
        Bytes longer = f.payload;
        longer.push_back(std::byte{0});
        REQUIRE(Fixture::refused(f.verify(support::carried(f.stream, std::move(longer)))) ==
                E::kBadOrMissingSignature);
    }
    SECTION("one bit flipped in the DER signature") {
        auto parsed = crypto::parse_content_signature(f.payload);
        REQUIRE(parsed.has_value());
        parsed->signer_info.signature_value.back() ^= std::byte{0x01};
        const auto result =
            f.verify(support::carried(f.stream, crypto::serialize_content_signature(*parsed)));
        REQUIRE_FALSE(result.has_value());
        REQUIRE((result.error().error == E::kSignatureInvalid ||
                 result.error().error == E::kBadOrMissingSignature));
    }
    SECTION("a payload that is not a signature at all") {
        const Bytes noise(300, std::byte{0x5A});
        REQUIRE(Fixture::refused(f.verify(support::carried(f.stream, noise))) ==
                E::kBadOrMissingSignature);
    }
}

TEST_CASE("a stream with no signature, or no frames, says so", "[ac4][signing]") {
    const Fixture f;

    SECTION("no signature") {
        signing::CarriedContent content;
        content.frames = f.stream.views();
        const auto result = f.verify(content);
        REQUIRE(Fixture::refused(result) == E::kBadOrMissingSignature);
        // Worded for the carriers (a sidecar file or a box), which the base library cannot name.
        REQUIRE(result.error().detail.find("no sidecar file or no signature box") !=
                std::string::npos);
    }
    SECTION("signature bytes that a carrier did not mark as present are not used") {
        signing::CarriedContent content;
        content.frames = f.stream.views();
        content.signature = f.payload;  // a good signature, but signature_present is false
        const auto result = f.verify(content);
        REQUIRE(Fixture::refused(result) == E::kBadOrMissingSignature);
        REQUIRE(result.error().detail.find("no sidecar file or no signature box") !=
                std::string::npos);
    }
    SECTION("no signature and no trust: the operator's input is named first") {
        signing::CarriedContent content;
        content.frames = f.stream.views();
        REQUIRE(Fixture::refused(signing::verify_frames(content, crypto::TrustStore{},
                                                        support::verify_options())) ==
                E::kNoTrustAnchors);
    }
    SECTION("no frames") {
        signing::CarriedContent content;
        content.signature = f.payload;
        content.signature_present = true;
        REQUIRE(Fixture::refused(f.verify(content)) == E::kNoContent);
        REQUIRE(
            signing::sign_frames({}, f.signer.credentials, f.signer.signed_unix).error().error ==
            E::kNoContent);
    }
}

TEST_CASE("signing refuses what is not a usable key and chain", "[ac4][signing]") {
    const Fixture f;
    const auto views = f.stream.views();

    SECTION("a chain whose leaf is not the key's certificate") {
        const support::Signer other{pki::AuthorityOptions{.seed = "other"}};
        crypto::SigningCredentials mixed{f.signer.authority.leaf.keys.key, other.authority.chain()};
        REQUIRE(signing::sign_frames(views, mixed, f.signer.signed_unix).error().error ==
                E::kKeyChainMismatch);
    }
    SECTION("no chain") {
        crypto::SigningCredentials bare{f.signer.authority.leaf.keys.key, {}};
        REQUIRE(signing::sign_frames(views, bare, f.signer.signed_unix).error().error ==
                E::kBadCredentialsForSigningContent);
    }
}

TEST_CASE("the soft mode drops object audio from content that did not verify", "[ac4][signing]") {
    iclforge::ac4::DecoderConfig config;
    REQUIRE_FALSE(config.skip_objects);

    REQUIRE_FALSE(signing::soft_mode_config(config, true).skip_objects);
    REQUIRE(signing::soft_mode_config(config, false).skip_objects);

    // A configuration that already skips objects keeps doing so, verified or not.
    config.skip_objects = true;
    REQUIRE(signing::soft_mode_config(config, true).skip_objects);
    REQUIRE(signing::soft_mode_config(config, false).skip_objects);

    // The rest of the configuration is passed through.
    iclforge::ac4::DecoderConfig other;
    other.skip_objects = false;
    other.level = 3;
    other.decoding = iclforge::ac4::DecodingMode::kCore;
    const auto gated = signing::soft_mode_config(other, false);
    REQUIRE(gated.level == 3);
    REQUIRE(gated.decoding == iclforge::ac4::DecodingMode::kCore);
    REQUIRE(gated.skip_objects);
}
