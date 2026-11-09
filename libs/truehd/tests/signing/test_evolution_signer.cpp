#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <regex>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "../encoder/encoder_support.hpp"
#include "../support/evolution_support.hpp"
#include "iclforge/base/crypto/hmac_sha256.hpp"
#include "iclforge/base/crypto/signing_key.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"
#include "iclforge/truehd/encoder/encoder.hpp"
#include "iclforge/truehd/io/access_unit.hpp"
#include "iclforge/truehd/io/elementary.hpp"
#include "iclforge/truehd/io/evolution.hpp"
#include "iclforge/truehd/signing/evolution_signer.hpp"
#include "platform/process.hpp"

// The per-frame evolution tag (libs/truehd/include/iclforge/truehd/signing/evolution_signer.hpp):
// ICL Forge's own bring-your-own-key scheme, not a Dolby tag. Every key here is made by the test
// from a seed; the frames and regions are written by tests/support/evolution_support.hpp, which
// shares no code with the signer, so a signed region is compared with one the test builds itself.

namespace iclforge::truehd::signing {
namespace {

using base::crypto::SigningKey;
using testing::bytes_of;
using testing::FrameSpec;

constexpr std::array<unsigned, 3> kCodes{1, 2, 3};

SigningKey key_a() {
    return testing::make_test_key(11);
}
SigningKey key_b() {
    return testing::make_test_key(22, 48);
}

std::vector<std::byte> signed_unit(const SigningKey& key, const FrameSpec& spec,
                                   bool major_sync = true) {
    std::vector<std::byte> unit = testing::build_unit(spec, major_sync);
    REQUIRE(sign_evolution_unit(unit, key, major_sync ? 0 : 1));
    return unit;
}

// ----------------------------------------------------------------------------------------------
// Streams

struct BuiltStream {
    std::vector<std::byte> bytes;
    std::vector<std::size_t> unit_offsets;  // where each unit starts
    std::vector<int> evolution_units;       // indexes of the units that carry a frame
    int frames = 0;
};

// An encoded stream whose even units carry an evolution frame of their own (different payload
// bytes in each, so no two tags are alike).
BuiltStream build_stream(bool with_frames = true, int primary_code = 2) {
    EncoderConfig config;
    config.restart_interval_units = 8;
    config.extra_data = [with_frames, primary_code](std::uint64_t index) {
        if (!with_frames || index % 2 != 0) {
            return std::vector<std::byte>{};
        }
        FrameSpec spec = testing::sample_spec(static_cast<unsigned>(index));
        spec.primary_code = static_cast<unsigned>(primary_code);
        return testing::build_frame(spec);
    };
    const auto encoded = testing::encode_all(config, testing::make_pcm(2, 40 * 12, 24, 3));
    BuiltStream out;
    int index = 0;
    for (const EncodedAccessUnit& unit : encoded.units) {
        out.unit_offsets.push_back(out.bytes.size());
        out.bytes.insert(out.bytes.end(), unit.bytes.begin(), unit.bytes.end());
        if (with_frames && index % 2 == 0) {
            out.evolution_units.push_back(index);
            ++out.frames;
        }
        ++index;
    }
    out.unit_offsets.push_back(out.bytes.size());
    return out;
}

std::span<const std::byte> unit_of(const BuiltStream& s, std::span<const std::byte> bytes, int i) {
    const auto at = s.unit_offsets[static_cast<std::size_t>(i)];
    return bytes.subspan(at, s.unit_offsets[static_cast<std::size_t>(i) + 1] - at);
}

// Where the extra data of unit `i` starts and how long it is, in the stream.
std::pair<std::size_t, std::size_t> extra_of(const BuiltStream& s, int i) {
    const auto unit = unit_of(s, s.bytes, i);
    // The first unit of the stream has a major sync, the rest inherit its substream count.
    const auto parsed = parse_access_unit(unit, 1, true);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->extra_data.has_value());
    return {s.unit_offsets[static_cast<std::size_t>(i)] +
                static_cast<std::size_t>(parsed->extra_data->bytes.data() - unit.data()),
            parsed->extra_data->bytes.size()};
}

std::vector<std::int32_t> decode_stream(std::span<const std::byte> stream) {
    Decoder decoder;
    ElementaryReader reader(stream);
    std::vector<std::int32_t> pcm;
    while (const auto unit = reader.next()) {
        auto decoded = decoder.decode(*unit);
        REQUIRE(decoded.has_value());
        if (decoded->has_value()) {
            pcm.insert(pcm.end(), (**decoded).pcm.begin(), (**decoded).pcm.end());
        }
    }
    return pcm;
}

// ----------------------------------------------------------------------------------------------

TEST_CASE(
    "a signed stream verifies under its key, the parity still holds and the audio is the same",
    "[truehd][signing]") {
    const BuiltStream built = build_stream();
    REQUIRE(built.frames >= 5);
    std::vector<std::byte> stream = built.bytes;
    const SigningKey key = key_a();
    CHECK(verify_evolution_stream(stream, key).valid == 0);  // unsigned: the field is zero

    CHECK(sign_evolution_stream(stream, key) == built.frames);
    const VerifySummary summary = verify_evolution_stream(stream, key);
    CHECK(summary.valid == built.frames);
    CHECK(summary.mismatch == 0);
    CHECK(summary.no_container == static_cast<int>(built.unit_offsets.size()) - 1 - built.frames);

    for (const int unit : built.evolution_units) {
        const auto [at, size] = extra_of(built, unit);
        const auto extra = read_extra_data(std::span<const std::byte>(stream).subspan(at, size));
        REQUIRE(extra.has_value());
        CHECK(extra->check_ok);
        CHECK(extra->framing_ok);
        CHECK(extra->parity_ok);
        // The lengths did not move.
        CHECK(unit_of(built, stream, unit).size() == unit_of(built, built.bytes, unit).size());
    }
    CHECK(decode_stream(stream) == decode_stream(built.bytes));
}

TEST_CASE("a signed unit is the one an independent writer builds with the right tag",
          "[truehd][signing]") {
    const SigningKey key = key_a();
    for (const unsigned code : kCodes) {
        for (const bool major : {true, false}) {
            CAPTURE(code, major);
            FrameSpec spec = testing::sample_spec(7);
            spec.primary_code = code;
            const std::vector<std::byte> unit = signed_unit(key, spec, major);
            const FrameSpec want = testing::signed_spec(key, spec, major);
            CHECK(testing::expected_tag(key, spec, major).size() == (code == 1   ? 1U
                                                                     : code == 2 ? 4U
                                                                                 : 16U));
            // The tag, the fill and the parity byte alike are what the test's writer makes.
            CHECK(unit == testing::build_unit(want, major));
            CHECK(verify_evolution_unit(unit, key, major ? 0 : 1) == VerifyResult::kValid);
        }
    }
}

TEST_CASE("the wrong key fails every frame and an empty key signs and accepts nothing",
          "[truehd][signing]") {
    const BuiltStream built = build_stream();
    std::vector<std::byte> stream = built.bytes;
    REQUIRE(sign_evolution_stream(stream, key_a()) == built.frames);

    const VerifySummary wrong = verify_evolution_stream(stream, key_b());
    CHECK(wrong.valid == 0);
    CHECK(wrong.mismatch == built.frames);

    const SigningKey empty;
    std::vector<std::byte> untouched = built.bytes;
    CHECK(sign_evolution_stream(untouched, empty) == 0);
    CHECK(untouched == built.bytes);
    const VerifySummary none = verify_evolution_stream(stream, empty);
    CHECK(none.valid == 0);
    CHECK(none.mismatch == built.frames);

    // A key shorter than the block and one longer than it are both just HMAC keys.
    for (const std::size_t size : {std::size_t{1}, std::size_t{64}, std::size_t{200}}) {
        const SigningKey key = testing::make_test_key(5, size);
        const std::vector<std::byte> unit = signed_unit(key, testing::sample_spec(3));
        CHECK(verify_evolution_unit(unit, key) == VerifyResult::kValid);
        CHECK(verify_evolution_unit(unit, key_a()) == VerifyResult::kMismatch);
    }
}

TEST_CASE("a changed bit anywhere the tag covers fails it, and one in the zeroed regions does not",
          "[truehd][signing]") {
    const SigningKey key = key_a();
    FrameSpec spec = testing::sample_spec(9);
    spec.zero_fill_bytes = 3;
    const std::vector<std::byte> good = signed_unit(key, spec);
    REQUIRE(verify_evolution_unit(good, key) == VerifyResult::kValid);
    const std::size_t region_bytes = testing::build_region(spec).size();
    const std::size_t extra_at = good.size() - region_bytes;  // where the extra data begins
    const std::size_t payload_bit0 = (extra_at + 4U) * 8U;
    const std::size_t payload_bits = testing::build_frame(spec).size() * 8U;
    const std::size_t prim_off = payload_bits - 3U * 8U - 4U * 8U;  // before the 3 fill bytes

    const auto flipped = [&good](std::size_t bit) {
        std::vector<std::byte> unit = good;
        unit.at(bit >> 3U) ^= static_cast<std::byte>(0x80U >> (bit & 7U));
        return unit;
    };
    struct Place {
        const char* name;
        std::size_t bit;
    };
    // Covered: the object layer's own fields and the access unit around them.
    const std::array<Place, 12> covered{{
        {"key_id", payload_bit0 + 3},
        {"payload id", payload_bit0 + 6},
        {"configuration (priority)", payload_bit0 + 20},
        {"object metadata byte", payload_bit0 + 10 + 15 + 9 + 8 * 5 + 2},
        {"extra data header word", extra_at * 8U + 20},
        {"evo_frame_byte_length", (extra_at + 3) * 8U + 3},
        {"access unit length", 4},
        {"input timing", 3 * 8U + 2},
        {"major sync", (4 + 6) * 8U + 1},
        {"substream directory", (4 + kMajorSyncBaseBytes) * 8U + 5},
        {"an audio byte", (4 + kMajorSyncBaseBytes + 2 + 3) * 8U + 4},
        {"the tag", payload_bit0 + prim_off + 5},
    }};
    for (const Place& place : covered) {
        CAPTURE(place.name);
        // A flip may make the unit unreadable (no container) or the frame a mismatch; it is never
        // valid.
        const VerifyResult result = verify_evolution_unit(flipped(place.bit), key);
        CHECK(result != VerifyResult::kValid);
    }
    // The ones that leave the unit readable are mismatches, and the audio one is exactly the tag.
    CHECK(verify_evolution_unit(flipped((4 + kMajorSyncBaseBytes + 2 + 3) * 8U + 4), key) ==
          VerifyResult::kMismatch);
    // (A flip in the header or the directory also breaks the unit's own check nibble, so there the
    // unit does not read at all; it is not valid either way.)
    CHECK(verify_evolution_unit(flipped(payload_bit0 + 3), key) == VerifyResult::kMismatch);

    // The parity byte is not in the message: a changed one leaves the tag good (the parity is the
    // extra data's own check, which the reader reports on its own).
    std::vector<std::byte> parity = good;
    parity.at(parity.size() - 1) ^= std::byte{0x5A};
    CHECK(verify_evolution_unit(parity, key) == VerifyResult::kValid);
    CHECK_FALSE(read_extra_data(std::span<const std::byte>(parity).subspan(extra_at))->parity_ok);
    // The fill and the padding are not in it either; the sole-container rule is what keeps them
    // empty, so a change there is a mismatch of that rule and not of the tag.
    CHECK(verify_evolution_unit(flipped(payload_bit0 + payload_bits - 1), key) ==
          VerifyResult::kMismatch);
}

TEST_CASE("the secondary field is zeroed in the message, so its value does not matter",
          "[truehd][signing]") {
    const SigningKey key = key_a();
    FrameSpec plain = testing::sample_spec(2);
    plain.secondary_code = 1;
    FrameSpec other = plain;
    other.secondary = bytes_of({0x5A});

    const std::vector<std::byte> unit = signed_unit(key, plain);
    CHECK(verify_evolution_unit(unit, key) == VerifyResult::kValid);
    // The same tag with a different secondary byte is the same unit as far as the tag goes.
    other.primary = testing::expected_tag(key, plain);
    CHECK(testing::expected_tag(key, other) == other.primary);
    CHECK(verify_evolution_unit(testing::build_unit(other), key) == VerifyResult::kValid);

    // Signing leaves the secondary field as the writer made it.
    std::vector<std::byte> with_secondary = testing::build_unit(other);
    other.primary.clear();
    std::vector<std::byte> unsigned_unit = testing::build_unit(other);
    REQUIRE(sign_evolution_unit(unsigned_unit, key));
    CHECK(unsigned_unit == with_secondary);
}

TEST_CASE("length codes 1, 2 and 3 give a one, four and sixteen byte tag; code 0 has no field",
          "[truehd][signing]") {
    const SigningKey key = key_a();
    for (const unsigned code : kCodes) {
        FrameSpec spec = testing::sample_spec(code);
        spec.primary_code = code;
        const std::vector<std::byte> unit = signed_unit(key, spec);
        const auto parsed = parse_access_unit(unit, 0, true);
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->extra_data.has_value());
        const auto frame = parse_evolution_frame(parsed->extra_data->evolution_payload);
        REQUIRE(frame.has_value());
        CHECK(frame->protection.primary.size() == (code == 1 ? 1U : code == 2 ? 4U : 16U));
        CHECK(frame->protection.primary == testing::expected_tag(key, spec));
    }
    for (const EvolutionReading reading : {EvolutionReading::kEmdf, EvolutionReading::kSpec36}) {
        FrameSpec spec = testing::sample_spec(1);
        spec.primary_code = 0;
        spec.spec36 = reading == EvolutionReading::kSpec36;
        std::vector<std::byte> unit = testing::build_unit(spec);
        const std::vector<std::byte> before = unit;
        CHECK_FALSE(sign_evolution_unit(unit, key, 0, reading));
        CHECK(unit == before);
        CHECK(verify_evolution_unit(unit, key, 0, reading) == VerifyResult::kMismatch);
        const auto parsed = parse_access_unit(unit, 0, true);
        REQUIRE(parsed.has_value());
        CHECK_FALSE(has_evolution_tag(parsed->extra_data->bytes, reading));
    }
}

TEST_CASE("the parity byte follows TH spec 16.4 with a non-zero evo_reserved nibble",
          "[truehd][signing][parity]") {
    // evo_reserved is 0 in every stream there is (§16.8), where the word form and a bare XOR of the
    // two fields agree. With a nibble set they differ, and the unit built here (from the spec's
    // word, not from the library) is what the reader and the signer have to agree with.
    const SigningKey key = key_a();
    for (const unsigned reserved : {0U, 1U, 5U, 15U}) {
        CAPTURE(reserved);
        const FrameSpec spec = testing::sample_spec(2);
        const std::vector<std::byte> frame = testing::build_frame(spec);
        const std::vector<std::byte> unit =
            testing::build_unit(testing::build_region(frame, {}, reserved));
        const auto read = parse_access_unit(unit, 0, true);
        REQUIRE(read.has_value());
        CHECK(read->extra_data->evo_reserved == reserved);
        CHECK(read->extra_data->parity_ok);  // the reader accepts the independent builder's parity

        std::vector<std::byte> signed_bytes = unit;
        REQUIRE(sign_evolution_unit(signed_bytes, key));
        // The nibble is in the unit the tag covers: worked out here over this unit, parity zeroed.
        REQUIRE_FALSE(unit.empty());
        std::vector<std::byte> message(unit.begin(), unit.end() - 1);
        message.push_back(std::byte{0});
        const auto digest = base::crypto::hmac_sha256(key.bytes(), message);
        FrameSpec tagged = spec;
        tagged.primary = {digest[0], digest[1], digest[2], digest[3]};  // code 2: four bytes
        CHECK(signed_bytes == testing::build_unit(testing::build_region(
                                  testing::build_frame(tagged), {}, reserved)));
        const auto after = parse_access_unit(signed_bytes, 0, true);
        REQUIRE(after.has_value());
        CHECK(after->extra_data->parity_ok);
        CHECK(after->extra_data->evo_reserved == reserved);
        CHECK(verify_evolution_unit(signed_bytes, key) == VerifyResult::kValid);
    }
}

TEST_CASE("the same tag is written whichever reading parses the frame", "[truehd][signing]") {
    const SigningKey key = key_a();
    FrameSpec spec = testing::sample_spec(4);
    spec.spec36 = true;
    std::vector<std::byte> unit = testing::build_unit(spec);
    REQUIRE(sign_evolution_unit(unit, key, 0, EvolutionReading::kSpec36));
    CHECK(verify_evolution_unit(unit, key, 0, EvolutionReading::kSpec36) == VerifyResult::kValid);
    CHECK(unit == testing::build_unit(testing::signed_spec(key, spec)));
}

TEST_CASE("frames signed under two keys, spliced, verify under the keyring", "[truehd][signing]") {
    const BuiltStream built = build_stream();
    std::vector<std::byte> by_a = built.bytes;
    std::vector<std::byte> by_b = built.bytes;
    const SigningKey a = key_a();
    const SigningKey b = key_b();
    REQUIRE(sign_evolution_stream(by_a, a) == built.frames);
    REQUIRE(sign_evolution_stream(by_b, b) == built.frames);

    // Units are spliced whole: the tag covers the access unit, so only whole ones can be mixed.
    std::vector<std::byte> spliced = by_a;
    int from_b = 0;
    for (std::size_t i = 0; i < built.evolution_units.size(); ++i) {
        if (i % 2 == 1) {
            const auto unit = built.evolution_units[i];
            const auto at = built.unit_offsets[static_cast<std::size_t>(unit)];
            const auto size = built.unit_offsets[static_cast<std::size_t>(unit) + 1] - at;
            std::copy_n(by_b.begin() + static_cast<std::ptrdiff_t>(at), size,
                        spliced.begin() + static_cast<std::ptrdiff_t>(at));
            ++from_b;
        }
    }
    REQUIRE(from_b >= 2);

    const std::array<SigningKey, 2> ring{a, b};
    const KeyringSummary summary = verify_evolution_stream_any(spliced, ring);
    CHECK(summary.totals.valid == built.frames);
    CHECK(summary.totals.mismatch == 0);
    REQUIRE(summary.per_key.size() == 2);
    CHECK(summary.per_key[1] == from_b);
    CHECK(summary.per_key[0] == built.frames - from_b);
    CHECK(summary.per_key[0] + summary.per_key[1] == summary.totals.valid);

    // One key alone accepts only its own frames.
    CHECK(verify_evolution_stream(spliced, a).valid == built.frames - from_b);

    // A set with no usable key accepts nothing.
    const std::array<SigningKey, 2> empties{SigningKey{}, SigningKey{}};
    const KeyringSummary none = verify_evolution_stream_any(spliced, empties);
    CHECK(none.totals.valid == 0);
    CHECK(none.totals.mismatch == built.frames);
    const KeyringSummary no_keys = verify_evolution_stream_any(spliced, {});
    CHECK(no_keys.totals.valid == 0);
    CHECK(no_keys.totals.mismatch == built.frames);

    // The first accepting key's index comes with the unit.
    const std::vector<std::byte> unit = signed_unit(b, testing::sample_spec(1));
    const KeyringVerdict verdict = verify_evolution_unit_any(unit, ring);
    CHECK(verdict.result == VerifyResult::kValid);
    CHECK(verdict.key_index == 1);
    CHECK(verify_evolution_unit_any(unit, {}).result == VerifyResult::kMismatch);
}

TEST_CASE("a cut or joined stream keeps every per-frame tag valid", "[truehd][signing]") {
    // The tag is per frame and carries nothing from one unit to the next: whole units dropped or
    // joined leave each remaining tag good. This is a property of the format's element, not a
    // weakness to fix here, and it is why a tag is not a stream signature.
    const BuiltStream built = build_stream();
    std::vector<std::byte> stream = built.bytes;
    const SigningKey key = key_a();
    REQUIRE(sign_evolution_stream(stream, key) == built.frames);
    // A stream joined to itself: every tag of both halves is still valid.
    std::vector<std::byte> joined = stream;
    joined.insert(joined.end(), stream.begin(), stream.end());
    const VerifySummary twice = verify_evolution_stream(joined, key);
    CHECK(twice.valid == 2 * built.frames);
    CHECK(twice.mismatch == 0);
}

TEST_CASE("a tag is present without a key once a frame is signed", "[truehd][signing]") {
    const SigningKey key = key_a();
    // No frame at all, and the padding-only form.
    CHECK_FALSE(has_evolution_tag({}));
    CHECK_FALSE(has_evolution_tag(std::vector<std::byte>(8, std::byte{0})));
    // Garbage that is not extra data.
    CHECK_FALSE(has_evolution_tag(bytes_of({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF})));
    // An unsigned frame: the field is all zero.
    const FrameSpec spec = testing::sample_spec(5);
    CHECK_FALSE(has_evolution_tag(testing::build_region(spec)));
    // Signed: present, with no key given to the call.
    std::vector<std::byte> unit = testing::build_unit(spec);
    REQUIRE(sign_evolution_unit(unit, key));
    const auto parsed = parse_access_unit(unit, 0, true);
    REQUIRE(parsed.has_value());
    CHECK(has_evolution_tag(parsed->extra_data->bytes));
    CHECK(count_evolution_tags(unit) == 1);
    // A tag nobody here can reproduce is "present" too, and a mismatch under verify.
    FrameSpec foreign = spec;
    foreign.primary = bytes_of({1, 2, 3, 4});
    const std::vector<std::byte> other = testing::build_unit(foreign);
    const auto other_parsed = parse_access_unit(other, 0, true);
    CHECK(has_evolution_tag(other_parsed->extra_data->bytes));
    CHECK(verify_evolution_unit(other, key) == VerifyResult::kMismatch);
    // An unsigned stream has none.
    CHECK(count_evolution_tags(build_stream().bytes) == 0);
}

TEST_CASE("a frame with more in the extra data than the tag covers does not verify",
          "[truehd][signing]") {
    const SigningKey key = key_a();
    const FrameSpec spec = testing::sample_spec(6);

    SECTION("bytes after the container inside the payload") {
        FrameSpec wide = spec;
        wide.trailing = bytes_of({0xAB, 0xCD});
        // The right tag for the rest of the unit, worked out by the test: the digest matches.
        const FrameSpec tagged = testing::signed_spec(key, wide);
        const std::vector<std::byte> unit = testing::build_unit(tagged);
        CHECK(verify_evolution_unit(unit, key) == VerifyResult::kMismatch);
        std::vector<std::byte> copy = unit;
        CHECK_FALSE(sign_evolution_unit(copy, key));
        CHECK(copy == unit);
    }
    SECTION("a second frame beside the first, in the padding") {
        const FrameSpec tagged = testing::signed_spec(key, spec);
        const std::vector<std::byte> second = testing::build_frame(testing::sample_spec(8));
        const std::vector<std::byte> unit =
            testing::build_unit(testing::build_region(testing::build_frame(tagged), second));
        // The first frame on its own is valid; with a second one next to it, it is not.
        CHECK(verify_evolution_unit(testing::build_unit(tagged), key) == VerifyResult::kValid);
        CHECK(verify_evolution_unit(unit, key) == VerifyResult::kMismatch);
        std::vector<std::byte> copy =
            testing::build_unit(testing::build_region(testing::build_frame(spec), second));
        CHECK_FALSE(sign_evolution_unit(copy, key));
    }
    SECTION("the container alone is valid") {
        const std::vector<std::byte> unit = testing::build_unit(testing::signed_spec(key, spec));
        CHECK(verify_evolution_unit(unit, key) == VerifyResult::kValid);
    }
}

TEST_CASE("a unit with no readable frame is neither verified nor failed", "[truehd][signing]") {
    const SigningKey key = key_a();
    // No extra data at all, and the padding-only form.
    const std::vector<std::byte> bare = testing::stereo_unit(true);
    CHECK(verify_evolution_unit(bare, key) == VerifyResult::kNoContainer);
    CHECK(verify_evolution_unit(testing::build_unit(std::vector<std::byte>(8, std::byte{0})),
                                key) == VerifyResult::kNoContainer);
    // A frame that ends inside a payload does not read.
    std::vector<std::byte> region = testing::build_region(testing::sample_spec(1));
    region[3] = static_cast<std::byte>(3);  // a frame length that cuts it short
    std::vector<std::byte> broken = testing::build_unit(region);
    CHECK(verify_evolution_unit(broken, key) == VerifyResult::kNoContainer);
    CHECK_FALSE(sign_evolution_unit(broken, key));
    // Bytes that are not a unit are no container either.
    std::vector<std::byte> junk(40, std::byte{0x55});
    CHECK(verify_evolution_unit(junk, key) == VerifyResult::kNoContainer);
    CHECK_FALSE(sign_evolution_unit(junk, key));
}

TEST_CASE("the gate passes the units that verify and removes the object layer from the rest",
          "[truehd][signing][gate]") {
    const BuiltStream built = build_stream();
    const SigningKey a = key_a();
    const SigningKey b = key_b();
    REQUIRE(built.frames >= 6);

    // Units 0, 4, 8 ... signed under A; 2, 6 ... under B; the rest of the frames left unsigned.
    std::vector<std::byte> stream = built.bytes;
    int under_a = 0;
    int under_b = 0;
    int unsigned_units = 0;
    for (std::size_t i = 0; i < built.evolution_units.size(); ++i) {
        const int index = built.evolution_units[i];
        const auto from = built.unit_offsets[static_cast<std::size_t>(index)];
        const auto unit = std::span<std::byte>(stream).subspan(
            from, built.unit_offsets[static_cast<std::size_t>(index) + 1] - from);
        const int inherited = index == 0 ? 0 : 1;  // only the first unit carries a major sync
        if (i % 3 == 0) {
            REQUIRE(sign_evolution_unit(unit, a, inherited));
            ++under_a;
        } else if (i % 3 == 1) {
            REQUIRE(sign_evolution_unit(unit, b, inherited));
            ++under_b;
        } else {
            ++unsigned_units;
        }
    }
    REQUIRE(under_a > 0);
    REQUIRE(under_b > 0);
    REQUIRE(unsigned_units > 0);

    const std::array<SigningKey, 1> ring_a{a};
    const auto gated = gate_evolution_stream(stream, ring_a);
    REQUIRE(gated.has_value());
    CHECK(gated->summary.passed == under_a);
    CHECK(gated->summary.gated == under_b + unsigned_units);
    CHECK(gated->summary.no_objects ==
          static_cast<int>(built.unit_offsets.size()) - 1 - built.frames);
    REQUIRE(gated->summary.per_key.size() == 1);
    CHECK(gated->summary.per_key[0] == under_a);
    REQUIRE(gated->bytes.size() == stream.size());

    for (std::size_t i = 0; i < built.evolution_units.size(); ++i) {
        const auto [at, size] = extra_of(built, built.evolution_units[i]);
        const auto before = std::span<const std::byte>(stream).subspan(at, size);
        const auto after = std::span<const std::byte>(gated->bytes).subspan(at, size);
        if (i % 3 == 0) {
            CHECK(std::ranges::equal(before, after));  // passed: byte for byte
        } else {
            CHECK(std::ranges::all_of(after, [](std::byte x) { return x == std::byte{0}; }));
            const auto extra = read_extra_data(after);
            REQUIRE(extra.has_value());
            CHECK(extra->padding_only);
        }
    }
    // Nothing outside the gated extra data moved, and the result gates to itself.
    std::size_t differing = 0;
    for (std::size_t i = 0; i < stream.size(); ++i) {
        differing += stream[i] != gated->bytes[i] ? 1U : 0U;
    }
    CHECK(differing > 0);
    std::vector<std::byte> expected = stream;
    for (std::size_t i = 0; i < built.evolution_units.size(); ++i) {
        if (i % 3 != 0) {
            const auto [at, size] = extra_of(built, built.evolution_units[i]);
            std::fill_n(expected.begin() + static_cast<std::ptrdiff_t>(at), size, std::byte{0});
        }
    }
    CHECK(expected == gated->bytes);
    // The bed plays the same as the stream it was cut from.
    CHECK(decode_stream(gated->bytes) == decode_stream(built.bytes));
    // A gated stream has no evolution frame that fails its tag, so no two in a row (which a
    // licensed decoder with the Atmos flag set treats as fatal, where one is tolerated): what is
    // left is the frames that verify, and that is by construction, not by luck.
    const KeyringSummary left = verify_evolution_stream_any(gated->bytes, ring_a);
    CHECK(left.totals.mismatch == 0);
    CHECK(left.totals.valid == under_a);

    // With both keys the units signed under B pass as well; with none, every frame is gated.
    const std::array<SigningKey, 2> both{a, b};
    const auto wide = gate_evolution_stream(stream, both);
    REQUIRE(wide.has_value());
    CHECK(wide->summary.passed == under_a + under_b);
    CHECK(wide->summary.gated == unsigned_units);
    const auto closed = gate_evolution_stream(stream, {});
    REQUIRE(closed.has_value());
    CHECK(closed->summary.passed == 0);
    CHECK(closed->summary.gated == built.frames);
    // With no key every frame goes, and what is left has none to fail.
    const KeyringSummary none_left = verify_evolution_stream_any(closed->bytes, ring_a);
    CHECK(none_left.totals.valid == 0);
    CHECK(none_left.totals.mismatch == 0);
}

TEST_CASE("a stream with no object layer gates to itself", "[truehd][signing][gate]") {
    const BuiltStream plain = build_stream(false);
    const std::array<SigningKey, 1> ring{key_a()};
    const auto gated = gate_evolution_stream(plain.bytes, ring);
    REQUIRE(gated.has_value());
    CHECK(gated->bytes == plain.bytes);
    CHECK(gated->summary.passed == 0);
    CHECK(gated->summary.gated == 0);
    CHECK(gated->summary.no_objects == static_cast<int>(plain.unit_offsets.size()) - 1);
}

TEST_CASE("the gate fails closed on bytes that are not access units", "[truehd][signing][gate]") {
    const BuiltStream built = build_stream();
    const std::array<SigningKey, 1> ring{key_a()};

    SECTION("no stream at all") {
        const std::vector<std::byte> nothing(64, std::byte{0x55});
        const auto result = gate_evolution_stream(nothing, ring);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == GateErrorKind::kNoStream);
    }
    SECTION("a damaged unit in the middle") {
        std::vector<std::byte> stream = built.bytes;
        const std::size_t at = built.unit_offsets[3];
        stream[at] ^= std::byte{0x10};  // the unit's check nibble
        const auto result = gate_evolution_stream(stream, ring);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == GateErrorKind::kUnreadableUnit);
        CHECK(result.error().skipped_bytes > 0);
    }
    SECTION("bytes ahead of the first unit are not a loss") {
        std::vector<std::byte> stream(100, std::byte{0});
        stream.insert(stream.end(), built.bytes.begin(), built.bytes.end());
        const auto result = gate_evolution_stream(stream, ring);
        REQUIRE(result.has_value());
        CHECK(result->summary.gated == built.frames);
        CHECK(std::equal(result->bytes.begin(), result->bytes.begin() + 100, stream.begin()));
    }
}

TEST_CASE("garbage extra data is gated, not passed", "[truehd][signing][gate]") {
    const BuiltStream built = build_stream();
    std::vector<std::byte> stream = built.bytes;
    // Break the first frame's container: its length now cuts it short, so no frame reads.
    const auto [at, size] = extra_of(built, built.evolution_units.front());
    stream[at + 3] = static_cast<std::byte>(3);
    const std::array<SigningKey, 1> ring{key_a()};
    const auto gated = gate_evolution_stream(stream, ring);
    REQUIRE(gated.has_value());
    CHECK(gated->summary.passed == 0);
    CHECK(gated->summary.gated == built.frames);  // that unit too: it has extra data, no good one
    const auto extra = read_extra_data(std::span<const std::byte>(gated->bytes).subspan(at, size));
    REQUIRE(extra.has_value());
    CHECK(extra->padding_only);
}

// ----------------------------------------------------------------------------------------------
// The independent reference: tools/references/truehd_evolution_signing.py.

std::string hex_of(std::span<const std::byte> bytes) {
    constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (const std::byte b : bytes) {
        out.push_back(kDigits[std::to_integer<unsigned>(b) >> 4U]);
        out.push_back(kDigits[std::to_integer<unsigned>(b) & 15U]);
    }
    return out;
}

std::vector<std::byte> from_hex(const std::string& text) {
    std::vector<std::byte> out;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
        out.push_back(static_cast<std::byte>(std::stoi(text.substr(i, 2), nullptr, 16)));
    }
    return out;
}

TEST_CASE("the Python transcription writes the same bytes as the C++ signer",
          "[truehd][signing][reference]") {
    // Access units: those of an encoded stream that carry a frame, and one of each length code
    // under both readings. The key is made here from a seed; it goes to the script in a file in a
    // directory this test makes and removes.
    const SigningKey key = key_a();
    const BuiltStream built = build_stream();
    struct Case {
        std::vector<std::byte> unit;
        int inherited;
        EvolutionReading reading;
    };
    std::vector<Case> cases;
    for (const int unit : built.evolution_units) {
        const auto bytes = unit_of(built, built.bytes, unit);
        cases.push_back({std::vector<std::byte>(bytes.begin(), bytes.end()), unit == 0 ? 0 : 1,
                         EvolutionReading::kEmdf});
    }
    for (const unsigned code : kCodes) {
        for (const bool spec36 : {false, true}) {
            FrameSpec spec = testing::sample_spec(code + (spec36 ? 10U : 0U));
            spec.primary_code = code;
            spec.secondary_code = code == 2 ? 1U : 0U;
            spec.secondary = bytes_of({0x3C});
            spec.spec36 = spec36;
            cases.push_back({testing::build_unit(spec), 0,
                             spec36 ? EvolutionReading::kSpec36 : EvolutionReading::kEmdf});
        }
    }

    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("iclforge_thd_sign_ref_" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(dir);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    } cleanup{dir};
    const auto script =
        std::filesystem::path(ICLFORGE_TRUEHD_REFERENCE_DIR) / "truehd_evolution_signing.py";
    REQUIRE(std::filesystem::exists(script));

    bool ran = false;
    for (const EvolutionReading reading : {EvolutionReading::kEmdf, EvolutionReading::kSpec36}) {
        nlohmann::json job;
        job["key"] = hex_of(key.bytes());
        job["reading"] = reading == EvolutionReading::kEmdf ? "emdf" : "spec36";
        job["units"] = nlohmann::json::array();
        job["inherited"] = nlohmann::json::array();
        std::vector<const Case*> used;
        for (const Case& one : cases) {
            if (one.reading == reading) {
                job["units"].push_back(hex_of(one.unit));
                job["inherited"].push_back(one.inherited);
                used.push_back(&one);
            }
        }
        const auto in_path = dir / "in.json";
        const auto out_path = dir / "out.json";
        const auto log_path = dir / "run.log";
        std::filesystem::remove(out_path);
        {
            std::ofstream out(in_path, std::ios::binary);
            out << job.dump();
        }
        int status = -1;
        for (const char* python : {"python3", "python"}) {
            const std::string command = std::string("\"") + python + "\" -I \"" + script.string() +
                                        "\" batch \"" + in_path.string() + "\" \"" +
                                        out_path.string() + "\" > \"" + log_path.string() +
                                        "\" 2>&1";
            status = iclforge::test::platform::run_shell(command);
            if (status == 0 && std::filesystem::exists(out_path)) {
                break;
            }
        }
        if (status != 0 || !std::filesystem::exists(out_path)) {
            SKIP("no Python 3 to run tools/references/truehd_evolution_signing.py");
        }
        ran = true;
        std::ifstream result_file(out_path, std::ios::binary);
        const auto result = nlohmann::json::parse(result_file);
        REQUIRE(result["signed"].size() == used.size());
        for (std::size_t i = 0; i < used.size(); ++i) {
            CAPTURE(i);
            std::vector<std::byte> mine = used[i]->unit;
            REQUIRE(sign_evolution_unit(mine, key, used[i]->inherited, reading));
            REQUIRE_FALSE(result["signed"][i].is_null());
            CHECK(result["signed"][i].get<std::string>() == hex_of(mine));
            CHECK(result["verdicts"][i].get<std::string>() == "valid");
            CHECK(verify_evolution_unit(from_hex(result["signed"][i].get<std::string>()), key,
                                        used[i]->inherited, reading) == VerifyResult::kValid);
        }
    }
    CHECK(ran);
}

// ----------------------------------------------------------------------------------------------
// The signer and its tests hold no key.

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

TEST_CASE("the signer and its tests contain no key constant", "[truehd][signing][no-key]") {
    const std::filesystem::path root(ICLFORGE_TRUEHD_SIGNING_SOURCE_DIR);
    const std::array<std::filesystem::path, 4> files{
        root / "include/iclforge/truehd/signing/evolution_signer.hpp",
        root / "src/signing/evolution_signer.cpp",
        root / "tests/signing/test_evolution_signer.cpp",
        root / "tests/support/evolution_support.hpp",
    };
    // A run of eight or more 0xHH bytes, a quoted run of 24 or more base64 characters, a run of 32
    // or more hex digits: the three shapes a key written into source takes. The patterns are built
    // from pieces so that this file does not contain what it looks for.
    const std::string hex_byte = std::string("0") + "x[0-9A-Fa-f]{2}";
    const std::regex byte_array("(" + hex_byte + "\\s*,\\s*){7}" + hex_byte);
    const std::regex base64_literal(std::string("\"[A-Za-z0-9+/=]") + "{24,}\"");
    const std::regex hex_run(std::string("[0-9A-Fa-f]") + "{32,}");
    int scanned = 0;
    for (const auto& file : files) {
        REQUIRE(std::filesystem::exists(file));
        ++scanned;
        const std::string text = read_text(file);
        CAPTURE(file.string());
        CHECK_FALSE(std::regex_search(text, byte_array));
        CHECK_FALSE(std::regex_search(text, base64_literal));
        CHECK_FALSE(std::regex_search(text, hex_run));
    }
    CHECK(scanned == 4);
}

}  // namespace
}  // namespace iclforge::truehd::signing
