#include "iclforge/sink/sink.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "iclforge/lyrics/follower.hpp"
#include "iclforge/lyrics/lyrics.hpp"
#include "iclforge/sendspin/iclforge_player.hpp"
#include "iclforge/sendspin/arbiter.hpp"
#include "iclforge/sendspin/base64url.hpp"
#include "iclforge/sendspin/chunks.hpp"
#include "iclforge/sendspin/discovery.hpp"
#include "iclforge/sendspin/handshake.hpp"
#include "iclforge/sendspin/mdns.hpp"
#include "iclforge/sendspin/messages.hpp"
#include "iclforge/sendspin/pairing.hpp"
#include "iclforge/sendspin/pairing_flow.hpp"
#include "iclforge/sendspin/pairing_messages.hpp"
#include "iclforge/sendspin/player_session.hpp"
#include "iclforge/sendspin/session_driver.hpp"
#include "iclforge/sendspin/state_roles.hpp"
#include "iclforge/sendspin/stream_roles.hpp"
#include "iclforge/sendspin/transport.hpp"
#include "iclforge/sendspin/websocket.hpp"
#include "iclforge/sink/lyric_frame.hpp"
#include "iclforge/sink/store.hpp"

namespace iclforge::sink {

namespace {

namespace ac = sendspin::player;
namespace m = sendspin::messages;
namespace flow = sendspin::pairing_flow;
namespace pm = sendspin::pairing_messages;
namespace websocket = sendspin::transport::websocket;
using sendspin::Arbiter;

[[nodiscard]] std::string key_text(const Key32& key) {
    return sendspin::base64url::encode(key);
}

// The first eight characters of a key's base64url, enough to tell servers apart in a log.
[[nodiscard]] std::string short_key(const Key32& key) {
    return key_text(key).substr(0, 8);
}

// A dynamic code as an operator types it: digits grouped 3-3 or 4-4, or the SP:1 token of a QR
// code (pairing.md, Pairing Code Presentation).
[[nodiscard]] std::string code_text(const flow::Code& code) {
    if (const auto* digits = std::get_if<std::string>(&code)) {
        if (digits->size() == 6 || digits->size() == 8) {
            const std::size_t half = digits->size() / 2;
            return digits->substr(0, half) + "-" + digits->substr(half);
        }
        return *digits;
    }
    const auto& bytes = std::get<std::array<std::uint8_t, 24>>(code);
    return sendspin::pairing::encode_token(sendspin::pairing::TokenVersion::kDynamicCode, bytes);
}

[[nodiscard]] std::string_view abort_text(std::optional<pm::AbortReason> reason) {
    if (!reason) {
        return "ended";
    }
    switch (*reason) {
        case pm::AbortReason::kAttemptTimeout:
            return "timed out";
        case pm::AbortReason::kConcurrentAttempt:
            return "refused: another attempt";
        case pm::AbortReason::kMethodNotSupported:
            return "refused: method not supported";
        case pm::AbortReason::kCodeMismatch:
            return "code mismatch";
        case pm::AbortReason::kUserCancelled:
            return "cancelled";
        case pm::AbortReason::kPinLengthUnacceptable:
            return "refused: code length";
    }
    return "ended";
}

[[nodiscard]] std::string format_text(const m::AudioFormat& format) {
    const std::string_view codec = format.codec == m::Codec::kPcm    ? "PCM"
                                   : format.codec == m::Codec::kFlac ? "FLAC"
                                                                     : "Opus";
    return std::string(codec) + " " + std::to_string(format.sample_rate) + " Hz " + std::to_string(format.bit_depth) +
           "-bit " + std::to_string(format.channels) + " ch";
}

}  // namespace

// One server's connection: its session, driver and output.
class Connection final : public sendspin::PlayerListener, public std::enable_shared_from_this<Connection> {
   public:
    // Constructed with the session lock held: the session takes a number from the pairing state
    // its sessions share.
    Connection(Sink& sink, Arbiter::Id id, sendspin::PlayerConfig config)
        : sink_(&sink),
          id_(id),
          state_(config.player_state),
          iclforge_state_(config.iclforge_state),
          output_(sink.outputs_->stream(id)),
          bursts_(sink.outputs_->bursts(id)) {
        session_.emplace(std::move(config), *sink.store_, sink.pairing_state_, *this, sink.clock_);
    }

    ~Connection() override {
        // The driver's threads stop first; the session's destructor then tells the shared pairing
        // state its connection has gone, which needs the lock the threads were using.
        driver_.reset();
        const std::lock_guard lock(*sink_->session_lock_);
        session_.reset();
    }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) = delete;
    Connection& operator=(Connection&&) = delete;

    void start(std::unique_ptr<sendspin::transport::Connection> transport) {
        peer_ = transport->peer();
        Sink* sink = sink_;
        driver_ = std::make_unique<sendspin::SessionDriver>(
            std::move(transport),
            sendspin::DrivenSession{
                .receive = [this](const sendspin::transport::Frame& frame) { return session_->receive(frame); },
                .tick = [this] { return session_->tick(); },
                .next_tick_us = [this] { return session_->next_tick_us(); },
                .ended = [sink, id = id_] { sink->post([sink, id] { sink->remove(id); }); }},
            sendspin::SessionDriver::kDefaultMaxQueuedBytes, sink->session_lock_);
        log("connected from " + peer_);
        sendspin::SessionOutput first;
        {
            const std::lock_guard lock(*sink->session_lock_);
            first = session_->open();
        }
        driver_->start(std::move(first));
    }

    [[nodiscard]] sendspin::SessionDriver& driver() { return *driver_; }
    [[nodiscard]] sendspin::PlayerSession& session() { return *session_; }
    [[nodiscard]] const StreamOutput& output() const { return *output_; }
    [[nodiscard]] const BurstStreamOutput& bursts() const { return *bursts_; }

    // --- PlayerListener, called with the session lock held -------------------------------

    bool on_activation(const Key32& server_key, const m::Activate& activate, bool first) override {
        const Arbiter::Verdict verdict =
            sink_->arbiter_.activation(id_, server_key, Arbiter::rank_of(activate.activities), first);
        if (verdict.displaced) {
            Sink* sink = sink_;
            sink->post([sink, displaced = *verdict.displaced] { sink->displace(displaced); });
        }
        if (const std::optional<Key32> last = sink_->arbiter_.last_playback()) {
            sink_->store_->set_last_playback(*last);
        }
        const std::string activities = Arbiter::rank_of(activate.activities) == Arbiter::Rank::kPlayback ? "playback"
                                       : Arbiter::rank_of(activate.activities) == Arbiter::Rank::kPairing ? "pairing"
                                                                                                           : "nothing";
        log(std::string(verdict.admit ? "activated for " : "refused, another server holds the sink: ") + activities);
        if (verdict.admit) {
            activity_ = Arbiter::rank_of(activate.activities) == Arbiter::Rank::kPlayback ? Sink::Status::Activity::kPlayback
                        : Arbiter::rank_of(activate.activities) == Arbiter::Rank::kPairing ? Sink::Status::Activity::kPairing
                                                                                            : Sink::Status::Activity::kNothing;
        }
        return verdict.admit;
    }

    void on_pairing_attempt(bool in_progress) override { sink_->arbiter_.attempt(id_, in_progress); }

    void on_stream_start(const m::PlayerStream& stream) override {
        const bool writing = output_->start(stream);
        streaming_ = true;
        log("stream " + format_text(stream.format) +
            (writing ? (output_->file().empty() ? std::string{} : " to " + output_->file().string())
                     : std::string(" (not written)")));
    }
    void on_stream_clear() override {
        output_->clear();
        follower_.clear_position();
        log("stream cleared");
    }
    void on_stream_end() override {
        output_->end();
        streaming_ = false;
        follower_.clear_position();
        log("stream ended after " + std::to_string(output_->counts().units) + " chunks");
    }
    void on_audio(std::span<const std::uint8_t> frame, std::int64_t local_time) override {
        output_->write(frame, local_time);
    }

    void on_command(const m::PlayerCommandMessage& command) override {
        switch (command.command) {
            case m::PlayerCommand::kVolume:
                state_.volume = command.volume;
                log("volume " + std::to_string(command.volume));
                break;
            case m::PlayerCommand::kMute:
                state_.muted = command.mute;
                log(command.mute ? "muted" : "unmuted");
                break;
            case m::PlayerCommand::kSetOutputDelay:
                state_.output_delay_ms = command.output_delay_ms;
                log("output delay " + std::to_string(command.output_delay_ms) + " ms");
                break;
        }
        // The new state is reported from the sink's thread, outside this callback.
        const std::weak_ptr<Connection> self = weak_from_this();
        sink_->post([self] {
            if (const std::shared_ptr<Connection> connection = self.lock()) {
                connection->driver().call([&] { return connection->session_->set_state(connection->state_); });
            }
        });
    }

    void on_group(const m::GroupUpdate& update) override {
        if (update.group_name) {
            log("group " + *update.group_name);
        }
    }

    void on_unpaired(const Key32& server_key) override {
        sink_->store_->remove_record(server_key);
        log("unpaired by server " + short_key(server_key));
    }

    void on_pairing_code(const flow::Code& code) override {
        pairing_code_ = code_text(code);
        log("PAIRING CODE " + *pairing_code_);
    }

    void on_pairing_held_back() override {
        log("pairing waits for the operator: type 'window' to open the static code's window, or 'reset' at the "
            "round limit");
    }

    void on_paired(const Key32& server_key, const Key32& long_term_psk) override {
        pairing_code_.reset();
        const bool stored = sink_->store_->add_record(server_key, long_term_psk);
        log("paired with server " + short_key(server_key) + (stored ? "" : " (the record could not be saved)"));
    }

    void on_pairing_ended(std::optional<pm::AbortReason> reason) override {
        pairing_code_.reset();
        log("pairing " + std::string(abort_text(reason)));
    }

    void on_burst_stream_start(const ac::StreamStart& stream) override {
        const bool writing = bursts_->start(stream);
        streaming_ = true;
        // Only a stream the output took leaves the sink by the way the output says.
        burst_taken_ = writing;
        reported_decoder_.reset();
        const std::string_view codec = stream.data_type == ac::DataType::kAc3    ? "AC-3 "
                                       : stream.data_type == ac::DataType::kEac3 ? "E-AC-3 "
                                                                                 : "AC-4 ";
        // "to 7.1.4" when the program renders to a layout, and nothing when it does not (it hands the
        // bytes on, as the Android app does).
        const std::string layout = sink_->options_.layout.empty() ? std::string{} : " to " + sink_->options_.layout;
        log("burst stream " + std::string(codec) + std::to_string(stream.sample_rate) + " Hz" + layout +
            (writing ? (bursts_->file().empty() ? std::string{} : " in " + bursts_->file().string())
                     : std::string(" (not written)")));
        post_iclforge_state();
    }
    // A flush moves the sound: the document stays for the item and the position waits for the new
    // stream's first anchor. The end of a stream is nothing playing, and the document stays too: the
    // same item started again takes its anchors, and another item's document replaces it.
    void on_burst_stream_clear() override {
        bursts_->clear();
        follower_.clear_position();
        log("burst stream cleared");
    }
    void on_burst_stream_end() override {
        bursts_->end();
        streaming_ = false;
        burst_taken_ = false;
        follower_.clear_position();
        log("burst stream ended after " + std::to_string(bursts_->counts().units) + " bursts");
        post_iclforge_state();
    }
    void on_burst(const sendspin::BurstChunk& chunk, std::int64_t local_time) override {
        bursts_->write(chunk, local_time);
        if (bursts_->decoder() != reported_decoder_) {
            reported_decoder_ = bursts_->decoder();
            post_iclforge_state();
        }
    }
    void on_invalid_burst() override { ++iclforge_state_.counters.invalid_chunks; }

    void on_iclforge_command(const ac::CommandMessage& command) override {
        switch (command.command) {
            case ac::Command::kVolume:
                iclforge_state_.volume = command.volume;
                log("volume " + std::to_string(command.volume));
                break;
            case ac::Command::kMute:
                iclforge_state_.muted = command.mute;
                log(command.mute ? "muted" : "unmuted");
                break;
            case ac::Command::kSetOutputDelay:
                iclforge_state_.output_delay_ms = command.output_delay_ms;
                log("output delay " + std::to_string(command.output_delay_ms) + " ms");
                break;
            case ac::Command::kSettings:
                // Listed only with accept_settings or karaoke. Settings replace the last whole,
                // so a key left out is the sink's own default again. Nothing else renders from
                // them: "applied" is the revision reported back, which is all a server can read.
                iclforge_state_.settings_revision = command.settings.revision;
                iclforge_state_.settings_error.reset();
                if (sink_->options_.karaoke) {
                    const ac::DecoderSettings& decoder = command.settings.decoder;
                    const ac::Karaoke karaoke = decoder.karaoke.value_or(ac::Karaoke::kOff);
                    const ac::KaraokeVocals vocals = decoder.karaoke_vocals.value_or(ac::KaraokeVocals::kBoth);
                    const std::array<double, 3> levels{decoder.karaoke_melody_db.value_or(0.0),
                                                       decoder.karaoke_vocal1_db.value_or(0.0),
                                                       decoder.karaoke_vocal2_db.value_or(0.0)};
                    bursts_->set_karaoke(karaoke, vocals, levels);
                    // The levels that are not the table's, as "melody -6 dB": a test reads them.
                    std::string moved;
                    constexpr std::array<const char*, 3> kParts{"melody", "vocal 1", "vocal 2"};
                    for (std::size_t part = 0; part < levels.size(); ++part) {
                        if (levels[part] != 0.0) {
                            char text[48];
                            std::snprintf(text, sizeof text, ", %s %+.0f dB", kParts[part], levels[part]);
                            moved += text;
                        }
                    }
                    log("settings " + std::to_string(command.settings.revision) + " applied (karaoke " +
                        std::string(karaoke == ac::Karaoke::kOff       ? "off"
                                    : karaoke == ac::Karaoke::kAware   ? "aware"
                                                                       : "capable") +
                        ", vocals " +
                        std::string(vocals == ac::KaraokeVocals::kNone   ? "none"
                                    : vocals == ac::KaraokeVocals::kV1   ? "v1"
                                    : vocals == ac::KaraokeVocals::kV2   ? "v2"
                                                                         : "both") +
                        moved + ")");
                } else {
                    log("settings " + std::to_string(command.settings.revision) + " applied");
                }
                break;
            case ac::Command::kIdentify:
                // Not listed, so not sent.
                break;
            case ac::Command::kLyrics: {
                // The document is parsed as a board would: held when it has lyrics, the item
                // reported with the line -1 until an anchor says where the sound is (this sink has
                // no clock of its own to follow it by: it reports the line of the anchor's position).
                // The follower keeps the item even when the document is not held, so the anchors
                // that follow are the item's and are not mistaken for another's.
                const bool held = follower_.set_document(command.lyrics.item, command.lyrics.text);
                if (held) {
                    iclforge_state_.lyrics = ac::LyricsReport{.item = command.lyrics.item, .line = -1};
                } else {
                    iclforge_state_.lyrics.reset();
                }
                log("lyrics for item " + std::to_string(command.lyrics.item) + ": " +
                    (held ? "held" : "none") + " (" + std::to_string(command.lyrics.text.size()) + " bytes)");
                const std::lock_guard lock(sink_->roles_mutex_);
                sink_->roles_.lyric_commands.push_back(command);
                break;
            }
            case ac::Command::kLyricsPosition: {
                const ac::LyricsAnchor& anchor = command.lyrics_position;
                char text[96];
                std::snprintf(text, sizeof text, "lyrics position item %lld: %lld ms, speed %.0f",
                              static_cast<long long>(anchor.item), static_cast<long long>(anchor.position_ms),
                              anchor.speed);
                log(text);
                // The line the position is in, for an anchor of the item held, and the anchor for the
                // screen's playhead, which follows the sound by the server's clock.
                if (follower_.has_lyrics() && iclforge_state_.lyrics && iclforge_state_.lyrics->item == anchor.item) {
                    const iclforge::lyrics::LyricPosition at =
                        iclforge::lyrics::lyric_position(*follower_.lyrics(), anchor.position_ms);
                    iclforge_state_.lyrics->line = at.line ? static_cast<std::int32_t>(*at.line) : -1;
                }
                follower_.anchor(anchor.item, anchor.position_ms, anchor.at_us, anchor.speed);
                const std::lock_guard lock(sink_->roles_mutex_);
                sink_->roles_.lyric_commands.push_back(command);
                break;
            }
        }
        post_iclforge_state();
    }

    void on_settings_refused(const ac::SettingsError& error) override {
        log("settings " + std::to_string(error.revision) + " refused: " + error.why);
    }

    void on_server_state(const m::ServerState& state) override {
        {
            const std::lock_guard lock(sink_->roles_mutex_);
            Sink::Roles& roles = sink_->roles_;
            ++roles.states;
            if (state.metadata) {
                roles.metadata = *state.metadata;
                // This connection's, for the title a screen shows when it holds no document (D7).
                // Called with the session's lock held, as inspect() takes it to read.
                metadata_ = *state.metadata;
            }
            if (state.controller) {
                roles.controller = *state.controller;
            }
            if (state.color) {
                roles.colors = *state.color;
            }
        }
        // What the update says, for the log: built once the lock is let go, from the update
        // itself, so the locked scope holds only the stores and ends in nothing that can throw.
        std::string text;
        if (state.metadata) {
            const auto& metadata = *state.metadata;
            text += metadata ? " metadata \"" + metadata->title.value_or("") + "\""
                             : std::string(" metadata cleared");
        }
        if (state.controller) {
            const auto& controller = *state.controller;
            text += controller ? " controller volume " + std::to_string(controller->volume) +
                                     (controller->muted ? " muted" : "")
                               : std::string(" controller cleared");
        }
        if (state.color) {
            text += *state.color ? std::string(" colours") : std::string(" colours cleared");
        }
        log("state:" + text);
    }

    void on_artwork_stream_start(const sendspin::artwork::Channels& /*channels*/) override {
        const std::lock_guard lock(sink_->roles_mutex_);
        ++sink_->roles_.artwork_streams;
        transfer_.reset();
    }
    void on_artwork_message(const sendspin::artwork::Message& message) override {
        namespace artwork = sendspin::artwork;
        switch (message.kind) {
            case artwork::Kind::kAnnounce:
                transfer_ = Transfer{.channel = message.channel, .size = message.total_size, .bytes = {}};
                break;
            case artwork::Kind::kPart:
                if (transfer_ && transfer_->channel == message.channel) {
                    transfer_->bytes.insert(transfer_->bytes.end(), message.data.begin(), message.data.end());
                }
                break;
            case artwork::Kind::kCancel:
                transfer_.reset();
                return;
        }
        // A size of zero clears the channel; the session checks that parts stop at the size.
        if (transfer_ && transfer_->bytes.size() == transfer_->size) {
            const std::size_t channel = transfer_->channel;
            const std::size_t bytes = transfer_->size;
            {
                const std::lock_guard lock(sink_->roles_mutex_);
                sink_->roles_.images[channel] = std::move(transfer_->bytes);
            }
            transfer_.reset();
            log("artwork channel " + std::to_string(channel) +
                (bytes == 0 ? std::string(" cleared") : ": " + std::to_string(bytes) + " bytes"));
        }
    }
    void on_artwork_stream_end() override { transfer_.reset(); }

    void on_visualizer_stream_start(const sendspin::visualizer::StreamStart& start) override {
        const std::lock_guard lock(sink_->roles_mutex_);
        sink_->roles_.visualizer = start;
    }
    void on_visualizer_frame(const sendspin::visualizer::Frame& /*frame*/, std::int64_t /*local_time*/) override {
        const std::lock_guard lock(sink_->roles_mutex_);
        ++sink_->roles_.visualizer_frames;
    }
    void on_visualizer_stream_end() override {
        const std::lock_guard lock(sink_->roles_mutex_);
        sink_->roles_.visualizer.reset();
    }

    // This connection as a screen says it, read under the session's lock.
    [[nodiscard]] Sink::Status::Peer peer_status() {
        return driver().inspect([&] {
            Sink::Status::Peer peer;
            peer.id = id_;
            peer.address = peer_;
            peer.server_name = session_->server_name();
            peer.activity = activity_;
            peer.streaming = streaming_;
            peer.output = output_now();
            if (burst_taken_) {
                peer.decoder = bursts_->decoder();
            }
            return peer;
        });
    }
    [[nodiscard]] std::optional<std::string> pairing_code() {
        return driver().inspect([&] { return pairing_code_; });
    }

    // What this connection's server last said is playing (metadata@v1), none once it cleared it.
    [[nodiscard]] std::optional<sendspin::metadata::State> metadata() {
        return driver().inspect([&] { return metadata_; });
    }

    // Where the server says the item is and how long it is (metadata@v1), at the sink's own time
    // now on the server's clock. Nothing without a progress or a converged clock.
    struct MetadataProgress {
        std::int64_t position_ms = 0;
        std::optional<std::int64_t> length_ms{};
    };
    [[nodiscard]] std::optional<MetadataProgress> metadata_progress() {
        return driver().inspect([&]() -> std::optional<MetadataProgress> {
            if (!metadata_ || !metadata_->progress || !session_->clock_converged()) {
                return std::nullopt;
            }
            const std::optional<std::int64_t> position = sendspin::metadata::position_ms(
                *metadata_, session_->clock().to_server(sink_->clock_.now_us()));
            if (!position) {
                return std::nullopt;
            }
            MetadataProgress out;
            out.position_ms = *position;
            if (metadata_->progress->track_duration_ms > 0) {
                out.length_ms = metadata_->progress->track_duration_ms;
            }
            return out;
        });
    }

    // Whether this connection holds a lyric document, held or not (the item is known).
    [[nodiscard]] bool has_lyric_item() {
        return driver().inspect([&] { return follower_.item().has_value(); });
    }
    [[nodiscard]] bool streaming() {
        return driver().inspect([&] { return streaming_; });
    }

    // What a screen draws of this connection's lyrics now: the playhead on the server's clock, which
    // the session's time filter gives for the sink's own. Without a converged clock there is no
    // position, and the stage is "waiting" with the first lines to show.
    [[nodiscard]] LyricFrame lyric_frame(std::int64_t delay_ms, const iclforge::lyrics::StageOptions& options) {
        return driver().inspect([&] {
            std::optional<std::int64_t> position;
            if (session_->clock_converged()) {
                position = follower_.position_ms(session_->clock().to_server(sink_->clock_.now_us()));
            }
            const iclforge::lyrics::Stage stage =
                iclforge::lyrics::make_stage(follower_.lyrics(), position, delay_ms, options);
            return make_lyric_frame(follower_.item(), follower_.lyrics(), stage, streaming_);
        });
    }

    // Holds no lyrics from here, and reports it (Sink::forget_lyrics()).
    void forget_lyrics() {
        driver().call([&] {
            follower_.reset();
            iclforge_state_.lyrics.reset();
            ac::State state = iclforge_state_;
            state.decoder = bursts_->decoder();
            state.output = output_now();
            state.counters.bursts_played = bursts_->counts().units;
            return session_->set_iclforge_state(state);
        });
        log("lyrics dropped");
    }

   private:
    void log(std::string_view text) { sink_->log("[" + std::to_string(id_) + "] " + std::string(text)); }

    // How the burst stream leaves the sink, while one the output took is open. Under the session's
    // lock, as burst_taken_ is.
    [[nodiscard]] std::optional<ac::Output> output_now() const {
        return burst_taken_ ? bursts_->output() : std::nullopt;
    }

    // Reports the extension role's state from the sink's thread, outside the callback that
    // changed it, with what the burst output has found.
    void post_iclforge_state() {
        const std::weak_ptr<Connection> self = weak_from_this();
        sink_->post([self] {
            if (const std::shared_ptr<Connection> connection = self.lock()) {
                connection->driver().call([&] {
                    ac::State state = connection->iclforge_state_;
                    const OutputCounts counts = connection->bursts_->counts();
                    state.decoder = connection->bursts_->decoder();
                    state.output = connection->output_now();
                    state.counters.bursts_played = counts.units;
                    state.counters.underruns = counts.underruns;
                    state.counters.late_chunks = counts.late;
                    state.counters.dropped_chunks = counts.dropped;
                    return connection->session_->set_iclforge_state(state);
                });
            }
        });
    }

    Sink* sink_;
    Arbiter::Id id_;
    std::string peer_;
    m::PlayerState state_;
    ac::State iclforge_state_;
    // The document the sink holds, parsed, and the playhead the server's anchors move: the line an
    // anchor's position is in, and what a screen draws at a time on the server's clock.
    iclforge::lyrics::Follower follower_;
    // A stream is open: what the screen's "no lyrics" and its idle screen are told apart by.
    bool streaming_ = false;
    // metadata@v1's latest state from this connection's server, for Sink::lyric_frame().
    std::optional<sendspin::metadata::State> metadata_;
    // The burst output took the stream that is open (BurstStreamOutput::start returned true).
    bool burst_taken_ = false;
    // What this connection was admitted for, and the code its server asked to pair by.
    Sink::Status::Activity activity_ = Sink::Status::Activity::kNothing;
    std::optional<std::string> pairing_code_;
    std::unique_ptr<StreamOutput> output_;
    std::unique_ptr<BurstStreamOutput> bursts_;
    // The decoder report last sent.
    std::optional<ac::DecoderReport> reported_decoder_;
    // The artwork transfer in flight.
    struct Transfer {
        std::size_t channel = 0;
        std::size_t size = 0;
        std::vector<std::uint8_t> bytes;
    };
    std::optional<Transfer> transfer_;
    std::optional<sendspin::PlayerSession> session_;
    std::unique_ptr<sendspin::SessionDriver> driver_;
};

std::expected<std::unique_ptr<Sink>, std::string> Sink::start(Options options, Outputs& outputs, Log& log) {
    if (options.state_directory.empty()) {
        return std::unexpected(std::string("a state directory is required"));
    }
    if (options.code_method == CodeMethod::kStatic &&
        (options.static_code.size() != 8 ||
         !std::all_of(options.static_code.begin(), options.static_code.end(), [](char c) { return c >= '0' && c <= '9'; }))) {
        return std::unexpected(std::string("a static pairing code is eight digits"));
    }
    std::expected<std::unique_ptr<Store>, std::string> store = Store::open(options.state_directory);
    if (!store) {
        return std::unexpected(store.error());
    }
    std::unique_ptr<Sink> sink(new Sink(std::move(options), outputs, log, std::move(*store)));

    websocket::ListenerOptions listening;
    listening.address = sink->options_.address;
    listening.port = sink->options_.port;
    listening.max_connections = sink->options_.max_connections;
    Sink* raw = sink.get();
    sink->listener_ = websocket::Listener::start(
        listening, [raw](std::unique_ptr<sendspin::transport::Connection> transport) { raw->accept(std::move(transport)); });
    if (!sink->listener_) {
        return std::unexpected("cannot listen on " + sink->options_.address + ":" + std::to_string(sink->options_.port));
    }
    if (sink->options_.advertise) {
        sendspin::discovery::Advertisement advertisement{
            .service = std::string(sendspin::discovery::kPlayerService),
            .instance = sink->options_.name,
            .port = sink->listener_->port(),
            .txt = {{.key = "path", .value = std::string(websocket::kPath)}, {.key = "name", .value = sink->options_.name}}};
        sink->advertiser_ = sendspin::discovery::mdns::advertise(
            std::move(advertisement), {.interfaces = sink->options_.mdns_interfaces, .host = {}});
        if (!sink->advertiser_) {
            sink->log("not advertised: no network interface could be opened for mDNS");
        }
    }
    return sink;
}

Sink::Sink(Options options, Outputs& outputs, Log& log, std::unique_ptr<Store> store)
    : options_(std::move(options)),
      outputs_(&outputs),
      log_(&log),
      store_(std::move(store)),
      arbiter_(store_->last_playback()),
      worker_([this] { run_posted(); }) {}

Sink::~Sink() {
    advertiser_.reset();
    if (listener_) {
        listener_->stop();
    }
    {
        const std::lock_guard lock(posted_mutex_);
        stopping_ = true;
    }
    posted_changed_.notify_all();
    worker_.join();
    std::map<Arbiter::Id, std::shared_ptr<Connection>> remaining;
    {
        const std::lock_guard lock(connections_mutex_);
        remaining.swap(connections_);
    }
    remaining.clear();
}

void Sink::accept(std::unique_ptr<sendspin::transport::Connection> transport) {
    sendspin::PlayerConfig config;
    config.identity = store_->identity();
    config.name = options_.name;
    config.device_info = {.product_name = options_.product_name,
                          .manufacturer = options_.manufacturer,
                          .software_version = {},
                          .mac_address = {}};
    config.supported_roles = {"player@v1"};
    std::vector<m::AudioFormat> formats;
    for (const m::Codec codec : options_.codecs) {
        if (codec == m::Codec::kOpus) {
            formats.push_back(
                {.codec = codec, .channels = 2, .sample_rate = 48000, .bit_depth = 16});
            continue;
        }
        for (const std::int32_t channels : options_.pcm_channels) {
            formats.push_back(
                {.codec = codec, .channels = channels, .sample_rate = 48000, .bit_depth = 16});
            formats.push_back(
                {.codec = codec, .channels = channels, .sample_rate = 44100, .bit_depth = 16});
            formats.push_back(
                {.codec = codec, .channels = channels, .sample_rate = 48000, .bit_depth = 24});
        }
    }
    config.player_support = {.supported_formats = std::move(formats),
                             .buffer_capacity = options_.buffer_capacity,
                             .commands = {m::PlayerCommand::kVolume, m::PlayerCommand::kMute}};
    config.pair_methods = {{.method = m::PairMethod::kPairingPsk,
                            .locations = {m::SecretLocation::kOperator},
                            .out_channels = {},
                            .formats = {},
                            .min_pin_length = 0}};
    if (options_.code_method == CodeMethod::kDynamic) {
        config.pair_methods.push_back({.method = m::PairMethod::kDynamicCode,
                                       .locations = {},
                                       .out_channels = {m::OutChannel::kDisplay},
                                       .formats = {m::CodeFormat::kDigits, m::CodeFormat::kQrCode},
                                       .min_pin_length = 6});
    } else if (options_.code_method == CodeMethod::kStatic) {
        config.pair_methods.push_back({.method = m::PairMethod::kStaticCode,
                                       .locations = {m::SecretLocation::kOperator},
                                       .out_channels = {},
                                       .formats = {},
                                       .min_pin_length = 0});
        config.static_code = options_.static_code;
    }
    config.unpaired_access = options_.unpaired_access;
    config.player_state = {.volume = 100,
                           .muted = false,
                           .output_delay_ms = 0,
                           .required_lead_time_ms = options_.required_lead_time_ms,
                           .min_buffer_ms = options_.min_buffer_ms,
                           .supported_commands = std::vector<m::PlayerCommand>{m::PlayerCommand::kVolume,
                                                                               m::PlayerCommand::kMute},
                           .format = std::nullopt};
    const std::vector<ac::DataType> data_types = options_.data_types_now ? options_.data_types_now() : options_.data_types;
    if (options_.extension_role && !data_types.empty()) {
        config.supported_roles = {std::string(ac::kRole), "player@v1"};
        ac::Support support;
        support.data_types = data_types;
        support.sample_rates = options_.sample_rates;
        support.outputs.count = options_.output_count;
        support.outputs.bit_depth = 32;
        support.outputs.bit_depths = {32};
        support.layout_grammar = 1;
        // Nothing the server can manage: no routing, trims, delays, settings or identify tone.
        // Except the karaoke choice, when the sink is asked to take it.
        if (options_.karaoke) {
            support.decoder_settings = {"karaoke", "karaoke_vocals"};
            if (options_.karaoke_levels) {
                support.decoder_settings.insert(support.decoder_settings.end(),
                                                {"karaoke_melody_db", "karaoke_vocal1_db",
                                                 "karaoke_vocal2_db"});
            }
            if (options_.karaoke_eac3) {
                support.karaoke_codecs = {ac::DataType::kAc3, ac::DataType::kEac3};
            }
        }
        support.management.crossover_hz = options_.crossover_hz;
        support.buffer_capacity = options_.buffer_capacity;
        support.max_chunk_bytes = options_.max_chunk_bytes;
        if (options_.max_coded_channels != 0) {
            support.max_coded_channels.fill(options_.max_coded_channels);
        }
        if (options_.max_objects) {
            support.max_objects.fill(options_.max_objects);
        }
        if (options_.max_frame_rate != 0) {
            support.max_frame_rate.fill(options_.max_frame_rate);
        }
        if (options_.max_data_rate != 0) {
            support.max_data_rate.fill(options_.max_data_rate);
        }
        support.lyrics_max_bytes = options_.lyrics_max_bytes;
        support.requests = options_.requests;
        config.iclforge_support = std::move(support);
        // What the sink's streams are rendered to, as its own page would say.
        if (!options_.layout.empty()) {
            config.iclforge_state.layout = options_.layout;
        }
        config.iclforge_state.volume = 100;
        config.iclforge_state.muted = false;
        config.iclforge_state.required_lead_time_ms = options_.required_lead_time_ms;
        config.iclforge_state.min_buffer_ms = options_.min_buffer_ms;
        config.iclforge_state.supported_commands = {ac::Command::kVolume, ac::Command::kMute};
        if (options_.accept_settings || options_.karaoke) {
            config.iclforge_state.supported_commands.push_back(ac::Command::kSettings);
        }
        if (options_.lyrics_max_bytes != 0) {
            config.iclforge_state.supported_commands.push_back(ac::Command::kLyrics);
            config.iclforge_state.supported_commands.push_back(ac::Command::kLyricsPosition);
        }
    }
    for (const std::string& role : options_.other_roles) {
        config.supported_roles.push_back(role);
        if (role == sendspin::visualizer::kRole) {
            config.visualizer_support = sendspin::visualizer::Support{.buffer_capacity = 1024 * 1024};
        }
    }
    config.artwork_state = options_.artwork_channels;
    config.visualizer_state = options_.visualizer_request;

    Arbiter::Id id = 0;
    {
        const std::lock_guard lock(connections_mutex_);
        id = next_id_++;
    }
    std::shared_ptr<Connection> connection;
    {
        const std::lock_guard lock(*session_lock_);
        connection = std::make_shared<Connection>(*this, id, std::move(config));
    }
    {
        const std::lock_guard lock(connections_mutex_);
        connections_[id] = connection;
    }
    connection->start(std::move(transport));
}

void Sink::remove(Arbiter::Id id) {
    std::shared_ptr<Connection> ended;
    {
        const std::lock_guard lock(connections_mutex_);
        const auto found = connections_.find(id);
        if (found == connections_.end()) {
            return;
        }
        ended = std::move(found->second);
        connections_.erase(found);
    }
    arbiter_.ended(id);
    Totals counted;
    ended->driver().inspect([&] {
        const OutputCounts stream = ended->output().counts();
        const OutputCounts burst = ended->bursts().counts();
        counted.streams = stream.streams;
        counted.chunks = stream.units;
        counted.frames = stream.frames;
        counted.burst_streams = burst.streams;
        counted.bursts = burst.units;
        counted.burst_frames = burst.frames;
        return 0;
    });
    {
        const std::lock_guard lock(connections_mutex_);
        ended_totals_.streams += counted.streams;
        ended_totals_.chunks += counted.chunks;
        ended_totals_.frames += counted.frames;
        ended_totals_.burst_streams += counted.burst_streams;
        ended_totals_.bursts += counted.bursts;
        ended_totals_.burst_frames += counted.burst_frames;
    }
    log("[" + std::to_string(id) + "] closed");
}

void Sink::post(std::function<void()> work) {
    {
        const std::lock_guard lock(posted_mutex_);
        posted_.push_back(std::move(work));
    }
    posted_changed_.notify_all();
}

void Sink::run_posted() {
    std::unique_lock lock(posted_mutex_);
    while (true) {
        posted_changed_.wait(lock, [this] { return stopping_ || !posted_.empty(); });
        if (posted_.empty()) {
            return;
        }
        std::function<void()> work = std::move(posted_.front());
        posted_.pop_front();
        lock.unlock();
        work();
        lock.lock();
    }
}

void Sink::displace(Arbiter::Id id) {
    std::shared_ptr<Connection> displaced;
    {
        const std::lock_guard lock(connections_mutex_);
        const auto found = connections_.find(id);
        if (found != connections_.end()) {
            displaced = found->second;
        }
    }
    if (displaced) {
        log("[" + std::to_string(id) + "] displaced by another server");
        displaced->driver().call([&] { return displaced->session().displace(); });
    }
}

void Sink::for_each_connection(const std::function<void(Connection&)>& visit) {
    std::vector<std::shared_ptr<Connection>> all;
    {
        const std::lock_guard lock(connections_mutex_);
        for (const auto& [id, connection] : connections_) {
            all.push_back(connection);
        }
    }
    for (const std::shared_ptr<Connection>& connection : all) {
        visit(*connection);
    }
}

void Sink::open_window() {
    {
        const std::lock_guard lock(*session_lock_);
        pairing_state_.open_window(clock_.now_us());
    }
    log("pairing window open for five minutes");
    for_each_connection([](Connection& connection) {
        connection.driver().call([&] { return connection.session().resume_pairing(); });
    });
}

void Sink::reset_rounds() {
    {
        const std::lock_guard lock(*session_lock_);
        pairing_state_.reset_rounds();
    }
    log("round limit reset");
    for_each_connection([](Connection& connection) {
        connection.driver().call([&] { return connection.session().resume_pairing(); });
    });
}

void Sink::cancel_pairing() {
    for_each_connection([](Connection& connection) {
        connection.driver().call([&] { return connection.session().cancel_pairing(); });
    });
}

std::uint16_t Sink::port() const {
    return listener_ ? listener_->port() : std::uint16_t{0};
}

std::string Sink::client_id() const {
    return key_text(store_->identity().public_key());
}

std::string Sink::pairing_token() const {
    std::array<std::uint8_t, 64> payload{};
    std::copy(store_->identity().public_key().begin(), store_->identity().public_key().end(), payload.begin());
    std::copy(store_->pairing_psk().begin(), store_->pairing_psk().end(), payload.begin() + 32);
    return sendspin::pairing::encode_token(sendspin::pairing::TokenVersion::kPairingPsk, payload);
}

Sink::Totals Sink::totals() const {
    Totals totals;
    std::vector<std::shared_ptr<Connection>> all;
    {
        const std::lock_guard lock(connections_mutex_);
        totals = ended_totals_;
        for (const auto& [id, connection] : connections_) {
            all.push_back(connection);
        }
    }
    totals.connections = static_cast<std::uint32_t>(all.size());
    for (const std::shared_ptr<Connection>& connection : all) {
        connection->driver().inspect([&] {
            const OutputCounts stream = connection->output().counts();
            const OutputCounts burst = connection->bursts().counts();
            totals.streams += stream.streams;
            totals.chunks += stream.units;
            totals.frames += stream.frames;
            totals.burst_streams += burst.streams;
            totals.bursts += burst.units;
            totals.burst_frames += burst.frames;
            return 0;
        });
    }
    return totals;
}

Sink::Roles Sink::roles() const {
    const std::lock_guard lock(roles_mutex_);
    return roles_;
}

void Sink::set_external_source(bool external) {
    for_each_connection([&](Connection& connection) {
        connection.driver().call([&] { return connection.session().set_external_source(external); });
    });
}

void Sink::send_controller_command(const sendspin::controller::CommandMessage& command) {
    for_each_connection([&](Connection& connection) {
        connection.driver().call([&] { return connection.session().send_command(command); });
    });
}

Sink::Status Sink::status() const {
    std::vector<std::shared_ptr<Connection>> all;
    {
        const std::lock_guard lock(connections_mutex_);
        for (const auto& [id, connection] : connections_) {
            all.push_back(connection);
        }
    }
    Status out;
    for (const std::shared_ptr<Connection>& connection : all) {
        out.connections.push_back(connection->peer_status());
        if (!out.pairing_code) {
            out.pairing_code = connection->pairing_code();
        }
    }
    {
        // The records are added from a session's callback, under the sessions' shared lock.
        const std::lock_guard lock(*session_lock_);
        out.paired_servers = store_->records();
    }
    return out;
}

LyricFrame Sink::lyric_frame(std::int64_t delay_ms, const iclforge::lyrics::StageOptions& options) const {
    std::vector<std::shared_ptr<Connection>> all;
    {
        const std::lock_guard lock(connections_mutex_);
        for (const auto& [id, connection] : connections_) {
            all.push_back(connection);
        }
    }
    // The document of the first connection that has one: a sink plays for one server at a time, and
    // the arbiter keeps the others from replacing what is playing.
    LyricFrame frame;
    bool found = false;
    bool streaming = false;
    std::optional<sendspin::metadata::State> metadata;
    std::optional<Connection::MetadataProgress> progress;
    for (const std::shared_ptr<Connection>& connection : all) {
        if (!found && connection->has_lyric_item()) {
            frame = connection->lyric_frame(delay_ms, options);
            found = true;
            // What the server that sent the document says is playing, if it says.
            metadata = connection->metadata();
            progress = connection->metadata_progress();
        } else if (!found && !metadata) {
            // No document held yet: a server that says what is playing still names the item.
            metadata = connection->metadata();
            progress = connection->metadata_progress();
        }
        streaming = streaming || connection->streaming();
    }
    frame.streaming = streaming;
    // What the server says is playing (metadata@v1) names the item first, and the document's own
    // tags are the fallback (D7): an item with no lyrics still has a title to show.
    if (metadata) {
        const auto name = [](const std::optional<std::string>& said, std::string& into) {
            if (said && !said->empty()) {
                into = *said;
            }
        };
        name(metadata->title, frame.title);
        name(metadata->artist, frame.artist);
        name(metadata->album, frame.album);
    }
    // The header's time. A document's own playhead is the better clock, so the server's progress is
    // the position only where the stage has none (no document held, or one that is plain text), and
    // the length only where the document has no `[length:]`.
    if (progress) {
        const bool unstaged = frame.mode == iclforge::lyrics::StageMode::kNoLyrics ||
                              frame.mode == iclforge::lyrics::StageMode::kPlain;
        if (unstaged) {
            frame.position_ms = progress->position_ms;
            frame.length_ms = progress->length_ms;
            frame.has_progress = true;
        } else if (!frame.length_ms) {
            frame.length_ms = progress->length_ms;
        }
    }
    return frame;
}

sendspin::player::Request vocals_request(sendspin::player::KaraokeVocals vocals) {
    sendspin::player::Request request;
    request.decoder.karaoke = vocals == sendspin::player::KaraokeVocals::kBoth ? sendspin::player::Karaoke::kAware
                                                                              : sendspin::player::Karaoke::kCapable;
    request.decoder.karaoke_vocals = vocals;
    return request;
}

void Sink::send_request(const sendspin::player::Request& request) {
    for_each_connection([&](Connection& connection) {
        connection.driver().call([&] { return connection.session().send_iclforge_request(request); });
    });
}

void Sink::forget_lyrics() {
    for_each_connection([&](Connection& connection) { connection.forget_lyrics(); });
}

void Sink::log(std::string_view text) {
    log_->line(text);
}

}  // namespace iclforge::sink
