#pragma once

#include <array>
#include <optional>

#include "iclforge/ac4/decoder/config.hpp"
#include "iclforge/ac4/decoder/decoder.hpp"
#include "iclforge/ac4/decoder/presentation.hpp"
#include "iclforge/base/dynamic_range.hpp"

// What a listener's choice of dynamic range (iclforge/base/dynamic_range.hpp) means to the AC-4
// decoder, written into its OutputConfig.
//
// A decoder made with no output level (OutputConfig::output_level_dbfs) plays a stream as coded and
// compresses nothing, and that is what a board does today: kStandard changes nothing, and
// follow_dynamic_range() never touches a decoder it is given that choice for.
//
// kLateNight compresses with one of Part 1 Table 161's DRC decoder modes, chosen among those the
// STREAM configures (a mode a stream does not configure compresses nothing): portable speakers
// first, the strongest compression Table 161 names for speakers, then the flat-panel TV's, then
// the home theatre's. Headphones are never chosen (they ask for a different presentation too).
// The mode's curve works on the signal's level against its dialnorm, so it acts the same whatever
// the output level is.
//
// And the output level is the stream's own dialnorm: Part 1 clause 5.7.9.3.3's gain
// 2^((Lout - dialnorm) / 6) is then 1, so switching late night on moves no stream's level by its
// dialnorm. (Lout of -31 dBFS, the reference the AC-3 decoders normalise to and Hearth's own
// player defaults to, would be 13 dB down on a stream whose dialnorm is -18, which is the level
// trap RF mode is for AC-3.) A stream's dialnorm is read from its first frame, so that frame
// plays as coded and the compression starts with the next, which is why a decoder is brought to
// the choice after each frame by follow_dynamic_range() and not made with it.

namespace iclforge::ac4 {

using base::DynamicRange;

// The DRC decoder mode late night asks for among the modes `metadata` says the stream configures,
// or nothing where it configures none of the three.
[[nodiscard]] inline std::optional<DrcMode> late_night_drc_mode(const PresentationMetadata& metadata) {
    if (!metadata.drc) {
        return std::nullopt;
    }
    // Table 161's drc_decoder_mode_id of each: DrcMode::kHomeTheatre is 0, then one more each.
    constexpr std::array<DrcMode, 3> kPreferred{DrcMode::kPortableSpeakers, DrcMode::kFlatPanelTv,
                                                DrcMode::kHomeTheatre};
    for (const DrcMode mode : kPreferred) {
        const int id = static_cast<int>(mode) - static_cast<int>(DrcMode::kHomeTheatre);
        for (const DrcModeInfo& configured : metadata.drc->modes) {
            if (configured.id == id) {
                return mode;
            }
        }
    }
    return std::nullopt;
}

// `base` for kStandard. For kLateNight, `base` with the mode and the output level above, once
// `metadata` has a dialnorm and a mode to give; before that, and for a stream that configures no
// mode, `base` too.
[[nodiscard]] inline OutputConfig output_for_dynamic_range(DynamicRange range, OutputConfig base,
                                                           const PresentationMetadata& metadata) {
    if (range != DynamicRange::kLateNight) {
        return base;
    }
    const std::optional<DrcMode> mode = late_night_drc_mode(metadata);
    if (!mode || !metadata.loudness.dialnorm_dbfs) {
        return base;
    }
    base.output_level_dbfs = *metadata.loudness.dialnorm_dbfs;
    base.drc = *mode;
    return base;
}

// Brings `decoder` to the choice from its next frame: call it after each frame decoded. `base` is
// the output processing the decoder was made with, which every other field of the result is.
// False when nothing needed changing, and always false for kStandard.
inline bool follow_dynamic_range(Decoder& decoder, DynamicRange range, const OutputConfig& base) {
    if (range != DynamicRange::kLateNight) {
        return false;
    }
    const OutputConfig wanted = output_for_dynamic_range(range, base, decoder.metadata());
    const OutputConfig& now = decoder.output();
    if (wanted.output_level_dbfs == now.output_level_dbfs && wanted.drc == now.drc) {
        return false;
    }
    decoder.set_output(wanted);
    return true;
}

// Back to `base` for the first frame of a new stream: the output level the last stream's dialnorm gave
// would otherwise be the new stream's until its own first frame has said what its dialnorm is, a frame
// at the wrong level by the difference. False when the decoder already has the base's.
inline bool restore_base_output(Decoder& decoder, const OutputConfig& base) {
    const OutputConfig& now = decoder.output();
    if (base.output_level_dbfs == now.output_level_dbfs && base.drc == now.drc) {
        return false;
    }
    decoder.set_output(base);
    return true;
}

}  // namespace iclforge::ac4
