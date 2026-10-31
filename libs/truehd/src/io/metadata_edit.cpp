#include "iclforge/truehd/io/metadata_edit.hpp"

#include <algorithm>
#include <array>
#include <vector>

#include "iclforge/truehd/core/crc.hpp"
#include "iclforge/truehd/io/elementary.hpp"
#include "io/channel_meaning_fields.hpp"

namespace iclforge::truehd {

namespace {

constexpr std::size_t kUnitHeaderBytes = 4;  // the access unit's header, before the major sync
// From the sync word: the word and format_info (8 bytes), the signature, flags and reserved words
// (6), the rate word (2), the substream count (1) and substream_info (1). TH spec §4.1.
constexpr std::size_t kChannelMeaningAt = 18;
constexpr std::size_t kChannelMeaningBytes = 8;  // kChannelMeaningBits / 8

static_assert(kChannelMeaningBytes * 8 == kChannelMeaningBits);

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

std::expected<StreamEditSummary, Error> edit_channel_meaning(std::span<std::byte> stream,
                                                             const ChannelMeaningEdit& edit) {
    // A value that does not fit is found before anything is written.
    ChannelMeaning probe;
    if (const std::string_view field = apply_edit(edit, probe); !field.empty()) {
        return std::unexpected(Error{ErrorKind::kBadParameter, field});
    }

    // The major syncs, found first: the reader parses the units it hands out, and an edit that
    // changed a unit under it would be read back through its own CRC.
    const std::span<const std::byte> view = stream;
    ElementaryReader reader(view);
    std::vector<std::size_t> syncs;  // offsets of the sync words
    StreamEditSummary summary;
    while (const auto unit = reader.next()) {
        ++summary.units;
        if (unit->size() >= kUnitHeaderBytes + 4 &&
            starts_with_major_sync(unit->subspan(kUnitHeaderBytes))) {
            syncs.push_back(static_cast<std::size_t>(unit->data() - view.data()) +
                            kUnitHeaderBytes);
        }
    }
    summary.skipped_bytes = reader.skipped_bytes();
    if (syncs.empty()) {
        return std::unexpected(Error{ErrorKind::kBadSync, "no access unit with a major sync"});
    }

    struct Patch {
        std::size_t sync = 0;
        std::size_t size = 0;
        ChannelMeaning before;
    };
    std::vector<Patch> patches;
    patches.reserve(syncs.size());
    for (const std::size_t sync : syncs) {
        const auto major_sync = parse_major_sync(view.subspan(sync), true);
        if (!major_sync) {
            // The reader handed the unit out, so its major sync read; a failure here is a bug.
            return std::unexpected(major_sync.error());
        }
        if (major_sync->kind != StreamKind::kTrueHd) {
            return std::unexpected(Error{ErrorKind::kUnsupported,
                                         "an MLP stream's bytes after substream_info are not "
                                         "described (TH spec §5.2)"});
        }
        patches.push_back(Patch{sync, major_sync->size_bytes, major_sync->channel_meaning});
    }

    std::vector<ChannelMeaning> distinct;
    for (const Patch& patch : patches) {
        // any_of and not find: clang-cl on MSVC's standard library takes find over a type with a
        // defaulted operator== for a vectorised search and static_asserts on its size.
        const auto seen = [&patch](const ChannelMeaning& meaning) { return meaning == patch.before; };
        if (!std::ranges::any_of(distinct, seen)) {
            distinct.push_back(patch.before);
        }
    }
    summary.distinct_before = distinct.size();
    summary.major_syncs = patches.size();
    summary.before = patches.front().before;
    summary.after = patches.front().before;
    (void)apply_edit(edit, summary.after);  // fits: checked above

    for (const Patch& patch : patches) {
        ChannelMeaning meaning = patch.before;
        (void)apply_edit(edit, meaning);
        const std::span<std::byte> sync = stream.subspan(patch.sync, patch.size);
        const std::array<std::uint8_t, kChannelMeaningBytes> packed = pack_channel_meaning(meaning);
        for (std::size_t i = 0; i < packed.size(); ++i) {
            sync[kChannelMeaningAt + i] = static_cast<std::byte>(packed[i]);
        }
        const std::uint16_t crc =
            major_sync_crc(std::span<const std::byte>{sync}.first(sync.size() - 2));
        sync[sync.size() - 2] = static_cast<std::byte>(crc >> 8U);
        sync[sync.size() - 1] = static_cast<std::byte>(crc & 0xFFU);
        if (meaning != patch.before) {
            ++summary.changed;
        }
    }
    return summary;
}

}  // namespace iclforge::truehd
