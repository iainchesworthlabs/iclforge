#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

#include "iclforge/truehd/core/error.hpp"
#include "iclforge/truehd/export.hpp"
#include "iclforge/truehd/io/major_sync.hpp"

// Changing the channel meaning of a finished TrueHD stream (TH spec §14.1) without re-encoding it:
// dialogue normalisation, mix level, source format and the control-enabled bit of each presentation
// and the two DRC start-up gains. The major sync carries them and nothing else does; it repeats at
// every restart (every 128 access units by default), each copy under its own CRC-16, so an edit
// rewrites every major sync and re-stamps its CRC. No other check covers the major sync (the
// header's check nibble, the restart headers' CRC-8 and the segments' parity and CRC bytes start
// after it), so no other byte of the stream changes and a decoder gives the same samples. A major
// sync with the extension of §14.2 is rewritten to the size its CRC confirmed (parse_major_sync(),
// ERRATA.md question 44) and its extension bytes are kept.
//
// Provisional, no oracle until the DEE set: FFmpeg's encoder writes zeros for these fields and the
// specification gives no units, no table for the source formats and no word on what a
// control-enabled bit gates (ERRATA.md questions 51 to 55).

namespace iclforge::truehd {

// What to change. Every member is optional: one that is not set is left as the stream has it. The
// numbers are the fields' values, checked against their widths by apply_edit() and not before.
struct ChannelMeaningEdit {
    std::optional<int> heavy_drc_start_up_gain = std::nullopt;  // -32 to 31
    std::optional<int> drc_start_up_gain = std::nullopt;        // -64 to 63
    std::optional<bool> twoch_control_enabled = std::nullopt;
    std::optional<bool> sixch_control_enabled = std::nullopt;
    std::optional<bool> eightch_control_enabled = std::nullopt;
    std::optional<int> twoch_dialogue_norm = std::nullopt;    // 0 to 63
    std::optional<int> twoch_mix_level = std::nullopt;        // 0 to 63
    std::optional<int> sixch_dialogue_norm = std::nullopt;    // 0 to 31
    std::optional<int> sixch_mix_level = std::nullopt;        // 0 to 63
    std::optional<int> sixch_source_format = std::nullopt;    // 0 to 31
    std::optional<int> eightch_dialogue_norm = std::nullopt;  // 0 to 31
    std::optional<int> eightch_mix_level = std::nullopt;      // 0 to 63
    std::optional<int> eightch_source_format = std::nullopt;  // 0 to 63

    // True when nothing is set.
    [[nodiscard]] bool empty() const noexcept {
        return !heavy_drc_start_up_gain && !drc_start_up_gain && !twoch_control_enabled &&
               !sixch_control_enabled && !eightch_control_enabled && !twoch_dialogue_norm &&
               !twoch_mix_level && !sixch_dialogue_norm && !sixch_mix_level &&
               !sixch_source_format && !eightch_dialogue_norm && !eightch_mix_level &&
               !eightch_source_format;
    }
};

// Sets the named fields of `meaning`. A value its field cannot hold is returned as the sentence
// that names the field and its range (the same ones channel_meaning_problem() gives), and then
// `meaning` is unchanged; an empty view means every named field was set.
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT std::string_view apply_edit(const ChannelMeaningEdit& edit,
                                                                 ChannelMeaning& meaning) noexcept;

struct StreamEditSummary {
    std::size_t units = 0;            // access units read
    std::size_t major_syncs = 0;      // ... that carry a major sync, each rewritten
    std::size_t changed = 0;          // major syncs whose bytes the edit changed
    std::size_t distinct_before = 0;  // different channel meanings the major syncs had before
    std::size_t skipped_bytes = 0;    // bytes that did not read as an access unit, left as they are
    ChannelMeaning before;            // the first major sync's channel meaning, before the edit
    ChannelMeaning after;             // ... and after
};

// Applies `edit` to the channel meaning of every major sync of the TrueHD stream in `stream`, in
// place, and re-stamps each one's CRC-16. The units are found as ElementaryReader finds them;
// bytes that are not part of a unit that reads (an AC-3 core, damage) and every unit without a
// major sync are not touched, and neither is any byte of a major sync outside its channel meaning
// and CRC (the 16-channel extension of TH spec §14.2 stays as it is). An edit with nothing set
// changes nothing and still reports.
//
// kBadSync: no access unit with a major sync. kUnsupported: an MLP stream, whose bytes after
// substream_info the specification does not describe (TH spec §5.2). kBadParameter: a value its
// field cannot hold, the detail naming the field; the stream is not changed in that case.
[[nodiscard]] ICLFORGE_TRUEHD_EXPORT std::expected<StreamEditSummary, Error> edit_channel_meaning(
    std::span<std::byte> stream, const ChannelMeaningEdit& edit);

}  // namespace iclforge::truehd
