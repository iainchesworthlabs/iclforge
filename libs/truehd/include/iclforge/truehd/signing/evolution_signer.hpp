#pragma once

// TrueHD evolution-frame object signing: a keyed, per-frame authenticity tag in evo_protection().
//
// This is ICL Forge's OWN bring-your-own-key scheme. It reuses the evo_protection() field of the
// evolution frame (TH spec v3.6 §15.4, the container ETSI TS 102 366 Annex H renames) and the
// machinery iclforge::ac3::signing gives E-AC-3 Atmos: sign, verify against one key or a keyring,
// a key-free "is a tag present" check, and a gate that falls back to the bed. The tag is
//
//     HMAC-SHA-256(key, evolution payload with both protection fields zeroed)[0 .. Np/8)
//
// (HMAC: RFC 2104; SHA-256: FIPS 180-4), where Np is the width of the primary protection field.
// The construction is this library's own. It is not observed in any Dolby stream, it is not a
// recovery of any vendor's tag, and it carries no key: the operator supplies one at run time
// (iclforge::base::crypto::SigningKey) and the library never embeds, derives or searches for one.
// TrueHD spec §16.3.3 still holds: no TrueHD stream examined carries a content signature, and
// nothing here claims interoperability. A licensed Dolby decoder holds other keys and will not
// accept these tags as Dolby-authentic; a kValid result says only that the tag matches the key the
// caller supplied. See docs/concepts/object-signing.md.
//
// Where the tag lives follows the container's own protection length codes, so whether a tag is
// PRESENT is answerable without a key (has_evolution_tag); only whether it is the right one needs
// the key. The tag's construction is the same under either container reading (EvolutionReading
// kEmdf or kSpec36, libs/truehd/ERRATA.md question 23), and each function takes the reading as its
// last argument, kEmdf by default, as parse_evolution_frame does.
//
// The "frame" functions take one unit's extra data region (ExtraData::bytes, from the header word
// to the end of the unit). The "stream" functions take a raw .thd stream and walk its access units
// with ElementaryReader; they change bytes in place and never a length.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include "iclforge/base/crypto/signing_key.hpp"
#include "iclforge/truehd/export.hpp"
#include "iclforge/truehd/io/evolution.hpp"

namespace iclforge::truehd::signing {

// Signs the evolution frame in `extra_data` with `key`, in place: the tag goes into
// protection_bits_primary and the extra data's parity byte (TH spec §16.4) is recomputed. False,
// with nothing changed, for an empty key, extra data with no readable evolution frame, a primary
// length code of 0 (nowhere to put a tag) or a frame that is not the sole container (see
// verify_evolution_frame): a frame is never signed where it would not then verify.
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT bool sign_evolution_frame(
    std::span<std::byte> extra_data, const base::crypto::SigningKey& key,
    EvolutionReading reading = EvolutionReading::kEmdf);

// Signs every access unit of `stream` that carries an evolution frame, in place. Returns the number
// signed. A unit that cannot be signed is left as it was.
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT int sign_evolution_stream(
    std::span<std::byte> stream, const base::crypto::SigningKey& key,
    EvolutionReading reading = EvolutionReading::kEmdf);

// Whether this extra data carries a non-zero primary tag, answered WITHOUT a key. False for the
// padding-only form, extra data with no readable evolution frame, a primary length code of 0 and a
// field of all zeros (what an unsigned frame of this project's own carries). A true says a tag is
// present, never that it is valid; a tag from another producer reads as present here and
// kMismatch under verify, which is the correct reading: present, not authenticated.
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT bool has_evolution_tag(
    std::span<const std::byte> extra_data, EvolutionReading reading = EvolutionReading::kEmdf);

// A unit with no evolution frame to read is neither verified nor failed: nothing in it to check.
enum class VerifyResult : std::uint8_t {
    kNoContainer,  // no readable evolution frame
    kValid,        // a frame, the sole container, and its tag matches the key
    kMismatch,     // a frame is there and the tag does not match, a primary length code of 0, or
                   // the frame is not the sole container (bytes beside the tag's coverage)
};

struct VerifySummary {
    int valid = 0;
    int mismatch = 0;
    int no_container = 0;
    friend bool operator==(const VerifySummary&, const VerifySummary&) = default;
};

// Checks the evolution frame against `key` without modifying anything. kValid needs both a matching
// tag and the sole-container rule: exactly the evolution payload is the object layer, nothing
// follows the container inside it but zero fill, and the extra data's padding is zero. A frame with
// more than that was not written by this signer and is not covered by its tag.
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT VerifyResult
verify_evolution_frame(std::span<const std::byte> extra_data, const base::crypto::SigningKey& key,
                       EvolutionReading reading = EvolutionReading::kEmdf);

[[nodiscard]] ICLFORGE_TRUEHD_EXPORT VerifySummary
verify_evolution_stream(std::span<const std::byte> stream, const base::crypto::SigningKey& key,
                        EvolutionReading reading = EvolutionReading::kEmdf);

// --- Several keys: the keyring -------------------------------------------------------------------
//
// An ordered set of keys, tried in turn; a frame is valid when ANY reproduces its tag. It adds no
// authority. An empty key is skipped, and a set with no usable key accepts nothing: a frame with a
// container is then a mismatch, never a pass.

struct KeyringVerdict {
    VerifyResult result = VerifyResult::kNoContainer;
    std::size_t key_index = 0;  // the first accepting key; meaningful only when result is kValid
};

[[nodiscard]] ICLFORGE_TRUEHD_EXPORT KeyringVerdict verify_evolution_frame_any(
    std::span<const std::byte> extra_data, std::span<const base::crypto::SigningKey> keys,
    EvolutionReading reading = EvolutionReading::kEmdf);

struct KeyringSummary {
    VerifySummary totals;
    std::vector<int> per_key;  // per_key[i]: frames key i accepted; adds up to totals.valid
};

[[nodiscard]] ICLFORGE_TRUEHD_EXPORT KeyringSummary verify_evolution_stream_any(
    std::span<const std::byte> stream, std::span<const base::crypto::SigningKey> keys,
    EvolutionReading reading = EvolutionReading::kEmdf);

// --- The licensed policy: objects only where the tag is good
// ---------------------------------------
//
// A unit whose evolution frame verifies under some key is copied byte for byte (passed). Any other
// unit that has an evolution frame (unsigned, another key's, altered after signing, a container
// that does not read) has its object layer REMOVED and carries on as its bed (gated): the extra
// data is rewritten to the padding-only form (header word zero, the rest zero words; TH spec
// §16.2) and nothing else, so the substreams are untouched and the bed decodes bit-identically, no
// re-encode and no change of length. For a gated unit a decoder reads the best of substreams 0 to 2
// (Presentation::kBest) and not substream 3, whose object channels have no metadata left to place
// them. A unit with no evolution frame is copied through (no_objects). The gate is a pure function
// of (stream, keys); the decoder never learns signing exists.
//
// FAIL CLOSED. A stream with bytes that are not access units (a unit that does not read) is an
// error: that unit might carry an object layer this build can neither verify nor remove. A stream
// with no object layer at all is not an error and returns byte-identical.

struct GateSummary {
    int passed = 0;
    int gated = 0;
    int no_objects = 0;
    std::vector<int> per_key;  // as KeyringSummary::per_key, over the passed units
};

struct GatedStream {
    std::vector<std::byte> bytes;
    GateSummary summary;
};

enum class GateErrorKind : std::uint8_t {
    kNoStream,        // no access unit found
    kUnreadableUnit,  // bytes between or after the units that do not read as an access unit
};

struct GateError {
    GateErrorKind kind = GateErrorKind::kNoStream;
    std::size_t skipped_bytes = 0;  // for kUnreadableUnit: how many bytes were passed over
    friend bool operator==(const GateError&, const GateError&) = default;
};

[[nodiscard]] ICLFORGE_TRUEHD_EXPORT std::expected<GatedStream, GateError> gate_evolution_stream(
    std::span<const std::byte> stream, std::span<const base::crypto::SigningKey> keys,
    EvolutionReading reading = EvolutionReading::kEmdf);

}  // namespace iclforge::truehd::signing
