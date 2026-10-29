#pragma once

#include <cstddef>
#include <string>

#include "iclforge/truehd/io/major_sync.hpp"

// The channel meaning of a TrueHD stream (TH spec §14.1) as `probe`, `metadata` and `normalize`
// show it: per presentation (2, 6 and 8 channels) the dialogue norm, the mix level, the source
// format and the control-enabled bit, and the two DRC start-up gains.
//
// PROVISIONAL, no oracle until the DEE set (libs/truehd/ERRATA.md, questions 51 to 55). The values
// are the fields as written; the units are the specification's where it gives one (§12.5: a
// dialogue norm of n is -n dBFS for the 6 and 8 channel presentations) and unknown elsewhere (the
// mix level, the source format, the gains, what a control-enabled bit gates), and every report says
// so.

namespace forge_cli::commands {

// A presentation of the major sync's channel meaning: the 2, 6 or 8 channel one.
enum class MeaningPresentation : int { k2ch = 0, k6ch = 1, k8ch = 2 };

[[nodiscard]] const char* presentation_name(MeaningPresentation presentation) noexcept;

// Whether a stream with `substreams` substreams has the presentation (substream 0 is the 2 channel
// one, 1 the 6 channel one and 2 the 8 channel one: ERRATA.md question 51).
[[nodiscard]] bool stream_has_presentation(MeaningPresentation presentation,
                                           int substreams) noexcept;

// "control on, dialogue norm 27 (-27 dBFS), mix level 17, source format 5"; the source format is
// left out for the 2 channel presentation, which has none.
[[nodiscard]] std::string describe_presentation(const iclforge::truehd::ChannelMeaning& meaning,
                                                MeaningPresentation presentation);

// "heavy -9, standard 21".
[[nodiscard]] std::string describe_start_up_gains(const iclforge::truehd::ChannelMeaning& meaning);

}  // namespace forge_cli::commands
