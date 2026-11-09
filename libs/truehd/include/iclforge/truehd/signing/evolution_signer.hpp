#pragma once

// TrueHD evolution-frame object signing: a keyed, per-frame authenticity tag in evo_protection().
//
// This is ICL Forge's OWN bring-your-own-key scheme. It fills the evo_protection() field of the
// evolution frame (TH spec v3.6 §15.4, the container ETSI TS 102 366 Annex H renames) with a tag
// made from the operator's own key, and gives it the capability set iclforge::ac3::signing gives
// E-AC-3 Atmos: sign, verify against one key or a keyring, a key-free "is a tag present" check, and
// a gate that falls back to the bed. The tag is
//
//     HMAC-SHA-256(key, M)[0 .. Np/8)
//
// (HMAC: RFC 2104; SHA-256: FIPS 180-4), where Np is the width of the primary protection field and
// M is the whole ACCESS UNIT with four regions zeroed and nothing else changed: the primary and
// secondary protection fields; the zero fill inside the evolution payload after the frame; the
// extra data's padding between the payload and its parity byte; and the parity byte. Everything
// else is covered: the unit's header word (its check nibble and length) and input_timing, the
// substream directory, the major sync with its CRC, every substream segment, and the evolution
// payload's version, key id, payload ids, configurations and bytes. An edit to the audio, the major
// sync or the directory therefore fails the tag, as does an edit to the object metadata. The three
// zeroed regions outside the protection field are the ones a decoder reads no meaning from (the
// sole- container rule below keeps them empty), and the parity byte is repaired when a tag is
// written. There is no state carried from one unit to the next, so a stream cut or joined at unit
// boundaries keeps every tag valid: this is a per-frame tag and not a stream signature.
//
// The coverage matches what is observed of the tag in streams from the Dolby tools (the
// sensitivities to the object metadata, the major sync, the unit header and directory, and the
// insensitivity to the padding, fill and parity). The construction is nothing more than that: the
// algorithm and key of those tags are not public and are not derived or searched for here, and this
// library cannot make a tag a licensed decoder accepts. A licensed Dolby decoder holds other keys.
// What it does with a frame whose tag does not check depends on the stream (the Atmos flag in the
// major sync, when set, makes it enforce the tag): two failing frames in a row end the output about
// one access unit early per report, one failing frame is tolerated, a valid frame between failures
// resets the count, and a stream with few or no evolution frames is not affected.
// gate_evolution_stream, which removes the object layer of every unit whose tag does not verify,
// leaves a stream with no failing frame by construction; an unsigned stream, a stream signed under
// a key a given decoder does not hold, and the choice between the two are the operator's. TrueHD
// spec §16.3.3 still holds: no TrueHD stream carries a content signature, and this is not one.
// Nothing here claims interoperability. The library holds no key: the operator supplies one at run
// time (iclforge::base::crypto::SigningKey). See docs/concepts/object-signing.md.
//
// Strength. The field is as wide as its length code says, and every stream observed has used one
// byte (code 1). A one-byte tag is accepted by a wrong key about once in 256 units. It detects
// tampering and mix-ups; it does not authenticate. The signer does not choose a width: it writes
// the tag into the field the unit has, and a wider field is a different stream variant for the
// writer of the stream to choose.
//
// The tag's construction is the same under either container reading (EvolutionReading kEmdf or
// kSpec36, libs/truehd/ERRATA.md question 23); each function takes the reading as its last
// argument, kEmdf by default, as parse_evolution_frame does.
//
// The "unit" functions take one access unit (its first four bytes are the header word); a unit that
// carries no major sync needs `inherited_substreams`, the substream count of the major sync before
// it (parse_access_unit's argument). The "stream" functions take a raw .thd stream and walk its
// units with ElementaryReader; they change bytes in place and never a length.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include "iclforge/base/crypto/signing_key.hpp"
#include "iclforge/truehd/export.hpp"
#include "iclforge/truehd/io/evolution.hpp"

namespace iclforge::truehd::signing {

// Signs the evolution frame of `unit` with `key`, in place: the tag goes into
// protection_bits_primary and the extra data's parity byte (TH spec §16.4) is recomputed. False,
// with nothing changed, for an empty key, a unit that does not read or has no readable evolution
// frame, a primary length code of 0 (nowhere to put a tag), or a frame that is not the sole
// container (see verify_evolution_unit): a frame is never signed where it would not then verify.
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT bool sign_evolution_unit(
    std::span<std::byte> unit, const base::crypto::SigningKey& key, int inherited_substreams = 0,
    EvolutionReading reading = EvolutionReading::kEmdf);

// Signs every access unit of `stream` that carries an evolution frame, in place. Returns the number
// signed. A unit that cannot be signed is left as it was. Sign after the last edit of the stream:
// an edit to anything the tag covers (the audio, the major sync, the directory) fails it.
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

// The units of `stream` whose evolution frame carries a non-zero tag (has_evolution_tag), without a
// key.
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT int count_evolution_tags(
    std::span<const std::byte> stream, EvolutionReading reading = EvolutionReading::kEmdf);

// A unit with no evolution frame to read is neither verified nor failed: nothing in it to check.
enum class VerifyResult : std::uint8_t {
    kNoContainer,  // no readable evolution frame
    kValid,        // a frame, the sole container, and its tag matches the key
    kMismatch,     // a frame is there and the tag does not match, a primary length code of 0, or
                   // the frame is not the sole container (bytes beside what the tag covers)
};

struct VerifySummary {
    int valid = 0;
    int mismatch = 0;
    int no_container = 0;
    friend bool operator==(const VerifySummary&, const VerifySummary&) = default;
};

// Checks the unit's evolution frame against `key` without modifying anything. kValid needs both a
// matching tag and the sole-container rule: exactly the evolution payload is the object layer,
// nothing follows the container inside it but zero fill, and the extra data's padding is zero. The
// fill and the padding are not in the tag's message, so the rule is what keeps data out of them; a
// frame with more than that was not written by this signer and is a mismatch.
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT VerifyResult verify_evolution_unit(
    std::span<const std::byte> unit, const base::crypto::SigningKey& key,
    int inherited_substreams = 0, EvolutionReading reading = EvolutionReading::kEmdf);

[[nodiscard]] ICLFORGE_TRUEHD_EXPORT VerifySummary
verify_evolution_stream(std::span<const std::byte> stream, const base::crypto::SigningKey& key,
                        EvolutionReading reading = EvolutionReading::kEmdf);

// --- Several keys: the keyring -------------------------------------------------------------------
//
// An ordered set of keys, tried in turn; a unit is valid when ANY reproduces its tag. It adds no
// authority. An empty key is skipped, and a set with no usable key accepts nothing: a unit with a
// frame is then a mismatch, never a pass.

struct KeyringVerdict {
    VerifyResult result = VerifyResult::kNoContainer;
    std::size_t key_index = 0;  // the first accepting key; meaningful only when result is kValid
};

[[nodiscard]] ICLFORGE_TRUEHD_EXPORT KeyringVerdict verify_evolution_unit_any(
    std::span<const std::byte> unit, std::span<const base::crypto::SigningKey> keys,
    int inherited_substreams = 0, EvolutionReading reading = EvolutionReading::kEmdf);

struct KeyringSummary {
    VerifySummary totals;
    std::vector<int> per_key;  // per_key[i]: units key i accepted; adds up to totals.valid
};

[[nodiscard]] ICLFORGE_TRUEHD_EXPORT KeyringSummary verify_evolution_stream_any(
    std::span<const std::byte> stream, std::span<const base::crypto::SigningKey> keys,
    EvolutionReading reading = EvolutionReading::kEmdf);

// --- The licensed policy: objects only where the tag is good
// ---------------------------------------
//
// A unit whose evolution frame verifies under some key is copied byte for byte (passed). Any other
// unit that has extra data (unsigned, another key's, altered, a container that does not read) has
// its object layer REMOVED and carries on as its bed (gated): the extra data is rewritten to the
// padding-only form (header word zero, the rest zero words; TH spec §16.2) and nothing else, so the
// substreams are untouched and the bed decodes bit-identically, no re-encode and no change of
// length. For a gated unit a decoder reads the best of substreams 0 to 2 (Presentation::kBest) and
// not substream 3, whose object channels have no metadata left to place them. A unit with no
// evolution frame is copied through (no_objects). What is left in a gated stream is the frames that
// verify, so it has no frame that fails its tag, and never two in a row. The gate is a pure
// function of (stream, keys); the decoder never learns signing exists.
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
