#include "iclforge/truehd/signing/evolution_signer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "iclforge/base/crypto/hmac_sha256.hpp"
#include "iclforge/truehd/io/access_unit.hpp"
#include "iclforge/truehd/io/elementary.hpp"

// The tag construction is specified in the header; this file is the unit walk and the placement. No
// key is held, derived or searched for here: every key arrives as a SigningKey from the caller.

namespace iclforge::truehd::signing {

namespace {

using base::crypto::SigningKey;

constexpr std::size_t kFramingBytes = 4;   // header word, then evo_reserved + evo_frame_byte_length
constexpr unsigned kExtraDataSeed = 0xA9;  // TH spec §16.4
constexpr std::array<std::size_t, 4> kProtectionBytes{0, 1, 4, 16};  // per length code

// Where the tag is, in one access unit, and whether the unit has an evolution frame at all.
struct Located {
    enum class State : std::uint8_t {
        kNone,   // no extra data, padding-only, framing not ok, or a container that does not read
        kFrame,  // an evolution frame is there
    };
    State state = State::kNone;
    std::optional<AccessUnit> parsed;
    EvolutionFrame frame;
    // False for a primary length code of 0, which leaves nowhere for a tag.
    bool has_field = false;
    std::size_t extra_offset = 0;  // the extra data region, in bytes from the unit's first
    std::size_t prim_off = 0;      // bits from the first bit of the evolution payload
    std::size_t np_bits = 0;
    std::size_t ms_bits = 0;
    // The sole-container rule: nothing in the extra data is outside what the tag covers or is an
    // empty region the tag leaves out (the fill and the padding are zero).
    bool sole = false;

    // The first bit of the evolution payload, in bits from the unit's first.
    [[nodiscard]] std::size_t payload_bit0() const noexcept {
        return (extra_offset + kFramingBytes) * 8U;
    }
};

[[nodiscard]] bool all_zero(std::span<const std::byte> bytes) noexcept {
    return std::ranges::all_of(bytes, [](std::byte b) { return b == std::byte{0}; });
}

[[nodiscard]] Located locate(std::span<const std::byte> unit, int inherited_substreams,
                             EvolutionReading reading) {
    Located out;
    auto unit_read = parse_access_unit(unit, inherited_substreams, true);
    if (unit_read) {
        out.parsed = std::move(*unit_read);
    }
    if (!out.parsed || !out.parsed->extra_data || out.parsed->extra_data->padding_only ||
        !out.parsed->extra_data->framing_ok) {
        return out;
    }
    const ExtraData& extra = *out.parsed->extra_data;
    out.extra_offset = static_cast<std::size_t>(extra.bytes.data() - unit.data());
    auto parsed = parse_evolution_frame(extra.evolution_payload, reading);
    if (!parsed) {
        // Under kEmdf a primary length code of 0 is reserved and the parse refuses it. A frame
        // that is there but has no field to hold a tag is a frame, not an absence.
        if (parsed.error().kind == EvolutionErrorKind::kReservedProtectionLength) {
            out.state = Located::State::kFrame;
        }
        return out;
    }
    out.state = Located::State::kFrame;
    out.frame = std::move(*parsed);
    const std::uint8_t primary_code = out.frame.protection.primary_code;
    if (primary_code == 0) {
        return out;
    }
    out.has_field = true;
    out.np_bits = kProtectionBytes[primary_code] * 8U;
    out.ms_bits = kProtectionBytes[out.frame.protection.secondary_code] * 8U;
    out.prim_off = out.frame.bits_used - out.ms_bits - out.np_bits;

    // The padding between the payload and the parity byte.
    const std::size_t payload_end = kFramingBytes + extra.evo_frame_byte_length;
    const bool padding_zero =
        all_zero(extra.bytes.subspan(payload_end, extra.bytes.size() - 1U - payload_end));
    out.sole = out.frame.trailing_zero && padding_zero;
    return out;
}

void clear_bits(std::span<std::byte> bytes, std::size_t first_bit, std::size_t count) noexcept {
    for (std::size_t bit = first_bit; bit < first_bit + count; ++bit) {
        bytes[bit >> 3U] &= static_cast<std::byte>(~(0x80U >> (bit & 7U)) & 0xFFU);
    }
}

// The authenticated message: the access unit with the protection fields, the fill inside the
// evolution payload, the extra data's padding and its parity byte zeroed.
[[nodiscard]] std::vector<std::byte> message_of(std::span<const std::byte> unit,
                                                const Located& at) {
    std::vector<std::byte> message(unit.begin(), unit.end());
    const std::size_t base = at.payload_bit0();
    clear_bits(message, base + at.prim_off, at.np_bits + at.ms_bits);
    const std::size_t payload_bytes = at.parsed->extra_data->evo_frame_byte_length;
    clear_bits(message, base + at.frame.bits_used, payload_bytes * 8U - at.frame.bits_used);
    const std::size_t padding_start = at.extra_offset + kFramingBytes + payload_bytes;
    std::fill(message.begin() + static_cast<std::ptrdiff_t>(padding_start), message.end(),
              std::byte{0});  // the padding, then the parity byte that ends the unit
    return message;
}

[[nodiscard]] std::vector<std::byte> tag_of(std::span<const std::byte> message,
                                            const SigningKey& key, std::size_t np_bits) {
    const auto digest = base::crypto::hmac_sha256(key.bytes(), message);
    return {digest.begin(), digest.begin() + static_cast<std::ptrdiff_t>(np_bits / 8U)};
}

// No early exit on the first differing byte: the tag is a secret-derived value.
[[nodiscard]] bool equal_constant_time(std::span<const std::byte> a, std::span<const std::byte> b) {
    if (a.size() != b.size()) {
        return false;
    }
    unsigned difference = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        difference |= std::to_integer<unsigned>(a[i] ^ b[i]);
    }
    return difference == 0;
}

// TH spec §16.4: the 16-bit word of evo_reserved (4 bits) and the frame length (12), the seed, then
// every byte between the framing and the parity byte; the last byte of `region` is the parity byte.
[[nodiscard]] std::uint8_t parity_byte(const ExtraData& extra,
                                       std::span<const std::byte> region) noexcept {
    unsigned accumulator =
        (((unsigned{extra.evo_reserved} & 0xFU) << 12U) | (extra.evo_frame_byte_length & 0xFFFU)) ^
        kExtraDataSeed;
    for (std::size_t i = kFramingBytes; i + 1 < region.size(); ++i) {
        accumulator ^= std::to_integer<unsigned>(region[i]);
    }
    return static_cast<std::uint8_t>(((accumulator >> 8U) ^ accumulator) & 0xFFU);
}

// The verdict for a located unit, against the keys in order (empty keys skipped).
[[nodiscard]] KeyringVerdict verdict(std::span<const std::byte> unit, const Located& at,
                                     std::span<const SigningKey> keys) {
    if (at.state == Located::State::kNone) {
        return {VerifyResult::kNoContainer, 0};
    }
    if (!at.has_field || !at.sole) {
        return {VerifyResult::kMismatch, 0};
    }
    std::vector<std::byte> message;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (keys[i].empty()) {
            continue;
        }
        if (message.empty()) {
            message = message_of(unit, at);
        }
        if (equal_constant_time(tag_of(message, keys[i], at.np_bits),
                                at.frame.protection.primary)) {
            return {VerifyResult::kValid, i};
        }
    }
    return {VerifyResult::kMismatch, 0};
}

// A stream's access units, in order, as offsets into `stream`, with the substream count each one
// inherits. `skipped` is the bytes the reader passed over beyond the ones ahead of the first unit.
struct UnitRef {
    std::size_t offset = 0;
    std::size_t size = 0;
    int inherited = 0;
};

struct Walk {
    std::vector<UnitRef> units;
    std::size_t skipped = 0;
};

[[nodiscard]] Walk walk(std::span<const std::byte> stream) {
    Walk out;
    ElementaryReader reader(stream);
    int substreams = 0;
    while (const auto unit = reader.next()) {
        const auto parsed = parse_access_unit(*unit, substreams, true);
        if (!parsed) {
            continue;
        }
        out.units.push_back(
            {static_cast<std::size_t>(unit->data() - stream.data()), unit->size(), substreams});
        if (parsed->major_sync) {
            substreams = parsed->major_sync->num_substreams;
        }
    }
    out.skipped = reader.skipped_bytes() - reader.leading_bytes();
    return out;
}

void count(VerifySummary& summary, VerifyResult result) noexcept {
    switch (result) {
        case VerifyResult::kValid:
            ++summary.valid;
            break;
        case VerifyResult::kMismatch:
            ++summary.mismatch;
            break;
        case VerifyResult::kNoContainer:
            ++summary.no_container;
            break;
    }
}

}  // namespace

bool sign_evolution_unit(std::span<std::byte> unit, const SigningKey& key, int inherited_substreams,
                         EvolutionReading reading) {
    if (key.empty()) {
        return false;
    }
    const Located at = locate(unit, inherited_substreams, reading);
    if (at.state != Located::State::kFrame || !at.has_field || !at.sole) {
        return false;
    }
    const std::vector<std::byte> tag = tag_of(message_of(unit, at), key, at.np_bits);
    // The tag goes in as np_bits bits, MSB first, from prim_off bits into the evolution payload.
    const std::size_t base_bit = at.payload_bit0() + at.prim_off;
    for (std::size_t i = 0; i < at.np_bits; ++i) {
        const std::size_t bit = base_bit + i;
        const bool one = (std::to_integer<unsigned>(tag[i >> 3U]) >> (7U - (i & 7U))) & 1U;
        const auto mask = static_cast<std::byte>(0x80U >> (bit & 7U));
        unit[bit >> 3U] = one ? (unit[bit >> 3U] | mask) : (unit[bit >> 3U] & ~mask);
    }
    const std::span<const std::byte> region =
        std::span<const std::byte>(unit).subspan(at.extra_offset);
    unit.back() = static_cast<std::byte>(parity_byte(*at.parsed->extra_data, region));
    return true;
}

int sign_evolution_stream(std::span<std::byte> stream, const SigningKey& key,
                          EvolutionReading reading) {
    int signed_units = 0;
    for (const UnitRef& unit : walk(stream).units) {
        if (sign_evolution_unit(stream.subspan(unit.offset, unit.size), key, unit.inherited,
                                reading)) {
            ++signed_units;
        }
    }
    return signed_units;
}

bool has_evolution_tag(std::span<const std::byte> extra_data, EvolutionReading reading) {
    const auto extra = read_extra_data(extra_data);
    if (!extra || extra->padding_only || !extra->framing_ok) {
        return false;
    }
    const auto frame = parse_evolution_frame(extra->evolution_payload, reading);
    if (!frame) {
        return false;
    }
    return frame->protection.primary_code != 0 && !all_zero(frame->protection.primary);
}

int count_evolution_tags(std::span<const std::byte> stream, EvolutionReading reading) {
    int tagged = 0;
    for (const UnitRef& unit : walk(stream).units) {
        const auto parsed =
            parse_access_unit(stream.subspan(unit.offset, unit.size), unit.inherited, true);
        if (parsed && parsed->extra_data && has_evolution_tag(parsed->extra_data->bytes, reading)) {
            ++tagged;
        }
    }
    return tagged;
}

VerifyResult verify_evolution_unit(std::span<const std::byte> unit, const SigningKey& key,
                                   int inherited_substreams, EvolutionReading reading) {
    const std::array<SigningKey, 1> keys{key};
    return verdict(unit, locate(unit, inherited_substreams, reading), keys).result;
}

VerifySummary verify_evolution_stream(std::span<const std::byte> stream, const SigningKey& key,
                                      EvolutionReading reading) {
    return verify_evolution_stream_any(stream, std::span<const SigningKey>(&key, 1), reading)
        .totals;
}

KeyringVerdict verify_evolution_unit_any(std::span<const std::byte> unit,
                                         std::span<const SigningKey> keys, int inherited_substreams,
                                         EvolutionReading reading) {
    return verdict(unit, locate(unit, inherited_substreams, reading), keys);
}

KeyringSummary verify_evolution_stream_any(std::span<const std::byte> stream,
                                           std::span<const SigningKey> keys,
                                           EvolutionReading reading) {
    KeyringSummary out;
    out.per_key.assign(keys.size(), 0);
    for (const UnitRef& unit : walk(stream).units) {
        const KeyringVerdict one = verify_evolution_unit_any(stream.subspan(unit.offset, unit.size),
                                                             keys, unit.inherited, reading);
        count(out.totals, one.result);  // a unit with no frame to check counts as none
        if (one.result == VerifyResult::kValid) {
            ++out.per_key[one.key_index];
        }
    }
    return out;
}

std::expected<GatedStream, GateError> gate_evolution_stream(std::span<const std::byte> stream,
                                                            std::span<const SigningKey> keys,
                                                            EvolutionReading reading) {
    const Walk found = walk(stream);
    if (found.units.empty()) {
        return std::unexpected(GateError{GateErrorKind::kNoStream, 0});
    }
    if (found.skipped != 0) {
        return std::unexpected(GateError{GateErrorKind::kUnreadableUnit, found.skipped});
    }
    GatedStream out;
    out.bytes.assign(stream.begin(), stream.end());
    out.summary.per_key.assign(keys.size(), 0);
    for (const UnitRef& unit : found.units) {
        const std::span<const std::byte> bytes = stream.subspan(unit.offset, unit.size);
        const auto parsed = parse_access_unit(bytes, unit.inherited, true);
        if (!parsed || !parsed->extra_data || parsed->extra_data->padding_only) {
            ++out.summary.no_objects;
            continue;
        }
        const KeyringVerdict one = verify_evolution_unit_any(bytes, keys, unit.inherited, reading);
        if (one.result == VerifyResult::kValid) {
            ++out.summary.passed;
            ++out.summary.per_key[one.key_index];
            continue;
        }
        // Anything else with extra data in it loses its object layer: the padding-only form is a
        // header word of zero and zero words after it (TH spec §16.2).
        const std::size_t offset =
            unit.offset + static_cast<std::size_t>(parsed->extra_data->bytes.data() - bytes.data());
        std::fill_n(out.bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                    parsed->extra_data->bytes.size(), std::byte{0});
        ++out.summary.gated;
    }
    return out;
}

}  // namespace iclforge::truehd::signing
