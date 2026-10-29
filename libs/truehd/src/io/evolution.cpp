#include "iclforge/truehd/io/evolution.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "iclforge/base/bitreader.hpp"

// Provisional, owner to confirm: see the header and libs/truehd/ERRATA.md, "Atmos, spec v3.6".

namespace iclforge::truehd {

namespace {

constexpr int kEmdfEscapeId = 0x1F;
constexpr std::size_t kPayloadIdBits = 5;

// The protection field sizes in bytes for the length codes 0 to 3 (TH spec v3.6 §15.4 SIZE_TABLE; Annex H
// Tables H.2.5 and H.2.6 in bits).
constexpr std::array<std::size_t, 4> kProtectionBytes = {0, 1, 4, 16};

// TH spec v3.6 §15.4's maximum number of extension reads for each variable_bits(n, max_ext).
constexpr int kExtVersion = 16;
constexpr int kExtKeyId = 10;
constexpr int kExtSampleOffset = 2;
constexpr int kExtDuration = 2;
constexpr int kExtGroupId = 16;
constexpr int kExtSize = 4;

class Cursor {
   public:
    Cursor(std::span<const std::byte> data, EvolutionReading reading) noexcept
        : reader_(data), reading_(reading) {}

    [[nodiscard]] BitReader& reader() noexcept { return reader_; }
    [[nodiscard]] bool emdf() const noexcept { return reading_ == EvolutionReading::kEmdf; }
    [[nodiscard]] bool truncated() const noexcept { return reader_.overflowed(); }
    [[nodiscard]] std::size_t position() const noexcept { return reader_.bit_position(); }

    // variable_bits(n, max_ext) in the active reading. kEmdf: groups of n bits, each followed by a
    // read-more flag, a further group adding 2^n to the shifted value (the coding BitReader implements,
    // which iclforge::objects reads the E-AC-3 container with). kSpec36: an escape value, n bits and,
    // when all are ones, n more, at most max_ext times.
    std::uint64_t variable(int n, int max_ext) noexcept {
        if (emdf()) {
            return reader_.variable_bits(n);
        }
        const std::uint32_t all_ones = (std::uint32_t{1} << n) - 1U;
        std::uint64_t value = 0;
        std::uint32_t group = reader_.read(n);
        value += group;
        for (int extension = 0; group == all_ones && extension < max_ext && !reader_.overflowed();
             ++extension) {
            group = reader_.read(n);
            value += group;
        }
        return value;
    }

   private:
    BitReader reader_;
    EvolutionReading reading_;
};

[[nodiscard]] EvolutionError fail(EvolutionErrorKind kind, std::size_t at) noexcept {
    return EvolutionError{kind, at};
}

// evo_payload_config(). Returns false when the data ends inside it.
[[nodiscard]] bool read_config(Cursor& cursor, EvolutionPayloadConfig& config) {
    BitReader& r = cursor.reader();
    const bool sample_offset_present = r.read(1) != 0;
    if (sample_offset_present) {
        if (cursor.emdf()) {
            config.sample_offset = r.read(11);
            r.skip(1);  // the reserved bit
        } else {
            config.sample_offset = cursor.variable(11, kExtSampleOffset);
        }
    }
    if (r.read(1) != 0) {
        config.duration = cursor.variable(11, kExtDuration);
    }
    if (r.read(1) != 0) {
        config.group_id = cursor.variable(2, kExtGroupId);
    }
    if (r.read(1) != 0) {
        config.codec_data = static_cast<std::uint8_t>(r.read(8));
    }
    config.discard_unknown = r.read(1) != 0;
    if (!config.discard_unknown) {
        if (!sample_offset_present) {
            config.frame_aligned = r.read(1) != 0;
            if (config.frame_aligned) {
                config.create_duplicate = r.read(1) != 0;
                config.remove_duplicate = r.read(1) != 0;
            }
        }
        if (sample_offset_present || config.frame_aligned) {
            config.priority = static_cast<std::uint8_t>(r.read(5));
            config.proc_allowed = static_cast<std::uint8_t>(r.read(2));
        }
    }
    return !cursor.truncated();
}

[[nodiscard]] std::vector<std::byte> read_bytes(BitReader& r, std::size_t count) {
    std::vector<std::byte> bytes;
    bytes.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        bytes.push_back(static_cast<std::byte>(r.read(8)));
    }
    return bytes;
}

}  // namespace

const EvolutionPayload* EvolutionFrame::find(std::uint64_t id) const noexcept {
    for (const EvolutionPayload& payload : payloads) {
        if (payload.id == id) {
            return &payload;
        }
    }
    return nullptr;
}

std::span<const std::byte> EvolutionFrame::oamd() const noexcept {
    const EvolutionPayload* payload = find(kEvolutionPayloadOamd);
    return payload == nullptr ? std::span<const std::byte>{} : std::span<const std::byte>{payload->bytes};
}

std::string_view to_string(EvolutionErrorKind kind) noexcept {
    switch (kind) {
        case EvolutionErrorKind::kNoEvolutionPayload: return "no_evolution_payload";
        case EvolutionErrorKind::kEmpty: return "empty";
        case EvolutionErrorKind::kTruncatedHeader: return "truncated_header";
        case EvolutionErrorKind::kMissingTerminator: return "missing_terminator";
        case EvolutionErrorKind::kTruncatedPayloadId: return "truncated_payload_id";
        case EvolutionErrorKind::kTruncatedConfig: return "truncated_config";
        case EvolutionErrorKind::kTruncatedSize: return "truncated_size";
        case EvolutionErrorKind::kTruncatedPayload: return "truncated_payload";
        case EvolutionErrorKind::kReservedProtectionLength: return "reserved_protection_length";
        case EvolutionErrorKind::kTruncatedProtection: return "truncated_protection";
    }
    return "unknown";
}

std::string_view to_string(EvolutionReading reading) noexcept {
    return reading == EvolutionReading::kEmdf ? "emdf" : "spec36";
}

std::expected<EvolutionFrame, EvolutionError> parse_evolution_frame(
    std::span<const std::byte> evolution_payload, EvolutionReading reading) {
    if (evolution_payload.empty()) {
        return std::unexpected(fail(EvolutionErrorKind::kEmpty, 0));
    }
    Cursor cursor{evolution_payload, reading};
    BitReader& r = cursor.reader();
    EvolutionFrame frame;

    // evo_version, key_id: 2 and 3 bits, each with an extension at its maximum value.
    frame.version = r.read(2);
    if (frame.version == 3) {
        frame.version += cursor.variable(2, kExtVersion);
    }
    frame.key_id = r.read(3);
    if (frame.key_id == 7) {
        frame.key_id += cursor.variable(3, kExtKeyId);
    }
    if (cursor.truncated()) {
        return std::unexpected(fail(EvolutionErrorKind::kTruncatedHeader, 0));
    }

    // The payloads, up to the id 0 that ends the list.
    while (true) {
        const std::size_t id_at = cursor.position();
        if (r.remaining_bits() < kPayloadIdBits) {
            return std::unexpected(fail(EvolutionErrorKind::kMissingTerminator, id_at));
        }
        EvolutionPayload payload;
        payload.id = r.read(static_cast<int>(kPayloadIdBits));
        if (payload.id == 0) {
            break;
        }
        if (cursor.emdf() && payload.id == static_cast<std::uint64_t>(kEmdfEscapeId)) {
            payload.id += cursor.variable(static_cast<int>(kPayloadIdBits), 0);
            if (cursor.truncated()) {
                return std::unexpected(fail(EvolutionErrorKind::kTruncatedPayloadId, id_at));
            }
        }
        const std::size_t config_at = cursor.position();
        if (!read_config(cursor, payload.config)) {
            return std::unexpected(fail(EvolutionErrorKind::kTruncatedConfig, config_at));
        }
        const std::size_t size_at = cursor.position();
        const std::uint64_t size = cursor.variable(8, kExtSize);
        if (cursor.truncated()) {
            return std::unexpected(fail(EvolutionErrorKind::kTruncatedSize, size_at));
        }
        const std::size_t payload_at = cursor.position();
        if (size > r.remaining_bits() / 8U) {
            return std::unexpected(fail(EvolutionErrorKind::kTruncatedPayload, payload_at));
        }
        payload.bytes = read_bytes(r, static_cast<std::size_t>(size));
        frame.payloads.push_back(std::move(payload));
    }

    // evo_protection(): two length codes, then the bytes they size.
    const std::size_t protection_at = cursor.position();
    frame.protection.primary_code = static_cast<std::uint8_t>(r.read(2));
    frame.protection.secondary_code = static_cast<std::uint8_t>(r.read(2));
    if (cursor.truncated()) {
        return std::unexpected(fail(EvolutionErrorKind::kTruncatedProtection, protection_at));
    }
    if (cursor.emdf() && frame.protection.primary_code == 0) {
        return std::unexpected(fail(EvolutionErrorKind::kReservedProtectionLength, protection_at));
    }
    const std::size_t primary_bytes = kProtectionBytes[frame.protection.primary_code];
    const std::size_t secondary_bytes = kProtectionBytes[frame.protection.secondary_code];
    if ((primary_bytes + secondary_bytes) * 8U > r.remaining_bits()) {
        return std::unexpected(fail(EvolutionErrorKind::kTruncatedProtection, protection_at + 4U));
    }
    frame.protection.primary = read_bytes(r, primary_bytes);
    frame.protection.secondary = read_bytes(r, secondary_bytes);

    frame.bits_used = cursor.position();
    while (r.remaining_bits() > 0) {
        const int chunk = r.remaining_bits() > 32U ? 32 : static_cast<int>(r.remaining_bits());
        if (r.read(chunk) != 0) {
            frame.trailing_zero = false;
            break;
        }
    }
    return frame;
}

EvolutionRead read_evolution(const ExtraData& extra, EvolutionReading reading) {
    EvolutionRead out;
    out.check_ok = extra.check_ok;
    out.framing_ok = extra.framing_ok;
    out.parity_ok = extra.framing_ok && extra.parity_ok;
    out.reserved_zero = extra.framing_ok && extra.evo_reserved == 0;
    out.extra_data_length = extra.length;
    out.evo_frame_byte_length = extra.evo_frame_byte_length;
    out.length_fits = extra.framing_ok &&
                      (std::size_t{extra.length} + 1U) * 16U >=
                          std::size_t{extra.evo_frame_byte_length} * 8U + 40U;
    if (extra.framing_ok) {
        out.container = parse_evolution_frame(extra.evolution_payload, reading);
    }
    return out;
}

}  // namespace iclforge::truehd
