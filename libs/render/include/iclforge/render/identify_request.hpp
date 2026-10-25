#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "iclforge/render/identify_sequence.hpp"
#include "iclforge/render/speaker_trim.hpp"

// How a person asks a board for its identify tone (identify_sequence.hpp), and what the board answers: the words
// of POST /identify, the request a player is handed, why one is refused, and the JSON of GET /identify.
//
//   POST /identify   off | all | <speaker> | <speaker> loop
//
// "all" plays the speakers of the layout in force one after another; a speaker's name (L, Ls, LFE, S3, FL ...)
// plays that one, once; with "loop" it plays that one again and again until "off", or until the time limit every
// sequence has ends it (a minute). "off" stops whatever plays. The level is not a word: it is the sequence's own
// default, and nothing a request says lifts its cap. Standard library only, tested on the host
// (libs/render/tests/test_identify_request.cpp).

namespace iclforge::render {

struct IdentifyRequest {
    enum class Shape : std::uint8_t {
        kOff,
        kOnce,
        kLoop,
        kEach,
    };
    Shape shape = Shape::kOff;
    // The speaker for kOnce and kLoop.
    speaker_trim::RoleCode role = speaker_trim::kNoRole;
    double level_db = IdentifySequence::kDefaultLevelDb;
};

enum class IdentifyOutcome : std::uint8_t {
    kStarted,
    kStopped,
    // Refused: a stream is playing.
    kPlaying,
    // Refused: something else has the sink (a play from the control surface, flash mode), or the player is going.
    kBusy,
    // Refused: a server's identify tone is on.
    kServer,
    // Refused: the layout in force has no speaker with that role at an output.
    kNoSuchSpeaker,
    // Refused: this board has no player to play a tone through.
    kNoPlayer,
};

[[nodiscard]] constexpr bool identify_accepted(IdentifyOutcome outcome) {
    return outcome == IdentifyOutcome::kStarted || outcome == IdentifyOutcome::kStopped;
}

[[nodiscard]] constexpr const char* identify_outcome_text(IdentifyOutcome outcome) {
    switch (outcome) {
        case IdentifyOutcome::kStarted:
            return "ok; the tone is on its way";
        case IdentifyOutcome::kStopped:
            return "ok; stopped";
        case IdentifyOutcome::kPlaying:
            return "a stream is playing; the tone would play over it";
        case IdentifyOutcome::kBusy:
            return "something else has the speakers: a play from this page, or an update";
        case IdentifyOutcome::kServer:
            return "a server's identify tone is on";
        case IdentifyOutcome::kNoSuchSpeaker:
            return "the layout in force has no such speaker";
        case IdentifyOutcome::kNoPlayer:
            return "this board has no Sendspin player to play a tone through";
    }
    return "";
}

// The request a POST /identify body asks for, or nothing for words that are not one.
[[nodiscard]] inline std::optional<IdentifyRequest> parse_identify(std::string_view body) {
    body = speaker_trim::detail::trimmed(body);
    if (body.empty()) {
        return std::nullopt;
    }
    if (speaker_trim::detail::same_word(body, "off")) {
        return IdentifyRequest{};
    }
    if (speaker_trim::detail::same_word(body, "all")) {
        return IdentifyRequest{.shape = IdentifyRequest::Shape::kEach};
    }
    IdentifyRequest request{.shape = IdentifyRequest::Shape::kOnce};
    const std::size_t space = body.find_first_of(" \t,");
    std::string_view name = body.substr(0, space);
    if (space != std::string_view::npos) {
        const std::string_view rest = speaker_trim::detail::trimmed(body.substr(space + 1));
        if (!speaker_trim::detail::same_word(rest, "loop")) {
            return std::nullopt;
        }
        request.shape = IdentifyRequest::Shape::kLoop;
    }
    const std::optional<speaker_trim::RoleCode> role = speaker_trim::parse_role(name);
    if (!role) {
        return std::nullopt;
    }
    request.role = *role;
    return request;
}

// What GET /identify says, from plain values.
struct IdentifyReport {
    // A player to play through; why not, when there is none.
    bool available = false;
    const char* reason = "";
    bool active = false;
    // The output the tone is on now, or -1 between two speakers, and that output's speaker.
    int output = -1;
    speaker_trim::RoleCode role = speaker_trim::kNoRole;
    std::uint32_t remaining_ms = 0;
    // The speakers the layout in force has at an output, in output order.
    std::array<speaker_trim::RoleCode, speaker_trim::kOutputs> speakers{};
    std::size_t speaker_count = 0;
};

// {"available":true,"reason":"","active":false,"output":-1,"role":"","label":"","remaining_ms":0,
//  "level_db":-30.0,"max_level_db":-24.0,"tone_ms":1500,"gap_ms":500,"total_ms":60000,"speakers":["L","R"]}
[[nodiscard]] inline std::string identify_json(const IdentifyReport& report) {
    std::string out = "{\"available\":";
    out += report.available ? "true" : "false";
    out += ",\"reason\":";
    speaker_trim::detail::append_json_text(out, report.available ? "" : report.reason);
    out += ",\"active\":";
    out += report.active ? "true" : "false";
    out += ",\"output\":";
    out += std::to_string(report.output);
    out += ",\"role\":";
    speaker_trim::detail::append_json_text(out, speaker_trim::role_name(report.role));
    out += ",\"label\":";
    speaker_trim::detail::append_json_text(out, speaker_trim::role_label(report.role));
    out += ",\"remaining_ms\":";
    out += std::to_string(report.remaining_ms);
    out += ",\"level_db\":";
    speaker_trim::detail::append_fixed(out, IdentifySequence::kDefaultLevelDb, 1);
    out += ",\"max_level_db\":";
    speaker_trim::detail::append_fixed(out, IdentifySequence::kMaxLevelDb, 1);
    out += ",\"tone_ms\":";
    out += std::to_string(IdentifySequence::kToneMs);
    out += ",\"gap_ms\":";
    out += std::to_string(IdentifySequence::kGapMs);
    out += ",\"total_ms\":";
    out += std::to_string(IdentifySequence::kMaxTotalMs);
    out += ",\"speakers\":[";
    for (std::size_t i = 0; i < report.speaker_count && i < report.speakers.size(); ++i) {
        if (i != 0) {
            out += ',';
        }
        speaker_trim::detail::append_json_text(out, speaker_trim::role_name(report.speakers[i]));
    }
    out += "]}";
    return out;
}

}  // namespace iclforge::render
