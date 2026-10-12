#pragma once

#include <expected>
#include <filesystem>
#include <memory>
#include <string>

#include "iclforge/sink/sink.hpp"

// hearth-testsink: iclforge::sink as a program (planning/hearth-reference-player.md, The test
// sink). A Sendspin client that waits for servers, which decodes each stream it plays to a WAV file
// with a play-time log: PCM, FLAC or Opus over player@v1, and AC-3 or E-AC-3, objects included, or
// AC-4, over _iclforge_player@v1 (planning/hearth-sendspin-extension.md), rendered to its speaker
// layout. The sink itself, the pairing and the store are libs/sink's; what is here is the output
// (wav_output.hpp, burst_output.hpp) and the speaker layout it renders to.
//
// Several sinks run side by side in one process or several, with distinct names, ports and state
// directories.

namespace iclforge::hearth::testsink {

using sink::CodeMethod;
using SinkLog = sink::Log;

// libs/sink's options, with where the files go and the layout the streams are rendered to.
struct SinkOptions : sink::Options {
    SinkOptions() {
        name = "Hearth test sink";
        product_name = "hearth-testsink";
        layout = "7.1.4";
    }

    // WAV files and play-time logs; empty to count what is played and write nothing.
    std::filesystem::path output_directory;
    // `layout` is the speaker layout the extension role's streams are rendered to, in
    // iclforge::render::OutputLayout's grammar.
};

class Sink {
   public:
    using Totals = sink::Sink::Totals;
    using Roles = sink::Sink::Roles;

    [[nodiscard]] static std::expected<std::unique_ptr<Sink>, std::string> start(SinkOptions options, SinkLog& log);
    ~Sink();
    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;
    Sink(Sink&&) = delete;
    Sink& operator=(Sink&&) = delete;

    [[nodiscard]] std::uint16_t port() const { return core_->port(); }
    // client_id, and the SP:0 token that pairs this sink by its pairing PSK.
    [[nodiscard]] std::string client_id() const { return core_->client_id(); }
    [[nodiscard]] std::string pairing_token() const { return core_->pairing_token(); }

    // The operator's actions on the device: a gesture opening the static code's window, a reset
    // of the round limit, and cancelling the attempt in progress.
    void open_window() { core_->open_window(); }
    void reset_rounds() { core_->reset_rounds(); }
    void cancel_pairing() { core_->cancel_pairing(); }

    // Totals over every connection, for tests.
    [[nodiscard]] Totals totals() const { return core_->totals(); }
    // What the other roles have received, over every connection.
    [[nodiscard]] Roles roles() const { return core_->roles(); }
    // A controller@v1 command, sent on every connection where the role is active.
    void send_controller_command(const sendspin::controller::CommandMessage& command) {
        core_->send_controller_command(command);
    }
    // The sink loses the lyrics it holds and says so on every connection, as a board does that
    // restarted its player, or reconnected: a test of what a server does about a member that
    // reports no lyrics (or another item's) for the item it is playing.
    void forget_lyrics() { core_->forget_lyrics(); }
    // What a screen would draw of the lyrics now (libs/sink's lyric_frame.hpp), for a test of the
    // chain from a server's document and anchors to the sink's own clock.
    [[nodiscard]] sink::LyricFrame lyric_frame(std::int64_t delay_ms = 0) const {
        return core_->lyric_frame(delay_ms);
    }

   private:
    class FileOutputs;

    Sink(std::unique_ptr<FileOutputs> outputs);

    // The outputs are the sink's own: declared first, so that they outlive it.
    std::unique_ptr<FileOutputs> outputs_;
    std::unique_ptr<sink::Sink> core_;
};

}  // namespace iclforge::hearth::testsink
