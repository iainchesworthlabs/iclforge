#include <cstddef>
#include <string>

#include "iclforge/truehd/core/crc.hpp"
#include "iclforge/truehd/encoder/layout.hpp"
#include "iclforge/truehd/io/major_sync.hpp"

namespace iclforge::truehd {

namespace {

constexpr std::size_t kMaxEndWord = 0xFFF;  // the 12 bit end pointer of TH spec §6.1

[[nodiscard]] std::string unit_text(std::uint64_t unit, const char* what) {
    return "access unit " + std::to_string(unit) + ": " + what;
}

}  // namespace

std::string_view layout_cadence_problem(const LayoutCadence& cadence) noexcept {
    if (cadence.restart_units != 1 && cadence.restart_units < 8) {
        return "the restart interval must be 1 or at least 8 access units (TH spec §7.5)";
    }
    if (cadence.restart_units > 4096) {
        return "restart_interval_units above 4096";
    }
    if (cadence.major_sync_units < 0) {
        return "major_sync_units is negative";
    }
    if (cadence.major_sync_units != 0 && cadence.major_sync_units % cadence.restart_units != 0) {
        return "major_sync_units must be a multiple of restart_interval_units: a major sync "
               "opens a restart interval";
    }
    if (cadence.major_sync_units > 65536) {
        return "major_sync_units above 65536";
    }
    return {};
}

bool layout_has_major_sync(const LayoutCadence& cadence, std::uint64_t interval) noexcept {
    if (cadence.major_sync_units == 0 || cadence.major_sync_units == cadence.restart_units) {
        return true;
    }
    const auto every =
        static_cast<std::uint64_t>(cadence.major_sync_units / cadence.restart_units);
    return interval % every == 0;
}

void layout_seal_segment(std::vector<std::byte>& payload) {
    const std::uint8_t parity = substream_parity(payload);
    const std::uint8_t crc = substream_crc(payload);
    payload.push_back(static_cast<std::byte>(parity));
    payload.push_back(static_cast<std::byte>(crc));
}

std::expected<SubstreamEntry, Error> layout_directory_entry(bool restart, std::size_t end_word) {
    if (end_word > kMaxEndWord) {
        return std::unexpected(Error{ErrorKind::kBadLength, "a segment ends past 4095 words"});
    }
    SubstreamEntry entry;
    entry.restart_nonexistent = !restart;
    entry.crc_present = true;
    entry.end_ptr = static_cast<std::uint16_t>(end_word);
    return entry;
}

LayoutAudit audit_layout(std::span<const std::byte> stream) {
    LayoutAudit audit;
    const auto fail = [&audit](std::string text) {
        audit.ok = false;
        audit.problem = std::move(text);
        return audit;
    };
    std::size_t at = 0;
    int substreams = 0;
    while (at < stream.size()) {
        const std::uint64_t number = audit.units;
        const std::span<const std::byte> rest = stream.subspan(at);
        const std::size_t length = access_unit_length_bytes(rest);
        if (length == 0 || length > rest.size()) {
            return fail(unit_text(number, "the length runs past the stream or is 0"));
        }
        const auto parsed = parse_access_unit(rest.first(length), substreams, true);
        if (!parsed) {
            return fail(unit_text(number, to_string(parsed.error().kind).data()));
        }
        if (parsed->length_bytes() != length) {
            return fail(unit_text(number, "the header's length is not the unit's"));
        }
        if (number == 0 && !parsed->major_sync) {
            return fail(unit_text(number, "the first unit has no major sync"));
        }
        if (parsed->major_sync) {
            substreams = parsed->major_sync->num_substreams;
            ++audit.major_syncs;
        }
        if (parsed->num_substreams != substreams) {
            return fail(unit_text(number, "the directory's substream count is not the major "
                                          "sync's"));
        }
        const bool restart = !parsed->entries[0].restart_nonexistent;
        for (int i = 0; i < parsed->num_substreams; ++i) {
            const SubstreamEntry& entry = parsed->entries[static_cast<std::size_t>(i)];
            // the first entry decides: every substream restarts together
            if (!entry.restart_nonexistent != restart) {
                return fail(unit_text(number, "the substreams do not restart together"));
            }
        }
        if (parsed->major_sync && !restart) {
            return fail(unit_text(number, "a major sync without a restart header"));
        }
        if (restart) {
            ++audit.restarts;
        }
        for (int i = 0; i < parsed->num_substreams; ++i) {
            const SubstreamEntry& entry = parsed->entries[static_cast<std::size_t>(i)];
            const auto segment = parsed->segments[static_cast<std::size_t>(i)];
            if (!entry.crc_present) {
                continue;
            }
            if (segment.size() < 3 || segment.size() % 2 != 0) {
                return fail(unit_text(number, "a segment with parity and CRC is shorter than 3 "
                                              "bytes or an odd size"));
            }
            const auto payload = segment.first(segment.size() - 2);
            if (substream_parity(payload) !=
                std::to_integer<std::uint8_t>(segment[segment.size() - 2])) {
                return fail(unit_text(number, "a segment's parity byte is wrong"));
            }
            if (substream_crc(payload) !=
                std::to_integer<std::uint8_t>(segment[segment.size() - 1])) {
                return fail(unit_text(number, "a segment's CRC byte is wrong"));
            }
            ++audit.segments;
        }
        if (parsed->extra_data && !parsed->extra_data->padding_only) {
            const ExtraData& extra = *parsed->extra_data;
            if (!extra.check_ok || !extra.framing_ok || !extra.parity_ok) {
                return fail(unit_text(number, "the extra data's framing, check nibble or parity "
                                              "does not hold"));
            }
            ++audit.extra_data_units;
        }
        ++audit.units;
        at += length;
    }
    if (audit.units == 0) {
        return fail("the stream has no access unit");
    }
    return audit;
}

}  // namespace iclforge::truehd
