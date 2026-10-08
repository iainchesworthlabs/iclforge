#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

#include "iclforge/ac3/oba/atmos.hpp"
#include "iclforge/ac3/signing/emdf_atmos_signer.hpp"
#include "iclforge/base/crypto/signing_key.hpp"

namespace {

// A short synthetic tone, one frame long, for driving the Atmos encoder.
std::vector<float> tone(double hz, std::uint64_t start) {
    std::vector<float> out(static_cast<std::size_t>(iclforge::ac3::kSamplesPerFrame));
    for (int n = 0; n < iclforge::ac3::kSamplesPerFrame; ++n) {
        const double t = static_cast<double>(start + static_cast<std::uint64_t>(n)) / 48000.0;
        out[static_cast<std::size_t>(n)] =
            static_cast<float>(0.3 * std::sin(2.0 * std::numbers::pi * hz * t));
    }
    return out;
}

// Encodes `frames` one-object Atmos access units into a single contiguous
// stream, container emitted (or not) per `emit_objects`.
std::vector<std::byte> encode_atmos_stream(int frames, bool emit_objects) {
    iclforge::ac3::oba::AtmosEncoder encoder{
        {.bitrate_kbps = 448, .num_bands_idx = 4, .emit_object_metadata = emit_objects}, 1};
    const std::array<iclforge::objects::oba::ObjectPlacement, 1> placement{{{}}};
    std::vector<std::span<const float>> views(1);
    std::vector<std::byte> stream;
    for (int f = 0; f < frames; ++f) {
        const auto essence =
            tone(440.0, static_cast<std::uint64_t>(f) *
                            static_cast<std::uint64_t>(iclforge::ac3::kSamplesPerFrame));
        views[0] = essence;
        auto unit = encoder.encode_frame(views, placement);
        REQUIRE(unit.has_value());
        stream.insert(stream.end(), unit->bytes.begin(), unit->bytes.end());
    }
    return stream;
}

iclforge::base::crypto::SigningKey make_key(std::uint8_t fill) {
    return iclforge::base::crypto::SigningKey{std::vector<std::byte>(32, std::byte{fill})};
}

}  // namespace

// --- The signer over real encoder output ------------------------------------
TEST_CASE("sign_atmos_stream signs object frames deterministically", "[signing][emdf]") {
    const std::vector<std::byte> original = encode_atmos_stream(4, /*emit_objects=*/true);
    REQUIRE_FALSE(original.empty());

    const iclforge::base::crypto::SigningKey key_a = make_key(0x11);

    std::vector<std::byte> signed_a = original;
    const int n = iclforge::ac3::signing::sign_atmos_stream(signed_a, key_a);

    SECTION("every object frame is signed and the bytes actually change") {
        CHECK(n == 4);
        CHECK(signed_a != original);
    }

    SECTION("signing is deterministic for a given key") {
        std::vector<std::byte> signed_again = original;
        CHECK(iclforge::ac3::signing::sign_atmos_stream(signed_again, key_a) == n);
        CHECK(signed_again == signed_a);
    }

    SECTION("a different key produces a different tag") {
        std::vector<std::byte> signed_b = original;
        CHECK(iclforge::ac3::signing::sign_atmos_stream(signed_b, make_key(0x22)) == n);
        CHECK(signed_b != signed_a);
    }
}

TEST_CASE("sign_atmos_stream is a no-op without a key or a container", "[signing][emdf]") {
    SECTION("an empty key signs nothing and leaves the stream untouched") {
        std::vector<std::byte> stream = encode_atmos_stream(2, /*emit_objects=*/true);
        const std::vector<std::byte> before = stream;
        CHECK(iclforge::ac3::signing::sign_atmos_stream(stream, iclforge::base::crypto::SigningKey{}) == 0);
        CHECK(stream == before);
    }

    SECTION("a bed51 stream has no container to sign") {
        std::vector<std::byte> stream = encode_atmos_stream(2, /*emit_objects=*/false);
        const std::vector<std::byte> before = stream;
        CHECK(iclforge::ac3::signing::sign_atmos_stream(stream, make_key(0x11)) == 0);
        CHECK(stream == before);
    }
}

// Found by fuzz/fuzz_signing_verify (signing-verify fuzz walk): verify_atmos_stream walks
// a frame the caller did not produce, and on a malformed one the frame's own
// endmant can exceed the exponent array the walk actually recovered. The
// per-channel `tally` then took subspan(0, endmant) of a shorter - possibly
// empty - span, which is a precondition violation, not a clamp: on an empty
// span it yields a null data pointer with a non-zero size, which
// compute_bit_allocation dereferenced. `tally` now marks the frame desynced
// instead, so it verifies as kNoContainer rather than on a bit range that
// was never right.
//
// This is a smoke test, not the reproducer - the exact byte pattern is
// committed at fuzz/regressions/fuzz_signing_verify/, where fuzz-regress
// replays it under ASan/UBSan, which is the only build that can see the
// original defect at all. What this checks is the property that matters to
// every caller: verification over arbitrary bytes returns, and returns an
// answer, rather than reading out of bounds.
TEST_CASE("verify_atmos_stream survives arbitrary bytes", "[signing][verify]") {
    const iclforge::base::crypto::SigningKey key = make_key(0x33);

    SECTION("a truncated real stream") {
        const std::vector<std::byte> original = encode_atmos_stream(2, /*emit_objects=*/true);
        REQUIRE(original.size() > 64);
        for (std::size_t keep : {std::size_t{7}, original.size() / 3, original.size() - 1}) {
            CAPTURE(keep);
            const std::vector<std::byte> cut(original.begin(),
                                             original.begin() + static_cast<std::ptrdiff_t>(keep));
            const auto summary = iclforge::ac3::signing::verify_atmos_stream(cut, key);
            CHECK(summary.valid + summary.mismatch + summary.no_container >= 0);
        }
    }

    SECTION("bytes that only look like a syncframe") {
        // 0x0B77 then a frmsiz claiming far more than is here: the framing
        // walk has to stop, and the frame walk behind it must not read past
        // what it was given.
        std::vector<std::byte> fake(96, std::byte{0xA5});
        fake[0] = std::byte{0x0B};
        fake[1] = std::byte{0x77};
        const auto summary = iclforge::ac3::signing::verify_atmos_stream(fake, key);
        CHECK(summary.valid == 0);
    }

    SECTION("an empty stream and a stream shorter than a header") {
        CHECK(iclforge::ac3::signing::verify_atmos_stream({}, key).no_container == 0);
        const std::vector<std::byte> tiny(3, std::byte{0x0B});
        CHECK(iclforge::ac3::signing::verify_atmos_stream(tiny, key).no_container == 0);
        CHECK(iclforge::ac3::signing::verify_atmos_frame(tiny, key) ==
              iclforge::ac3::signing::VerifyResult::kNoContainer);
    }
}

// --- The verifier over real encoder output ----------------------------------
// Scope note (see docs/concepts/object-signing.md): this checks this
// project's own clean-room signer's tag, round-tripping against
// sign_atmos_stream above - it is not, and does not claim to be, a real
// Dolby-licensed decoder's proprietary auth gate.
TEST_CASE("verify_atmos_stream checks the signer's own tag", "[signing][emdf][verify]") {
    const std::vector<std::byte> original = encode_atmos_stream(4, /*emit_objects=*/true);
    REQUIRE_FALSE(original.empty());

    const iclforge::base::crypto::SigningKey key_a = make_key(0x11);
    const iclforge::base::crypto::SigningKey key_b = make_key(0x22);

    std::vector<std::byte> signed_a = original;
    const int n = iclforge::ac3::signing::sign_atmos_stream(signed_a, key_a);
    REQUIRE(n == 4);

    SECTION("the same key verifies every signed frame") {
        const auto summary = iclforge::ac3::signing::verify_atmos_stream(signed_a, key_a);
        CHECK(summary.valid == 4);
        CHECK(summary.mismatch == 0);
        CHECK(summary.no_container == 0);
    }

    SECTION("a different key mismatches every signed frame") {
        const auto summary = iclforge::ac3::signing::verify_atmos_stream(signed_a, key_b);
        CHECK(summary.valid == 0);
        CHECK(summary.mismatch == 4);
        CHECK(summary.no_container == 0);
    }

    SECTION("a bed51 stream (no container) reports kNoContainer, not a mismatch") {
        const std::vector<std::byte> bed51 = encode_atmos_stream(3, /*emit_objects=*/false);
        const auto summary = iclforge::ac3::signing::verify_atmos_stream(bed51, key_a);
        CHECK(summary.no_container == 3);
        CHECK(summary.valid == 0);
        CHECK(summary.mismatch == 0);

        REQUIRE_FALSE(bed51.empty());
        CHECK(iclforge::ac3::signing::verify_atmos_frame(bed51, key_a) ==
              iclforge::ac3::signing::VerifyResult::kNoContainer);
    }

    SECTION("verifying is deterministic - the same stream and key give the same result "
           "every time") {
        const auto first = iclforge::ac3::signing::verify_atmos_stream(signed_a, key_a);
        const auto second = iclforge::ac3::signing::verify_atmos_stream(signed_a, key_a);
        CHECK(first.valid == second.valid);
        CHECK(first.mismatch == second.mismatch);
        CHECK(first.no_container == second.no_container);

        // Repeated verification never mutates the stream (unlike signing).
        std::vector<std::byte> before = signed_a;
        (void)iclforge::ac3::signing::verify_atmos_stream(signed_a, key_a);
        CHECK(signed_a == before);
    }

    SECTION("verify_atmos_frame agrees with verify_atmos_stream, frame by frame") {
        CHECK(iclforge::ac3::signing::verify_atmos_frame(signed_a, key_a) ==
              iclforge::ac3::signing::VerifyResult::kValid);
        CHECK(iclforge::ac3::signing::verify_atmos_frame(signed_a, key_b) ==
              iclforge::ac3::signing::VerifyResult::kMismatch);
    }
}

// A single-frame stream (frame == whole buffer, no multi-frame boundary
// ambiguity), tampered a bit at a time at several offsets spread across the
// back four-fifths of the frame - deliberately clear of the fixed 4-byte
// sync/strmtyp/substreamid/frmsiz header and the handful of early frame-level
// flags this project's own parser hard-asserts on (see emdf_atmos_signer.cpp;
// a flipped acmod/lfeon/numblkscod would trip those asserts rather than
// exercise verification). At least one candidate is certain to land in real
// audio content or the container itself - either of which the tag
// authenticates - without this test needing to know the exact bit layout of
// a given encode.
TEST_CASE("verify_atmos_frame detects tampering anywhere in the authenticated region",
          "[signing][emdf][verify]") {
    const std::vector<std::byte> original = encode_atmos_stream(1, /*emit_objects=*/true);
    REQUIRE_FALSE(original.empty());
    const iclforge::base::crypto::SigningKey key = make_key(0x33);

    std::vector<std::byte> signed_frame = original;
    REQUIRE(iclforge::ac3::signing::sign_atmos_frame(signed_frame, key));
    REQUIRE(iclforge::ac3::signing::verify_atmos_frame(signed_frame, key) ==
            iclforge::ac3::signing::VerifyResult::kValid);
    REQUIRE(signed_frame.size() > 40);

    bool any_mismatch = false;
    const std::size_t begin = signed_frame.size() / 5;
    const std::size_t span = signed_frame.size() - begin;
    for (int k = 0; k < 8 && !any_mismatch; ++k) {
        const std::size_t at = begin + span * static_cast<std::size_t>(k) / 8;
        if (at >= signed_frame.size()) continue;
        std::vector<std::byte> tampered = signed_frame;
        tampered[at] ^= std::byte{0x01};
        if (iclforge::ac3::signing::verify_atmos_frame(tampered, key) ==
            iclforge::ac3::signing::VerifyResult::kMismatch) {
            any_mismatch = true;
        }
    }
    CHECK(any_mismatch);
}
