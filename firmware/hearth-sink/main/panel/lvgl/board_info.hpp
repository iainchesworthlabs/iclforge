#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "iclforge/meters_tap.hpp"
#include "iclforge/now_view.hpp"
#include "iclforge/preferences.hpp"
#include "iclforge/render/room_plan.hpp"
#include "iclforge/wall_clock.hpp"

#include "../lyrics_reader.hpp"
#include "lv_alloc.hpp"

// What the lyric screen needs to know of the board beyond the lyrics (planning/karaoke-p4x-panel.md,
// "The screen", S1): its name and address, the network and the server, and whether anything is
// playing. A seam of its own so that the screen builds on a computer (preview/): the firmware's
// definitions are in panel.cpp, over the settings, the network and the Sendspin player.

namespace player::panel {

struct BoardInfo {
    std::string name;
    std::string address;
    // The Wi-Fi network's name; empty on Ethernet and while not joined.
    std::string network;
    // The interface the address is on: "ethernet" or "wifi"; empty where the board does not say
    // (the preview), which reads as Wi-Fi whenever there is a network name.
    std::string via;
    // The server the board is connected to, when one is.
    std::string server;
    // The player's stream: "bursts", "pcm" or "idle"; empty with no player.
    std::string stream;
    // The dynamic pairing code while an attempt shows one.
    std::string pairing_code;
    std::string firmware;
    unsigned paired = 0;
    unsigned connections = 0;
    // The build has a Sendspin player.
    bool player = false;

    bool operator==(const BoardInfo&) const = default;
};

// Called a couple of times a second.
[[nodiscard]] BoardInfo read_board_info();

// What the speakers view draws while a stream plays and no lyrics are held (planning/p4x-now-playing.md):
// the room the layout in force describes, the last 100 ms of each output's level, and the line over it.
// The containers are LVGL's (PSRAM on the firmware): the one NowPlaying the screen keeps is filled in
// place five times a second, and its capacity stays, so that a read allocates nothing from the heap
// a decoder leaves little of.
struct NowPlaying {
    // "Dolby Digital Plus 5.1 to 7.1.4": what arrives and what it is played on.
    LvString heading;
    // What the decoder found, and the coded stream's bitrate in kb/s (0 until it is known): the footer says
    // them (iclforge::nowview::stream_info_options).
    iclforge::nowview::StreamFacts facts;
    int kbps = 0;
    // One per output slot, in slot order; empty while the board does not know its layout.
    LvVector<iclforge::render::PlanSpot> spots;
    // Per slot, in dB of full scale (-120 is silence); empty before the first 100 ms window has closed.
    LvVector<float> peak_db;
    LvVector<float> rms_db;
};

// Fills `out` from the player, called five times a second. False when no stream is playing.
[[nodiscard]] bool read_now_playing(NowPlaying& out);

// The levels of the last 100 ms window alone, which the speakers view reads itself every frame and follows when
// the window is a new one (planning/p4x-speakers-options.md): a lock and two short arrays, no heap. `serial`
// moves by one for each window the player closes, so that the view knows how long it has been since the last.
// False when no stream is playing or no window has closed yet.
struct NowLevels {
    std::uint32_t serial = 0;
    std::size_t outputs = 0;
    std::array<float, iclforge::render::OutputLayout::kMaxSlots> peak_db{};
    std::array<float, iclforge::render::OutputLayout::kMaxSlots> rms_db{};
};
[[nodiscard]] bool read_now_levels(NowLevels& out);

// What the server says is playing (metadata@v1), for the title card and the speakers' header
// (planning/p4x-now-playing.md, stage 2): the player's store copied in place five times a second, the
// strings only when the store's own revision moved, into containers that keep their capacity.
struct NowTitle {
    // The server has said a title.
    bool known = false;
    LvString title;
    LvString artist;
    // The third line of the card: the album, and the year after it when there is one ("Kind of Blue - 1959").
    LvString detail;
    // Bumped whenever a string changed, so that the screen rebuilds the card (and composes its header)
    // only then.
    std::uint32_t generation = 0;
    // The store's revision the strings were copied at.
    std::uint32_t revision = 0;
    // Where the song is on the board's own clock and how long it is, when the server gave a progress and
    // the board's clock has converged; they move every read, so they are not part of the generation.
    std::optional<std::int64_t> position_ms;
    std::optional<std::int64_t> length_ms;
};

// Fills `out` from the player, called five times a second while a no-lyrics view that wants it shows.
// False in a build without a player.
bool read_now_title(NowTitle& out);

// The settings the no-lyrics screen follows (settings.hpp), read every frame.
struct NowSettings {
    iclforge::nowview::View view = iclforge::nowview::kDefaultView;
    int dim_after_minutes = iclforge::nowview::kDefaultDimMinutes;
    iclforge::nowview::SpeakersStyle speakers_style = iclforge::nowview::kDefaultSpeakersStyle;
    // The night schedule, and the board's local minute of the day while the schedule is on and the time is known
    // (nothing otherwise: a board that does not know the time is never dark by accident).
    iclforge::nowview::NightSchedule night;
    std::optional<int> minute_of_day;
};
[[nodiscard]] NowSettings read_now_settings();

// What the clock view shows (iclforge/wall_clock.hpp, wall_clock.hpp): no time until a time server has
// answered, and the time on the board's zone after. Read twice a second while that view shows.
[[nodiscard]] iclforge::wallclock::Reading read_wall_clock();

// The backlight, percent (display.hpp's display_set_backlight on the firmware).
void set_backlight(int percent);

// The preferences' side of this board (iclforge/preferences.hpp): what the Preferences sheet reads and sets, stored
// (settings.hpp) and followed at once, as PUT /now-playing, PUT /dim-after and the rest do.
[[nodiscard]] iclforge::prefs::Backend& preferences_backend();

// The player's output as the Meters view takes it (iclforge/meters_tap.hpp): a mono mix in a ring and each output's peak over 50 ms. The firmware's is made
// the first time it is asked for, in LVGL's heap, and handed to the player (sendspin_set_block_tap); the screen asks for blocks while it shows.
[[nodiscard]] iclforge::meters::MeterTap& meters_tap();

// The time between the panel task's frames while it draws (microseconds): what a view that updates on its own beat may be late by without
// anyone having held the task (CONFIG_ICLFORGE_EXAMPLE_PANEL_FPS; the preview's own).
[[nodiscard]] int panel_frame_us();

// A monotonic clock, microseconds.
[[nodiscard]] std::int64_t time_us();

// The lyrics the screen shows, as sendspin_read_lyrics() gives them: the player's, or in the
// firmware's demo mode (POST /panel/demo) a song the panel carries, which loops with no server and
// no network. False without a player and without a demo.
bool read_lyrics(LyricsReader& reader, std::int64_t delay_ms,
                 const iclforge::lyrics::StageOptions& options);

// The demo's mode: 0 off, 1 when the player holds no lyrics, 2 always (panel.cpp).
[[nodiscard]] int demo_mode();
void set_demo_mode(int mode);

}  // namespace player::panel
