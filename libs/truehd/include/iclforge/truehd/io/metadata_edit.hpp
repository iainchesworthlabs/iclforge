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

// The channel meaning's fields as a set of changes (TH spec §14.1): dialogue normalisation, mix
// level, source format and the control-enabled bit of each presentation, and the two DRC start-up
// gains.
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

}  // namespace iclforge::truehd
