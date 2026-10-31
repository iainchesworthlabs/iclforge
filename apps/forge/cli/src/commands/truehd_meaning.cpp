#include "truehd_meaning.hpp"

#include <fmt/format.h>
#include <array>
#include <cstdint>
#include <optional>

#include "iclforge/truehd/io/major_sync.hpp"
#include "json_sink.hpp"

namespace forge_cli::commands {

namespace {

namespace th = iclforge::truehd;

constexpr std::array<MeaningPresentation, 3> kPresentations{
    MeaningPresentation::k2ch, MeaningPresentation::k6ch, MeaningPresentation::k8ch};

constexpr const char* kQuestions = "libs/truehd/ERRATA.md, questions 51 to 55";

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

void print_channel_meaning_text(const th::ChannelMeaning& meaning, int substreams,
                                std::size_t distinct) {
    fmt::println("{:<16}provisional, no oracle until the DEE set ({})", "channel meaning",
                 kQuestions);
    for (const MeaningPresentation presentation : kPresentations) {
        fmt::println("  {:<14}{}{}", presentation_name(presentation),
                     describe_presentation(meaning, presentation),
                     stream_has_presentation(presentation, substreams)
                         ? ""
                         : " (the stream has no such substream)");
    }
    fmt::println("  {:<14}{}", "drc start-up", describe_start_up_gains(meaning));
    if (distinct > 1) {
        fmt::println(
            "  {:<14}the major syncs carry {} different channel meanings; the first is "
            "shown",
            "note", distinct);
    }
}

void write_channel_meaning_json(iclforge::apps::JsonSink& json, const th::ChannelMeaning& meaning,
                                int substreams, std::size_t distinct) {
    json.begin_object();
    json.member("provisional", true);
    json.member("question", kQuestions);
    json.member("distinct_in_stream", static_cast<std::uint64_t>(distinct));
    json.member("heavy_drc_start_up_gain",
                static_cast<std::int64_t>(meaning.heavy_drc_start_up_gain));
    json.member("drc_start_up_gain", static_cast<std::int64_t>(meaning.drc_start_up_gain));
    json.key("presentations");
    json.begin_object();
    for (const MeaningPresentation presentation : kPresentations) {
        const Fields fields = fields_of(meaning, presentation);
        json.key(presentation_name(presentation));
        json.begin_object();
        json.member("in_stream", stream_has_presentation(presentation, substreams));
        json.member("control_enabled", fields.control);
        json.member("dialogue_norm", static_cast<std::uint64_t>(fields.dialogue_norm));
        json.member("dialogue_norm_dbfs", static_cast<std::int64_t>(-fields.dialogue_norm));
        json.member("mix_level", static_cast<std::uint64_t>(fields.mix_level));
        if (fields.source_format) {
            json.member("source_format", static_cast<std::uint64_t>(*fields.source_format));
        }
        json.end_object();
    }
    json.end_object();
    json.end_object();
}

}  // namespace forge_cli::commands
