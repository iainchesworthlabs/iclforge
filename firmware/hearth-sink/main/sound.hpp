#pragma once

// The board's sound preferences: the test tone through the speakers and the level and distance kept for each
// (planning/hearth-speaker-trim-identify.md, iclforge/render/speaker_trim.hpp). One implementation for three callers:
// the control surface's routes (GET and PUT /trim, GET and POST /identify - hearth_sink.cpp hands them out), the
// Preferences' backend (preferences.cpp: the Identify speakers button and the trim's summary) and the Speakers sheet
// (panel/lvgl/sound_sheet.cpp), so that a person on the glass and one on the web page are refused in the same words and
// for the same reasons.
//
// Free of LVGL and of the control surface (the routes are sound_routes.hpp's), like the rest of main/ that the panel is
// built over: the sheet reads sound_sheet() and writes sound_set_trim_text() and sound_identify_request(); the preview
// tool (firmware/hearth-sink/preview) supplies its own.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "iclforge/render/speaker_trim.hpp"

namespace player {

// The sample rate every sink opens at, which the delay a distance comes to is counted in.
inline constexpr std::uint32_t kSinkSampleRate = 48000;

// A play from the control surface (POST /play) holds the player: nothing else plays over it. Defined by hearth_sink.cpp.
[[nodiscard]] bool control_play_active();

// --- the Preferences ----------------------------------------------------------------------------------

// The tone the board's own control surface started is running now.
[[nodiscard]] bool sound_identify_running();
// Why the tone cannot start now ("A song is playing: ..."), or empty when it can.
[[nodiscard]] std::string sound_identify_blocked();
// "none set", "1 speaker set", "3 speakers set".
[[nodiscard]] std::string sound_trim_summary();

// --- the Speakers sheet -------------------------------------------------------------------------------

// Everything the sheet draws, read once a tick: the layout in force with a role at each output, what is set for each
// role, what the build can do about it, and where the tone is.
struct SoundSheet {
    // A player to apply a trim and play a tone through. False: the sheet says why (identify_reason) and shows nothing.
    bool player = false;
    std::string identify_reason;
    // The layout in force, as text ("5.1", "7.1.4"), and the speaker at each output in output order.
    std::string layout;
    struct Speaker {
        iclforge::render::speaker_trim::RoleCode role = iclforge::render::speaker_trim::kNoRole;
        std::uint8_t output = 0;
        iclforge::render::speaker_trim::SpeakerTrim trim;
        float applied_level_db = 0.0F;
        std::uint32_t applied_delay_samples = 0;
        bool delay_clamped = false;
    };
    std::vector<Speaker> speakers;
    // A server's settings stand for the levels, or the delays, in force: the table is not what plays.
    bool server_level = false;
    bool server_delay = false;
    // Distances: whether this build holds delay lines, why not, and how far apart the nearest and the farthest speaker
    // can be set, in centimetres.
    bool delay = false;
    std::string delay_reason;
    std::uint16_t max_span_cm = 0;
    // The distance the other speakers' are counted from (the farthest set), in centimetres; 0 when none is set.
    std::uint16_t farthest_cm = 0;
    // The tone: running, the speaker it is on (kNoRole between two), the milliseconds left of its limit; and why it
    // cannot start now, when it cannot.
    bool tone_running = false;
    iclforge::render::speaker_trim::RoleCode tone_role = iclforge::render::speaker_trim::kNoRole;
    std::uint32_t tone_remaining_ms = 0;
    std::string tone_blocked;
    // The whole table, for the sheet's summary and for "reset all".
    iclforge::render::speaker_trim::SpeakerTrimTable table;
};
[[nodiscard]] SoundSheet sound_sheet();

// The sheet's writes. Both return the reason in words when the board refused ("the nearest and the farthest speaker can
// be at most 6.86 m apart on this board"), and an empty string when it took it.
[[nodiscard]] std::string sound_set_trim_text(std::string_view statements);
[[nodiscard]] std::string sound_identify_request(std::string_view request);

}  // namespace player
