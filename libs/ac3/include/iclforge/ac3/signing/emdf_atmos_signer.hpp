#pragma once

// EMDF Atmos object-signing - clean-room.
//
// Computes the keyed EMDF-protection primary tag over an iclforge Atmos
// syncframe and writes it into protection_bits_primary (recomputing crc2), so a
// decoder that validates the emdf_protection field will accept the frame's JOC
// object container instead of falling back to the 5.1 bed. See
// docs/concepts/object-signing.md.
//
// Provenance: the HMAC-SHA-256 construction (RFC 2104 / FIPS 180-4) and the
// choice of which frame regions are authenticated are derived from the public
// container layout this codec already emits (libs/objects/src/emdf.cpp,
// ETSI TS 103 420 / the E-AC-3 syntax) and reuse this project's own clean-room
// parsing primitives (BitReader, decode_exponents, compute_bit_allocation, the
// spx helpers). The ONLY externally-provisioned input is the key (SigningKey),
// which the operator supplies at runtime and this code never embeds - the same
// posture a licensed tool (DEE, via iLok) takes with its own key. A stream
// signed with a key that does not match a given decoder's simply fails that
// decoder's check, exactly as an unsigned one does; nothing here reconstructs a
// key.
//
// verify_atmos_frame/verify_atmos_stream below check a tag this same signer
// wrote - round-trip testing, tamper detection on this project's own signed
// test assets, and CI/delivery QC. That is NOT the same thing as, and grants
// no interoperability with, a real Dolby-licensed decoder's own proprietary
// auth gate: that one uses a completely different key and scheme baked into
// Dolby's binary, and this project has deliberately never attempted to forge
// or replicate it. See docs/concepts/object-signing.md.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include "iclforge/ac3/export.hpp"
#include "iclforge/ac3/io/object_strip.hpp"
#include "iclforge/base/crypto/signing_key.hpp"

namespace iclforge::ac3::signing {

// Signs, in place, every syncframe in `stream` that carries an EMDF object
// container (OAMD payload), using `key`. Frames without a container are left
// untouched. Returns the number of frames signed.
//
// Scope: the iclforge "atmos" output - a single independent 5.1 substream,
// frame-level exponent strategy and SNR, no coupling. A frame outside that
// subset is left unsigned rather than signed wrong - the same "nothing to
// do here" answer every entry point in this file gives a frame it does not
// recognise, not a caller error - iclforge::ac3::emdf::walk_frame's own `supported`
// field (ac3/emdf/frame_layout.hpp) is what draws that scope.
[[nodiscard]] ICLFORGE_AC3_EXPORT int sign_atmos_stream(std::span<std::byte> stream,
                                                       const base::crypto::SigningKey& key);

// One syncframe. Returns true if it carried a container and was signed.
[[nodiscard]] ICLFORGE_AC3_EXPORT bool sign_atmos_frame(std::span<std::byte> frame,
                                                       const base::crypto::SigningKey& key);

// Whether this syncframe carries a non-zero authenticity tag - that is,
// whether anyone has signed it - answered WITHOUT a key.
//
// Where the tag lives is fixed by the EMDF container's own protection-length
// codes (§H.2.2.4); only whether it is the RIGHT tag needs the key. So the
// two questions separate cleanly, and an inspection tool (`forge probe`) can
// report that a stream is signed, and by how many frames, while holding
// nothing secret. False for a frame with no container, for one whose
// container declares no primary protection field, and for one whose field is
// all zeros - which is what this project's own writer leaves behind until
// sign_atmos_frame replaces it.
//
// Note what this does NOT claim: a true here says a tag is present, never
// that it is valid. Only verify_atmos_frame below, with the key, says that.
[[nodiscard]] ICLFORGE_AC3_EXPORT bool has_authenticity_tag(std::span<const std::byte> frame);

// A frame with no EMDF object container is neither "verified" nor "failed" -
// there is nothing in it to check - so that case is its own outcome
// (kNoContainer) rather than being folded into kMismatch, which would
// misreport every plain/non-Atmos frame as a signature failure.
enum class VerifyResult : std::uint8_t {
    kNoContainer,  // no EMDF object container - nothing to verify
    kValid,        // container present, tag matches `key`
    kMismatch,     // container present, tag does not match `key`
};

// Frame counts by outcome, mirroring sign_atmos_stream's own aggregate
// (frames signed) rather than a per-frame vector - a caller wants "how many
// verified, how many didn't, how many had nothing to check", the same shape
// apply_object_signing's own callers already consume.
struct VerifySummary {
    int valid = 0;
    int mismatch = 0;
    int no_container = 0;
};

// Checks every syncframe in `stream` against `key`, without modifying it.
//
// Verifying runs on a stream the caller did not produce (`forge decode
// ... verify-objects` points it at whatever arrived), so a plain non-Atmos
// E-AC-3 frame is an ordinary input here, answered with kNoContainer, not a
// caller error - see sign_atmos_stream's own comment above for where that
// tolerance actually lives.
[[nodiscard]] ICLFORGE_AC3_EXPORT VerifySummary
verify_atmos_stream(std::span<const std::byte> stream, const base::crypto::SigningKey& key);

// One syncframe. Mirrors sign_atmos_frame's exact construction (excise the
// framing/metadata/skip/CRC holes into message A, zero the tag bits in the
// container to build message B, HMAC(key, A||B) truncated to the primary
// protection field's width) but reads the existing protection_bits_primary
// bits instead of writing computed ones, and compares.
[[nodiscard]] ICLFORGE_AC3_EXPORT VerifyResult
verify_atmos_frame(std::span<const std::byte> frame, const base::crypto::SigningKey& key);

// --- Several keys: the keyring ------------------------------------------------
//
// One operator rarely holds one key. A facility signs with a key per show, per
// year or per customer, and a QC pass over a delivery has to accept a stream
// signed by any of them - and, because signing is per frame, a stream that was
// spliced from material signed by different ones. A keyring is nothing more
// than that: an ordered set of keys, tried in turn, where a frame is valid when
// ANY of them reproduces the tag it carries. It adds no authority: every key
// in it is one the operator provisioned, exactly as for a single key, and a
// tag none of them reproduces is a mismatch exactly as before.
//
// The functions are named apart from the single-key ones above rather than
// overloaded on a span of keys, deliberately: `verify_atmos_frame(frame, {})`
// would be ambiguous between a default key and an empty span, which is a
// compile error in a caller that wrote neither.
//
// An empty key (SigningKey{}) in the set is skipped - it can authenticate
// nothing - and a set with no usable key accepts nothing: every frame that
// carries a container is a mismatch, never a pass.

struct KeyringVerdict {
    VerifyResult result = VerifyResult::kNoContainer;
    // Which key of the set accepted the frame - the first, in order, that
    // reproduced its tag. Meaningful only when `result` is kValid.
    std::size_t key_index = 0;
};

// One syncframe against every key in `keys`. kNoContainer when the frame has
// nothing to check (whatever the keys), kValid with the accepting key's index,
// kMismatch when it has a container and none of the keys reproduces its tag.
[[nodiscard]] ICLFORGE_AC3_EXPORT KeyringVerdict
verify_atmos_frame_any(std::span<const std::byte> frame,
                       std::span<const base::crypto::SigningKey> keys);

struct KeyringSummary {
    // The same three counts verify_atmos_stream gives, with `valid` meaning
    // "valid under some key of the set".
    VerifySummary totals;
    // per_key[i] is how many frames key i accepted - first accepting key
    // wins, so the entries add up to totals.valid. Sized to the key set (an
    // empty key's entry stays 0).
    std::vector<int> per_key;
};

[[nodiscard]] ICLFORGE_AC3_EXPORT KeyringSummary
verify_atmos_stream_any(std::span<const std::byte> stream,
                        std::span<const base::crypto::SigningKey> keys);

// --- The licensed decode policy: objects only where the tag is good -----------
//
// What a licensed AVR does: a stream whose object layer authenticates is
// reconstructed as objects, and one that does not - signed with another key,
// unsigned, or altered after signing - is not refused but played as its 5.1
// bed, the decode carrying on. verify_atmos_stream's policy is the other one
// (a mismatch fails the whole job); this is the permissive half of the pair.
//
// It is built from what already exists, not from a second path into the
// decoder: a frame that does not verify has its object layer REMOVED, by
// iclforge::ac3::io::strip_objects's own per-frame rewrite, and the result is
// an ordinary stream any decoder - this project's or anyone's - plays as the
// bed, bit-identically (that tool's guarantee). A frame that does verify is
// copied through byte for byte. So the gate is a pure function of (stream,
// keys), testable without a decoder, and Eac3Decoder still never learns that
// signing exists.
//
// The decision is per syncframe, like the tag. A stream with a good first half
// and an unsigned second half therefore plays as objects and then as its bed;
// the reconstruction state simply finds no object layer from there on.
//
// FAIL CLOSED. A frame that carries an object layer this build can neither
// verify nor remove - an object-bearing shape outside the walker's scope
// (emdf/frame_layout.hpp) - is an error, not a pass: handing it on would play
// objects nothing authenticated. The error is strip_objects's own
// (kUnsupportedFrame, kFrameSizeDependentField), and so are the framing ones
// (kEmpty, kLostSync, kTruncated). A stream with no object layer in it at all
// is not an error; it comes back byte-identical with every frame counted
// under `no_objects`. An AC-3 syncframe cannot carry one, and passes as is.
struct GateSummary {
    // Frames whose object container verified under some key: kept as they were.
    int passed = 0;
    // Frames whose object layer was removed: a container no key reproduced
    // (unsigned frames included), or an object marker with no readable
    // container to verify.
    int gated = 0;
    // Frames with no object layer to gate: plain E-AC-3, a bed51 stream, AC-3.
    int no_objects = 0;
    // As KeyringSummary::per_key, over the `passed` frames.
    std::vector<int> per_key;
};

struct GatedStream {
    std::vector<std::byte> bytes;
    GateSummary summary;
};

[[nodiscard]] ICLFORGE_AC3_EXPORT std::expected<GatedStream, io::StripError> gate_atmos_stream(
    std::span<const std::byte> stream, std::span<const base::crypto::SigningKey> keys);

}  // namespace iclforge::ac3::signing
