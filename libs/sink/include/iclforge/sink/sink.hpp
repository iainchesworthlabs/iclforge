#pragma once

#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "iclforge/sendspin/arbiter.hpp"
#include "iclforge/sendspin/chunks.hpp"
#include "iclforge/sendspin/discovery.hpp"
#include "iclforge/sendspin/iclforge_player.hpp"
#include "iclforge/sendspin/messages.hpp"
#include "iclforge/sendspin/pairing_flow.hpp"
#include "iclforge/sendspin/session.hpp"
#include "iclforge/sendspin/session_driver.hpp"
#include "iclforge/sendspin/state_roles.hpp"
#include "iclforge/sendspin/stream_roles.hpp"
#include "iclforge/sendspin/websocket.hpp"
#include "iclforge/lyrics/stage.hpp"
#include "iclforge/sink/lyric_frame.hpp"
#include "iclforge/sink/store.hpp"

// iclforge::sink: libs/sendspin's player half as a sink that a program runs (planning/hearth-
// android.md, D3). A Sendspin client that waits for servers: it listens on a WebSocket, advertises
// _sendspin._tcp, pairs by its pairing PSK and a dynamic or static code, admits servers as the
// specification ranks them, and hands each stream it is sent to an output the program supplies:
// PCM, FLAC or Opus over player@v1, and AC-3 or E-AC-3, objects included, or AC-4, over
// _iclforge_player@v1 (planning/hearth-sendspin-extension.md). It can list the other roles too,
// keeping what a server sends them and sending controller commands.
//
// It was apps/hearth/testsink's, whose outputs write a WAV file and decode a burst; it knows
// nothing of them now. What a stream becomes is the program's: hearth-testsink writes files, and
// the Android app gives the receiver the bytes (apps/hearth/android).
//
// Several sinks run side by side in one process or several, with distinct names, ports and state
// directories.

namespace iclforge::sink {

enum class CodeMethod : std::uint8_t {
    kNone,
    kDynamic,
    kStatic,
};

struct Options {
    // The name servers show, and the instance name it is advertised under.
    std::string name = "Sendspin sink";
    // DeviceInfo, as the server's pages show it: what the program is, and who makes it.
    std::string product_name;
    std::string manufacturer = "ICL Forge";
    std::string address = "0.0.0.0";
    std::uint16_t port = sendspin::transport::websocket::kClientPort;
    // The identity, pairing PSK and pairing records.
    std::filesystem::path state_directory;
    bool unpaired_access = false;
    // The codecs offered, most preferred first: each as stereo at 48 kHz 16-bit, and PCM and FLAC
    // also at 44.1 kHz 16-bit and 48 kHz 24-bit.
    std::vector<sendspin::messages::Codec> codecs{sendspin::messages::Codec::kPcm, sendspin::messages::Codec::kFlac,
                                                  sendspin::messages::Codec::kOpus};
    // The channel counts PCM and FLAC are offered at, each of the formats above (Opus stays at
    // two): a player that renders to a layout wider than stereo lists that width.
    std::vector<std::int32_t> pcm_channels{2};
    // The coded formats the extension role lists: what a server may send as bursts. A program lists
    // the ones its output takes; the test sink's decoders take all four.
    std::vector<sendspin::player::DataType> data_types{
        sendspin::player::DataType::kAc3, sendspin::player::DataType::kEac3, sendspin::player::DataType::kAc4,
        sendspin::player::DataType::kTruehd};
    // When set, asked for each connection instead of `data_types`: what the output takes now. A
    // route comes and goes with a TV (an Android box loses its HDMI output when the display sleeps),
    // so what a program could take when the sink started is not what it can when a server dials.
    // An empty answer leaves the extension role unlisted for that connection.
    std::function<std::vector<sendspin::player::DataType>()> data_types_now;
    // Offer _iclforge_player@v1 before player@v1, as a Hearth sink does.
    bool extension_role = true;
    // Lists the Settings command in the extension role's state and applies
    // a settings command by reporting its revision (logged "settings N
    // applied") - for a server-side test of a sink that takes settings.
    // Off by default: the sink manages nothing (see Sink's config).
    bool accept_settings = false;
    // Lists karaoke and karaoke_vocals in the extension role's decoder settings, takes the
    // settings command (as accept_settings does), and hands a settings command's karaoke keys to
    // the burst output, which reproduces a karaoke AC-3 stream as they say: a sink with a decoder
    // that does what a board's does (planning/karaoke-phase2.md, W7). Without it a karaoke stream
    // is played as coded, as a sink from before the keys plays it.
    bool karaoke = false;
    // With `karaoke`: also lists E-AC-3 among the codecs it reproduces karaoke for (the support
    // object's `karaoke_codecs`), as a board whose firmware has the E-AC-3 path does
    // (planning/karaoke-eac3.md, D6). Off, it lists none, which a server reads as AC-3 alone: a
    // board from before it. The decoder is the library's either way, so an E-AC-3 stream sent to
    // an off sink is reproduced all the same; it is the server's choice, which is what the
    // sink is for, that is under test.
    bool karaoke_eac3 = false;
    // With `karaoke`: also lists karaoke_melody_db, karaoke_vocal1_db and karaoke_vocal2_db and
    // applies them (W10), as a board with the part levels does. Off, it is a board from before
    // them: it reproduces karaoke and has no way to be told a level.
    bool karaoke_levels = true;
    // The most bytes of lyric document the sink says it holds, and lists the `lyrics` and
    // `lyrics_position` commands (W7d, planning/karaoke-phase2.md); 0 lists neither, as a board
    // from before them, or one with no heap to spare. What it receives is kept (Sink::roles()).
    std::uint32_t lyrics_max_bytes = 0;
    // The decoder keys the sink may ask a server to change (the extension role's support `requests`:
    // any of sendspin::player::kRequestableKeys), for a sink whose screen or remote chooses the
    // singer's karaoke choice (D8, planning/hearth-android.md). Empty lists none, and request()
    // then sends nothing.
    std::vector<std::string> requests;
    // What the extension role's streams are rendered to, as its support and state objects say it:
    // the layout in the layout grammar (state.layout; none is stated when it is empty, as for a program
    // that bitstreams and renders nothing), the number of outputs (support.outputs.count) and the
    // crossover's range in Hz (support.management.crossover_hz). The sink does not render: the
    // program's output does, and says here what it renders to.
    std::string layout;
    std::int32_t output_count = 2;
    std::array<double, 2> crossover_hz{0.0, 0.0};
    // Roles beyond the playback roles to list, for testing a server's: any of controller@v1,
    // metadata@v1, color@v1, artwork@v1 and visualizer@v1. What they receive is kept (Sink::roles).
    std::vector<std::string> other_roles;
    // artwork@v1's channels, and visualizer@v1's request, when those roles are listed.
    sendspin::artwork::Channels artwork_channels;
    sendspin::visualizer::State visualizer_request;
    CodeMethod code_method = CodeMethod::kDynamic;
    // Eight digits, for CodeMethod::kStatic.
    std::string static_code;
    bool advertise = true;
    // The IPv4 interfaces to advertise on; empty for every one.
    std::vector<std::string> mdns_interfaces;
    std::size_t max_connections = 4;
    // The buffer the sink says it has, in bytes (player@v1's and the extension role's
    // buffer_capacity): what a server may have sent that has not played yet. A board's is a
    // few tens of KB.
    std::uint64_t buffer_capacity = 32ULL * 1024 * 1024;
    // What the sink asks of a server in time (both roles' state): how far ahead of its play time a
    // chunk must be sent to be played on time, and the least the sink wants buffered. A device that
    // takes time to start (an AudioTrack, which needs the whole of its buffer queued before it moves)
    // asks for more than a sink that writes a file.
    std::int32_t required_lead_time_ms = 500;
    std::int32_t min_buffer_ms = 200;
    // The most bytes one chunk to the sink may be, which the extension role's support object
    // states; 0 states none, as a sink from before the key does.
    std::uint32_t max_chunk_bytes = 0;
    // The most channels the extension role's support object states it decodes, for every data
    // type it lists; 0 states none, as a sink from before the key does.
    std::uint8_t max_coded_channels = 0;
    // The most objects the extension role's support object states it places, for every data type
    // it lists; unset states none. 0 is a statement: bed only.
    std::optional<std::uint8_t> max_objects;
    // The most frames a second the extension role's support object states it decodes, for every
    // data type it lists; 0 states none.
    std::uint16_t max_frame_rate = 0;
    // The most coded kbit/s the extension role's support object states it takes, for every data
    // type it lists; 0 states none.
    std::uint32_t max_data_rate = 0;
    // The sample rates the extension role lists for the coded streams it takes.
    std::vector<std::int32_t> sample_rates{48000};
};

// What a sink reports, one line at a time, from any of its threads.
class Log {
   public:
    Log() = default;
    virtual ~Log() = default;
    Log(const Log&) = delete;
    Log& operator=(const Log&) = delete;
    Log(Log&&) = delete;
    Log& operator=(Log&&) = delete;

    virtual void line(std::string_view text) = 0;
};

// What an output has been given, for the sink's totals and its log.
struct OutputCounts {
    // Streams started, and the chunks (player@v1) or bursts (the extension role) and the frames
    // those made.
    std::uint32_t streams = 0;
    std::uint64_t units = 0;
    std::uint64_t frames = 0;
    // What went wrong at the output, which the extension role's state reports as its counters: gaps
    // the device played where there was no audio, units that came after their time, and units that
    // were dropped (late ones, or ones that could not be taken).
    std::uint64_t underruns = 0;
    std::uint64_t late = 0;
    std::uint64_t dropped = 0;
};

// Where one connection's player@v1 stream goes: PCM, FLAC or Opus, which an output decodes or
// hands on. Not thread-safe: the sink calls it from the session's callbacks, under the session's
// lock, and from `driver().inspect` for the totals.
class StreamOutput {
   public:
    StreamOutput() = default;
    virtual ~StreamOutput() = default;
    StreamOutput(const StreamOutput&) = delete;
    StreamOutput& operator=(const StreamOutput&) = delete;
    StreamOutput(StreamOutput&&) = delete;
    StreamOutput& operator=(StreamOutput&&) = delete;

    // A stream began, or changed format. False for a stream this output cannot take, whose audio
    // is then counted and dropped.
    virtual bool start(const sendspin::messages::PlayerStream& stream) = 0;
    // stream/clear: what was buffered is dropped.
    virtual void clear() = 0;
    virtual void end() = 0;
    // One chunk to be played from `local_time`, in the sink's steady clock's microseconds.
    virtual void write(std::span<const std::uint8_t> frame, std::int64_t local_time) = 0;
    [[nodiscard]] virtual OutputCounts counts() const = 0;
    // The file the current or last stream went to, for the log; empty when it went to none.
    [[nodiscard]] virtual const std::filesystem::path& file() const = 0;
};

// Where one connection's _iclforge_player@v1 stream goes: the bursts of AC-3, E-AC-3 or AC-4,
// which an output decodes, or hands to a receiver as they are. Called as StreamOutput is.
class BurstStreamOutput {
   public:
    BurstStreamOutput() = default;
    virtual ~BurstStreamOutput() = default;
    BurstStreamOutput(const BurstStreamOutput&) = delete;
    BurstStreamOutput& operator=(const BurstStreamOutput&) = delete;
    BurstStreamOutput(BurstStreamOutput&&) = delete;
    BurstStreamOutput& operator=(BurstStreamOutput&&) = delete;

    // A stream began, or changed in place. False for a stream this output cannot take.
    virtual bool start(const sendspin::player::StreamStart& stream) = 0;
    virtual void clear() = 0;
    virtual void end() = 0;
    // One burst to be played from `local_time`.
    virtual void write(const sendspin::BurstChunk& chunk, std::int64_t local_time) = 0;
    // Annex C's reproduction of a karaoke AC-3 stream, and which vocals the capable one plays,
    // with the melody's, vocal 1's and vocal 2's levels in dB against the table (an absent key of
    // the settings command is 0): what a settings command's decoder object says, handed on when
    // Options::karaoke is on. An output that does not reproduce ignores it.
    virtual void set_karaoke(sendspin::player::Karaoke reproduction, sendspin::player::KaraokeVocals vocals,
                             const std::array<double, 3>& levels_db) = 0;
    [[nodiscard]] virtual OutputCounts counts() const = 0;
    [[nodiscard]] virtual const std::filesystem::path& file() const = 0;
    // What the output found in the current stream, once it has: the role's `decoder` state.
    [[nodiscard]] virtual const std::optional<sendspin::player::DecoderReport>& decoder() const = 0;
    // How a stream this output took leaves the sink, which the role's `output` state says (D6,
    // planning/hearth-android.md): handed to a receiver as it was coded (kPassthrough), or decoded
    // here and played as samples (kPcm). Unset for an output that does not say, which a server reads
    // as not known; the sink reports it only while a stream the output took is open.
    [[nodiscard]] virtual std::optional<sendspin::player::Output> output() const { return std::nullopt; }
};

// What a sink asks of its program for each connection: the two outputs, made when the connection
// is. `connection` is the sink's own number for it, from 1, which a program may name files by.
// Called from the sink's thread that accepts a connection, with the session lock held.
class Outputs {
   public:
    Outputs() = default;
    virtual ~Outputs() = default;
    Outputs(const Outputs&) = delete;
    Outputs& operator=(const Outputs&) = delete;
    Outputs(Outputs&&) = delete;
    Outputs& operator=(Outputs&&) = delete;

    [[nodiscard]] virtual std::unique_ptr<StreamOutput> stream(std::uint64_t connection) = 0;
    [[nodiscard]] virtual std::unique_ptr<BurstStreamOutput> bursts(std::uint64_t connection) = 0;
};

// The request a sink's remote makes when the singer picks a vocals choice (D8,
// planning/hearth-android.md). "Both" asks for the aware reproduction, which plays both voices as
// the song's mix does; the others ask for the capable one with that choice, the only reproduction
// that has them. The owner's call, 2026-10-13. Advice only: see Sink::send_request.
[[nodiscard]] sendspin::player::Request vocals_request(sendspin::player::KaraokeVocals vocals);

class Connection;

class Sink {
   public:
    // `outputs` and `log` must outlive the sink.
    [[nodiscard]] static std::expected<std::unique_ptr<Sink>, std::string> start(Options options, Outputs& outputs,
                                                                                 Log& log);
    ~Sink();
    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;
    Sink(Sink&&) = delete;
    Sink& operator=(Sink&&) = delete;

    [[nodiscard]] std::uint16_t port() const;
    // client_id, and the SP:0 token that pairs this sink by its pairing PSK.
    [[nodiscard]] std::string client_id() const;
    [[nodiscard]] std::string pairing_token() const;

    // The operator's actions on the device: a gesture opening the static code's window, a reset
    // of the round limit, and cancelling the attempt in progress.
    void open_window();
    void reset_rounds();
    void cancel_pairing();

    // Another source holds the device (audio focus was lost to a call, a navigation voice, another
    // app): tells every server so, as the specification's `external_source`, and again when it is
    // given back. The output is the program's to pause.
    void set_external_source(bool external);

    // Totals over every connection, for tests.
    struct Totals {
        std::uint32_t connections = 0;
        std::uint32_t streams = 0;
        std::uint64_t chunks = 0;
        std::uint64_t frames = 0;
        // _iclforge_player@v1's.
        std::uint32_t burst_streams = 0;
        std::uint64_t bursts = 0;
        std::uint64_t burst_frames = 0;
    };
    [[nodiscard]] Totals totals() const;

    // What a screen says of the sink (planning/hearth-sink-screen.md, the model): who is connected and
    // what each is doing here, the code a server asked to pair by that has not yet been typed there,
    // and how many servers this sink holds a pairing record for.
    struct Status {
        enum class Activity : std::uint8_t { kNothing, kPairing, kPlayback };
        struct Peer {
            std::uint64_t id = 0;
            // The peer's address, as the transport saw it.
            std::string address;
            // What the server called itself; empty until it has said.
            std::string server_name;
            Activity activity = Activity::kNothing;
            // A stream is open on this connection.
            bool streaming = false;
            // How that stream leaves the sink, as the role's state says it to the server: unset
            // with no stream, with one the output did not take, and for an output that does not say.
            std::optional<sendspin::player::Output> output;
            // What the output found in the stream's first syncframe, the role's `decoder` state, for
            // the screen's format chip: unset with no stream the output took, and until it has found
            // something.
            std::optional<sendspin::player::DecoderReport> decoder;
        };
        std::vector<Peer> connections;
        std::optional<std::string> pairing_code;
        std::size_t paired_servers = 0;
    };
    [[nodiscard]] Status status() const;

    // What the other roles have received, over every connection: the server/state messages and the
    // latest state of each state role, none once cleared; the last image on each artwork channel,
    // empty once cleared; and the visualizer streams and frames.
    struct Roles {
        std::uint32_t states = 0;
        std::optional<sendspin::metadata::State> metadata;
        std::optional<sendspin::controller::State> controller;
        std::optional<sendspin::color::State> colors;
        std::uint32_t artwork_streams = 0;
        std::map<std::size_t, std::vector<std::uint8_t>> images;
        std::optional<sendspin::visualizer::StreamStart> visualizer;
        std::uint64_t visualizer_frames = 0;
        // The extension role's lyric commands, in the order they came, over every connection.
        std::vector<sendspin::player::CommandMessage> lyric_commands;
    };
    [[nodiscard]] Roles roles() const;
    // A controller@v1 command, sent on every connection where the role is active.
    void send_controller_command(const sendspin::controller::CommandMessage& command);
    // Asks the server of every connection where the role is active to change the singer's karaoke
    // choice, as the sink's screen or remote chose it. Cut to Options::requests, and advice: the
    // server decides, and what it did comes back as the settings the sink is sent.
    void send_request(const sendspin::player::Request& request);
    // The sink loses the lyrics it holds and says so on every connection, as a board does that
    // restarted its player, or reconnected: a test of what a server does about a member that
    // reports no lyrics (or another item's) for the item it is playing.
    void forget_lyrics();

    // What a screen draws of the lyrics now (lyric_frame.hpp): the document the sink holds for the
    // item playing, followed on the sink's own clock by the server's anchors, less `delay_ms`, the
    // singer's nudge (positive shows the lyrics later). A sink that holds none answers
    // StageMode::kNoLyrics, with `streaming` saying whether a stream is open. Safe to call from a
    // thread of the program's own, as the totals are.
    [[nodiscard]] LyricFrame lyric_frame(std::int64_t delay_ms = 0,
                                         const lyrics::StageOptions& options = {}) const;

   private:
    friend class Connection;

    Sink(Options options, Outputs& outputs, Log& log, std::unique_ptr<Store> store);
    void accept(std::unique_ptr<sendspin::transport::Connection> transport);
    // Runs `work` on the sink's own thread, where connections may be called and destroyed.
    void post(std::function<void()> work);
    void run_posted();
    // Tells connection `id` that another server has taken the sink.
    void displace(sendspin::Arbiter::Id id);
    // Forgets connection `id`, whose connection has ended.
    void remove(sendspin::Arbiter::Id id);
    void for_each_connection(const std::function<void(Connection&)>& visit);
    void log(std::string_view text);

    Options options_;
    Outputs* outputs_;
    Log* log_;
    std::unique_ptr<Store> store_;
    sendspin::SteadyClock clock_;
    sendspin::Arbiter arbiter_;
    // Every connection's session lock, which also guards the pairing state they share.
    std::shared_ptr<std::mutex> session_lock_ = std::make_shared<std::mutex>();
    sendspin::pairing_flow::ClientPairingState pairing_state_;

    mutable std::mutex connections_mutex_;
    std::map<sendspin::Arbiter::Id, std::shared_ptr<Connection>> connections_;
    std::uint64_t next_id_ = 1;
    Totals ended_totals_;

    mutable std::mutex roles_mutex_;
    Roles roles_;

    std::mutex posted_mutex_;
    std::condition_variable posted_changed_;
    std::deque<std::function<void()>> posted_;
    bool stopping_ = false;
    std::thread worker_;

    std::unique_ptr<sendspin::transport::websocket::Listener> listener_;
    std::unique_ptr<sendspin::discovery::Advertiser> advertiser_;
};

}  // namespace iclforge::sink
