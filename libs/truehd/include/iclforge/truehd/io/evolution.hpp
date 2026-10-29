#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "iclforge/truehd/export.hpp"
#include "iclforge/truehd/io/access_unit.hpp"

// The evolution frame of a unit's extra data (TH spec v3.6 §15.4, §16.1), read into plain
// structures: the payload ids, their configuration, their bytes and the protection field.
// PROVISIONAL, OWNER TO CONFIRM (libs/truehd/ERRATA.md, "Atmos, spec v3.6", question 20): no stream
// this project has carries an evolution frame. v3.6 describes the same bytes twice, as the bit-level
// container read here (§15.4) and as a Thrift container with a signature (§16.3, whose layout
// §16.3.3 says is not confirmed). The owner's reading (2026-10-29) is that real streams carry both,
// nested: §15.4's container is the outer one and the Thrift ContentSignature is inside one of its
// payloads; which payload, where, and what it covers are unconfirmed. This reads the outer container
// and carries every payload as bytes with its id and size; it does not look inside a payload that
// is not the object audio metadata, and it has no Thrift reader (`forge probe detail=evolution`
// walks one with iclforge::base). The reading is written and tested from §15.4 and from the EMDF
// container of ETSI TS 102 366 Annex H that §15.4 renames, which iclforge::objects reads for
// E-AC-3; nothing here decodes from it. The raw bytes stay available unchanged as
// AccessUnit::extra_data and DecodedAccessUnit::extra_data.
//
// What is read: the extra data region's framing (§16.1: the check nibble, extra_data_length,
// evo_reserved, evo_frame_byte_length, the payload, the padding and the parity byte, §16.2 and
// §16.4 for the two checks) through ExtraData, and then the evolution payload as evo_frame():
// version, key id, the list of payloads each with its configuration and bytes, and
// evo_protection(). The check nibble, the parity and evo_reserved are reported (EvolutionRead), not
// enforced: a unit whose extra data fails them still decodes. Nothing is verified: the protection
// bytes are returned as they are and no signature (§16.3, §16.6) is looked at.
//
// The payload with id 11 is the object audio metadata (TS 103 420 Table 55). It is handed out as
// bytes (EvolutionFrame::oamd). This library links iclforge::base and nothing else, so the bytes
// are parsed by a program that links iclforge::objects too: iclforge::objects::oba::parse_payload
// takes a bare payload, and that is how `forge probe` reads the object count and the bed
// assignment.
//
// Two readings, because §15.4's text and the standard it renames differ (ERRATA questions 21 and
// 22):
//
//   kEmdf   the container of ETSI TS 102 366 Annex H as iclforge::objects::emdf::parse_container
//           reads it: variable_bits are groups of n bits each followed by a read-more flag
//           (TS 103 420 §5.5.1), payload id 31 escapes to a longer id, smploffst is 11 bits and a
//           reserved bit, and a primary protection length code of 0 is reserved. The default.
//   kSpec36 §15.4 to the letter: variable_bits(n, max_ext) is an escape value (n bits, and when all
//   are
//           ones n more, up to max_ext times), no payload id escape, smploffst is a
//           variable_bits(11, 2) with no reserved bit, a primary protection length code of 0 means
//           no bytes.
//
// Neither is known to be what a TrueHD stream carries. `forge probe` says which of the two reads
// each frame it finds, which is how a real stream settles it.

namespace iclforge::truehd {

// TS 103 420 Table 55; TH spec v3.6 §15.4.
inline constexpr std::uint32_t kEvolutionPayloadOamd = 11;

enum class EvolutionReading : std::uint8_t { kEmdf, kSpec36 };

// evo_payload_config(). A field the syntax did not send is empty / false / 0.
struct EvolutionPayloadConfig {
    std::optional<std::uint64_t> sample_offset;  // smploffst, in samples
    std::optional<std::uint64_t> duration;
    std::optional<std::uint64_t> group_id;
    std::optional<std::uint8_t> codec_data;  // the 8 bits after the flag (reserved in Annex H)
    bool discard_unknown = false;
    bool frame_aligned = false;
    bool create_duplicate = false;
    bool remove_duplicate = false;
    std::uint8_t priority = 0;      // 5 bits
    std::uint8_t proc_allowed = 0;  // 2 bits
    friend bool operator==(const EvolutionPayloadConfig&, const EvolutionPayloadConfig&) = default;
};

struct EvolutionPayload {
    std::uint64_t id = 0;  // after the escape, when there is one
    EvolutionPayloadConfig config;
    std::vector<std::byte> bytes;  // copied: a payload is not byte-aligned in the frame
    friend bool operator==(const EvolutionPayload&, const EvolutionPayload&) = default;
};

// evo_protection(): the two length codes (2 bits each) and the bytes they size (0, 1, 4 or 16).
struct EvolutionProtection {
    std::uint8_t primary_code = 0;
    std::uint8_t secondary_code = 0;
    std::vector<std::byte> primary;
    std::vector<std::byte> secondary;
    friend bool operator==(const EvolutionProtection&, const EvolutionProtection&) = default;
};

struct EvolutionFrame {
    std::uint64_t version = 0;  // evo_version after its extension
    std::uint64_t key_id = 0;   // key_id after its extension
    std::vector<EvolutionPayload> payloads;
    EvolutionProtection protection;
    std::size_t bits_used =
        0;  // the frame's length in bits, from the first bit to the last of the protection
    bool trailing_zero = true;  // every bit after that, to the end of the payload, is 0

    // The first payload with this id, or null.
    [[nodiscard]] const EvolutionPayload* find(std::uint64_t id) const noexcept;
    // The bytes of the first payload with id 11 (the object audio metadata), empty when there is
    // none.
    [[nodiscard]] std::span<const std::byte> oamd() const noexcept;
};

// Every way the container can fail to read, each its own kind.
enum class EvolutionErrorKind : std::uint8_t {
    kNoEvolutionPayload,  // the extra data's framing did not hold an evolution payload
    kEmpty,               // the payload has no bytes
    kTruncatedHeader,     // inside evo_version or key_id
    kMissingTerminator,   // fewer than 5 bits where the next payload id, or the terminating 0,
                          // belongs
    kTruncatedPayloadId,  // inside an escaped payload id
    kTruncatedConfig,     // inside evo_payload_config()
    kTruncatedSize,       // inside evo_payload_size
    kTruncatedPayload,    // evo_payload_size says more bytes than the frame has
    kReservedProtectionLength,  // kEmdf: the primary protection length code is 0
    kTruncatedProtection,       // inside evo_protection()
};

struct EvolutionError {
    EvolutionErrorKind kind = EvolutionErrorKind::kEmpty;
    std::size_t bit_offset = 0;  // where the field that failed began
    friend bool operator==(const EvolutionError&, const EvolutionError&) = default;
};

[[nodiscard]] ICLFORGE_TRUEHD_EXPORT std::string_view to_string(EvolutionErrorKind kind) noexcept;
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT std::string_view to_string(EvolutionReading reading) noexcept;

// evo_frame() of the evolution payload (ExtraData::evolution_payload).
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT std::expected<EvolutionFrame, EvolutionError>
parse_evolution_frame(std::span<const std::byte> evolution_payload,
                      EvolutionReading reading = EvolutionReading::kEmdf);

// What one unit's extra data comes to: the checks of §16.1 to §16.4 and the container.
struct EvolutionRead {
    bool check_ok = false;       // header_check_nibble against extra_data_length (§16.2)
    bool framing_ok = false;     // the evolution payload and the parity byte fit
    bool parity_ok = false;      // §16.4
    bool reserved_zero = false;  // evo_reserved is 0 (§16.8)
    bool length_fits =
        false;  // §16.8's capacity rule, with extra_data_length as the words less one
    std::uint16_t extra_data_length = 0;
    std::uint16_t evo_frame_byte_length = 0;
    std::expected<EvolutionFrame, EvolutionError> container{
        std::unexpected(EvolutionError{EvolutionErrorKind::kNoEvolutionPayload, 0})};
};

[[nodiscard]] ICLFORGE_TRUEHD_EXPORT EvolutionRead
read_evolution(const ExtraData& extra, EvolutionReading reading = EvolutionReading::kEmdf);

}  // namespace iclforge::truehd
