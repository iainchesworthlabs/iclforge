// The MP4 carrier of an AC-4 content signature (mp4_signature_carrier.hpp): a stream encoded here,
// signed, written to an MP4 with the signature in a `uuid` box, and read back and verified. The
// property the carrier exists for is that the samples of the MP4 are the frames the elementary
// stream's signature covers, so one signature serves both forms; the cases hold that, and the
// ways an MP4 goes wrong: a changed sample, a changed box, two boxes, no box, another codec.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ac4_signing_fixture.hpp"
#include "iclforge/ac4/signing/carrier.hpp"
#include "iclforge/ac4/signing/content_signing.hpp"
#include "iclforge/base/crypto/content_signature.hpp"
#include "iclforge/base/crypto/sha256.hpp"
#include "iclforge/containers/mp4/mp4.hpp"
#include "iclforge/containers/mp4/reader.hpp"
#include "mp4_signature_carrier.hpp"
#include "test_pki.hpp"

namespace {

namespace mp4 = iclforge::containers::mp4;
namespace signing = iclforge::ac4::signing;
namespace crypto = iclforge::base::crypto;
namespace pki = iclforge::test::pki;
using E = crypto::ContentSignatureError;
using Bytes = std::vector<std::byte>;

constexpr std::int64_t kNow = 1793232000;  // 2026-10-29T00:00:00Z

using Encoded = iclforge::test::media_signing::Encoded;

struct Fixture {
    const Encoded& stream = iclforge::test::media_signing::shared();
    pki::Authority authority = pki::make_authority();
    crypto::SigningCredentials credentials{authority.leaf.keys.key, authority.chain()};
    Bytes payload;

    Fixture() {
        auto signed_payload =
            signing::sign_frames(stream.views(), credentials, pki::at(2026, 10, 28, 12));
        REQUIRE(signed_payload.has_value());
        payload = std::move(*signed_payload);
    }

    [[nodiscard]] crypto::TrustStore trust() const {
        crypto::TrustStore store;
        store.anchors.push_back(crypto::TrustAnchor{authority.root.cert, "test root"});
        return store;
    }
    [[nodiscard]] static crypto::VerifyOptions options() {
        crypto::VerifyOptions o;
        o.chain.now_unix = kNow;
        return o;
    }

    // The MP4 of the stream with `boxes` on its track.
    [[nodiscard]] Bytes mp4_with(std::vector<mp4::UserBox> boxes) const {
        mp4::AudioTrack track = stream.track;
        track.user_boxes = std::move(boxes);
        const auto file = mp4::mux(track, stream.views());
        REQUIRE(file.has_value());
        return *file;
    }
    [[nodiscard]] Bytes signed_mp4() const {
        return mp4_with({iclforge::apps::content_signature_user_box(payload)});
    }

    [[nodiscard]] auto verify(const signing::CarriedContent& content) const {
        return signing::verify_frames(content, trust(), options());
    }
};

}  // namespace

TEST_CASE("an AC-4 MP4 signed in its uuid box verifies", "[ac4][signing][mp4]") {
    const Fixture f;
    const Bytes file = f.signed_mp4();
    const iclforge::apps::Mp4UuidCarrier carrier;
    REQUIRE(carrier.name() == "MP4 uuid box");

    const auto content = carrier.read(file, {});
    REQUIRE(content.has_value());
    REQUIRE(content->signature_present);
    REQUIRE(content->signature == f.payload);
    REQUIRE(content->frames.size() == f.stream.frames.size());
    for (std::size_t i = 0; i < content->frames.size(); ++i) {
        const Bytes& raw = f.stream.frames[i].raw_ac4_frame;
        REQUIRE(std::equal(raw.begin(), raw.end(), content->frames[i].begin(),
                           content->frames[i].end()));
    }
    const auto verified = f.verify(*content);
    REQUIRE(verified.has_value());
    REQUIRE(verified->leaf().serial_hex() == "1798A2B5D62000000005");
}

TEST_CASE("the digest of the MP4's samples is the digest of the stream", "[ac4][signing][mp4]") {
    const Fixture f;
    const Bytes file = f.signed_mp4();

    // Computed apart from the library: the samples laid end to end, and SHA-256 of them.
    const auto demuxed = mp4::demux(file);
    REQUIRE(demuxed.has_value());
    Bytes laid_end_to_end;
    for (const auto sample : demuxed->samples) {
        laid_end_to_end.insert(laid_end_to_end.end(), sample.begin(), sample.end());
    }
    const crypto::Digest256 expected = crypto::sha256(laid_end_to_end);
    REQUIRE(signing::digest_of_frames(f.stream.views()) == expected);

    const iclforge::apps::Mp4UuidCarrier carrier;
    const auto content = carrier.read(file, {});
    REQUIRE(content.has_value());
    REQUIRE(signing::digest_of_frames(content->frames) == expected);

    // So the signature recorded in the payload is over exactly these bytes.
    const auto parsed = crypto::parse_content_signature(f.payload);
    REQUIRE(parsed.has_value());
    const auto& recorded = parsed->signature_values.signed_info.digests.front().digest_value;
    REQUIRE(recorded.size() == expected.size());
    REQUIRE(std::equal(expected.begin(), expected.end(), recorded.begin()));
}

TEST_CASE("one signature serves the elementary stream and the MP4 alike", "[ac4][signing][mp4]") {
    const Fixture f;
    const Bytes file = f.signed_mp4();
    const Bytes elementary = f.stream.elementary();

    const iclforge::apps::Mp4UuidCarrier mp4_carrier;
    const signing::ElementaryCarrier elementary_carrier;

    const auto from_mp4 = mp4_carrier.read(file, {});
    const auto from_elementary = elementary_carrier.read(elementary, f.payload);
    REQUIRE(from_mp4.has_value());
    REQUIRE(from_elementary.has_value());
    REQUIRE(from_mp4->signature == from_elementary->signature);
    REQUIRE(signing::digest_of_frames(from_mp4->frames) ==
            signing::digest_of_frames(from_elementary->frames));
    REQUIRE(f.verify(*from_mp4).has_value());
    REQUIRE(f.verify(*from_elementary).has_value());
}

TEST_CASE("a changed MP4 fails however it is changed", "[ac4][signing][mp4]") {
    const Fixture f;
    const iclforge::apps::Mp4UuidCarrier carrier;
    const Bytes file = f.signed_mp4();

    const auto demuxed = mp4::demux(file);
    REQUIRE(demuxed.has_value());
    REQUIRE(demuxed->samples.size() >= 3);

    SECTION("a bit flipped in each sample's first, middle and last byte") {
        for (std::size_t i = 0; i < demuxed->samples.size(); ++i) {
            const auto sample = demuxed->samples[i];
            const auto start = static_cast<std::size_t>(sample.data() - file.data());
            for (const std::size_t at : {std::size_t{0}, sample.size() / 2, sample.size() - 1}) {
                CAPTURE(i, at);
                Bytes changed = file;
                changed[start + at] ^= std::byte{0x01};
                const auto content = carrier.read(changed, {});
                REQUIRE(content.has_value());
                const auto result = f.verify(*content);
                REQUIRE_FALSE(result.has_value());
                REQUIRE(result.error().error == E::kSignatureInvalid);
            }
        }
    }
    SECTION("a bit flipped in the signature payload in the box") {
        const auto& box = demuxed->track.user_boxes.front();
        // Find the payload in the file: it is the box's bytes, once.
        const auto at =
            std::search(file.begin(), file.end(), box.payload.begin(), box.payload.end());
        REQUIRE(at != file.end());
        Bytes changed = file;
        changed[static_cast<std::size_t>(at - file.begin()) + box.payload.size() / 2] ^=
            std::byte{0x20};
        const auto content = carrier.read(changed, {});
        REQUIRE(content.has_value());
        REQUIRE_FALSE(f.verify(*content).has_value());
    }
    SECTION("an MP4 muxed from fewer frames, carrying the original signature") {
        mp4::AudioTrack track = f.stream.track;
        track.user_boxes = {iclforge::apps::content_signature_user_box(f.payload)};
        auto views = f.stream.views();
        views.pop_back();
        const auto shorter = mp4::mux(track, views);
        REQUIRE(shorter.has_value());
        const auto content = carrier.read(*shorter, {});
        REQUIRE(content.has_value());
        const auto result = f.verify(*content);
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().error == E::kSignatureInvalid);
    }
}

TEST_CASE("an MP4 with no signature box is unsigned", "[ac4][signing][mp4]") {
    const Fixture f;
    const iclforge::apps::Mp4UuidCarrier carrier;
    const Bytes file = f.mp4_with({});
    const auto content = carrier.read(file, {});
    REQUIRE(content.has_value());
    REQUIRE_FALSE(content->signature_present);
    REQUIRE(content->frames.size() == f.stream.frames.size());
    const auto result = f.verify(*content);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().error == E::kBadOrMissingSignature);
}

TEST_CASE("a uuid box with another usertype is not a signature", "[ac4][signing][mp4]") {
    const Fixture f;
    const iclforge::apps::Mp4UuidCarrier carrier;
    mp4::UserBox other = iclforge::apps::content_signature_user_box(f.payload);
    other.usertype[15] ^= std::byte{0x01};  // the UUID, one bit off
    const Bytes file = f.mp4_with({other});
    const auto content = carrier.read(file, {});
    REQUIRE(content.has_value());
    REQUIRE_FALSE(content->signature_present);

    // And alongside a real one, it is not counted.
    const Bytes both = f.mp4_with({other, iclforge::apps::content_signature_user_box(f.payload)});
    const auto with_both = carrier.read(both, {});
    REQUIRE(with_both.has_value());
    REQUIRE(with_both->signature_present);
    REQUIRE(f.verify(*with_both).has_value());
}

TEST_CASE("two signature boxes are ambiguous and refused", "[ac4][signing][mp4]") {
    const Fixture f;
    const iclforge::apps::Mp4UuidCarrier carrier;
    const auto box = iclforge::apps::content_signature_user_box(f.payload);
    const Bytes file = f.mp4_with({box, box});
    const auto content = carrier.read(file, {});
    REQUIRE_FALSE(content.has_value());
    REQUIRE(content.error().kind == signing::CarrierErrorKind::kAmbiguousSignature);
}

TEST_CASE("a file that is not an AC-4 MP4 is refused with the reason", "[ac4][signing][mp4]") {
    const Fixture f;
    const iclforge::apps::Mp4UuidCarrier carrier;

    SECTION("not an MP4") {
        const Bytes noise(2000, std::byte{0x41});
        const auto content = carrier.read(noise, {});
        REQUIRE_FALSE(content.has_value());
        REQUIRE(content.error().kind == signing::CarrierErrorKind::kContainerUnreadable);
    }
    SECTION("an empty file") {
        const auto content = carrier.read({}, {});
        REQUIRE_FALSE(content.has_value());
        REQUIRE(content.error().kind == signing::CarrierErrorKind::kContainerUnreadable);
    }
    SECTION("an MP4 truncated in the middle of its samples") {
        const Bytes file = f.signed_mp4();
        const Bytes cut(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(file.size() / 2));
        const auto content = carrier.read(cut, {});
        // Either the reader refuses it or it reads fewer samples; a verdict on fewer samples
        // fails the signature, which is the point.
        if (content.has_value()) {
            REQUIRE_FALSE(f.verify(*content).has_value());
        }
    }
    SECTION("an AC-3 track") {
        mp4::AudioTrack track;
        track.codec_id = std::string{mp4::kCodecAc3};
        track.codec_config = {std::byte{0x10}, std::byte{0x3D}, std::byte{0xC0}};
        const std::vector<Bytes> frames(3, Bytes(128, std::byte{0x0B}));
        const auto file = mp4::mux(track, frames);
        REQUIRE(file.has_value());
        const auto content = carrier.read(*file, {});
        REQUIRE_FALSE(content.has_value());
        REQUIRE(content.error().kind == signing::CarrierErrorKind::kNoAc4Track);
        REQUIRE(content.error().detail.find("ac-3") != std::string::npos);
    }
}

TEST_CASE("a fragmented MP4 carries the signature in its initialization segment",
          "[ac4][signing][mp4]") {
    const Fixture f;
    const iclforge::apps::Mp4UuidCarrier carrier;
    mp4::AudioTrack track = f.stream.track;
    track.user_boxes = {iclforge::apps::content_signature_user_box(f.payload)};
    mp4::FragmentOptions options;
    options.frames_per_fragment = 3;
    const auto fragmented = mp4::fragment(track, f.stream.views(), options);
    REQUIRE(fragmented.has_value());
    REQUIRE(fragmented->media_segments.size() >= 2);

    // The box is in the init segment and not in the media segments.
    const auto init = carrier.read(fragmented->init_segment, {});
    REQUIRE(init.has_value());
    REQUIRE(init->signature_present);
    REQUIRE(init->frames.empty());

    // One file made of the init segment and the segments reads as a whole and verifies.
    Bytes whole = fragmented->init_segment;
    for (const mp4::MediaSegment& segment : fragmented->media_segments) {
        whole.insert(whole.end(), segment.bytes.begin(), segment.bytes.end());
    }
    const auto content = carrier.read(whole, {});
    REQUIRE(content.has_value());
    REQUIRE(content->frames.size() == f.stream.frames.size());
    REQUIRE(f.verify(*content).has_value());
}

TEST_CASE("the signature box is the one the specification's UUID names", "[ac4][signing][mp4]") {
    const std::array<unsigned, 16> expected{0xB7, 0x15, 0xF9, 0xDB, 0x6B, 0x41, 0x40, 0x3D,
                                            0x8B, 0xBE, 0x42, 0x9F, 0x32, 0x09, 0xCA, 0x2E};
    const Bytes payload(5, std::byte{1});
    const mp4::UserBox box = iclforge::apps::content_signature_user_box(payload);
    for (std::size_t i = 0; i < expected.size(); ++i) {
        REQUIRE(std::to_integer<unsigned>(box.usertype[i]) == expected[i]);
    }
    REQUIRE(box.payload == payload);
}
