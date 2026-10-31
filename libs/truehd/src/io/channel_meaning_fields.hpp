#pragma once

#include <string_view>

// The widths of the channel meaning's fields (TH spec §14.1, §12.3, §12.5) as ranges, with the
// sentence that names a field when a value does not fit. Shared by the major sync writer, the
// encoder's configuration check and the in-place edit, so that all three reject the same values
// with the same words. Each sentence is a string literal: it goes into an Error's detail, which
// owns nothing.

namespace iclforge::truehd::detail {

struct MeaningField {
    std::string_view problem;
    int low;
    int high;

    [[nodiscard]] constexpr bool holds(int value) const noexcept {
        return value >= low && value <= high;
    }
};

inline constexpr MeaningField kHeavyDrcStartUpGain{
    "heavy_drc_start_up_gain is outside -32 to 31 (6 bits, signed; TH spec §12.3)", -32, 31};
inline constexpr MeaningField kDrcStartUpGain{
    "drc_start_up_gain is outside -64 to 63 (7 bits, signed; TH spec §12.3)", -64, 63};
inline constexpr MeaningField kTwochDialogueNorm{
    "twoch_dialogue_norm is outside 0 to 63 (6 bits; TH spec §12.5)", 0, 63};
inline constexpr MeaningField kTwochMixLevel{
    "twoch_mix_level is outside 0 to 63 (6 bits; TH spec §12.5)", 0, 63};
inline constexpr MeaningField kSixchDialogueNorm{
    "sixch_dialogue_norm is outside 0 to 31 (5 bits; TH spec §12.5)", 0, 31};
inline constexpr MeaningField kSixchMixLevel{
    "sixch_mix_level is outside 0 to 63 (6 bits; TH spec §12.5)", 0, 63};
inline constexpr MeaningField kSixchSourceFormat{
    "sixch_source_format is outside 0 to 31 (5 bits; TH spec §12.5)", 0, 31};
inline constexpr MeaningField kEightchDialogueNorm{
    "eightch_dialogue_norm is outside 0 to 31 (5 bits; TH spec §12.5)", 0, 31};
inline constexpr MeaningField kEightchMixLevel{
    "eightch_mix_level is outside 0 to 63 (6 bits; TH spec §12.5)", 0, 63};
inline constexpr MeaningField kEightchSourceFormat{
    "eightch_source_format is outside 0 to 63 (6 bits; TH spec §12.5)", 0, 63};

}  // namespace iclforge::truehd::detail
