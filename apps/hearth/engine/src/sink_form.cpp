#include "sink_form.hpp"

#include "iclforge/sendspin/chunks.hpp"

#include <fmt/format.h>

#include "iclforge/ac3/decoder/output.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>
#include <utility>
#include <vector>

// See sink_form.hpp for what this decides and why it is a pure function. The
// order of the rules below is the order they are applied, and each one is a
// case in apps/hearth/engine/tests/test_sink_form.cpp.

namespace iclforge::hearth {

namespace {

namespace sp = sendspin::player;

[[nodiscard]] std::string_view type_text(sp::DataType type) {
    switch (type) {
        case sp::DataType::kAc3:
            return "AC-3";
        case sp::DataType::kEac3:
            return "E-AC-3";
        case sp::DataType::kAc4:
            break;
        case sp::DataType::kTruehd:
            return "TrueHD";
    }
    return "AC-4";
}

// "48000 Hz", or "44100 or 48000 Hz".
// The rate among `listed` to convert audio at `from` Hz to: one of its own family (the 44.1 kHz
// multiples or the 48 kHz ones, so that 88.2 kHz goes to 44.1 and 96 kHz to 48), the highest of
// those not above `from`, else the lowest above it; where the sink lists none of the family, the
// same choice among every rate it lists.
[[nodiscard]] std::uint32_t nearest_rate(std::uint32_t from, const std::vector<std::int32_t>& listed) {
    const auto family_of = [](std::uint32_t rate) { return rate % 44100U == 0 ? 44100U : 48000U; };
    const auto pick = [&](bool same_family) -> std::uint32_t {
        std::uint32_t below = 0;
        std::uint32_t above = 0;
        for (const std::int32_t entry : listed) {
            const auto rate = static_cast<std::uint32_t>(entry);
            if (rate == 0 || (same_family && family_of(rate) != family_of(from))) {
                continue;
            }
            if (rate <= from) {
                below = std::max(below, rate);
            } else {
                above = above == 0 ? rate : std::min(above, rate);
            }
        }
        return below != 0 ? below : above;
    };
    const std::uint32_t same = pick(true);
    return same != 0 ? same : pick(false);
}

[[nodiscard]] std::string rates_text(const std::vector<std::int32_t>& rates) {
    std::string text;
    for (const std::int32_t rate : rates) {
        if (!text.empty()) {
            text += " or ";
        }
        text += std::to_string(rate);
    }
    return text + " Hz";
}

// "25", "29.97": a frame rate to two places with its trailing zeros dropped.
[[nodiscard]] std::string rate_text(double frames_per_second) {
    std::string text = fmt::format("{:.2f}", frames_per_second);
    while (text.back() == '0') {
        text.pop_back();
    }
    if (text.back() == '.') {
        text.pop_back();
    }
    return text;
}

// A stream's frame rate is read from its sample counts and lasts to a few parts in a thousand
// (29.97 as 29.9700 or 29.9712 by where the programme's frames are counted), and a sink's limit
// is a whole number a rate may sit on: 25 takes 25.
constexpr double kFrameRateSlack = 1.005;

// Why the stream cannot go to the sink as it is, or nothing when it can.
// Every sentence names the sink's own limit, because that is what a person
// holding the phone can act on.
[[nodiscard]] std::optional<std::string> coded_refusal(const StreamNeeds& stream,
                                                       const SinkFacts& sink) {
    if (!stream.stream) {
        return std::string("The source has no coded form to send");
    }
    const sp::DataType type = data_type_of(*stream.stream);
    if (!sink.iclforge_support) {
        return std::string("This sink takes PCM, not coded streams");
    }
    const sp::Support& support = *sink.iclforge_support;
    if (sink.pair_state != PairState::kPaired) {
        return std::string("This sink is not paired, and takes a coded stream only once it is");
    }
    if (std::ranges::find(support.data_types, type) == support.data_types.end()) {
        return fmt::format("This sink does not decode {}", type_text(type));
    }
    if (stream.sample_rate != 0 &&
        std::ranges::find(support.sample_rates, static_cast<std::int32_t>(stream.sample_rate)) ==
            support.sample_rates.end()) {
        return fmt::format("This sink plays coded streams at {} only, not {} Hz",
                           rates_text(support.sample_rates), stream.sample_rate);
    }
    const std::uint8_t limit = support.max_coded_channels_of(type);
    if (limit != 0 && stream.coded_channels > limit) {
        return fmt::format("This sink decodes {} up to {} channels, and this stream has {}",
                           type_text(type), limit, stream.coded_channels);
    }
    const std::uint16_t rate_limit = support.max_frame_rate_of(type);
    if (rate_limit != 0 && stream.frame_rate > static_cast<double>(rate_limit) * kFrameRateSlack) {
        return fmt::format("This sink decodes {} up to {} frames a second, and this stream has {}",
                           type_text(type), rate_limit, rate_text(stream.frame_rate));
    }
    if (type == sp::DataType::kTruehd && stream.peak_kbps != 0) {
        const std::uint32_t data_limit = support.max_data_rate_of(type);
        if (data_limit != 0 && stream.peak_kbps > data_limit) {
            return fmt::format("This sink takes {} up to {} kbit/s, and this stream's peak is {}",
                               type_text(type), data_limit, stream.peak_kbps);
        }
        // The longest chunk: 24 units, 20 ms, at the stream's peak, with the chunk header.
        const std::uint64_t chunk_bytes =
            (static_cast<std::uint64_t>(stream.peak_kbps) * 1000U / 8U / 50U) +
            sendspin::kBurstChunkHeaderBytes;
        if (support.max_chunk_bytes != 0 && chunk_bytes > support.max_chunk_bytes) {
            return fmt::format("This sink takes chunks of up to {} bytes, and this stream's peak makes {}",
                               support.max_chunk_bytes, chunk_bytes);
        }
    }
    return std::nullopt;
}

// The wire's span of a part's level is the library's: one conversion on every side.
static_assert(sp::kKaraokeLevelMinDb == ac3::kKaraokeMinLevelDb &&
                  sp::kKaraokeLevelMaxDb == ac3::kKaraokeMaxLevelDb,
              "the Sendspin extension's karaoke levels and libs/ac3's span must be the same");

// Whether the sink lists `key` among the decoder settings it takes.
[[nodiscard]] bool takes_setting(const sp::Support& support, std::string_view key) {
    return std::ranges::find(support.decoder_settings, key) != support.decoder_settings.end();
}

// "AC-3", or "AC-3 and E-AC-3": the codecs a sink says it reproduces karaoke for.
[[nodiscard]] std::string karaoke_codecs_text(const sp::Support& support) {
    std::string text;
    for (const sp::DataType type :
         {sp::DataType::kAc3, sp::DataType::kEac3, sp::DataType::kAc4}) {
        if (support.reproduces_karaoke(type)) {
            text += text.empty() ? "" : " and ";
            text += type_text(type);
        }
    }
    return text.empty() ? std::string("no codec") : text;
}

// Why a karaoke stream whose reproduction is wanted is not to be left to this sink's own decoder:
// it has not been told how, so it would play the coded channels, and a group would play two
// things. Nothing for a stream that is not to be reproduced, and for a sink that takes the
// settings (the settings command is among the commands it supports, which is how a server
// learns it can send one) and reproduces karaoke for the stream's codec (`karaoke_codecs`,
// which a sink from before the key leaves out and so reproduces AC-3's alone).
struct KaraokeRefusal {
    std::string why;
    // The sink reproduces the karaoke all the same and only misses a level, so a stream that is
    // sent to it as it is plays at the table's levels, not as coded.
    bool reproduces = false;
};

[[nodiscard]] std::optional<KaraokeRefusal> karaoke_refusal(const StreamNeeds& stream,
                                                            const SinkFacts& sink) {
    if (!stream.karaoke || !sink.iclforge_support) {
        return std::nullopt;
    }
    const sp::Support& support = *sink.iclforge_support;
    if (!takes_setting(support, "karaoke")) {
        return KaraokeRefusal{
            .why = "This sink does not reproduce karaoke: it does not take the setting"};
    }
    // A sink that takes the setting has a path for the codecs it lists, and plays any other
    // stream as coded: an E-AC-3 karaoke stream sent to a board whose path is AC-3's would be
    // heard as coded beside a member that reproduces it (planning/karaoke-eac3.md, D6).
    if (stream.stream && !support.reproduces_karaoke(data_type_of(*stream.stream))) {
        return KaraokeRefusal{
            .why = fmt::format("This sink reproduces karaoke for {}, not for {}",
                               karaoke_codecs_text(support),
                               type_text(data_type_of(*stream.stream)))};
    }
    if (stream.karaoke->reproduction == sp::Karaoke::kCapable &&
        !takes_setting(support, "karaoke_vocals")) {
        return KaraokeRefusal{
            .why = "This sink does not choose which vocals to play: it does not take "
                   "karaoke_vocals"};
    }
    const bool settings_command =
        sink.iclforge_state &&
        std::ranges::find(sink.iclforge_state->supported_commands, sp::Command::kSettings) !=
            sink.iclforge_state->supported_commands.end();
    if (!settings_command) {
        return KaraokeRefusal{
            .why = "This sink takes no settings from this app, so it cannot be told"};
    }
    // A level that moves a part (planning/karaoke-phase2.md, W10) is for the sink to apply, or
    // for this app to apply in the PCM it sends: a sink that would play the table instead of it
    // would put a group's members at different levels.
    constexpr std::array<std::string_view, 3> kLevelKeys{"karaoke_melody_db", "karaoke_vocal1_db",
                                                         "karaoke_vocal2_db"};
    constexpr std::array<std::string_view, 3> kLevelParts{"melody", "vocal 1", "vocal 2"};
    const std::array<double, 3> levels = stream.karaoke->heard_levels_db();
    for (std::size_t part = 0; part < levels.size(); ++part) {
        if (levels[part] != 0.0 && !takes_setting(support, kLevelKeys[part])) {
            return KaraokeRefusal{
                .why = fmt::format("This sink does not set the level of the {}: it does not take {}",
                                   kLevelParts[part], kLevelKeys[part]),
                .reproduces = true};
        }
    }
    return std::nullopt;
}

// The layout a sink is configured to, as the sink itself says before this
// app's own record of what it last sent it, then the player's own for a sink
// that has said none (a standard player has no layout at all), and 2.0
// failing that.
[[nodiscard]] render::OutputLayout configured_layout(const SinkFacts& sink,
                                                     const StreamNeeds& needs) {
    const auto parsed =
        [](const std::optional<std::string>& text) -> std::optional<render::OutputLayout> {
        return text ? render::OutputLayout::parse(*text) : std::nullopt;
    };
    if (sink.iclforge_state) {
        if (const auto layout = parsed(sink.iclforge_state->layout)) {
            return *layout;
        }
    }
    if (sink.intended_settings) {
        if (const auto layout = parsed(sink.intended_settings->layout)) {
            return *layout;
        }
    }
    return needs.default_layout.value_or(render::OutputLayout::stereo());
}

struct Folded {
    std::string_view name;
    std::size_t channels;
};

// The layouts a fold may land on, widest first: the ones a board's PCM formats
// name by their channel counts. Every one is a name OutputLayout knows.
constexpr std::array<Folded, 3> kFoldTargets{{{"7.1", 8}, {"5.1", 6}, {"2.0", 2}}};

// Why a sink that would place this stream's objects is not to be sent it as it is: it states
// fewer objects than the stream has, and would place them. Nothing for a stream with none, for a
// sink that states no limit for the type, and for one whose settings or layout leave the objects
// out (it plays the bed, which is what the stream is to it). A stream of objects alone (an AC-4
// presentation with no channels) has no bed, so the sink's settings and layout do not matter:
// past its limit it plays nothing.
[[nodiscard]] std::optional<std::string> object_refusal(const StreamNeeds& stream,
                                                        const SinkFacts& sink) {
    if (!stream.stream || stream.objects == 0 || !sink.iclforge_support) {
        return std::nullopt;
    }
    const sp::DataType type = data_type_of(*stream.stream);
    const std::optional<std::uint8_t> limit = sink.iclforge_support->max_objects_of(type);
    if (!limit || stream.objects <= *limit) {
        return std::nullopt;
    }
    bool places = stream.objects_alone;
    const std::optional<sp::ObjectsPolicy> policy =
        sink.intended_settings ? sink.intended_settings->decoder.objects : std::nullopt;
    if (stream.objects_alone) {
        // Placed or not, there is nothing else to play.
    } else if (policy == sp::ObjectsPolicy::kAlways) {
        places = true;
    } else if (policy != sp::ObjectsPolicy::kNever) {
        places = configured_layout(sink, stream).has_height();
    }
    if (!places) {
        return std::nullopt;
    }
    if (*limit == 0) {
        return fmt::format("This sink does not place objects, and this stream has {}",
                           stream.objects);
    }
    return fmt::format("This sink places up to {} objects, and this stream has {}", *limit,
                       stream.objects);
}

// PCM for a sink that is not to be sent the stream as it is, for `refusal`'s reason: at the rate
// the stream has, at the sink's layout and, where `may_fold`, folded where its formats do not take
// that; or nothing, and why. A refusal that the sink's own decode would answer with its bed (the
// objects it does not place) is not worth a fold: a 7.1.4 sink sent 2.0 PCM for the sake of a
// few objects loses its speakers, which is worse than the bed alone.
[[nodiscard]] SinkChoice pcm_choice(const std::string& refusal, const StreamNeeds& stream,
                                    const SinkFacts& sink, const SinkFormPolicy& policy,
                                    bool may_fold = true) {
    // Opus where the person set the sink to it and it lists it: stereo at 48 kHz, which is all
    // Opus is listed at, so a stream of another rate goes the PCM way below.
    constexpr std::uint32_t kOpusRate = 48000;
    // Opus is 48 kHz. A stream at another rate is converted to it only where the sink lists no PCM
    // at the stream's own, so that a rate the sink takes as PCM is never converted to Opus for it.
    const bool lists_pcm_at_stream_rate = std::ranges::any_of(
        sink.pcm_formats, [&](const PcmFormat& format) { return format.sample_rate == stream.sample_rate; });
    const bool opus_rate_ok =
        (policy.pcm_rate == 0 || policy.pcm_rate == kOpusRate) &&
        (stream.sample_rate == 0 || stream.sample_rate == kOpusRate ||
         (policy.convert_rate && !lists_pcm_at_stream_rate));
    if (policy.pcm_fallback && policy.opus && sink_lists_opus(sink) &&
        (may_fold || configured_layout(sink, stream).slots() <= 2) && opus_rate_ok) {
        if (const std::optional<render::OutputLayout> stereo = render::OutputLayout::named("2.0")) {
            const bool converted = stream.sample_rate != 0 && stream.sample_rate != kOpusRate;
            return {.form = SinkForm::kPcm,
                    .layout = *stereo,
                    .codec = PcmCodec::kOpus,
                    .sample_rate = converted ? kOpusRate : 0,
                    .reason = fmt::format(
                        "{}, so it is {}sent as Opus at 2.0, as this sink is set to: lossy, and a "
                        "twelfth of PCM's bytes.{}",
                        refusal, stream.stream ? "decoded here and " : "",
                        converted ? fmt::format(" Opus is 48 kHz, so it is converted here from {} Hz.",
                                                stream.sample_rate)
                                  : std::string{})};
        }
    }

    // PCM, at the rate the stream has, or the one the group has settled on; failing that, at the
    // rate the sink lists nearest to the stream's, converted here (dsp::StreamResampler).
    std::uint32_t rate = policy.pcm_rate != 0 ? policy.pcm_rate : stream.sample_rate;
    std::vector<std::size_t> channels;
    std::vector<std::int32_t> rates;
    const auto channels_at = [&](std::uint32_t wanted) {
        std::vector<std::size_t> at;
        for (const PcmFormat& format : sink.pcm_formats) {
            if (wanted == 0 || format.sample_rate == wanted) {
                at.push_back(format.channels);
            }
        }
        return at;
    };
    for (const PcmFormat& format : sink.pcm_formats) {
        rates.push_back(static_cast<std::int32_t>(format.sample_rate));
    }
    channels = channels_at(rate);
    if (channels.empty() && policy.pcm_rate == 0 && policy.convert_rate && policy.pcm_fallback &&
        stream.sample_rate != 0 && !sink.pcm_formats.empty()) {
        rate = nearest_rate(stream.sample_rate, rates);
        channels = channels_at(rate);
    }
    const bool converted = stream.sample_rate != 0 && rate != stream.sample_rate;
    const std::uint32_t sent_rate = converted ? rate : 0;
    const std::string conversion =
        converted ? fmt::format(" It is converted here from {} Hz to {} Hz.", stream.sample_rate, rate)
                  : std::string{};
    const auto lists = [&](std::size_t count) {
        return std::ranges::find(channels, count) != channels.end();
    };

    if (sink.pcm_formats.empty()) {
        return {.form = SinkForm::kNone,
                .reason = fmt::format("{}, and it lists no PCM to be sent instead.", refusal)};
    }
    if (!policy.pcm_fallback) {
        return {
            .form = SinkForm::kNone,
            .reason = fmt::format("{}, and sending it PCM decoded here is switched off.", refusal)};
    }
    if (channels.empty()) {
        std::ranges::sort(rates);
        rates.erase(std::unique(rates.begin(), rates.end()), rates.end());
        return {.form = SinkForm::kNone,
                .reason = fmt::format("{}, and its PCM is {} only, not {} Hz.", refusal,
                                      rates_text(rates), rate)};
    }

    // A source with no coded form is PCM already, and is not decoded here.
    const std::string_view decoded = stream.stream ? "decoded here and " : "";
    const render::OutputLayout wanted = configured_layout(sink, stream);
    const std::string caveat = policy.pcm_caveat.empty() ? "" : " " + policy.pcm_caveat;
    if (lists(wanted.slots())) {
        return {.form = SinkForm::kPcm,
                .layout = wanted,
                .sample_rate = sent_rate,
                .reason = fmt::format("{}, so it is {}sent as PCM at {}.{}{}", refusal, decoded,
                                      wanted.text(), conversion, caveat)};
    }
    for (const Folded& target : kFoldTargets) {
        if (may_fold && target.channels < wanted.slots() && lists(target.channels)) {
            const std::optional<render::OutputLayout> folded =
                render::OutputLayout::named(target.name);
            if (folded) {
                return {.form = SinkForm::kPcm,
                        .layout = folded,
                        .sample_rate = sent_rate,
                        .reason = fmt::format(
                            "{}, so it is {}folded to {} PCM, the widest its PCM formats take.{}{}",
                            refusal, decoded, target.name, conversion, caveat)};
            }
        }
    }
    return {
        .form = SinkForm::kNone,
        .reason = fmt::format("{}, and none of its PCM formats takes the {} layout it is set to.",
                              refusal, wanted.text())};
}

}  // namespace

std::array<double, 3> KaraokeChoice::heard_levels_db() const {
    if (reproduction != sp::Karaoke::kCapable) {
        return {0.0, 0.0, 0.0};
    }
    const bool vocal1 = vocals == sp::KaraokeVocals::kV1 || vocals == sp::KaraokeVocals::kBoth;
    const bool vocal2 = vocals == sp::KaraokeVocals::kV2 || vocals == sp::KaraokeVocals::kBoth;
    return {ac3::karaoke_level_db(melody_db), vocal1 ? ac3::karaoke_level_db(vocal1_db) : 0.0,
            vocal2 ? ac3::karaoke_level_db(vocal2_db) : 0.0};
}

bool sink_lists_opus(const SinkFacts& sink) {
    return std::ranges::find(sink.codecs, "opus") != sink.codecs.end();
}

sp::DataType data_type_of(audio::BitstreamFormat format) {
    switch (format) {
        case audio::BitstreamFormat::kAc3:
            return sp::DataType::kAc3;
        case audio::BitstreamFormat::kEac3:
            return sp::DataType::kEac3;
        case audio::BitstreamFormat::kAc4:
        case audio::BitstreamFormat::kAc4Hbr4:
        case audio::BitstreamFormat::kAc4Hbr16:
            break;
        case audio::BitstreamFormat::kMat:
            // TrueHD in a group is the access units, not the MAT burst (sendspin::kTruehdUnitsPerChunk).
            return sp::DataType::kTruehd;
    }
    return sp::DataType::kAc4;
}

SinkChoice choose_sink_form(const StreamNeeds& stream, const SinkFacts& sink,
                            const SinkFormPolicy& policy) {
    if (const std::optional<std::string> refusal = coded_refusal(stream, sink)) {
        return pcm_choice(*refusal, stream, sink, policy);
    }
    const std::string coded =
        fmt::format("Sending {} as it is", stream.stream == audio::BitstreamFormat::kMat
                                               ? std::string_view{"TrueHD"}
                                               : audio::format_name(*stream.stream));
    // A karaoke stream is sent as it is only to a sink that reproduces it: decoded here for one
    // that does not, so that a group hears one reproduction. Where this app cannot send it PCM,
    // the stream still plays, as coded, and the reason says so.
    if (const std::optional<KaraokeRefusal> karaoke = karaoke_refusal(stream, sink)) {
        const SinkChoice pcm = pcm_choice(karaoke->why, stream, sink, policy);
        if (pcm.form == SinkForm::kPcm) {
            return pcm;
        }
        return {.form = SinkForm::kCoded,
                .layout = std::nullopt,
                .reason = fmt::format("{}. {}, so the karaoke is played {}.", coded, karaoke->why,
                                      karaoke->reproduces ? "at the table's levels" : "as coded")};
    }
    const std::optional<std::string> objects = object_refusal(stream, sink);
    if (!objects) {
        const std::string_view reproduced =
            stream.karaoke ? " and reproduces the karaoke as asked" : "";
        return {.form = SinkForm::kCoded,
                .layout = std::nullopt,
                .reason = fmt::format("{}: the sink decodes it for its own speakers{}.", coded,
                                      reproduced)};
    }
    // The sink decodes the stream but would play its bed alone, past the objects it places.
    // Decoded here, the objects are placed; where that is not possible it plays the bed. A stream
    // of objects alone has no bed, so there a fold to a width the sink takes is worth having
    // (the other way, it plays nothing), and where PCM is not possible the sink is sent nothing,
    // with the reason, and not a stream it would play silence of.
    SinkChoice pcm = pcm_choice(*objects, stream, sink, policy, /*may_fold=*/stream.objects_alone);
    if (pcm.form == SinkForm::kPcm || stream.objects_alone) {
        return pcm;
    }
    return {.form = SinkForm::kCoded,
            .layout = std::nullopt,
            .reason = fmt::format("{}. {}, so it plays the bed without them.", coded, *objects)};
}

SinkFormPolicy form_policy(const SinkFacts& sink) {
    SinkFormPolicy policy;
    std::string hardware = sink.hardware;
    std::ranges::transform(hardware, hardware.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    policy.opus = sink.opus_fallback && sink_lists_opus(sink);
    if (hardware.find("esp32-c6") != std::string::npos) {
        policy.pcm_caveat =
            "This board holds under a fifth of a second of PCM, so on a busy WiFi the stream can "
            "drop out.";
    }
    return policy;
}

GroupFormPlan plan_group_forms(const StreamNeeds& stream, const render::OutputLayout& master,
                               std::span<const SinkFacts> sinks) {
    StreamNeeds needs = stream;
    if (!needs.default_layout) {
        needs.default_layout = master;
    }
    GroupFormPlan plan;
    // PCM goes to the group at one rate. Each sink that cannot take the stream's is first given
    // the rate it lists nearest (choose_sink_form()); when they differ, the rate most of the sinks
    // sent PCM can take is the group's, and a sink that does not list it is held back with the
    // reason. A group that also carries the stream as it is has one timeline, in the stream's
    // frames, so PCM at another rate cannot go with it.
    std::vector<SinkChoice> first;
    first.reserve(sinks.size());
    for (const SinkFacts& sink : sinks) {
        first.push_back(choose_sink_form(needs, sink, form_policy(sink)));
    }
    const std::uint32_t stream_rate = needs.sample_rate;
    const auto rate_of = [&](const SinkChoice& choice) {
        return choice.sample_rate != 0 ? choice.sample_rate : stream_rate;
    };
    std::uint32_t group_rate = stream_rate;
    {
        std::vector<std::pair<std::uint32_t, int>> counts;
        for (const SinkChoice& choice : first) {
            if (choice.form != SinkForm::kPcm) {
                continue;
            }
            const auto found = std::ranges::find(counts, rate_of(choice), &std::pair<std::uint32_t, int>::first);
            if (found == counts.end()) {
                counts.emplace_back(rate_of(choice), 1);
            } else {
                ++found->second;
            }
        }
        // The most members; on a tie the stream's own rate, then the higher.
        int best = 0;
        for (const auto& [rate, count] : counts) {
            if (count > best) {
                best = count;
                group_rate = rate;
            } else if (count == best && rate != group_rate &&
                       (rate == stream_rate || (group_rate != stream_rate && rate > group_rate))) {
                group_rate = rate;
            }
        }
    }
    const bool any_coded = std::ranges::any_of(
        first, [](const SinkChoice& choice) { return choice.form == SinkForm::kCoded; });
    const bool converting = group_rate != stream_rate;
    // With a member sent the stream as it is there is one timeline, in the stream's frames, and no
    // PCM at another rate goes with it (below).
    plan.pcm_rate = converting && !any_coded ? group_rate : 0;
    // The layouts the group renders: the player's first, then each variant in the order the
    // sinks asked for it.
    std::vector<render::OutputLayout> taken{master};
    for (std::size_t index = 0; index < sinks.size(); ++index) {
        const SinkFacts& sink = sinks[index];
        MemberForm member;
        member.choice = first[index];
        if (member.choice.form == SinkForm::kPcm && rate_of(member.choice) != group_rate) {
            SinkFormPolicy policy = form_policy(sink);
            policy.pcm_rate = group_rate;
            policy.convert_rate = false;
            member.choice = choose_sink_form(needs, sink, policy);
            if (member.choice.form == SinkForm::kNone) {
                member.choice.reason += fmt::format(
                    " The group is sent PCM at {} Hz, which most of its sinks take.", group_rate);
            }
        }
        if (member.choice.form == SinkForm::kPcm && converting && any_coded) {
            member.choice.form = SinkForm::kNone;
            member.choice.layout.reset();
            member.choice.reason += fmt::format(
                " The group also carries the stream as it is, on its own timeline at {} Hz, so PCM "
                "at {} Hz cannot go with it and this sink is held back.",
                stream_rate, group_rate);
        }
        switch (member.choice.form) {
            case SinkForm::kCoded:
                member.action = MemberAction::kCoded;
                break;
            case SinkForm::kNone:
                member.action = MemberAction::kHold;
                break;
            case SinkForm::kPcm: {
                const render::OutputLayout layout = *member.choice.layout;
                member.channels = static_cast<std::int32_t>(layout.slots());
                member.codec = member.choice.codec;
                member.sample_rate = member.choice.sample_rate;
                const auto same =
                    std::ranges::find_if(taken, [&](const render::OutputLayout& other) {
                        return other.slots() == layout.slots();
                    });
                if (same == taken.end()) {
                    taken.push_back(layout);
                    plan.variants.push_back(layout);
                    member.action = MemberAction::kPcm;
                } else if (same->text() == layout.text()) {
                    member.action = MemberAction::kPcm;
                } else {
                    member.action = MemberAction::kHold;
                    member.channels = 0;
                    member.choice.form = SinkForm::kNone;
                    member.choice.layout.reset();
                    member.choice.reason += fmt::format(
                        " The group is already sent {} at that width, and sends one layout of "
                        "each, "
                        "so this sink is held back.",
                        same->text());
                }
                break;
            }
        }
        switch (member.action) {
            case MemberAction::kCoded:
                member.label = fmt::format("{} as it is",
                                           stream.stream ? type_text(data_type_of(*stream.stream))
                                                         : std::string_view{"The stream"});
                break;
            case MemberAction::kPcm:
                member.label = fmt::format("{} · {}", member.codec == PcmCodec::kOpus ? "Opus" : "PCM",
                                           member.choice.layout->text());
                if (member.sample_rate != 0) {
                    member.label += fmt::format(" · {} Hz", member.sample_rate);
                }
                break;
            case MemberAction::kHold:
                member.label = "Nothing · held back";
                break;
        }
        plan.members.push_back(std::move(member));
    }
    return plan;
}

}  // namespace iclforge::hearth
