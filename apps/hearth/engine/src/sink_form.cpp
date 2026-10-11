#include "sink_form.hpp"

#include <fmt/format.h>

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
    }
    return "AC-4";
}

// "48000 Hz", or "44100 or 48000 Hz".
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

}  // namespace

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
    }
    return sp::DataType::kAc4;
}

SinkChoice choose_sink_form(const StreamNeeds& stream, const SinkFacts& sink,
                            const SinkFormPolicy& policy) {
    const std::optional<std::string> refusal = coded_refusal(stream, sink);
    if (!refusal) {
        return {
            .form = SinkForm::kCoded,
            .layout = std::nullopt,
            .reason = fmt::format("Sending {} as it is: the sink decodes it for its own speakers.",
                                  audio::format_name(*stream.stream))};
    }

    // PCM, at the rate the stream has: this app does not resample, so a sink
    // that lists PCM at another rate only is a sink it has no PCM for.
    std::vector<std::size_t> channels;
    std::vector<std::int32_t> rates;
    for (const PcmFormat& format : sink.pcm_formats) {
        rates.push_back(static_cast<std::int32_t>(format.sample_rate));
        if (stream.sample_rate == 0 || format.sample_rate == stream.sample_rate) {
            channels.push_back(format.channels);
        }
    }
    const auto lists = [&](std::size_t count) {
        return std::ranges::find(channels, count) != channels.end();
    };

    if (sink.pcm_formats.empty()) {
        return {.form = SinkForm::kNone,
                .reason = fmt::format("{}, and it lists no PCM to be sent instead.", *refusal)};
    }
    if (!policy.pcm_fallback) {
        return {.form = SinkForm::kNone,
                .reason =
                    fmt::format("{}, and sending it PCM decoded here is switched off.", *refusal)};
    }
    if (channels.empty()) {
        std::ranges::sort(rates);
        rates.erase(std::unique(rates.begin(), rates.end()), rates.end());
        return {.form = SinkForm::kNone,
                .reason = fmt::format("{}, and its PCM is {} only, not {} Hz.", *refusal,
                                      rates_text(rates), stream.sample_rate)};
    }

    // A source with no coded form is PCM already, and is not decoded here.
    const std::string_view decoded = stream.stream ? "decoded here and " : "";
    const render::OutputLayout wanted = configured_layout(sink, stream);
    if (lists(wanted.slots())) {
        return {.form = SinkForm::kPcm,
                .layout = wanted,
                .reason = fmt::format("{}, so it is {}sent as PCM at {}.", *refusal, decoded,
                                      wanted.text())};
    }
    for (const Folded& target : kFoldTargets) {
        if (target.channels < wanted.slots() && lists(target.channels)) {
            const std::optional<render::OutputLayout> folded =
                render::OutputLayout::named(target.name);
            if (folded) {
                return {.form = SinkForm::kPcm,
                        .layout = folded,
                        .reason = fmt::format(
                            "{}, so it is {}folded to {} PCM, the widest its PCM formats take.",
                            *refusal, decoded, target.name)};
            }
        }
    }
    return {
        .form = SinkForm::kNone,
        .reason = fmt::format("{}, and none of its PCM formats takes the {} layout it is set to.",
                              *refusal, wanted.text())};
}

SinkFormPolicy form_policy(const SinkFacts& sink) {
    SinkFormPolicy policy;
    std::string hardware = sink.hardware;
    std::ranges::transform(hardware, hardware.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (hardware.find("esp32-c6") != std::string::npos) {
        policy.pcm_fallback = false;
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
    // The layouts the group renders: the player's first, then each variant in the order the
    // sinks asked for it.
    std::vector<render::OutputLayout> taken{master};
    for (const SinkFacts& sink : sinks) {
        MemberForm member;
        member.choice = choose_sink_form(needs, sink, form_policy(sink));
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
                member.label = fmt::format("PCM · {}", member.choice.layout->text());
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
