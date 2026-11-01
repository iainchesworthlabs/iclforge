#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/truehd/encoder/encoder.hpp"
#include "iclforge/truehd/export.hpp"
#include "iclforge/truehd/io/access_unit.hpp"

// The container of a TrueHD Atmos stream: where restart headers and major syncs fall, how a
// substream segment is closed and listed in the access unit's directory, and how the object audio
// metadata is cut into the access units' extra data. What the encoder writes and what the decoder
// checks are written down here once, and audit_layout() holds a stream to them.

namespace iclforge::truehd {

// --- restart intervals and the major sync cadence (TH spec §4, §6, §7.5)
// ----------------------------

struct LayoutCadence {
    // The access units between restart headers: 1, or 8 to 4096. Every substream restarts together.
    int restart_units = 128;
    // The access units between major syncs. 0 puts one at every restart (a restart interval is then
    // what a random access can start from, as in FFmpeg's streams). Otherwise a multiple of
    // `restart_units`: the restarts between two major syncs carry a restart header in each
    // substream's first block and no major sync. A decoder that joins a stream waits for the next
    // major sync, and the major sync's bytes are paid less often.
    int major_sync_units = 0;
};

// The sentence that says why a cadence is not written, or an empty view.
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT std::string_view layout_cadence_problem(
    const LayoutCadence& cadence) noexcept;

// Whether restart interval number `interval` (0 for the first of a stream) opens with a major sync.
// The first interval of a stream always does.
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT bool layout_has_major_sync(const LayoutCadence& cadence,
                                                                std::uint64_t interval) noexcept;

// --- the substream segments and the directory (TH spec §6.1, §6.2)
// ----------------------------------

// Closes a segment: pads `payload` (a whole number of 16-bit words) with the parity byte and the
// CRC byte of TH spec §6.2. The segment is then a whole number of words again.
void ICLFORGE_TRUEHD_EXPORT layout_seal_segment(std::vector<std::byte>& payload);

// The directory entry of a segment that ends `end_word` 16-bit words from the first segment's start
// (sealed, so with parity and CRC), and does (`restart`) or does not carry a restart header.
// kBadLength when `end_word` does not fit the 12 bit field.
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT std::expected<SubstreamEntry, Error> layout_directory_entry(
    bool restart, std::size_t end_word);

// --- audit
// ----------------------------------------------------------------------------------------

struct LayoutAudit {
    bool ok = true;
    std::string problem;  // the first thing wrong, with its access unit's number; empty when ok
    std::uint64_t units = 0;
    std::uint64_t restarts = 0;  // units that start a restart interval
    std::uint64_t major_syncs = 0;
    std::uint64_t segments = 0;  // segments whose parity and CRC were checked
    std::uint64_t extra_data_units = 0;
};

// Holds a whole stream to what a decoder checks: every unit's check nibble, length and directory,
// the major sync's CRC, every segment's end pointer, parity and CRC, the framing and parity of the
// extra data, that the first unit has a major sync, that a unit with a major sync restarts every
// substream and that every substream of a unit restarts together. Stops at the first fault.
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT LayoutAudit audit_layout(std::span<const std::byte> stream);

// --- the object audio metadata in the extra data (TH spec §15.5.3, §16; ERRATA.md entries 62 to
// 64) -

// The metadata is cut at the cadence of the owner's discs: one frame of 1536 samples, E-AC-3's,
// where §15.5.3 says one a 40 sample unit.
inline constexpr int kOamdFrameSamples = 1536;

// The frame whose first sample falls in access unit `unit` (of `unit_samples` samples) and where.
struct OamdFrameAt {
    std::uint64_t frame = 0;
    int offset = 0;  // samples from the first sample of the unit to the frame's first sample
};

// The frame that starts inside unit `unit`, if one does: frame k starts at sample 1536 k, so over a
// stream one unit in 38.4 holds one (the gaps are 38 and 39 units).
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT std::optional<OamdFrameAt> oamd_frame_in_unit(
    std::uint64_t unit, int unit_samples, int frame_samples = kOamdFrameSamples) noexcept;

// How a payload says the offset: the update's own sample offset (0 to 31) and the block offset
// factor, in steps of 32 samples (TS 103 420 §5.5.7), which the reader adds back.
struct OamdOffset {
    int sample_offset = 0;
    int block_offset_factor = 0;
};
inline constexpr int kOamdOffsetStep = 32;
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT OamdOffset split_oamd_offset(int offset) noexcept;

// The evolution frame (the container, without the 32 bits of sync word and length that introduce it
// in E-AC-3) of frame number `frame`, starting `offset` samples into its access unit.
using OamdFrameSource = std::function<std::vector<std::byte>(std::uint64_t frame, int offset)>;

// An EncoderConfig::extra_data that gives `source`'s frame to the unit that holds its first sample
// and nothing to the others. The encoder frames what it returns (TH spec §16.1, §16.4) and counts
// its bytes in the unit's size, so the rate control sees them.
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT ExtraDataSource
oamd_extra_data(OamdFrameSource source, int unit_samples, int frame_samples = kOamdFrameSamples);

// TH spec §16.1 and §16.4: the extra data region of `payload`: check nibble and length (in 16-bit
// words, less one), evo_reserved and the payload length, the payload, zero padding to a whole word
// and the parity byte.
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT std::vector<std::byte> layout_frame_extra_data(
    std::span<const std::byte> payload);

}  // namespace iclforge::truehd
