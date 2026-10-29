#include "iclforge/truehd/io/metadata_edit.hpp"

#include "io/channel_meaning_fields.hpp"

namespace iclforge::truehd {

namespace {

// Sets one numeric field, or reports it when its width cannot hold the value.
template <typename Field>
[[nodiscard]] bool set(const std::optional<int>& value, const detail::MeaningField& range,
                       Field& field) noexcept {
    if (!value) {
        return true;
    }
    if (!range.holds(*value)) {
        return false;
    }
    field = static_cast<Field>(*value);
    return true;
}

void set_flag(const std::optional<bool>& value, bool& field) noexcept {
    if (value) {
        field = *value;
    }
}

}  // namespace

std::string_view apply_edit(const ChannelMeaningEdit& edit, ChannelMeaning& meaning) noexcept {
    using namespace detail;
    ChannelMeaning next = meaning;
    if (!set(edit.heavy_drc_start_up_gain, kHeavyDrcStartUpGain, next.heavy_drc_start_up_gain)) {
        return kHeavyDrcStartUpGain.problem;
    }
    if (!set(edit.drc_start_up_gain, kDrcStartUpGain, next.drc_start_up_gain)) {
        return kDrcStartUpGain.problem;
    }
    if (!set(edit.twoch_dialogue_norm, kTwochDialogueNorm, next.twoch_dialogue_norm)) {
        return kTwochDialogueNorm.problem;
    }
    if (!set(edit.twoch_mix_level, kTwochMixLevel, next.twoch_mix_level)) {
        return kTwochMixLevel.problem;
    }
    if (!set(edit.sixch_dialogue_norm, kSixchDialogueNorm, next.sixch_dialogue_norm)) {
        return kSixchDialogueNorm.problem;
    }
    if (!set(edit.sixch_mix_level, kSixchMixLevel, next.sixch_mix_level)) {
        return kSixchMixLevel.problem;
    }
    if (!set(edit.sixch_source_format, kSixchSourceFormat, next.sixch_source_format)) {
        return kSixchSourceFormat.problem;
    }
    if (!set(edit.eightch_dialogue_norm, kEightchDialogueNorm, next.eightch_dialogue_norm)) {
        return kEightchDialogueNorm.problem;
    }
    if (!set(edit.eightch_mix_level, kEightchMixLevel, next.eightch_mix_level)) {
        return kEightchMixLevel.problem;
    }
    if (!set(edit.eightch_source_format, kEightchSourceFormat, next.eightch_source_format)) {
        return kEightchSourceFormat.problem;
    }
    set_flag(edit.twoch_control_enabled, next.twoch_control_enabled);
    set_flag(edit.sixch_control_enabled, next.sixch_control_enabled);
    set_flag(edit.eightch_control_enabled, next.eightch_control_enabled);
    meaning = next;
    return {};
}

}  // namespace iclforge::truehd
