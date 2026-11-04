#include "iclforge/truehd/signing/evolution_signer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

#include "iclforge/base/crypto/hmac_sha256.hpp"
#include "iclforge/truehd/io/access_unit.hpp"
#include "iclforge/truehd/io/elementary.hpp"

// The tag construction is specified in the header; this file is the frame walk and the placement.
// No key is held, derived or searched for here: every key arrives as a SigningKey from the caller.

namespace iclforge::truehd::signing {

namespace {

using base::crypto::SigningKey;

constexpr std::size_t kFramingBytes = 4;   // header word, then evo_reserved + evo_frame_byte_length
constexpr unsigned kExtraDataSeed = 0xA9;  // TH spec §16.4
constexpr std::array<std::size_t, 4> kProtectionBytes{0, 1, 4, 16};  // per length code

// Where the tag is, in one unit's extra data, and whether the unit has an evolution frame at all.
struct Located {
    enum class State : std::uint8_t {
        kNone,   // padding-only, framing not ok, or a container that does not read
        kFrame,  // an evolution frame is there
    };
    State state = State::kNone;
    std::optional<ExtraData> extra;
    EvolutionFrame frame;
    // False for a primary length code of 0, which leaves nowhere for a tag.
    bool has_field = false;
    std::size_t prim_off = 0;  // bits from the first bit of the evolution payload
    std::size_t np_bits = 0;
    std::size_t ms_bits = 0;
    // The sole-container rule: nothing in the extra data is outside what the tag covers.
    bool sole = false;
};

[[nodiscard]] bool all_zero(std::span<const std::byte> bytes) noexcept {
    return std::ranges::all_of(bytes, [](std::byte b) { return b == std::byte{0}; });
}

[[nodiscard]] Located locate(std::span<const std::byte> extra_data, EvolutionReading reading) {
    Located out;
    out.extra = read_extra_data(extra_data);
    if (!out.extra || out.extra->padding_only || !out.extra->framing_ok) {
        return out;
    }
    auto parsed = parse_evolution_frame(out.extra->evolution_payload, reading);
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
    const std::span<const std::byte> bytes = out.extra->bytes;
    const std::size_t payload_end = kFramingBytes + out.extra->evo_frame_byte_length;
    const bool padding_zero = all_zero(bytes.subspan(payload_end, bytes.size() - 1U - payload_end));
    out.sole = out.frame.trailing_zero && padding_zero;
    return out;
}

// The authenticated message: the evolution payload with both protection fields cleared.
[[nodiscard]] std::vector<std::byte> message_of(const Located& at) {
    const std::span<const std::byte> payload = at.extra->evolution_payload;
    std::vector<std::byte> message(payload.begin(), payload.end());
    for (std::size_t bit = at.prim_off; bit < at.prim_off + at.np_bits + at.ms_bits; ++bit) {
        message[bit >> 3U] &= static_cast<std::byte>(~(0x80U >> (bit & 7U)) & 0xFFU);
    }
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

[[nodiscard]] std::uint8_t parity_byte(const ExtraData& extra,
                                       std::span<const std::byte> bytes) noexcept {
    // TH spec §16.4: the 16-bit word of evo_reserved (4 bits) and the frame length (12), the seed,
    // then every byte between the framing and the parity byte.
    unsigned accumulator =
        (((unsigned{extra.evo_reserved} & 0xFU) << 12U) | (extra.evo_frame_byte_length & 0xFFFU)) ^
        kExtraDataSeed;
    for (std::size_t i = kFramingBytes; i + 1 < bytes.size(); ++i) {
        accumulator ^= std::to_integer<unsigned>(bytes[i]);
    }
    return static_cast<std::uint8_t>(((accumulator >> 8U) ^ accumulator) & 0xFFU);
}

// The verdict for a located frame, against the keys in order (empty keys skipped).
[[nodiscard]] KeyringVerdict verdict(const Located& at, std::span<const SigningKey> keys) {
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
            message = message_of(at);
        }
        if (equal_constant_time(tag_of(message, keys[i], at.np_bits),
                                at.frame.protection.primary)) {
            return {VerifyResult::kValid, i};
        }
    }
    return {VerifyResult::kMismatch, 0};
}

// A stream's extra data regions, in order, as offsets into `stream`. `skipped` is the bytes the
// reader passed over beyond the ones ahead of the first unit.
struct Walk {
    std::vector<std::pair<std::size_t, std::size_t>> regions;  // offset, length; one per unit
                                                               // that has extra data
    std::size_t units = 0;
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
        ++out.units;
        if (parsed->major_sync) {
            substreams = parsed->major_sync->num_substreams;
        }
        if (parsed->extra_data) {
            const auto offset =
                static_cast<std::size_t>(parsed->extra_data->bytes.data() - stream.data());
            out.regions.emplace_back(offset, parsed->extra_data->bytes.size());
        } else {
            out.regions.emplace_back(0, 0);
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

bool sign_evolution_frame(std::span<std::byte> extra_data, const SigningKey& key,
                          EvolutionReading reading) {
    if (key.empty()) {
        return false;
    }
    const Located at = locate(extra_data, reading);
    if (at.state != Located::State::kFrame || !at.has_field || !at.sole) {
        return false;
    }
    const std::vector<std::byte> tag = tag_of(message_of(at), key, at.np_bits);
    // The tag goes in as np_bits bits, MSB first, from prim_off bits into the evolution payload.
    const std::size_t base_bit = kFramingBytes * 8U + at.prim_off;
    for (std::size_t i = 0; i < at.np_bits; ++i) {
        const std::size_t bit = base_bit + i;
        const bool one = (std::to_integer<unsigned>(tag[i >> 3U]) >> (7U - (i & 7U))) & 1U;
        const auto mask = static_cast<std::byte>(0x80U >> (bit & 7U));
        extra_data[bit >> 3U] =
            one ? (extra_data[bit >> 3U] | mask) : (extra_data[bit >> 3U] & ~mask);
    }
    extra_data.back() = static_cast<std::byte>(parity_byte(*at.extra, extra_data));
    return true;
}

int sign_evolution_stream(std::span<std::byte> stream, const SigningKey& key,
                          EvolutionReading reading) {
    int signed_units = 0;
    for (const auto& [offset, length] : walk(stream).regions) {
        if (length != 0 && sign_evolution_frame(stream.subspan(offset, length), key, reading)) {
            ++signed_units;
        }
    }
    return signed_units;
}

bool has_evolution_tag(std::span<const std::byte> extra_data, EvolutionReading reading) {
    const Located at = locate(extra_data, reading);
    return at.state == Located::State::kFrame && at.has_field &&
           !all_zero(at.frame.protection.primary);
}

VerifyResult verify_evolution_frame(std::span<const std::byte> extra_data, const SigningKey& key,
                                    EvolutionReading reading) {
    const std::array<SigningKey, 1> keys{key};
    return verdict(locate(extra_data, reading), keys).result;
}

VerifySummary verify_evolution_stream(std::span<const std::byte> stream, const SigningKey& key,
                                      EvolutionReading reading) {
    return verify_evolution_stream_any(stream, std::span<const SigningKey>(&key, 1), reading)
        .totals;
}

KeyringVerdict verify_evolution_frame_any(std::span<const std::byte> extra_data,
                                          std::span<const SigningKey> keys,
                                          EvolutionReading reading) {
    return verdict(locate(extra_data, reading), keys);
}

KeyringSummary verify_evolution_stream_any(std::span<const std::byte> stream,
                                           std::span<const SigningKey> keys,
                                           EvolutionReading reading) {
    KeyringSummary out;
    out.per_key.assign(keys.size(), 0);
    for (const auto& [offset, length] : walk(stream).regions) {
        if (length == 0) {
            ++out.totals.no_container;  // a unit with no extra data has no frame to check
            continue;
        }
        const KeyringVerdict one =
            verify_evolution_frame_any(stream.subspan(offset, length), keys, reading);
        count(out.totals, one.result);
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
    if (found.units == 0) {
        return std::unexpected(GateError{GateErrorKind::kNoStream, 0});
    }
    if (found.skipped != 0) {
        return std::unexpected(GateError{GateErrorKind::kUnreadableUnit, found.skipped});
    }
    GatedStream out;
    out.bytes.assign(stream.begin(), stream.end());
    out.summary.per_key.assign(keys.size(), 0);
    for (const auto& [offset, length] : found.regions) {
        const std::span<const std::byte> region = stream.subspan(offset, length);
        const auto extra = length == 0 ? std::nullopt : read_extra_data(region);
        if (!extra || extra->padding_only) {
            ++out.summary.no_objects;
            continue;
        }
        const KeyringVerdict one = verify_evolution_frame_any(region, keys, reading);
        if (one.result == VerifyResult::kValid) {
            ++out.summary.passed;
            ++out.summary.per_key[one.key_index];
            continue;
        }
        // Anything else with extra data in it loses its object layer: the padding-only form is a
        // header word of zero and zero words after it (TH spec §16.2).
        std::fill_n(out.bytes.begin() + static_cast<std::ptrdiff_t>(offset), length, std::byte{0});
        ++out.summary.gated;
    }
    return out;
}

}  // namespace iclforge::truehd::signing
