#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "frame_device.hpp"
#include "iclforge/sink/sink.hpp"
#include "scheduled_sink.hpp"

// hearth-player (docs/hearth/player.md): a Sendspin player for a computer. libs/sink's sink waits
// for servers and pairs with them as the test sink does; what is here is where the streams go: the
// coded ones (AC-3 and E-AC-3, objects included) decoded by libs/sink's DecodingBurstOutput and
// rendered to stereo, PCM, FLAC and Opus decoded as player@v1 says, and both played, each frame at
// the time the server gave it, by a ScheduledPcmSink over an output device.

namespace iclforge::hearth::player {

class PlayerOutputs;

// "hearth-pc-<host>": the name a computer advertises unless it is given one.
[[nodiscard]] std::string default_player_name();
// Where the identity and the long-term PSK are kept between runs: the user's application-data
// directory, never the repository.
[[nodiscard]] std::filesystem::path default_state_directory();

using PlayerLog = sink::Log;
using sink::CodeMethod;

// Makes the device one stream plays to.
using DeviceFactory = std::function<std::unique_ptr<FrameDevice>()>;

struct PlayerOptions : sink::Options {
    PlayerOptions() {
        name = default_player_name();
        product_name = "hearth-player";
        // Every coded format the decoder reads here except AC-4, which this build's renderer
        // folds but the owner's demo does not need yet.
        data_types = {sendspin::player::DataType::kAc3, sendspin::player::DataType::kEac3};
        layout = "2.0";
        output_count = 2;
        required_lead_time_ms = 500;
        min_buffer_ms = 200;
    }

    // The output device: an endpoint id from --list-devices, "null" for one that plays to
    // nowhere in real time, or empty for the default endpoint.
    std::string device;
    // Gain in dB on everything played (negative to be quiet); 0 plays it as decoded.
    double gain_db = 0.0;
};

// What the status line says.
struct PlayerStatus {
    std::string state = "idle";
    std::string codec;
    std::int32_t channels = 0;
    std::int32_t objects = 0;
    bool objects_placed = false;
    PlayStats play;
    std::uint64_t invalid = 0;
    std::uint64_t bursts = 0;
};

class Player {
   public:
    // Starts the player; `device` makes one device for each stream played, so a test can give it a
    // fake. The default is the WASAPI endpoint (or the null device) PlayerOptions::device names.
    [[nodiscard]] static std::expected<std::unique_ptr<Player>, std::string> start(PlayerOptions options,
                                                                                   PlayerLog& log,
                                                                                   DeviceFactory device = {});
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;
    Player(Player&&) = delete;
    Player& operator=(Player&&) = delete;

    [[nodiscard]] std::uint16_t port() const;
    [[nodiscard]] std::string client_id() const;
    [[nodiscard]] std::string pairing_token() const;
    void open_window();
    void reset_rounds();
    void cancel_pairing();
    [[nodiscard]] sink::Sink::Totals totals() const;
    [[nodiscard]] PlayerStatus status() const;
    // One line for the console.
    [[nodiscard]] std::string status_line() const;

   private:
    explicit Player(std::unique_ptr<PlayerOutputs> outputs);

    // Declared first, so that it outlives the sink.
    std::unique_ptr<PlayerOutputs> outputs_;
    std::unique_ptr<sink::Sink> core_;
};

}  // namespace iclforge::hearth::player
