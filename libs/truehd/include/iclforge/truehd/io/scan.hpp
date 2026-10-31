#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

#include "iclforge/truehd/core/error.hpp"
#include "iclforge/truehd/core/format.hpp"
#include "iclforge/truehd/export.hpp"

// A scan of a TrueHD or MLP stream held in memory: what the stream is, how long it is, and where a
// decoder can join it. This is audit row Co11 (planning/truehd-parity-library.md): the same
// capability AC-3 and E-AC-3 have in ScannedStream and access_unit_at_sample, with the one
// difference TrueHD forces, which is the granule.
//
// An access unit holds 40, 80 or 160 samples (at 48, 96 or 192 kHz; the 44.1 kHz family the same),
// so a second of a stream is 1,200 units or more, and every unit has the same length in samples. A
// unit's first sample is therefore its number times that length and needs no record. What does need
// one is where each unit lies in the data, and a decoder can only start at a unit that carries a
// major sync: it has no state before a restart header, and a unit with a major sync restarts every
// substream. So the scan keeps those units, thinned to one every `restart_spacing` units at most,
// with their byte offsets, and reaching a place in the stream is a read from the last of them
// before it. The restart interval is the seek granule.
//
// Only the last access unit of a stream that does not end on a unit boundary is shorter, by the
// padding its end-of-stream marker counts (TH spec §16.5). The marker is in the substream
// segments, so the scan reads it by decoding the last restart interval once.

namespace iclforge::truehd {

// A unit a decoder can start at: its number counted from the first unit of the stream, and where it
// begins in the data.
struct RestartPoint {
    std::uint64_t unit = 0;
    std::uint64_t offset = 0;
    friend bool operator==(const RestartPoint&, const RestartPoint&) = default;
};

struct ScanOptions {
    // Units are kept as joining points at least this far apart (0.21 s at 48 kHz), whatever the
    // stream offers: a stream that restarts at every unit would otherwise cost an entry for each.
    // 1 keeps every unit that carries a major sync, which are also the only places a cut or a join
    // may start. 0 is taken as 1.
    std::uint64_t restart_spacing = 256;
};

inline constexpr std::uint64_t kDefaultRestartSpacing = 256;

struct ScannedStream {
    StreamKind kind = StreamKind::kTrueHd;
    std::uint32_t sample_rate = 0;
    // The samples in each access unit, from the sample rate.
    std::uint32_t samples_per_unit = 0;
    int num_substreams = 0;
    // What each substream's first restart header says a decode of it delivers (max_matrix_chan +
    // 1); 0 where the scan did not reach one.
    std::array<int, kMaxSubstreams> channels{};
    // The major sync's rate fields (TH spec §4.4): variable rate, and the peak the stream is
    // declared not to pass, in the unit the field has.
    bool variable_rate = false;
    std::uint32_t peak_data_rate = 0;
    // A fourth substream, or an object bit in the substream info: the stream has an object layer.
    bool has_object_substream = false;

    // Units that read, counting from the first one, which carries a major sync.
    std::uint64_t units = 0;
    // The bytes of those units together.
    std::uint64_t unit_bytes = 0;
    // Bytes of the data that were not part of a unit that read, those ahead of the first unit
    // included (so a stream that starts clean and loses nothing has 0), and the part of them that
    // came before the first unit, which is not a loss of the stream.
    std::uint64_t skipped_bytes = 0;
    std::uint64_t leading_bytes = 0;
    // Units that carry a major sync, all of them and not only those `restarts` keeps.
    std::uint64_t major_syncs = 0;
    // The samples at the end of the last unit that are padding; 0 for a stream whose last restart
    // interval does not decode to its end.
    std::uint32_t padding_samples = 0;
    // The units to join at, in order, the first unit among them and none closer to the last than
    // the spacing the scan was made with.
    std::vector<RestartPoint> restarts;

    // Every sample the stream holds, padding left out.
    [[nodiscard]] std::uint64_t samples() const noexcept {
        const std::uint64_t whole = units * samples_per_unit;
        return whole > padding_samples ? whole - padding_samples : 0;
    }

    [[nodiscard]] double duration_seconds() const noexcept {
        return sample_rate == 0 ? 0.0
                                : static_cast<double>(samples()) / static_cast<double>(sample_rate);
    }

    // The sample a unit begins at.
    [[nodiscard]] std::uint64_t unit_start_sample(std::uint64_t unit) const noexcept {
        return unit * samples_per_unit;
    }

    // The unit that holds `sample`, or nothing for a sample past the end of the stream.
    [[nodiscard]] std::optional<std::uint64_t> unit_at_sample(std::uint64_t sample) const noexcept {
        if (samples_per_unit == 0 || sample >= samples()) {
            return std::nullopt;
        }
        return sample / samples_per_unit;
    }

    // The sample at a time, rounded down; a negative time is the first sample. Past the end it
    // gives a sample past the end (unit_at_sample then has nothing to say).
    [[nodiscard]] std::uint64_t sample_at_seconds(double seconds) const noexcept {
        return seconds <= 0.0 ? 0U
                              : static_cast<std::uint64_t>(seconds * static_cast<double>(sample_rate));
    }

    // The last restart at or before `unit`. The first unit is one, so there is always an answer.
    [[nodiscard]] const RestartPoint& restart_at_or_before(std::uint64_t unit) const noexcept {
        const auto after = std::ranges::upper_bound(restarts, unit, {}, &RestartPoint::unit);
        return after == restarts.begin() ? restarts.front() : *std::prev(after);
    }

    // Where to start decoding to reach `sample`: the restart at or before its unit. Nothing for a
    // sample past the end.
    [[nodiscard]] std::optional<RestartPoint> restart_for_sample(
        std::uint64_t sample) const noexcept {
        const auto unit = unit_at_sample(sample);
        if (!unit) {
            return std::nullopt;
        }
        return restart_at_or_before(*unit);
    }
};

// Reads the whole of `data` once: the stream's facts from its first major sync, the units, the
// restarts to keep, and the padding at the end. The stream may start up to kMaxLeadingBytes into
// `data` (find_stream_start). The error is kBadSync for data with no unit that reads.
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT std::expected<ScannedStream, Error> scan_stream(
    std::span<const std::byte> data, const ScanOptions& options = {});

}  // namespace iclforge::truehd
