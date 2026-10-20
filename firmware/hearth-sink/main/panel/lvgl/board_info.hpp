#pragma once

#include <cstdint>
#include <string>

#include "../lyrics_reader.hpp"

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
