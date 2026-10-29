#include "truehd_meaning.hpp"

#include <fmt/format.h>
#include <optional>

#include "iclforge/truehd/io/major_sync.hpp"

namespace forge_cli::commands {

namespace {

namespace th = iclforge::truehd;

struct Fields {
    bool control = false;
    int dialogue_norm = 0;
    int mix_level = 0;
    std::optional<int> source_format = std::nullopt;  // the 2 channel presentation has none
};

Fields fields_of(const th::ChannelMeaning& meaning, MeaningPresentation presentation) {
    switch (presentation) {
        case MeaningPresentation::k2ch:
            return {meaning.twoch_control_enabled, meaning.twoch_dialogue_norm,
                    meaning.twoch_mix_level, std::nullopt};
        case MeaningPresentation::k6ch:
            return {meaning.sixch_control_enabled, meaning.sixch_dialogue_norm,
                    meaning.sixch_mix_level, meaning.sixch_source_format};
        case MeaningPresentation::k8ch:
            break;
    }
    return {meaning.eightch_control_enabled, meaning.eightch_dialogue_norm,
            meaning.eightch_mix_level, meaning.eightch_source_format};
}

}  // namespace

const char* presentation_name(MeaningPresentation presentation) noexcept {
    switch (presentation) {
        case MeaningPresentation::k2ch:
            return "2ch";
        case MeaningPresentation::k6ch:
            return "6ch";
        case MeaningPresentation::k8ch:
            break;
    }
    return "8ch";
}

bool stream_has_presentation(MeaningPresentation presentation, int substreams) noexcept {
    return substreams > static_cast<int>(presentation);
}

std::string describe_presentation(const th::ChannelMeaning& meaning,
                                  MeaningPresentation presentation) {
    const Fields fields = fields_of(meaning, presentation);
    // TH spec §12.5: for the 6 and 8 channel presentations "0 to -31 dBFS"; the 2 channel field is
    // read the same way, provisionally (ERRATA.md, question 51).
    std::string text = fmt::format("control {}, dialogue norm {} ({} dBFS), mix level {}",
                                   fields.control ? "on" : "off", fields.dialogue_norm,
                                   -fields.dialogue_norm, fields.mix_level);
    if (fields.source_format) {
        text += fmt::format(", source format {}", *fields.source_format);
    }
    return text;
}

std::string describe_start_up_gains(const th::ChannelMeaning& meaning) {
    return fmt::format("heavy {}, standard {}", meaning.heavy_drc_start_up_gain,
                       meaning.drc_start_up_gain);
}

}  // namespace forge_cli::commands
