// The elementary stream's carrier (ac4/signing/carrier.hpp): a stream and the sidecar beside it,
// taken apart for verification. The stream is the encoder's, framed both ways an ac4_syncframe()
// can be (0xAC40 and 0xAC41 with a CRC word), and the cases are the ways a stream and a sidecar
// go wrong in transit: a truncated copy, a tail of other bytes, a sidecar from another stream.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "iclforge/ac4/io/elementary.hpp"
#include "iclforge/ac4/signing/carrier.hpp"
#include "iclforge/ac4/signing/content_signing.hpp"
#include "signing_support.hpp"

namespace {

namespace signing = iclforge::ac4::signing;
namespace crypto = iclforge::base::crypto;
namespace support = iclforge::test::ac4_signing;
using E = crypto::ContentSignatureError;
using Bytes = std::vector<std::byte>;

struct Fixture {
    const support::Stream& stream = support::shared_stream();
    support::Signer signer;
    Bytes payload;
    signing::ElementaryCarrier carrier;

    Fixture() {
        auto signed_payload =
            signing::sign_frames(stream.views(), signer.credentials, signer.signed_unix);
        REQUIRE(signed_payload.has_value());
        payload = std::move(*signed_payload);
    }

    [[nodiscard]] auto verify(const Bytes& file, const Bytes& sidecar) const {
        const auto content = carrier.read(file, sidecar);
        REQUIRE(content.has_value());
        return signing::verify_frames(*content, signer.trust(), support::verify_options());
    }
};

}  // namespace

TEST_CASE("an elementary stream and its sidecar verify, with and without the CRC word",
          "[ac4][signing][carrier]") {
    const Fixture f;
    REQUIRE(f.carrier.name() == "elementary stream with sidecar");

    for (const bool crc : {false, true}) {
        CAPTURE(crc);
        const Bytes file = f.stream.elementary(crc);
        const auto content = f.carrier.read(file, f.payload);
        REQUIRE(content.has_value());
        REQUIRE(content->signature_present);
        REQUIRE(content->signature == f.payload);
        REQUIRE(content->frames.size() == f.stream.encoded.size());
        // The frames are the raw frames, views into the file, with the framing taken off.
        for (std::size_t i = 0; i < content->frames.size(); ++i) {
            const Bytes& raw = f.stream.encoded[i].raw_ac4_frame;
            REQUIRE(std::equal(raw.begin(), raw.end(), content->frames[i].begin(),
                               content->frames[i].end()));
        }
        REQUIRE(f.verify(file, f.payload).has_value());
    }
}

TEST_CASE("the framing of an elementary stream is outside the signature",
          "[ac4][signing][carrier]") {
    // The signed span is the raw frames (register 3 of the design), so the same signature
    // verifies a stream written with the CRC word and one written without it. This records the
    // property, so that a change to it is a decision and not an accident.
    const Fixture f;
    const Bytes plain = f.stream.elementary(false);
    const Bytes with_crc = f.stream.elementary(true);
    REQUIRE(plain != with_crc);
    REQUIRE(f.verify(plain, f.payload).has_value());
    REQUIRE(f.verify(with_crc, f.payload).has_value());
}

TEST_CASE("a missing sidecar is reported as an unsigned stream", "[ac4][signing][carrier]") {
    const Fixture f;
    const Bytes file = f.stream.elementary(false);
    const auto content = f.carrier.read(file, {});
    REQUIRE(content.has_value());
    REQUIRE_FALSE(content->signature_present);
    const auto result = f.verify(file, {});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().error == E::kBadOrMissingSignature);
}

TEST_CASE("a sidecar from another stream, or damaged, fails", "[ac4][signing][carrier]") {
    const Fixture f;
    const Bytes file = f.stream.elementary(false);

    const support::Stream other = support::encode_stream(12000);
    const auto other_payload =
        signing::sign_frames(other.views(), f.signer.credentials, f.signer.signed_unix);
    REQUIRE(other_payload.has_value());
    const auto wrong = f.verify(file, *other_payload);
    REQUIRE_FALSE(wrong.has_value());
    REQUIRE(wrong.error().error == E::kSignatureInvalid);

    Bytes damaged = f.payload;
    damaged[damaged.size() / 2] ^= std::byte{0x10};
    REQUIRE_FALSE(f.verify(file, damaged).has_value());
}

TEST_CASE("a changed stream fails however the change is made", "[ac4][signing][carrier]") {
    const Fixture f;
    const Bytes file = f.stream.elementary(false);

    SECTION("one bit flipped inside a frame's audio") {
        // 0xAC40 frames have no CRC word, so the carrier reads the frame and the signature fails.
        Bytes changed = file;
        changed[file.size() / 2] ^= std::byte{0x04};
        const auto content = f.carrier.read(changed, f.payload);
        if (content.has_value()) {
            const auto result =
                signing::verify_frames(*content, f.signer.trust(), support::verify_options());
            REQUIRE_FALSE(result.has_value());
            REQUIRE(result.error().error == E::kSignatureInvalid);
        } else {
            // A flipped bit in a frame_size field is also a way to be refused.
            SUCCEED("the carrier refused the stream");
        }
    }
    SECTION("the last frame cut short") {
        const Bytes cut(file.begin(), file.end() - 5);
        const auto content = f.carrier.read(cut, f.payload);
        REQUIRE_FALSE(content.has_value());
        REQUIRE(content.error().kind == signing::CarrierErrorKind::kTruncatedFrame);
        REQUIRE(content.error().offset > 0);
        REQUIRE(content.error().detail.find("ends inside the frame") != std::string::npos);
    }
    SECTION("whole frames removed from the end still scan, and fail the signature") {
        const std::size_t keep = f.stream.encoded.size() - 2;
        Bytes shorter;
        for (std::size_t i = 0; i < keep; ++i) {
            const Bytes sync = iclforge::ac4::sync_frame(f.stream.encoded[i].raw_ac4_frame, false);
            shorter.insert(shorter.end(), sync.begin(), sync.end());
        }
        const auto result = f.verify(shorter, f.payload);
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().error == E::kSignatureInvalid);
    }
    SECTION("bytes after the last frame are refused, not ignored") {
        Bytes longer = file;
        longer.insert(longer.end(), 40, std::byte{0x7E});
        const auto content = f.carrier.read(longer, f.payload);
        REQUIRE_FALSE(content.has_value());
        REQUIRE(content.error().kind == signing::CarrierErrorKind::kLostSync);
        REQUIRE(content.error().offset == file.size());
    }
    SECTION("bytes before the first frame are refused") {
        Bytes longer(8, std::byte{0});
        longer.insert(longer.end(), file.begin(), file.end());
        const auto content = f.carrier.read(longer, f.payload);
        REQUIRE_FALSE(content.has_value());
        REQUIRE(content.error().kind == signing::CarrierErrorKind::kLostSync);
        REQUIRE(content.error().offset == 0);
    }
    SECTION("something that is not an AC-4 stream at all") {
        const Bytes noise(5000, std::byte{0x33});
        const auto content = f.carrier.read(noise, f.payload);
        REQUIRE_FALSE(content.has_value());
        REQUIRE(content.error().kind == signing::CarrierErrorKind::kLostSync);
    }
    SECTION("an empty stream reads as no frames, which has nothing to verify") {
        const auto content = f.carrier.read({}, f.payload);
        REQUIRE(content.has_value());
        REQUIRE(content->frames.empty());
        const auto result =
            signing::verify_frames(*content, f.signer.trust(), support::verify_options());
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().error == E::kNoContent);
    }
}

TEST_CASE("the sidecar is named after the stream", "[ac4][signing][carrier]") {
    REQUIRE(signing::sidecar_path("movie.ac4") == "movie.ac4.sig");
    REQUIRE(signing::sidecar_path("/a/b c/stream") == "/a/b c/stream.sig");
    REQUIRE(signing::sidecar_path("") == ".sig");
}

TEST_CASE("every carrier error has words", "[ac4][signing][carrier]") {
    using K = signing::CarrierErrorKind;
    std::vector<std::string_view> seen;
    for (const K kind : {K::kLostSync, K::kTruncatedFrame, K::kContainerUnreadable, K::kNoAc4Track,
                         K::kAmbiguousSignature}) {
        REQUIRE_FALSE(signing::describe(kind).empty());
        REQUIRE(signing::describe(kind) != "unknown carrier error");
        seen.push_back(signing::describe(kind));
    }
    std::sort(seen.begin(), seen.end());
    REQUIRE(std::adjacent_find(seen.begin(), seen.end()) == seen.end());
}
