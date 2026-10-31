#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "panel/lyrics_reader.hpp"
#include "iclforge/render/identify_request.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/render/speaker_trim.hpp"
#include "iclforge/ac3/decoder/dynamic_range.hpp"
#include "iclforge/block_tap.hpp"
#include "iclforge/control.hpp"
#include "iclforge/now_view.hpp"

// The board as a Sendspin player (planning/hearth-reference-player.md, B3), as
// a seam CMake resolves, like the source and the sink:
//
//   sendspin/player/  the component's Sendspin player
//                     (firmware/esp-idf/iclforge/include/iclforge/sendspin_host.hpp and
//                     burst_player.hpp), for a build with a network and
//                     CONFIG_ICLFORGE_SENDSPIN;
//   sendspin/none/    everything else, where each of these does nothing.
//
// A server connects to the board, pairs with it and plays to it: bursts over
// _iclforge_player@v1 from hearth, PCM over player@v1 from Music
// Assistant. The player owns the sink while a stream plays; a play from the
// control surface (POST /play, kept for debugging) owns it otherwise, and the
// two never run at once.
//
// sendspin_start() is app_main's. The rest may be called from any task: the
// control surface's, the console's, app_main. Until the player has started they
// find no player, and it has started only once its server has too.

namespace player {

// The port a server connects to, which the board's mDNS record advertises
// (planning/hearth-sendspin-extension.md, row T4: a player's port).
inline constexpr std::uint16_t kSendspinPort = 8928;

// Whether this build has a player at all.
[[nodiscard]] bool sendspin_built();

// Starts the player on a board that is on a network, playing streams that no
// server has given a layout onto `layout`. Nothing without either, or while
// the player runs. app_main calls it at boot, and again when a network joined
// after boot comes up.
void sendspin_start(const iclforge::render::OutputLayout& layout);

// Whether this build has a player, running.
[[nodiscard]] bool sendspin_running();

// A Sendspin stream is playing.
[[nodiscard]] bool sendspin_playing();

// A play from the control surface is about to take the sink (true), or has
// given it back (false). While it has the sink the player tells its servers
// the board is not available, and a stream they send is not played.
void sendspin_set_external(bool external);

// The layout the control surface set, for streams no server sets one for. One
// set before the player has started, or while it starts, is kept for it.
[[nodiscard]] bool sendspin_set_layout(const iclforge::render::OutputLayout& layout);

// The listener's choice of dynamic range (iclforge/ac3/decoder/dynamic_range.hpp), from the next stream's
// start. The player reads the stored one (settings().dynamic_range) as it starts, so this is for a
// change made while it runs; the route stores the setting first.
void sendspin_set_dynamic_range(iclforge::ac3::DynamicRange range);

// The tap that sees each block of the output as it is about to be heard, while it wants to (iclforge/block_tap.hpp: the Meters view's mix and
// peaks). Not owned; null takes it away. Kept for a player that has yet to start, and handed to it when it does.
void sendspin_set_block_tap(iclforge::BlockTap* tap);

// The board's name, slot width or wiring changed: what it tells servers in
// its hello follows, and a server that was told otherwise connects again. A
// player that is starting takes the change up before it has started.
void sendspin_board_changed();

// The board was renamed. The hello a server was given says the old name, and giving it the new one
// means telling its connection to go (client/goodbye restart) and be dialled again, which stops what it
// is playing. So an idle board does that at once, as sendspin_board_changed() does, and one that is
// playing a stream waits until the stream has ended, so that a rename never cuts the music.
void sendspin_name_changed();

// The board is going away, into flash mode or through a restart
// (planning/esp32-ota.md): every server is told the board is restarting,
// the player stops writing to the sink, and the server stops listening. The
// player and its host stay in memory, since other tasks hold pointers to
// them; nothing starts them again, because the board restarts next.
void sendspin_leave();

// GET /status's "sendspin" object, or nothing without a player.
[[nodiscard]] std::optional<iclforge::ControlSendspin> sendspin_status();

// --- The listener's sound: a level and distance for each speaker, and the identify tone ---------------
// (planning/p4x-preferences.md, iclforge/render/speaker_trim.hpp). Both are applied by the player in the same
// output stage as the music, after the decode and before the DAC, so they cover PCM, AC-3, E-AC-3, AC-4 and
// objects alike. Without a player (sendspin/none/) there is nothing to apply them to.

// What this build can do about them, which does not change while the board runs: for the screen, which greys what
// the board cannot do and says why, and for GET /trim and GET /identify.
struct SoundCaps {
    // A player to apply a trim and play a tone through.
    bool player = false;
    // Level is always possible with a player; delay needs the build's delay lines (CONFIG_ICLFORGE_EXAMPLE_
    // SENDSPIN_MAX_DELAY_MS), which a board without PSRAM does not have room for. max_span_m is how far apart the
    // nearest and the farthest speaker can be set; delay_reason says why it is 0.
    iclforge::render::speaker_trim::TrimCaps trim;
    // Why there is no identify tone, when there is none.
    const char* identify_reason = "this build has no Sendspin player to play a tone through";
};
[[nodiscard]] SoundCaps sendspin_sound_caps();

// The table in settings() is the player's from here (the caller has stored it): in force at the next chunk of a
// stream that plays, and at once otherwise. One set before the player has started is picked up by its start.
void sendspin_set_speaker_trim(const iclforge::render::speaker_trim::SpeakerTrimTable& table);

// What is in force at each output now, as the player last said it, for GET /trim and the screen's editor. Nothing
// without a player running.
struct SpeakerTrimStatus {
    // The layout in force, as text.
    std::array<char, iclforge::render::OutputLayout::kTextBytes> layout{};
    // A server's settings stand for the levels, or the delays, in force: the table is not what plays.
    bool server_level = false;
    bool server_delay = false;
    // The distance the delays are counted from, in centimetres; 0 when no speaker has one.
    std::uint16_t farthest_cm = 0;
    // The speakers the layout has at an output, in output order.
    struct Row {
        iclforge::render::speaker_trim::RoleCode role = iclforge::render::speaker_trim::kNoRole;
        std::uint8_t output = 0;
        float applied_level_db = 0.0F;
        std::uint32_t applied_delay_samples = 0;
        bool delay_clamped = false;
    };
    std::array<Row, iclforge::render::speaker_trim::kOutputs> rows{};
    std::size_t count = 0;
};
[[nodiscard]] std::optional<SpeakerTrimStatus> sendspin_speaker_trim_status();

// The board's own identify tone (iclforge/render/identify_request.hpp). Refused while a stream plays or something
// else has the sink; stopped at once by a play's start, by flash mode, and by kOff; ended by its own limits
// otherwise, whatever is asked. Never leaves the sink in any state but the one it found.
[[nodiscard]] iclforge::render::IdentifyOutcome sendspin_identify(const iclforge::render::IdentifyRequest& request);

// Where the tone is, for GET /identify and the screen's room: running, the output it is on now (-1 between two
// speakers) and that output's speaker, the milliseconds left of its limit. All at rest without a player.
struct IdentifyStatus {
    bool active = false;
    int output = -1;
    iclforge::render::speaker_trim::RoleCode role = iclforge::render::speaker_trim::kNoRole;
    std::uint32_t remaining_ms = 0;
};
[[nodiscard]] IdentifyStatus sendspin_identify_status();

// What a screen draws while a stream plays (planning/p4x-now-playing.md): the layout in force, the
// last 100 ms of each output's level, and what the decoder found. One read of the player's own status,
// with no JSON and none of the server's state, so that a panel can ask five times a second.
struct SendspinPlaying {
    // A stream is running.
    bool active = false;
    // The layout in force, as text (a name or a speaker list; empty before one is set), and the outputs
    // it has. A fixed buffer: the panel asks five times a second and the heap it would allocate from
    // is the one a decoder leaves little of.
    std::array<char, iclforge::render::OutputLayout::kTextBytes> layout{};
    std::size_t outputs = 0;
    // Per output, in dB of full scale (-120 is silence), once the first 100 ms window has closed.
    bool have_levels = false;
    std::array<float, iclforge::render::OutputLayout::kMaxSlots> peak_db{};
    std::array<float, iclforge::render::OutputLayout::kMaxSlots> rms_db{};
    // The codec, and once a unit has decoded the coded layout and the objects.
    iclforge::nowview::StreamFacts facts;
    // The coded bytes played so far in this stream, for the bitrate (iclforge::nowview::BitrateMeter).
    std::uint64_t coded_bytes = 0;
};
[[nodiscard]] SendspinPlaying sendspin_now_playing();

// The levels alone, for a view that follows every 100 ms window (planning/p4x-speakers-options.md): the same
// peak and RMS per output as sendspin_now_playing(), with the count of windows the player has closed, so that a
// caller can tell a new window from the one it has. A lock and two short arrays, where sendspin_now_playing()
// copies the player's whole status; fixed-size, so it allocates nothing.
struct SendspinLevels {
    // A stream is running and its first window has closed.
    bool have = false;
    std::uint32_t serial = 0;
    std::size_t outputs = 0;
    std::array<float, iclforge::render::OutputLayout::kMaxSlots> peak_db{};
    std::array<float, iclforge::render::OutputLayout::kMaxSlots> rms_db{};
};
[[nodiscard]] SendspinLevels sendspin_now_levels();

// POST /pairing's actions: "reset", "cancel" or "forget". False for anything
// else, or without a player.
[[nodiscard]] bool sendspin_pairing(std::string_view action);

// GET /pairing: the servers this board is paired with, or nothing without a
// player.
[[nodiscard]] std::optional<iclforge::ControlPairings> sendspin_pairings();

// POST /pairing's "forget " and a server_id: that one server's pairing. False
// when the board has none with it, nothing without a player.
[[nodiscard]] std::optional<bool> sendspin_forget_server(std::string_view server_id);

// A line typed on the console. True when it was one of the player's
// commands: "pair list", "pair reset", "pair cancel", "pair forget" (every
// server), "pair forget " and a server_id or the first eight or more of its
// characters (that server alone), "pair token", "sendspin".
bool sendspin_console(std::string_view line);

// Called by app_main about ten times a second: what the player reports to its
// server - levels, counters and what the decoder found - goes out from here.
void sendspin_poll();

}  // namespace player
