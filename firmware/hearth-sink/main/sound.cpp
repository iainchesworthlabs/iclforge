// The board's sound preferences. See sound.hpp.

#include "sound.hpp"
#include "sound_routes.hpp"

#include <array>
#include <cstdio>
#include <string>
#include <utility>

#include "iclforge/render/identify_request.hpp"
#include "iclforge/render/speaker_trim.hpp"

#include "sendspin.hpp"
#include "settings.hpp"

namespace player {
namespace {

namespace st = iclforge::render::speaker_trim;
namespace render = iclforge::render;

// A sentence for a person looking at the glass: the routes' reasons are lower-case clauses.
[[nodiscard]] std::string sentence(std::string text) {
    if (!text.empty() && text.front() >= 'a' && text.front() <= 'z') {
        text.front() = static_cast<char>(text.front() - 'a' + 'A');
    }
    if (!text.empty() && text.back() != '.') {
        text += '.';
    }
    return text;
}

}  // namespace

// GET /trim: the listener's level and distance for each speaker the layout in force has at an output, with what is
// really in force there (a server's settings can differ) and what this build can do (iclforge/render/
// speaker_trim.hpp). Nothing on a board with no player.
std::optional<std::string> sound_trim_json() {
    const SoundCaps caps = sendspin_sound_caps();
    if (!caps.player) {
        return std::nullopt;
    }
    const std::optional<SpeakerTrimStatus> status = sendspin_speaker_trim_status();
    const st::SpeakerTrimTable& table = settings().speaker_trim;
    st::TrimReport report;
    report.caps = caps.trim;
    report.sample_rate_hz = kSinkSampleRate;
    if (status) {
        report.layout = status->layout.data();
        report.server_level = status->server_level;
        report.server_delay = status->server_delay;
        report.farthest_m = static_cast<double>(status->farthest_cm) / 100.0;
        for (std::size_t i = 0; i < status->count; ++i) {
            const SpeakerTrimStatus::Row& row = status->rows[i];
            const st::SpeakerTrim own = table.get(row.role);
            report.rows.push_back(st::TrimRow{.role = row.role,
                                              .output = row.output,
                                              .level_db = own.level_db(),
                                              .distance_m = own.distance_m(),
                                              .applied_level_db = static_cast<double>(row.applied_level_db),
                                              .applied_delay_samples = row.applied_delay_samples,
                                              .delay_clamped = row.delay_clamped});
        }
    }
    return st::trim_json(report);
}

// PUT /trim: statements that change the table (SpeakerTrimTable::apply_text), taken whole or not at all, stored,
// and handed to the player, which puts them in force at its next chunk.
std::optional<iclforge::ControlRefusal> sound_set_trim(std::string_view body) {
    using Kind = iclforge::ControlRefusal::Kind;
    const SoundCaps caps = sendspin_sound_caps();
    if (!caps.player) {
        return iclforge::ControlRefusal{Kind::kMissing, "this board has no player to apply a speaker trim to"};
    }
    st::SpeakerTrimTable next = settings().speaker_trim;
    const st::ParseResult parsed = next.apply_text(body);
    if (!parsed.ok) {
        return iclforge::ControlRefusal{Kind::kBadRequest, std::string("PUT /trim: ") + parsed.error};
    }
    if (next == settings().speaker_trim) {
        return std::nullopt;
    }
    // A distance is a delay, which needs delay lines: levels are taken, and a distance is refused with the reason
    // rather than stored for a board that will never play it.
    if (!caps.trim.delay) {
        for (st::RoleCode role = 0; role < st::kRoles; ++role) {
            if (next.get(role).cm != settings().speaker_trim.get(role).cm) {
                return iclforge::ControlRefusal{Kind::kConflict,
                                                std::string("a distance is a delay, and ") + caps.trim.delay_reason};
            }
        }
    }
    // And the delay lines hold only so much: the nearest and the farthest speaker can be no further apart than
    // sound travels in them. A span that is already past it (a table stored by a build with longer lines) can
    // still be made smaller, or left alone while a level is set.
    if (next.span_cm() > caps.trim.max_span_cm && next.span_cm() > settings().speaker_trim.span_cm()) {
        std::array<char, 16> apart{};
        std::snprintf(apart.data(), apart.size(), "%u.%02u", static_cast<unsigned>(caps.trim.max_span_cm / 100),
                      static_cast<unsigned>(caps.trim.max_span_cm % 100));
        return iclforge::ControlRefusal{
            Kind::kConflict, std::string("the nearest and the farthest speaker can be at most ") + apart.data() +
                                 " m apart on this board (its delay lines hold " +
                                 std::to_string((caps.trim.max_delay_samples * 1000U) / kSinkSampleRate) + " ms)"};
    }
    if (!settings_set_speaker_trim(next)) {
        return iclforge::ControlRefusal{Kind::kConflict, "the trim could not be stored"};
    }
    sendspin_set_speaker_trim(next);
    return std::nullopt;
}

// GET /identify: what the test tone is doing, and the speakers it can play.
std::optional<std::string> sound_identify_json() {
    const SoundCaps caps = sendspin_sound_caps();
    render::IdentifyReport report;
    report.available = caps.player;
    report.reason = caps.identify_reason;
    const IdentifyStatus now = sendspin_identify_status();
    report.active = now.active;
    report.output = now.output;
    report.role = now.role;
    report.remaining_ms = now.remaining_ms;
    if (const std::optional<SpeakerTrimStatus> trim = sendspin_speaker_trim_status()) {
        for (std::size_t i = 0; i < trim->count && report.speaker_count < report.speakers.size(); ++i) {
            report.speakers[report.speaker_count++] = trim->rows[i].role;
        }
    }
    return render::identify_json(report);
}

// POST /identify: off, all, a speaker, or a speaker and loop. Never over a play from the control surface (which
// holds the player), and the player refuses it over a stream.
std::optional<iclforge::ControlRefusal> sound_identify(std::string_view body) {
    using Kind = iclforge::ControlRefusal::Kind;
    const std::optional<render::IdentifyRequest> request = render::parse_identify(body);
    if (!request) {
        return iclforge::ControlRefusal{
            Kind::kBadRequest, "POST /identify wants off, all, a speaker such as L, Ls or LFE, or a speaker and loop"};
    }
    if (request->shape != render::IdentifyRequest::Shape::kOff && control_play_active()) {
        return iclforge::ControlRefusal{Kind::kConflict, render::identify_outcome_text(render::IdentifyOutcome::kBusy)};
    }
    const render::IdentifyOutcome outcome = sendspin_identify(*request);
    if (render::identify_accepted(outcome)) {
        return std::nullopt;
    }
    return iclforge::ControlRefusal{outcome == render::IdentifyOutcome::kNoPlayer ? Kind::kMissing : Kind::kConflict,
                                    render::identify_outcome_text(outcome)};
}

bool sound_identify_running() { return sendspin_identify_status().active; }

std::string sound_identify_blocked() {
    if (!sendspin_sound_caps().player) {
        return {};
    }
    if (sendspin_playing()) {
        return "A song is playing: the test tone waits until it stops.";
    }
    if (control_play_active()) {
        return "Something else has the speakers: a play from the web page, or an update.";
    }
    return {};
}

std::string sound_trim_summary() { return st::summary_text(settings().speaker_trim); }

SoundSheet sound_sheet() {
    SoundSheet sheet;
    const SoundCaps caps = sendspin_sound_caps();
    sheet.player = caps.player;
    sheet.identify_reason = sentence(caps.identify_reason);
    sheet.table = settings().speaker_trim;
    if (!caps.player) {
        return sheet;
    }
    sheet.delay = caps.trim.delay;
    sheet.delay_reason = sentence(caps.trim.delay_reason);
    sheet.max_span_cm = caps.trim.max_span_cm;
    if (const std::optional<SpeakerTrimStatus> status = sendspin_speaker_trim_status()) {
        sheet.layout = status->layout.data();
        sheet.server_level = status->server_level;
        sheet.server_delay = status->server_delay;
        sheet.farthest_cm = status->farthest_cm;
        for (std::size_t i = 0; i < status->count; ++i) {
            const SpeakerTrimStatus::Row& row = status->rows[i];
            sheet.speakers.push_back(SoundSheet::Speaker{.role = row.role,
                                                         .output = row.output,
                                                         .trim = sheet.table.get(row.role),
                                                         .applied_level_db = row.applied_level_db,
                                                         .applied_delay_samples = row.applied_delay_samples,
                                                         .delay_clamped = row.delay_clamped});
        }
    }
    const IdentifyStatus tone = sendspin_identify_status();
    sheet.tone_running = tone.active;
    sheet.tone_role = tone.active ? tone.role : st::kNoRole;
    sheet.tone_remaining_ms = tone.remaining_ms;
    sheet.tone_blocked = sound_identify_blocked();
    return sheet;
}

std::string sound_set_trim_text(std::string_view statements) {
    const std::optional<iclforge::ControlRefusal> refused = sound_set_trim(statements);
    return refused ? sentence(refused->why) : std::string();
}

std::string sound_identify_request(std::string_view request) {
    const std::optional<iclforge::ControlRefusal> refused = sound_identify(request);
    return refused ? sentence(refused->why) : std::string();
}

}  // namespace player
