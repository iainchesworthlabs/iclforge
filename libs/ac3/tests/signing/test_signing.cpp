#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

#include "iclforge/ac3/core/crc16.hpp"
#include "iclforge/ac3/decoder/decoder.hpp"
#include "iclforge/ac3/emdf/frame_layout.hpp"
#include "iclforge/ac3/encoder/eac3_frame.hpp"
#include "iclforge/ac3/encoder/silent_frame.hpp"
#include "iclforge/ac3/io/object_strip.hpp"
#include "iclforge/ac3/oba/atmos.hpp"
#include "iclforge/ac3/signing/emdf_atmos_signer.hpp"
#include "iclforge/base/crypto/signing_key.hpp"
#include "iclforge/objects/emdf.hpp"

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
// committed at libs/ac3/fuzz/regressions/fuzz_signing_verify/, where fuzz-regress
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

// --- Several keys: the keyring ------------------------------------------------
namespace {

using Bytes = std::vector<std::byte>;
using Keys = std::vector<iclforge::base::crypto::SigningKey>;

// Frame i of `stream` signed with keys[i % keys.size()]: a stream whose frames
// were signed by different keys, which is the case a keyring exists for.
Bytes sign_frames_alternately(Bytes stream, const Keys& keys) {
    std::size_t off = 0;
    std::size_t i = 0;
    while (off + 6 <= stream.size()) {
        const std::span<std::byte> rest{stream.data() + off, stream.size() - off};
        const std::size_t size = iclforge::ac3::emdf::syncframe_size(rest);
        REQUIRE(off + size <= stream.size());
        REQUIRE(iclforge::ac3::signing::sign_atmos_frame(rest.first(size), keys[i % keys.size()]));
        off += size;
        ++i;
    }
    return stream;
}

// Per access unit: did the decoder find an object layer in it? The container
// is parsed whenever it is there, so this is the question "did the gate leave
// this frame's objects in" without waiting on reconstruction delay.
std::vector<bool> units_with_objects(std::span<const std::byte> stream) {
    const auto units = iclforge::ac3::split_access_units(stream);
    REQUIRE(units.has_value());
    iclforge::ac3::Eac3Decoder decoder;
    std::vector<bool> found;
    for (const auto& unit : *units) {
        const auto decoded = decoder.decode_access_unit(unit);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        found.push_back((*decoded)->object_metadata.has_value());
    }
    return found;
}

// The bed a decode produces, every channel concatenated: one value to compare.
std::vector<float> bed_samples(std::span<const std::byte> stream) {
    const auto units = iclforge::ac3::split_access_units(stream);
    REQUIRE(units.has_value());
    iclforge::ac3::Eac3Decoder decoder;
    std::vector<std::vector<float>> rendered;
    for (const auto& unit : *units) {
        const auto decoded = decoder.decode_access_unit(unit);
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->has_value());
        if (rendered.empty()) {
            rendered.resize((*decoded)->channels.size());
        }
        for (std::size_t ch = 0; ch < rendered.size(); ++ch) {
            rendered[ch].insert(rendered[ch].end(), (*decoded)->channels[ch].begin(),
                                (*decoded)->channels[ch].end());
        }
    }
    std::vector<float> out;
    for (const auto& channel : rendered) {
        out.insert(out.end(), channel.begin(), channel.end());
    }
    return out;
}

// One E-AC-3 syncframe (a silent 5.1 bed) whose block-0 skip field carries an
// EMDF container and then `trailing` - bytes the container's own length field
// does not account for. Silent audio is enough: signing and verifying read the
// frame's structure, not its sound.
Bytes frame_with_container_and(std::span<const std::byte> trailing) {
    const std::array<std::byte, 6> oamd{std::byte{1}, std::byte{2}, std::byte{3},
                                        std::byte{4}, std::byte{5}, std::byte{6}};
    const std::array<iclforge::objects::emdf::Payload, 1> payloads{
        {{.id = iclforge::objects::emdf::kPayloadIdOamd, .bytes = oamd}}};
    Bytes aux = iclforge::objects::emdf::build_container(payloads);
    aux.insert(aux.end(), trailing.begin(), trailing.end());
    const iclforge::ac3::eac3::AccessUnitConfig config{
        .independent = {.bitrate_kbps = 448, .acmod = iclforge::ac3::Acmod::k3_2, .lfe = true}};
    const auto unit = iclforge::ac3::eac3::build_silent_access_unit(config, aux);
    REQUIRE(unit.has_value());
    return unit->bytes;
}

}  // namespace

TEST_CASE("verify_atmos_stream_any accepts a frame signed by any key of the set",
          "[signing][emdf][verify][keyring]") {
    const Keys keys{make_key(0x11), make_key(0x22)};
    const Bytes original = encode_atmos_stream(4, /*emit_objects=*/true);
    // Frames 0 and 2 under key 0x11, 1 and 3 under key 0x22.
    const Bytes mixed = sign_frames_alternately(original, keys);
    const iclforge::base::crypto::SigningKey stranger = make_key(0x99);

    SECTION("both keys together verify every frame, each credited with its own") {
        const auto summary = iclforge::ac3::signing::verify_atmos_stream_any(mixed, keys);
        CHECK(summary.totals.valid == 4);
        CHECK(summary.totals.mismatch == 0);
        REQUIRE(summary.per_key.size() == 2);
        CHECK(summary.per_key[0] == 2);
        CHECK(summary.per_key[1] == 2);
    }

    SECTION("the order of the set changes the credit, never the verdict") {
        const Keys reversed{keys[1], keys[0]};
        const auto summary = iclforge::ac3::signing::verify_atmos_stream_any(mixed, reversed);
        CHECK(summary.totals.valid == 4);
        CHECK(summary.per_key[0] == 2);
        CHECK(summary.per_key[1] == 2);
    }

    SECTION("one key of the two leaves the other's frames mismatched") {
        const Keys only_first{keys[0]};
        const auto summary = iclforge::ac3::signing::verify_atmos_stream_any(mixed, only_first);
        CHECK(summary.totals.valid == 2);
        CHECK(summary.totals.mismatch == 2);
        CHECK(summary.per_key[0] == 2);
    }

    SECTION("a set that holds neither signing key accepts nothing") {
        const Keys wrong{stranger, make_key(0x98)};
        const auto summary = iclforge::ac3::signing::verify_atmos_stream_any(mixed, wrong);
        CHECK(summary.totals.valid == 0);
        CHECK(summary.totals.mismatch == 4);
    }

    SECTION("an empty set, or a set of empty keys, accepts nothing - it is not a pass") {
        const Keys none;
        const auto from_none = iclforge::ac3::signing::verify_atmos_stream_any(mixed, none);
        CHECK(from_none.totals.valid == 0);
        CHECK(from_none.totals.mismatch == 4);
        CHECK(from_none.totals.no_container == 0);

        const Keys blank{iclforge::base::crypto::SigningKey{}};
        const auto from_blank = iclforge::ac3::signing::verify_atmos_stream_any(mixed, blank);
        CHECK(from_blank.totals.valid == 0);
        CHECK(from_blank.totals.mismatch == 4);
    }

    SECTION("an empty key in the set is skipped, not matched") {
        const Keys padded{iclforge::base::crypto::SigningKey{}, keys[0], keys[1]};
        const auto summary = iclforge::ac3::signing::verify_atmos_stream_any(mixed, padded);
        CHECK(summary.totals.valid == 4);
        CHECK(summary.per_key[0] == 0);
        CHECK(summary.per_key[1] == 2);
        CHECK(summary.per_key[2] == 2);
    }

    SECTION("the same key twice is credited to its first entry") {
        const Keys twice{keys[0], keys[0]};
        const auto summary = iclforge::ac3::signing::verify_atmos_stream_any(mixed, twice);
        CHECK(summary.totals.valid == 2);
        CHECK(summary.per_key[0] == 2);
        CHECK(summary.per_key[1] == 0);
    }

    SECTION("a frame with no container has nothing to check, whatever the set") {
        const Bytes bed51 = encode_atmos_stream(3, /*emit_objects=*/false);
        const Keys none;
        for (const Keys* set : {&keys, &none}) {
            const auto summary = iclforge::ac3::signing::verify_atmos_stream_any(bed51, *set);
            CHECK(summary.totals.no_container == 3);
            CHECK(summary.totals.valid == 0);
            CHECK(summary.totals.mismatch == 0);
        }
    }

    SECTION("verify_atmos_frame_any names the accepting key") {
        const Keys reversed{keys[1], keys[0]};
        const auto verdict = iclforge::ac3::signing::verify_atmos_frame_any(
            std::span<const std::byte>{mixed}.first(
                iclforge::ac3::emdf::syncframe_size(mixed)),
            reversed);
        CHECK(verdict.result == iclforge::ac3::signing::VerifyResult::kValid);
        // Frame 0 was signed with keys[0], which is entry 1 of `reversed`.
        CHECK(verdict.key_index == 1);
    }
}

TEST_CASE("a container that is not the frame's whole object layer does not verify",
          "[signing][emdf][verify][keyring]") {
    const iclforge::base::crypto::SigningKey key = make_key(0x44);

    SECTION("a container and nothing else verifies - the shape the signer's own pipeline writes") {
        Bytes frame = frame_with_container_and({});
        REQUIRE(iclforge::ac3::signing::sign_atmos_frame(frame, key));
        CHECK(iclforge::ac3::signing::verify_atmos_frame(frame, key) ==
              iclforge::ac3::signing::VerifyResult::kValid);
    }

    SECTION("bytes trailing the container in its skip field are outside the tag") {
        // The skip field is a hole in the authenticated message and only the
        // container's own bytes are hashed, so these ride along unauthenticated:
        // a frame must not read as valid while carrying them.
        const std::array<std::byte, 3> extra{std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}};
        Bytes frame = frame_with_container_and(extra);
        REQUIRE(iclforge::ac3::signing::sign_atmos_frame(frame, key));
        CHECK(iclforge::ac3::signing::verify_atmos_frame(frame, key) ==
              iclforge::ac3::signing::VerifyResult::kMismatch);
        const Keys keys{key};
        CHECK(iclforge::ac3::signing::verify_atmos_frame_any(frame, keys).result ==
              iclforge::ac3::signing::VerifyResult::kMismatch);
    }

    SECTION("a container with no primary protection field has no tag to match") {
        // protection_length_primary 00 is reserved in Table H.2.5: it names no
        // width, so there is nowhere for a tag to be and nothing for a key to
        // authenticate. Comparing zero bits used to read as a match.
        Bytes frame = frame_with_container_and({});
        const auto layout = iclforge::ac3::emdf::walk_frame(frame);
        REQUIRE(layout.supported);
        REQUIRE(layout.has_container);
        // Table H.2.5: code 2 is a 32-bit primary field and code 1 an 8-bit
        // secondary one, which is the pair build_container writes.
        REQUIRE(layout.protection_primary_code == 2);
        REQUIRE(layout.protection_secondary_code == 1);
        // protection_length_primary, protection_length_secondary, then the
        // 32 + 8 tag bits, end the parsed container.
        const std::size_t code_at =
            layout.container_start + layout.container_parsed_bits - 32 - 8 - 2 - 2;
        for (std::size_t bit = code_at; bit < code_at + 2; ++bit) {
            frame[bit >> 3] &= static_cast<std::byte>(~(0x80u >> (bit & 7)) & 0xFFu);
        }
        const std::uint16_t crc =
            iclforge::ac3::crc16(std::span<const std::byte>(frame).subspan(2, frame.size() - 4));
        frame[frame.size() - 2] = static_cast<std::byte>(crc >> 8);
        frame[frame.size() - 1] = static_cast<std::byte>(crc & 0xFF);

        const auto reread = iclforge::ac3::emdf::walk_frame(frame);
        REQUIRE(reread.has_container);
        REQUIRE(reread.protection_primary_code == 0);
        CHECK(iclforge::ac3::signing::verify_atmos_frame(frame, key) ==
              iclforge::ac3::signing::VerifyResult::kMismatch);
        const Keys keys{key};
        CHECK(iclforge::ac3::signing::verify_atmos_frame_any(frame, keys).result ==
              iclforge::ac3::signing::VerifyResult::kMismatch);
        // And signing it is not "signed": there is no field to write into.
        Bytes again = frame;
        CHECK_FALSE(iclforge::ac3::signing::sign_atmos_frame(again, key));
        CHECK(again == frame);
    }
}

// --- The licensed gate --------------------------------------------------------
TEST_CASE("gate_atmos_stream keeps objects where the tag verifies and the bed where it does not",
          "[signing][emdf][gate]") {
    const Keys keys{make_key(0x11)};
    const iclforge::base::crypto::SigningKey other = make_key(0x22);
    const Bytes original = encode_atmos_stream(6, /*emit_objects=*/true);

    // Frames 0, 1 and 5 under our key; 2 and 3 under a key we do not hold; 4
    // left exactly as the encoder wrote it, unsigned.
    Bytes mixed = original;
    {
        std::size_t off = 0;
        std::size_t index = 0;
        while (off + 6 <= mixed.size()) {
            const std::span<std::byte> rest{mixed.data() + off, mixed.size() - off};
            const std::size_t size = iclforge::ac3::emdf::syncframe_size(rest);
            const auto frame = rest.first(size);
            if (index == 2 || index == 3) {
                REQUIRE(iclforge::ac3::signing::sign_atmos_frame(frame, other));
            } else if (index != 4) {
                REQUIRE(iclforge::ac3::signing::sign_atmos_frame(frame, keys[0]));
            }
            off += size;
            ++index;
        }
    }

    const auto gated = iclforge::ac3::signing::gate_atmos_stream(mixed, keys);
    REQUIRE(gated.has_value());

    SECTION("the summary counts each outcome and credits the key") {
        CHECK(gated->summary.passed == 3);
        CHECK(gated->summary.gated == 3);
        CHECK(gated->summary.no_objects == 0);
        REQUIRE(gated->summary.per_key.size() == 1);
        CHECK(gated->summary.per_key[0] == 3);
    }

    SECTION("exactly the frames that verified still carry their object layer") {
        const std::vector<bool> expected{true, true, false, false, false, true};
        CHECK(units_with_objects(gated->bytes) == expected);
        // Gating only removes: shorter by what the three frames' object layers held.
        CHECK(gated->bytes.size() < mixed.size());
    }

    SECTION("the bed is untouched - sample for sample the same as the ungated decode") {
        CHECK(bed_samples(gated->bytes) == bed_samples(mixed));
    }

    SECTION("a frame that passed is copied through byte for byte") {
        const std::size_t first = iclforge::ac3::emdf::syncframe_size(mixed);
        CHECK(std::equal(mixed.begin(), mixed.begin() + static_cast<std::ptrdiff_t>(first),
                         gated->bytes.begin()));
    }

    SECTION("gating is idempotent: a gated stream gates to itself") {
        const auto twice = iclforge::ac3::signing::gate_atmos_stream(gated->bytes, keys);
        REQUIRE(twice.has_value());
        CHECK(twice->bytes == gated->bytes);
        CHECK(twice->summary.passed == 3);
        CHECK(twice->summary.gated == 0);
        CHECK(twice->summary.no_objects == 3);
    }
}

TEST_CASE("gate_atmos_stream at its edges", "[signing][emdf][gate]") {
    const Keys keys{make_key(0x11), make_key(0x22)};

    SECTION("a fully signed stream comes back byte for byte") {
        const Bytes signed_stream =
            sign_frames_alternately(encode_atmos_stream(4, /*emit_objects=*/true), keys);
        const auto gated = iclforge::ac3::signing::gate_atmos_stream(signed_stream, keys);
        REQUIRE(gated.has_value());
        CHECK(gated->bytes == signed_stream);
        CHECK(gated->summary.passed == 4);
        CHECK(gated->summary.gated == 0);
        CHECK(gated->summary.per_key[0] == 2);
        CHECK(gated->summary.per_key[1] == 2);
    }

    SECTION("with no key at all every object layer goes: the bed plays, nothing is refused") {
        const Bytes original = encode_atmos_stream(4, /*emit_objects=*/true);
        const Keys none;
        const auto gated = iclforge::ac3::signing::gate_atmos_stream(original, none);
        REQUIRE(gated.has_value());
        CHECK(gated->summary.passed == 0);
        CHECK(gated->summary.gated == 4);
        // The same bytes strip_objects gives - the gate adds no rewrite of its own.
        const auto stripped = iclforge::ac3::io::strip_objects(original);
        REQUIRE(stripped.has_value());
        CHECK(gated->bytes == stripped->bytes);
    }

    SECTION("a stream with no object layer is not an error - it comes back as it was") {
        const Bytes bed51 = encode_atmos_stream(3, /*emit_objects=*/false);
        const auto gated = iclforge::ac3::signing::gate_atmos_stream(bed51, keys);
        REQUIRE(gated.has_value());
        CHECK(gated->bytes == bed51);
        CHECK(gated->summary.no_objects == 3);
        CHECK(gated->summary.passed == 0);
        CHECK(gated->summary.gated == 0);
    }

    SECTION("an AC-3 stream carries no object layer and passes as it is") {
        Bytes ac3_stream;
        for (int f = 0; f < 3; ++f) {
            const auto frame = iclforge::ac3::build_silent_stereo_frame({.bitrate_kbps = 192});
            REQUIRE(frame.has_value());
            ac3_stream.insert(ac3_stream.end(), frame->begin(), frame->end());
        }
        const auto gated = iclforge::ac3::signing::gate_atmos_stream(ac3_stream, keys);
        REQUIRE(gated.has_value());
        CHECK(gated->bytes == ac3_stream);
        CHECK(gated->summary.no_objects == 3);
    }

    SECTION("an object layer in a shape that can be neither verified nor removed fails closed") {
        // Stereo E-AC-3 carrying TS 103 420 §8.3.1's object marker: outside
        // what the frame walker maps, so its container (were there one) could
        // not be authenticated and its layer cannot be rewritten out. Handing
        // it on would play objects nothing vouched for.
        const iclforge::ac3::eac3::AccessUnitConfig config{
            .independent = {.bitrate_kbps = 192,
                            .acmod = iclforge::ac3::Acmod::k2_0,
                            .oba_complexity_index = 1}};
        const auto unit = iclforge::ac3::eac3::build_silent_access_unit(config);
        REQUIRE(unit.has_value());
        const auto gated = iclforge::ac3::signing::gate_atmos_stream(unit->bytes, keys);
        REQUIRE_FALSE(gated.has_value());
        CHECK(gated.error() == iclforge::ac3::io::StripError::kUnsupportedFrame);
    }

    SECTION("an empty stream, and one that does not start on a syncframe, are refused") {
        const auto empty = iclforge::ac3::signing::gate_atmos_stream({}, keys);
        REQUIRE_FALSE(empty.has_value());
        CHECK(empty.error() == iclforge::ac3::io::StripError::kEmpty);

        const Bytes junk(64, std::byte{0xA5});
        const auto lost = iclforge::ac3::signing::gate_atmos_stream(junk, keys);
        REQUIRE_FALSE(lost.has_value());
    }
}
