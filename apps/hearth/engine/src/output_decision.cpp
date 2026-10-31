#include "output_decision.hpp"

#include <algorithm>
#include <fmt/format.h>

// See output_decision.hpp for what this decides and why it is a pure
// function. The order of the rules below is the order they are applied, and
// each one is a case in apps/hearth/engine/tests/test_output_decision.cpp.

namespace iclforge::hearth {

namespace {

using audio::BitstreamFormat;

// Whether `endpoint` will carry `stream` as it is. Never AC-4: Hearth sends
// AC-4 as a bitstream only over the extension role, to sinks that decode it,
// and decodes it for every local output (planning/ac4.md, I2), no receiver
// having been found that takes it.
[[nodiscard]] bool carries_native(const EndpointFacts& endpoint,
                                   std::optional<BitstreamFormat> stream) {
    if (!stream) {
        return false;
    }
    switch (*stream) {
        case BitstreamFormat::kAc3:
            return endpoint.accepts_ac3;
        case BitstreamFormat::kEac3:
            return endpoint.accepts_eac3;
        case BitstreamFormat::kAc4:
        case BitstreamFormat::kAc4Hbr4:
        case BitstreamFormat::kAc4Hbr16:
        case BitstreamFormat::kMat:
            break;
    }
    return false;
}

// Whether an E-AC-3 stream could reach `endpoint` as AC-3 instead.
[[nodiscard]] bool carries_as_ac3(const EndpointFacts& endpoint,
                                   std::optional<BitstreamFormat> stream,
                                   const OutputRequest& request) {
    return stream == BitstreamFormat::kEac3 && endpoint.accepts_ac3 && request.follow_sink &&
           request.transcode_available;
}

// TrueHD and MLP, which this player decodes and never sends as they are or to a network sink
// (planning/truehd-parity-hearth-player.md, H5 and H6): the name to say, and empty for any other.
[[nodiscard]] std::string_view lossless_name(ItemCodec codec) {
    switch (codec) {
        case ItemCodec::kTruehd: return "TrueHD";
        case ItemCodec::kMlp: return "MLP";
        case ItemCodec::kUnknown:
        case ItemCodec::kAc3:
        case ItemCodec::kEac3:
        case ItemCodec::kAc4: break;
    }
    return {};
}

// Whether this request may be answered with TrueHD sent as MAT: the player can send it, and the
// item is TrueHD (MLP has no carriage).
[[nodiscard]] bool mat_wanted(const OutputRequest& request) {
    return request.mat_passthrough && request.codec == ItemCodec::kTruehd;
}

// What is known of sending MAT through `endpoint`'s platform, as a clause to follow "but": empty
// once a run has passed. Every platform is not run (mat_status.hpp), and the clause says what
// the code does instead.
[[nodiscard]] std::string mat_run_clause(const EndpointFacts& endpoint,
                                         const OutputRequest& request) {
    const MatPlatformStatus status = request.mat_status_of(endpoint.backend);
    if (status.run == MatRun::kRun) {
        return {};
    }
    return fmt::format(
        "TrueHD passthrough through {} has not been run against a receiver ({}), so whether the "
        "receiver names the stream TrueHD is not known.",
        audio::backend_name(endpoint.backend), status.basis);
}

// What a capability answer is worth saying about, and nothing when it needs
// no explanation. A probe or a descriptor answered the question; the other
// two did not, and which of them it was is gap 6's whole point.
[[nodiscard]] std::string unknown_note(const EndpointFacts& endpoint) {
    switch (endpoint.source) {
        case CapabilitySource::kNoDescriptor:
            return fmt::format(" \"{}\" reports no descriptor, so nothing is known about what it "
                               "accepts - a receiver that is off or on another input looks like "
                               "this.",
                               endpoint.name);
        case CapabilitySource::kNoReader:
            return " This platform cannot read a sink's descriptor at all, so only a probe of the "
                   "output answered - a refusal can be this machine's, such as another "
                   "application holding the output, rather than a limit of the sink.";
        case CapabilitySource::kDescriptor:
        case CapabilitySource::kProbe: return {};
    }
    return {};
}

// The endpoint a local decode should go to when nothing else has chosen one:
// the default, else the widest that says how wide it is, else the first.
[[nodiscard]] const EndpointFacts* best_for_pcm(std::span<const EndpointFacts> endpoints) {
    if (endpoints.empty()) {
        return nullptr;
    }
    for (const auto& endpoint : endpoints) {
        if (endpoint.is_default) {
            return &endpoint;
        }
    }
    const auto widest = std::max_element(endpoints.begin(), endpoints.end(),
                                         [](const EndpointFacts& a, const EndpointFacts& b) {
                                             return a.channels < b.channels;
                                         });
    return widest != endpoints.end() ? &*widest : &endpoints.front();
}

[[nodiscard]] const EndpointFacts* find_endpoint(std::span<const EndpointFacts> endpoints,
                                                  std::string_view id) {
    if (id.empty()) {
        return nullptr;
    }
    const auto found = std::find_if(endpoints.begin(), endpoints.end(),
                                    [id](const EndpointFacts& e) { return e.id == id; });
    return found != endpoints.end() ? &*found : nullptr;
}

// A local decode to `endpoint`, with whatever the caller should know about
// it: what a decode loses against bitstreaming, and how wide the device is.
[[nodiscard]] OutputChoice local_pcm(const EndpointFacts& endpoint, const OutputRequest& request,
                                      std::string_view because) {
    std::string reason = fmt::format("Decoding here and playing PCM to \"{}\"", endpoint.name);
    if (endpoint.channels != 0) {
        reason += fmt::format(" at its own {} channels", endpoint.channels);
    }
    reason += ".";
    if (!because.empty()) {
        reason += fmt::format(" {}", because);
    }
    std::optional<truehd::Presentation> presentation;
    if (const std::string_view lossless = lossless_name(request.codec); !lossless.empty()) {
        // Said whichever output it is, since none can be sent it as it is: the receiver that takes
        // MAT, if there is one, is not offered it. When MAT can be sent the caller has already
        // said why it was not (`because`), and "not available" would contradict it.
        if (!mat_wanted(request)) {
            reason +=
                fmt::format(" Passthrough of {} is not available, so it is decoded here.", lossless);
        } else {
            reason += fmt::format(" {} is decoded here.", lossless);
        }
        // The fallback chain: the presentation the output states, else stereo. Data for the
        // player to apply; the reason does not claim it is, because nothing applies it yet.
        presentation = truehd_fallback_presentation(endpoint);
    }
    if (request.has_objects) {
        reason +=
            " The object layer is rendered to those speakers rather than sent on, which is what "
            "a decode of a JOC stream means.";
    }
    return {.mode = OutputMode::kLocalPcm,
            .endpoint_id = endpoint.id,
            .endpoint_name = endpoint.name,
            .reason = std::move(reason),
            .presentation = presentation};
}

[[nodiscard]] OutputChoice bitstream(const EndpointFacts& endpoint, BitstreamFormat format,
                                      const OutputRequest& request, std::string_view because) {
    std::string reason = fmt::format("Bitstreaming {} to \"{}\" over IEC 61937, untouched.",
                                     audio::format_name(format), endpoint.name);
    if (!because.empty()) {
        reason += fmt::format(" {}", because);
    }
    if (endpoint.is_default) {
        reason +=
            " It is this machine's default output, so holding it exclusively silences whatever "
            "else is playing - and can be refused outright while another application holds it.";
    }
    if (request.has_objects && format == BitstreamFormat::kEac3) {
        reason += " The object layer travels with it, for the sink to render.";
    }
    return {.mode = OutputMode::kBitstream,
            .endpoint_id = endpoint.id,
            .endpoint_name = endpoint.name,
            .reason = std::move(reason),
            .stream = format};
}

// TrueHD to `endpoint` as MAT, with what is known of the platform's run said in the reason.
[[nodiscard]] OutputChoice mat_bitstream(const EndpointFacts& endpoint,
                                          const OutputRequest& request) {
    return bitstream(endpoint, BitstreamFormat::kMat, request, mat_run_clause(endpoint, request));
}

[[nodiscard]] OutputChoice transcoded(const EndpointFacts& endpoint, const OutputRequest& request) {
    std::string reason = fmt::format(
        "\"{}\" takes AC-3 but not E-AC-3, so this is transcoded to AC-3 and bitstreamed.",
        endpoint.name);
    if (request.has_objects) {
        reason += " The object layer cannot survive that and is rendered into the 5.1 bed.";
    }
    return {.mode = OutputMode::kBitstreamAsAc3,
            .endpoint_id = endpoint.id,
            .endpoint_name = endpoint.name,
            .reason = std::move(reason)};
}

[[nodiscard]] OutputChoice nothing(std::string reason) {
    return OutputChoice{.mode = OutputMode::kNone,
                        .endpoint_id = {},
                        .endpoint_name = {},
                        .reason = std::move(reason)};
}

// The automatic choice for one endpoint, in preference order: the stream as
// it is, then transcoded, then a local decode. `nullptr` when this endpoint
// can carry nothing at all.
[[nodiscard]] std::optional<OutputChoice> best_for(const EndpointFacts& endpoint,
                                                    const OutputRequest& request) {
    if (carries_native(endpoint, request.stream)) {
        return bitstream(endpoint, *request.stream, request, {});
    }
    if (carries_as_ac3(endpoint, request.stream, request)) {
        return transcoded(endpoint, request);
    }
    if (endpoint.accepts_pcm || endpoint.channels != 0) {
        return local_pcm(endpoint, request, {});
    }
    return std::nullopt;
}

}  // namespace

std::string_view describe(OutputMode mode) {
    switch (mode) {
        case OutputMode::kBitstream: return "bitstream";
        case OutputMode::kBitstreamAsAc3: return "bitstream as AC-3";
        case OutputMode::kLocalPcm: return "local PCM";
        case OutputMode::kNetworkGroup: return "network group";
        case OutputMode::kNone: return "none";
    }
    return "unknown output mode";
}

truehd::Presentation truehd_fallback_presentation(const EndpointFacts& endpoint) {
    if (endpoint.channels >= 8) {
        return truehd::Presentation::k8ch;
    }
    if (endpoint.channels >= 6) {
        return truehd::Presentation::k6ch;
    }
    // Narrower than 5.1, or an output that does not say: the stream's stereo mix.
    return truehd::Presentation::k2ch;
}

std::string_view describe(CapabilitySource source) {
    switch (source) {
        case CapabilitySource::kDescriptor: return "the sink's own descriptor";
        case CapabilitySource::kProbe: return "a live probe of the endpoint";
        case CapabilitySource::kNoDescriptor: return "no descriptor reported";
        case CapabilitySource::kNoReader: return "no descriptor reader on this platform";
    }
    return "unknown capability source";
}

OutputChoice choose_output(const OutputRequest& request) {
    // A group of network sinks is not an endpoint of this machine and does
    // not compete with one: it is chosen when the user has chosen it, and
    // its readiness is the only question. Taken first so that a group the
    // user selected is never quietly replaced by a local output.
    const bool wants_group = request.pinned == OutputMode::kNetworkGroup ||
                              (!request.group_name.empty() && !request.pinned.has_value());
    if (wants_group) {
        if (request.group_ready && !lossless_name(request.codec).empty()) {
            return OutputChoice{
                .mode = OutputMode::kNetworkGroup,
                .endpoint_id = {},
                .endpoint_name = {},
                .group_name = request.group_name,
                .reason = fmt::format(
                    "Playing to the group \"{}\". Sending {} to a network sink as it is is not "
                    "available, so it is decoded here and each sink is sent PCM.",
                    request.group_name, lossless_name(request.codec))};
        }
        if (request.group_ready) {
            return OutputChoice{
                .mode = OutputMode::kNetworkGroup,
                .endpoint_id = {},
                .endpoint_name = {},
                .group_name = request.group_name,
                .reason = fmt::format("Playing to the group \"{}\". Each sink decodes for "
                                      "itself, so what travels is the stream, not PCM.",
                                      request.group_name)};
        }
        if (request.group_name.empty()) {
            return nothing(
                "No group is selected, so there is nothing to play to. Pick a group, or an output "
                "of this machine.");
        }
        // Not ready: say so, then carry on to the local choice unless the
        // user asked to be refused instead.
        const std::string held = fmt::format(
            "The group \"{}\" is not ready - a sink is unpaired, or another server is holding it.",
            request.group_name);
        if (!request.follow_sink || request.pinned == OutputMode::kNetworkGroup) {
            return nothing(held);
        }
        if (const auto* fallback = best_for_pcm(request.endpoints); fallback != nullptr) {
            if (auto choice = best_for(*fallback, request)) {
                choice->reason = fmt::format("{} {}", held, choice->reason);
                return *choice;
            }
        }
        return nothing(held);
    }

    if (request.endpoints.empty()) {
        return nothing(
            "This machine reports no output at all. Nothing can be played until one appears, or a "
            "group of network sinks is selected.");
    }

    // An endpoint the user named is taken with the best mode it can carry -
    // their pinned mode first if it can, and the reason says what it got
    // instead if it cannot.
    const EndpointFacts* preferred = find_endpoint(request.endpoints, request.preferred_endpoint_id);
    const std::span<const EndpointFacts> pool =
        preferred != nullptr ? std::span<const EndpointFacts>{preferred, 1} : request.endpoints;

    if (request.pinned == OutputMode::kBitstream || request.pinned == OutputMode::kBitstreamAsAc3) {
        if (!request.stream && !lossless_name(request.codec).empty()) {
            // A TrueHD item the player can send as MAT goes to the first output that takes it,
            // the user having asked for a bitstream, run or not.
            if (mat_wanted(request)) {
                for (const auto& endpoint : pool) {
                    if (endpoint.accepts_mat) {
                        return mat_bitstream(endpoint, request);
                    }
                }
            }
            // Not that it carries nothing a bitstream output could take: passthrough of it is
            // not available. A decode takes its place unless follow=off asked for a refusal.
            const auto* fallback = preferred != nullptr ? preferred : best_for_pcm(request.endpoints);
            const std::string unavailable =
                mat_wanted(request)
                    ? fmt::format("No {} output takes TrueHD (MAT) over IEC 61937.{}",
                                  preferred != nullptr ? "chosen" : "available",
                                  preferred != nullptr ? unknown_note(*preferred) : std::string{})
                    : fmt::format("Passthrough of {} is not available.", lossless_name(request.codec));
            if (!request.follow_sink || fallback == nullptr) {
                return nothing(fmt::format(
                    "{}{}", unavailable,
                    request.follow_sink ? std::string{}
                                        : std::string{" follow=off, so this is a refusal rather "
                                                      "than a decode."}));
            }
            return local_pcm(*fallback, request, mat_wanted(request) ? unavailable : std::string{});
        }
        if (!request.stream) {
            return nothing(
                "This item carries nothing IEC 61937 can wrap - a bitstream output has no format "
                "to send. Decode it instead.");
        }
        const bool want_ac3 = request.pinned == OutputMode::kBitstreamAsAc3;
        // AC-3 already is AC-3. E-AC-3 needs the transcode, and AC-4, which
        // Hearth decodes for every output here (planning/ac4.md, I2), has none.
        const bool needs_transcode = want_ac3 && *request.stream != BitstreamFormat::kAc3;
        const bool transcodes =
            *request.stream == BitstreamFormat::kEac3 && request.transcode_available;
        bool takes_ac3 = false;
        for (const auto& endpoint : pool) {
            if (!want_ac3 && carries_native(endpoint, request.stream)) {
                return bitstream(endpoint, *request.stream, request, {});
            }
            takes_ac3 = takes_ac3 || endpoint.accepts_ac3;
            if (want_ac3 && endpoint.accepts_ac3 && (!needs_transcode || transcodes)) {
                return needs_transcode ? transcoded(endpoint, request)
                                       : bitstream(endpoint, BitstreamFormat::kAc3, request, {});
            }
        }
        const std::string note = preferred != nullptr ? unknown_note(*preferred) : std::string{};
        // An output that takes AC-3 was there; what was missing is the
        // transcode to reach it.
        const std::string why =
            needs_transcode && takes_ac3
                ? fmt::format("{} takes AC-3, but {} cannot be transcoded to AC-3 here.{}",
                              preferred != nullptr ? "The chosen output" : "An output",
                              audio::format_name(*request.stream), note)
                : fmt::format(
                      "No {} output takes {} over IEC 61937.{}",
                      preferred != nullptr ? "chosen" : "available",
                      want_ac3 ? std::string_view{"AC-3"} : audio::format_name(*request.stream),
                      note);
        if (!request.follow_sink) {
            return nothing(fmt::format("{} follow=off, so this is a refusal rather than a decode.",
                                       why));
        }
        const auto* fallback = preferred != nullptr ? preferred : best_for_pcm(request.endpoints);
        if (fallback != nullptr) {
            return local_pcm(*fallback, request, why);
        }
        return nothing(why);
    }

    if (request.pinned == OutputMode::kLocalPcm) {
        const auto* endpoint = preferred != nullptr ? preferred : best_for_pcm(request.endpoints);
        if (endpoint == nullptr) {
            return nothing("No output of this machine can be played to.");
        }
        return local_pcm(*endpoint, request, {});
    }

    // Automatic. The stream as it is beats a transcode, which beats a decode,
    // and an endpoint the user named beats one they did not.
    for (const auto& endpoint : pool) {
        if (carries_native(endpoint, request.stream)) {
            return bitstream(endpoint, *request.stream, request, {});
        }
    }
    for (const auto& endpoint : pool) {
        if (carries_as_ac3(endpoint, request.stream, request)) {
            return transcoded(endpoint, request);
        }
    }
    // TrueHD as MAT, on its own merits: an output that takes it, through a platform that has been
    // run. One that takes it through a platform that has not is said so, and left for the user to
    // pin (Bitstream), since a receiver that is sent a stream it cannot name plays nothing.
    std::string mat_because;
    if (mat_wanted(request)) {
        for (const auto& endpoint : pool) {
            if (!endpoint.accepts_mat) {
                continue;
            }
            if (request.mat_status_of(endpoint.backend).run == MatRun::kRun) {
                return mat_bitstream(endpoint, request);
            }
            if (mat_because.empty()) {
                mat_because = fmt::format(
                    "\"{}\" takes TrueHD (MAT) over IEC 61937, but {} Pin Bitstream to send it "
                    "anyway.",
                    endpoint.name, mat_run_clause(endpoint, request));
            }
        }
    }
    if (!request.follow_sink && !lossless_name(request.codec).empty()) {
        return nothing(fmt::format(
            "Passthrough of {} is not available, and follow=off refuses a decode in its place.{}{}",
            lossless_name(request.codec),
            preferred != nullptr ? unknown_note(*preferred) : std::string{},
            mat_because.empty() ? std::string{} : " " + mat_because));
    }
    if (!request.follow_sink && request.stream) {
        return nothing(fmt::format(
            "No output takes {} over IEC 61937, and follow=off refuses a decode in its place.{}",
            audio::format_name(*request.stream),
            preferred != nullptr ? unknown_note(*preferred) : std::string{}));
    }
    const auto* endpoint = preferred != nullptr ? preferred : best_for_pcm(request.endpoints);
    if (endpoint == nullptr) {
        return nothing("No output of this machine can be played to.");
    }
    std::string because = mat_because;
    if (request.stream) {
        because = fmt::format("No output takes {} over IEC 61937.{}",
                              audio::format_name(*request.stream), unknown_note(*endpoint));
    }
    return local_pcm(*endpoint, request, because);
}

}  // namespace iclforge::hearth
