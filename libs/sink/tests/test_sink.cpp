#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "iclforge/sendspin/chunks.hpp"
#include "iclforge/sendspin/crypto.hpp"
#include "iclforge/sendspin/handshake.hpp"
#include "iclforge/sendspin/iclforge_player.hpp"
#include "iclforge/sendspin/messages.hpp"
#include "iclforge/sendspin/noise.hpp"
#include "iclforge/sendspin/pairing_messages.hpp"
#include "iclforge/sendspin/server_session.hpp"
#include "iclforge/sendspin/session_driver.hpp"
#include "iclforge/sendspin/websocket.hpp"
#include "iclforge/sink/sink.hpp"
#include "iclforge/sink/status_json.hpp"
#include "iclforge/sink/store.hpp"

// iclforge::sink with no program behind it: outputs that only write down what they were given. What
// libs/sink promises is the seam: each connection asks the program for its outputs, a stream's
// start, its audio and its end reach them as the server sent them, and what the output says it has
// counted is what the sink reports. hearth-testsink's own suites (apps/hearth/engine/tests) hold the
// rest of the sink's behaviour, through the program's real outputs.
//
// It dials a loopback WebSocket, so under ThreadSanitizer it needs what libs/sendspin/tests/
// test_websocket.cpp says.

namespace {

namespace fs = std::filesystem;
namespace m = iclforge::sendspin::messages;
namespace hs = iclforge::sendspin::handshake;
namespace ac = iclforge::sendspin::player;
namespace websocket = iclforge::sendspin::transport::websocket;
using iclforge::sendspin::crypto::Key32;
using namespace std::chrono_literals;

// Every line the sink logs, for a case to wait on.
class Lines final : public iclforge::sink::Log {
   public:
    void line(std::string_view text) override {
        {
            const std::lock_guard lock(mutex_);
            lines_.emplace_back(text);
        }
        changed_.notify_all();
    }

    bool wait(std::string_view needle, std::chrono::milliseconds timeout = 10s) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] {
            return std::ranges::any_of(lines_,
                                       [&](const std::string& l) { return l.find(needle) != std::string::npos; });
        });
    }

    // The pairing code of the last "PAIRING CODE " line, as the sink shows it (digits and a dash).
    [[nodiscard]] std::optional<std::string> code() {
        const std::lock_guard lock(mutex_);
        for (auto it = lines_.rbegin(); it != lines_.rend(); ++it) {
            const std::size_t at = it->find("PAIRING CODE ");
            if (at != std::string::npos) {
                return it->substr(at + 13);
            }
        }
        return std::nullopt;
    }

   private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::string> lines_;
};

// What the program's outputs were given, shared by both of a connection's outputs and read by the
// case's thread.
struct Recording {
    std::mutex mutex;
    std::vector<std::uint64_t> stream_connections;
    std::vector<std::uint64_t> burst_connections;
    // player@v1
    std::vector<m::PlayerStream> started;
    std::vector<std::vector<std::uint8_t>> chunks;
    std::vector<std::int64_t> chunk_times;
    int ended = 0;
    // _iclforge_player@v1
    std::vector<ac::StreamStart> burst_started;
    struct Burst {
        std::uint16_t pc = 0;
        std::uint16_t pd = 0;
        std::vector<std::uint8_t> payload;
    };
    std::vector<Burst> bursts;
    int burst_ended = 0;
    struct Karaoke {
        ac::Karaoke reproduction = ac::Karaoke::kOff;
        ac::KaraokeVocals vocals = ac::KaraokeVocals::kBoth;
        std::array<double, 3> levels{};
    };
    std::vector<Karaoke> karaoke;
    // What the burst output answers to start(), and what it says its streams do (D6).
    bool burst_takes = true;
    std::optional<ac::Output> burst_output;
    // What it says it found in a stream it took (the role's `decoder` state).
    std::optional<ac::DecoderReport> burst_decoder;
    // What the output says it has counted, as the program's would.
    std::uint32_t streams = 0;
    std::uint32_t burst_streams = 0;
};

class FakeStream final : public iclforge::sink::StreamOutput {
   public:
    explicit FakeStream(Recording& recording) : recording_(&recording) {}

    bool start(const m::PlayerStream& stream) override {
        const std::lock_guard lock(recording_->mutex);
        recording_->started.push_back(stream);
        ++recording_->streams;
        return true;
    }
    void clear() override {}
    void end() override {
        const std::lock_guard lock(recording_->mutex);
        ++recording_->ended;
    }
    void write(std::span<const std::uint8_t> frame, std::int64_t local_time) override {
        const std::lock_guard lock(recording_->mutex);
        recording_->chunks.emplace_back(frame.begin(), frame.end());
        recording_->chunk_times.push_back(local_time);
    }
    [[nodiscard]] iclforge::sink::OutputCounts counts() const override {
        const std::lock_guard lock(recording_->mutex);
        std::uint64_t frames = 0;
        for (const auto& chunk : recording_->chunks) {
            frames += chunk.size() / 4;
        }
        return {.streams = recording_->streams, .units = recording_->chunks.size(), .frames = frames};
    }
    [[nodiscard]] const fs::path& file() const override { return file_; }

   private:
    Recording* recording_;
    fs::path file_;
};

class FakeBursts final : public iclforge::sink::BurstStreamOutput {
   public:
    explicit FakeBursts(Recording& recording) : recording_(&recording) {}

    bool start(const ac::StreamStart& stream) override {
        const std::lock_guard lock(recording_->mutex);
        recording_->burst_started.push_back(stream);
        ++recording_->burst_streams;
        decoder_ = recording_->burst_decoder;
        return recording_->burst_takes;
    }
    void clear() override {}
    void end() override {
        const std::lock_guard lock(recording_->mutex);
        ++recording_->burst_ended;
    }
    void write(const iclforge::sendspin::BurstChunk& chunk, std::int64_t /*local_time*/) override {
        const std::lock_guard lock(recording_->mutex);
        recording_->bursts.push_back({.pc = chunk.pc,
                                      .pd = chunk.pd,
                                      .payload = {chunk.chunk.data.begin(), chunk.chunk.data.end()}});
    }
    void set_karaoke(ac::Karaoke reproduction, ac::KaraokeVocals vocals,
                     const std::array<double, 3>& levels_db) override {
        const std::lock_guard lock(recording_->mutex);
        recording_->karaoke.push_back({.reproduction = reproduction, .vocals = vocals, .levels = levels_db});
    }
    [[nodiscard]] iclforge::sink::OutputCounts counts() const override {
        const std::lock_guard lock(recording_->mutex);
        return {.streams = recording_->burst_streams, .units = recording_->bursts.size(), .frames = 0};
    }
    [[nodiscard]] const fs::path& file() const override { return file_; }
    [[nodiscard]] const std::optional<ac::DecoderReport>& decoder() const override { return decoder_; }
    [[nodiscard]] std::optional<ac::Output> output() const override {
        const std::lock_guard lock(recording_->mutex);
        return recording_->burst_output;
    }

   private:
    Recording* recording_;
    fs::path file_;
    std::optional<ac::DecoderReport> decoder_;
};

class FakeOutputs final : public iclforge::sink::Outputs {
   public:
    explicit FakeOutputs(Recording& recording) : recording_(&recording) {}

    [[nodiscard]] std::unique_ptr<iclforge::sink::StreamOutput> stream(std::uint64_t connection) override {
        {
            const std::lock_guard lock(recording_->mutex);
            recording_->stream_connections.push_back(connection);
        }
        return std::make_unique<FakeStream>(*recording_);
    }
    [[nodiscard]] std::unique_ptr<iclforge::sink::BurstStreamOutput> bursts(std::uint64_t connection) override {
        {
            const std::lock_guard lock(recording_->mutex);
            recording_->burst_connections.push_back(connection);
        }
        return std::make_unique<FakeBursts>(*recording_);
    }

   private:
    Recording* recording_;
};

class Keys final : public hs::ServerKeyring {
   public:
    [[nodiscard]] hs::PskChoice choose(const Key32& /*client_key*/) const override { return hs::sentinel_choice(); }
};

class Events final : public iclforge::sendspin::ServerListener {
   public:
    void on_hello(const m::ClientHello& /*hello*/) override { update([&] { ++hellos; }); }
    void on_state(const m::ClientState& state) override {
        update([&] {
            available = state.available;
            if (state.iclforge) {
                ++iclforge_states;
                iclforge_output = state.iclforge->output;
            }
        });
    }
    void on_goodbye(m::GoodbyeReason /*reason*/) override {}
    void on_leave() override {}
    void on_pairing_held_back(const std::optional<std::string>& /*message*/) override {}
    void on_pairing_code_wanted() override { update([&] { code_wanted = true; }); }
    bool on_paired(const Key32& /*client_key*/, const Key32& /*psk*/) override {
        update([&] { paired = true; });
        return true;
    }
    void on_pairing_ended(std::optional<iclforge::sendspin::pairing_messages::AbortReason> /*reason*/) override {}

    template <class Predicate>
    bool wait(Predicate&& predicate, std::chrono::milliseconds timeout = 10s) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, std::forward<Predicate>(predicate));
    }

    // The extension role's client/state messages so far, and the `output` of the last.
    struct Extension {
        int states = 0;
        std::optional<ac::Output> output;
    };
    [[nodiscard]] Extension extension() {
        const std::lock_guard lock(mutex_);
        return {.states = iclforge_states, .output = iclforge_output};
    }

    int hellos = 0;
    bool available = false;
    bool code_wanted = false;
    bool paired = false;
    int iclforge_states = 0;
    std::optional<ac::Output> iclforge_output;

   private:
    template <class Update>
    void update(Update&& change) {
        {
            const std::lock_guard lock(mutex_);
            change();
        }
        changed_.notify_all();
    }

    std::mutex mutex_;
    std::condition_variable changed_;
};

// One server dialled at a sink, active for `roles`.
struct Dialled {
    iclforge::sendspin::SteadyClock clock;
    Keys keys;
    Events events;
    iclforge::sendspin::ServerSession server;
    std::unique_ptr<iclforge::sendspin::SessionDriver> driver;

    Dialled(const iclforge::sink::Sink& sink, const iclforge::sendspin::noise::KeyPair& identity)
        : server({.identity = identity, .name = "Test server", .languages = {}, .max_message_bytes = 1 << 22}, keys,
                 events, clock) {
        auto dialled = websocket::connect("ws://127.0.0.1:" + std::to_string(sink.port()) + "/sendspin");
        REQUIRE(dialled.has_value());
        driver = std::make_unique<iclforge::sendspin::SessionDriver>(
            std::move(*dialled), iclforge::sendspin::DrivenSession{
                                     .receive = [this](const auto& frame) { return server.receive(frame); },
                                     .tick = [this] { return server.tick(); },
                                     .next_tick_us = [this] { return server.next_tick_us(); },
                                     .ended = {}});
        driver->start();
        REQUIRE(events.wait([&] { return events.hellos >= 1; }));
    }

    ~Dialled() {
        driver->close();
        driver->join();
    }
    Dialled(const Dialled&) = delete;
    Dialled& operator=(const Dialled&) = delete;
    Dialled(Dialled&&) = delete;
    Dialled& operator=(Dialled&&) = delete;

    template <class Call>
    bool call(Call&& work) {
        return driver->call(std::forward<Call>(work)).has_value();
    }

    bool play(std::vector<std::string> roles) {
        return call([&] {
            return server.activate(
                {.activities = {m::Activity::kPlayback}, .active_roles = std::move(roles), .pairing = std::nullopt});
        });
    }

    bool pair(m::PairMethod method, std::optional<m::CodeFormat> format, std::int32_t pin_length) {
        return call([&] {
            return server.activate({.activities = {m::Activity::kPairing},
                                    .active_roles = std::vector<std::string>{},
                                    .pairing = m::PairingActivation{
                                        .method = method, .format = format, .pin_length = pin_length, .languages = {}}});
        });
    }
};

iclforge::sendspin::noise::KeyPair generated() {
    std::optional<iclforge::sendspin::noise::KeyPair> pair = iclforge::sendspin::noise::KeyPair::generate();
    REQUIRE(pair.has_value());
    return *pair;
}

fs::path scratch(std::string_view leaf) {
    const fs::path dir = fs::path{ICLFORGE_TEST_SCRATCH_DIR} / ("iclforge_sink_" + std::string(leaf));
    fs::remove_all(dir);
    return dir;
}

iclforge::sink::Options options_in(const fs::path& dir) {
    iclforge::sink::Options options;
    options.name = "Library sink";
    options.product_name = "iclforge-sink-tests";
    options.address = "127.0.0.1";
    options.port = 0;
    options.state_directory = dir / "state";
    options.advertise = false;
    options.codecs = {m::Codec::kPcm};
    options.unpaired_access = true;
    return options;
}

}  // namespace

TEST_CASE("sink: a connection asks the program for its outputs and a PCM stream reaches the stream output",
          "[sink][websocket]") {
    const fs::path dir = scratch("stream");
    Recording recording;
    FakeOutputs outputs(recording);
    Lines log;
    auto sink = iclforge::sink::Sink::start(options_in(dir), outputs, log);
    REQUIRE(sink.has_value());

    const iclforge::sendspin::noise::KeyPair identity = generated();
    Dialled server(**sink, identity);
    REQUIRE(server.play({"player@v1"}));
    REQUIRE(log.wait("activated for playback"));
    REQUIRE(server.events.wait([&] { return server.events.available; }, 15s));

    const m::AudioFormat format{.codec = m::Codec::kPcm, .channels = 2, .sample_rate = 48000, .bit_depth = 16};
    REQUIRE(server.call([&] { return server.server.start_stream({.format = format, .codec_header = {}}); }));
    CHECK(log.wait("stream PCM 48000 Hz 16-bit 2 ch"));

    // Three chunks of 480 frames, each different, at times 20 ms apart.
    std::vector<std::vector<std::uint8_t>> sent;
    const std::int64_t first = server.clock.now_us() + 400'000;
    for (int index = 0; index < 3; ++index) {
        std::vector<std::uint8_t> chunk(480 * 4);
        for (std::size_t byte = 0; byte < chunk.size(); ++byte) {
            chunk[byte] = static_cast<std::uint8_t>((byte * 7U) + static_cast<std::size_t>(index) * 31U);
        }
        const std::int64_t at = first + (static_cast<std::int64_t>(index) * 10'000);
        REQUIRE(server.call([&] { return server.server.send_audio(at, chunk); }));
        sent.push_back(std::move(chunk));
    }
    REQUIRE(server.call([&] { return server.server.end_stream(); }));
    CHECK(log.wait("stream ended after 3 chunks"));

    const std::lock_guard lock(recording.mutex);
    // The sink's own number for the connection, from 1.
    REQUIRE(recording.stream_connections == std::vector<std::uint64_t>{1});
    REQUIRE(recording.burst_connections == std::vector<std::uint64_t>{1});
    REQUIRE(recording.started.size() == 1);
    CHECK(recording.started[0].format.codec == m::Codec::kPcm);
    CHECK(recording.started[0].format.channels == 2);
    CHECK(recording.started[0].format.sample_rate == 48000);
    CHECK(recording.started[0].format.bit_depth == 16);
    CHECK(recording.chunks == sent);
    REQUIRE(recording.chunk_times.size() == 3);
    // In the order sent, 10 ms of the sink's clock apart (the clock filter may move a little between
    // two chunks).
    CHECK(std::abs(recording.chunk_times[1] - recording.chunk_times[0] - 10'000) < 1'000);
    CHECK(std::abs(recording.chunk_times[2] - recording.chunk_times[1] - 10'000) < 1'000);
    CHECK(recording.ended == 1);
    CHECK(recording.burst_started.empty());
}

TEST_CASE("sink: a burst stream and its karaoke settings reach the burst output", "[sink][websocket]") {
    const fs::path dir = scratch("bursts");
    Recording recording;
    FakeOutputs outputs(recording);
    Lines log;
    iclforge::sink::Options options = options_in(dir);
    options.karaoke = true;
    options.layout = "5.1";
    auto sink = iclforge::sink::Sink::start(std::move(options), outputs, log);
    REQUIRE(sink.has_value());

    const iclforge::sendspin::noise::KeyPair identity = generated();
    Dialled server(**sink, identity);
    REQUIRE(server.play({std::string(ac::kRole)}));
    REQUIRE(log.wait("activated for playback"));
    REQUIRE(server.events.wait([&] { return server.events.available; }, 15s));

    REQUIRE(server.call(
        [&] { return server.server.start_burst_stream({.data_type = ac::DataType::kAc3, .sample_rate = 48000}); }));
    CHECK(log.wait("burst stream AC-3 48000 Hz to 5.1"));

    // Two bursts' payloads: a syncword, which the server's session checks, and bytes the sink does not
    // read.
    std::vector<std::vector<std::uint8_t>> sent;
    const std::int64_t first = server.clock.now_us() + 400'000;
    for (int index = 0; index < 2; ++index) {
        std::vector<std::uint8_t> payload(1792);
        for (std::size_t byte = 0; byte < payload.size(); ++byte) {
            payload[byte] = static_cast<std::uint8_t>((byte * 3U) + static_cast<std::size_t>(index));
        }
        payload[0] = 0x0B;
        payload[1] = 0x77;
        const std::uint16_t pd = iclforge::sendspin::burst_length_code(iclforge::sendspin::BurstDataType::kAc3,
                                                                       payload.size());
        const std::int64_t at = first + (static_cast<std::int64_t>(index) * 32'000);
        REQUIRE(server.call([&] {
            return server.server.send_burst(
                at, static_cast<std::uint16_t>(iclforge::sendspin::BurstDataType::kAc3), pd, payload);
        }));
        sent.push_back(std::move(payload));
    }

    // A settings command: the karaoke keys go to the output, and the revision is reported.
    ac::CommandMessage command;
    command.command = ac::Command::kSettings;
    command.settings.revision = 7;
    command.settings.decoder.karaoke = ac::Karaoke::kAware;
    command.settings.decoder.karaoke_vocals = ac::KaraokeVocals::kV1;
    command.settings.decoder.karaoke_melody_db = -6.0;
    REQUIRE(server.call([&] { return server.server.iclforge_command(command); }));
    CHECK(log.wait("settings 7 applied (karaoke aware, vocals v1, melody -6 dB)"));

    REQUIRE(server.call([&] { return server.server.end_burst_stream(); }));
    CHECK(log.wait("burst stream ended after 2 bursts"));

    const std::lock_guard lock(recording.mutex);
    REQUIRE(recording.burst_started.size() == 1);
    CHECK(recording.burst_started[0].data_type == ac::DataType::kAc3);
    CHECK(recording.burst_started[0].sample_rate == 48000);
    REQUIRE(recording.bursts.size() == 2);
    for (std::size_t index = 0; index < 2; ++index) {
        CHECK(recording.bursts[index].pc == static_cast<std::uint16_t>(iclforge::sendspin::BurstDataType::kAc3));
        CHECK(recording.bursts[index].payload == sent[index]);
    }
    CHECK(recording.bursts[0].pd == recording.bursts[1].pd);
    REQUIRE(recording.karaoke.size() == 1);
    CHECK(recording.karaoke[0].reproduction == ac::Karaoke::kAware);
    CHECK(recording.karaoke[0].vocals == ac::KaraokeVocals::kV1);
    CHECK(recording.karaoke[0].levels == std::array<double, 3>{-6.0, 0.0, 0.0});
    CHECK(recording.burst_ended == 1);
    CHECK(recording.started.empty());
}

namespace {

// What the sink said of how a burst stream leaves it: in the extension role's state the server read
// and in its own status, once the stream had started, and in the state once it had ended.
struct OutputSeen {
    std::optional<ac::Output> state_during;
    std::optional<ac::Output> status_during;
    std::optional<ac::Output> state_after;
    bool status_streaming = false;
};

OutputSeen watch_output(std::string_view leaf, std::optional<ac::Output> says, bool takes) {
    const fs::path dir = scratch(leaf);
    Recording recording;
    recording.burst_output = says;
    recording.burst_takes = takes;
    FakeOutputs outputs(recording);
    Lines log;
    auto sink = iclforge::sink::Sink::start(options_in(dir), outputs, log);
    REQUIRE(sink.has_value());

    const iclforge::sendspin::noise::KeyPair identity = generated();
    Dialled server(**sink, identity);
    REQUIRE(server.play({std::string(ac::kRole)}));
    REQUIRE(log.wait("activated for playback"));
    REQUIRE(server.events.wait([&] { return server.events.available; }, 15s));

    // Every set_iclforge_state sends a client/state, so each change below is one more message.
    OutputSeen seen;
    int states = server.events.extension().states;
    REQUIRE(server.call(
        [&] { return server.server.start_burst_stream({.data_type = ac::DataType::kAc3, .sample_rate = 48000}); }));
    REQUIRE(server.events.wait([&] { return server.events.iclforge_states > states; }));
    const Events::Extension during = server.events.extension();
    seen.state_during = during.output;
    states = during.states;
    const iclforge::sink::Sink::Status status = (*sink)->status();
    REQUIRE(status.connections.size() == 1);
    seen.status_during = status.connections[0].output;
    seen.status_streaming = status.connections[0].streaming;

    REQUIRE(server.call([&] { return server.server.end_burst_stream(); }));
    REQUIRE(server.events.wait([&] { return server.events.iclforge_states > states; }));
    seen.state_after = server.events.extension().output;
    return seen;
}

}  // namespace

TEST_CASE("sink: the role's state says how a stream the output took leaves the sink", "[sink][websocket]") {
    // An output that hands the bits on says so while the stream is open, to the server and in the
    // sink's own status, and the key goes with the stream.
    const OutputSeen passed = watch_output("output_passthrough", ac::Output::kPassthrough, true);
    CHECK(passed.state_during == ac::Output::kPassthrough);
    CHECK(passed.status_during == ac::Output::kPassthrough);
    CHECK(passed.status_streaming);
    CHECK_FALSE(passed.state_after.has_value());

    // One that decodes says pcm.
    const OutputSeen decoded = watch_output("output_pcm", ac::Output::kPcm, true);
    CHECK(decoded.state_during == ac::Output::kPcm);
    CHECK(decoded.status_during == ac::Output::kPcm);
    CHECK_FALSE(decoded.state_after.has_value());

    // A stream the output did not take leaves the sink by no way it can name: nothing is said, though
    // the stream is open and the output would pass a stream it took.
    const OutputSeen refused = watch_output("output_refused", ac::Output::kPassthrough, false);
    CHECK_FALSE(refused.state_during.has_value());
    CHECK_FALSE(refused.status_during.has_value());
    CHECK(refused.status_streaming);

    // An output that does not say, as one from before the key, is not known.
    const OutputSeen silent = watch_output("output_silent", std::nullopt, true);
    CHECK_FALSE(silent.state_during.has_value());
    CHECK_FALSE(silent.status_during.has_value());
}

TEST_CASE("sink: the status carries what the output found in the stream, for the screen's format chip",
          "[sink][websocket]") {
    const fs::path dir = scratch("status_decoder");
    Recording recording;
    recording.burst_output = ac::Output::kPassthrough;
    recording.burst_decoder = ac::DecoderReport{
        .data_type = ac::DataType::kAc3, .acmod = 7, .lfe = true, .objects = 0, .objects_placed = false};
    FakeOutputs outputs(recording);
    Lines log;
    auto sink = iclforge::sink::Sink::start(options_in(dir), outputs, log);
    REQUIRE(sink.has_value());

    const iclforge::sendspin::noise::KeyPair identity = generated();
    Dialled server(**sink, identity);
    REQUIRE(server.play({std::string(ac::kRole)}));
    REQUIRE(log.wait("activated for playback"));
    REQUIRE(server.events.wait([&] { return server.events.available; }, 15s));

    // No stream: nothing to name.
    iclforge::sink::Sink::Status status = (*sink)->status();
    REQUIRE(status.connections.size() == 1);
    CHECK_FALSE(status.connections[0].decoder.has_value());
    CHECK(iclforge::sink::status_json(status).find("\"output\":null,\"decoder\":null}") != std::string::npos);

    // A stream the output took: the report it found is the status's, as the role's state sends it.
    REQUIRE(server.call(
        [&] { return server.server.start_burst_stream({.data_type = ac::DataType::kAc3, .sample_rate = 48000}); }));
    CHECK(log.wait("burst stream AC-3 48000 Hz"));
    status = (*sink)->status();
    REQUIRE(status.connections.size() == 1);
    REQUIRE(status.connections[0].decoder.has_value());
    CHECK(status.connections[0].decoder->data_type == ac::DataType::kAc3);
    CHECK(status.connections[0].decoder->acmod == 7);
    CHECK(status.connections[0].decoder->lfe);
    // The JSON the app's screen reads says the same, in the one object it parses.
    const std::string json = iclforge::sink::status_json(status);
    CHECK(json.find("\"streaming\":true,\"output\":\"passthrough\",\"decoder\":{\"data_type\":\"ac3\","
                    "\"acmod\":7,\"lfe\":true,\"objects\":0,\"objects_placed\":false}}") != std::string::npos);
    CHECK(json.find("\"paired_servers\":") != std::string::npos);

    // And gone with the stream.
    REQUIRE(server.call([&] { return server.server.end_burst_stream(); }));
    CHECK(log.wait("burst stream ended after 0 bursts"));
    status = (*sink)->status();
    REQUIRE(status.connections.size() == 1);
    CHECK_FALSE(status.connections[0].decoder.has_value());
}

TEST_CASE("sink: a lyric document and its anchors become a frame on the sink's own clock", "[sink][websocket]") {
    const fs::path dir = scratch("lyric_frame");
    Recording recording;
    FakeOutputs outputs(recording);
    Lines log;
    iclforge::sink::Options options = options_in(dir);
    options.lyrics_max_bytes = ac::kMaxLyricsBytes;
    auto sink = iclforge::sink::Sink::start(std::move(options), outputs, log);
    REQUIRE(sink.has_value());

    const iclforge::sendspin::noise::KeyPair identity = generated();
    Dialled server(**sink, identity);
    REQUIRE(server.play({std::string(ac::kRole)}));
    REQUIRE(log.wait("activated for playback"));
    REQUIRE(server.events.wait([&] { return server.events.available; }, 15s));

    // Nothing held and nothing playing.
    const iclforge::sink::LyricFrame idle = (*sink)->lyric_frame();
    CHECK(idle.mode == iclforge::lyrics::StageMode::kNoLyrics);
    CHECK(idle.item == -1);
    CHECK_FALSE(idle.streaming);

    REQUIRE(server.call(
        [&] { return server.server.start_burst_stream({.data_type = ac::DataType::kAc3, .sample_rate = 48000}); }));
    CHECK(log.wait("burst stream AC-3 48000 Hz"));

    ac::CommandMessage document;
    document.command = ac::Command::kLyrics;
    document.lyrics = {.item = 42,
                       .text = "[ti:Test song]\n[00:00.00]First line\n[00:04.00]Second line\n[01:00.00]\n"};
    REQUIRE(server.call([&] { return server.server.iclforge_command(document); }));
    CHECK(log.wait("lyrics for item 42: held"));

    // The document is held and no anchor has come due: the first lines wait, and a stream is open.
    const iclforge::sink::LyricFrame waiting = (*sink)->lyric_frame();
    CHECK(waiting.item == 42);
    CHECK(waiting.streaming);
    CHECK(waiting.title == "Test song");
    CHECK(waiting.mode == iclforge::lyrics::StageMode::kWaiting);

    // The sound is at 5 s now, by the server's clock. Once the sink's own clock has converged on the
    // server's the frame is the second line, a little past 5 s.
    ac::CommandMessage anchor;
    anchor.command = ac::Command::kLyricsPosition;
    anchor.lyrics_position = {.item = 42, .position_ms = 5000, .at_us = server.clock.now_us(), .speed = 1.0};
    REQUIRE(server.call([&] { return server.server.iclforge_command(anchor); }));

    iclforge::sink::LyricFrame frame;
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (std::chrono::steady_clock::now() < deadline) {
        frame = (*sink)->lyric_frame();
        if (frame.mode == iclforge::lyrics::StageMode::kLine) {
            break;
        }
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(frame.mode == iclforge::lyrics::StageMode::kLine);
    CHECK(frame.current.text == "Second line");
    CHECK(frame.position_ms >= 5000);
    CHECK(frame.position_ms < 40'000);
    REQUIRE(frame.before.size() == 1);
    CHECK(frame.before[0].text == "First line");

    // A delay of 2 s shows the lyrics later: the stage is the position less the delay.
    const iclforge::sink::LyricFrame delayed = (*sink)->lyric_frame(2000);
    CHECK(delayed.position_ms + 2000 - frame.position_ms < 500);

    // The end of the stream is nothing playing: the document stays, the position goes.
    REQUIRE(server.call([&] { return server.server.end_burst_stream(); }));
    CHECK(log.wait("burst stream ended after 0 bursts"));
    const iclforge::sink::LyricFrame ended = (*sink)->lyric_frame();
    CHECK(ended.item == 42);
    CHECK_FALSE(ended.streaming);
    CHECK(ended.mode == iclforge::lyrics::StageMode::kWaiting);

    // And a sink that loses its lyrics has none.
    (*sink)->forget_lyrics();
    CHECK((*sink)->lyric_frame().mode == iclforge::lyrics::StageMode::kNoLyrics);
}

TEST_CASE("sink: what the server says is playing names an item with no lyrics, and the document's tags are the fallback",
          "[sink][websocket]") {
    const fs::path dir = scratch("now_playing");
    Recording recording;
    FakeOutputs outputs(recording);
    Lines log;
    iclforge::sink::Options options = options_in(dir);
    options.lyrics_max_bytes = ac::kMaxLyricsBytes;
    options.other_roles = {"metadata@v1"};
    auto sink = iclforge::sink::Sink::start(std::move(options), outputs, log);
    REQUIRE(sink.has_value());

    const iclforge::sendspin::noise::KeyPair identity = generated();
    Dialled server(**sink, identity);
    REQUIRE(server.play({std::string(ac::kRole), "metadata@v1"}));
    REQUIRE(log.wait("activated for playback"));
    REQUIRE(server.events.wait([&] { return server.events.available; }, 15s));
    REQUIRE(server.call(
        [&] { return server.server.start_burst_stream({.data_type = ac::DataType::kAc3, .sample_rate = 48000}); }));
    CHECK(log.wait("burst stream AC-3 48000 Hz"));

    // Nothing said and no document: the item has no name.
    iclforge::sink::LyricFrame frame = (*sink)->lyric_frame();
    CHECK(frame.mode == iclforge::lyrics::StageMode::kNoLyrics);
    CHECK(frame.streaming);
    CHECK(frame.title.empty());

    // The server says what is playing: the frame names the item, though it holds no lyrics.
    iclforge::sendspin::metadata::State playing;
    playing.title = "Spring";
    playing.artist = "Hearth";
    playing.album = "Seasons";
    REQUIRE(server.call([&] { return server.server.send_state({.metadata = playing}); }));
    REQUIRE(log.wait("metadata \"Spring\""));
    frame = (*sink)->lyric_frame();
    CHECK(frame.mode == iclforge::lyrics::StageMode::kNoLyrics);
    CHECK(frame.item == -1);
    CHECK(frame.streaming);
    CHECK(frame.title == "Spring");
    CHECK(frame.artist == "Hearth");
    CHECK(frame.album == "Seasons");

    // A document with its own title: what the server says comes first, and the other fields the
    // document has and the server leaves out are the document's.
    iclforge::sendspin::metadata::State titled;
    titled.title = "Spring (server)";
    REQUIRE(server.call([&] { return server.server.send_state({.metadata = titled}); }));
    REQUIRE(log.wait("metadata \"Spring (server)\""));
    ac::CommandMessage document;
    document.command = ac::Command::kLyrics;
    document.lyrics = {.item = 5,
                       .text = "[ti:Test song]" "\n" "[ar:The document]" "\n"
                               "[00:00.00]First line" "\n" "[00:04.00]Second" "\n"};
    REQUIRE(server.call([&] { return server.server.iclforge_command(document); }));
    CHECK(log.wait("lyrics for item 5: held"));
    frame = (*sink)->lyric_frame();
    CHECK(frame.item == 5);
    CHECK(frame.title == "Spring (server)");
    CHECK(frame.artist == "The document");

    // The server clears it: the document's tags are what is left.
    REQUIRE(server.call([&] { return server.server.send_state({.metadata = std::optional<iclforge::sendspin::metadata::State>{}}); }));
    REQUIRE(log.wait("metadata cleared"));
    frame = (*sink)->lyric_frame();
    CHECK(frame.title == "Test song");
    CHECK(frame.artist == "The document");
    CHECK(frame.album.empty());
}

TEST_CASE("sink: the status says who is connected, for what, and the code a server asked by",
          "[sink][websocket]") {
    const fs::path dir = scratch("status");
    Recording recording;
    FakeOutputs outputs(recording);
    Lines log;
    iclforge::sink::Options options = options_in(dir);
    options.code_method = iclforge::sink::CodeMethod::kDynamic;
    options.unpaired_access = false;
    auto sink = iclforge::sink::Sink::start(std::move(options), outputs, log);
    REQUIRE(sink.has_value());

    const iclforge::sink::Sink::Status idle = (*sink)->status();
    CHECK(idle.connections.empty());
    CHECK_FALSE(idle.pairing_code.has_value());
    CHECK(idle.paired_servers == 0);

    const iclforge::sendspin::noise::KeyPair identity = generated();
    Dialled server(**sink, identity);
    // A server that asks to pair by a code: the sink shows one, and says so in its status.
    REQUIRE(server.pair(m::PairMethod::kDynamicCode, m::CodeFormat::kDigits, 6));
    REQUIRE(log.wait("PAIRING CODE"));
    iclforge::sink::Sink::Status pairing = (*sink)->status();
    REQUIRE(pairing.connections.size() == 1);
    CHECK(pairing.connections[0].server_name == "Test server");
    CHECK(pairing.connections[0].activity == iclforge::sink::Sink::Status::Activity::kPairing);
    REQUIRE(pairing.pairing_code.has_value());
    CHECK(pairing.pairing_code->size() == 7);  // "123-456"
    CHECK((*pairing.pairing_code)[3] == '-');

    // Typed there, it pairs: the code goes from the status and the record is counted.
    std::string shown = *pairing.pairing_code;
    std::erase(shown, '-');
    REQUIRE(server.events.wait([&] { return server.events.code_wanted; }));
    REQUIRE(server.call(
        [&] { return server.server.enter_code(iclforge::sendspin::pairing_flow::Code{shown}); }));
    REQUIRE(log.wait("paired with server "));
    const iclforge::sink::Sink::Status paired = (*sink)->status();
    CHECK_FALSE(paired.pairing_code.has_value());
    CHECK(paired.paired_servers == 1);
}

TEST_CASE("sink: the totals are what the outputs say they counted", "[sink][websocket]") {
    const fs::path dir = scratch("totals");
    Recording recording;
    FakeOutputs outputs(recording);
    Lines log;
    auto sink = iclforge::sink::Sink::start(options_in(dir), outputs, log);
    REQUIRE(sink.has_value());

    const iclforge::sendspin::noise::KeyPair identity = generated();
    {
        Dialled server(**sink, identity);
        REQUIRE(server.play({"player@v1"}));
        REQUIRE(log.wait("activated for playback"));
        REQUIRE(server.events.wait([&] { return server.events.available; }, 15s));
        const m::AudioFormat format{.codec = m::Codec::kPcm, .channels = 2, .sample_rate = 48000, .bit_depth = 16};
        REQUIRE(server.call([&] { return server.server.start_stream({.format = format, .codec_header = {}}); }));
        const std::vector<std::uint8_t> chunk(480 * 4, 0x5A);
        const std::int64_t first = server.clock.now_us() + 400'000;
        for (int index = 0; index < 4; ++index) {
            const std::int64_t at = first + (static_cast<std::int64_t>(index) * 10'000);
            REQUIRE(server.call([&] { return server.server.send_audio(at, chunk); }));
        }
        REQUIRE(log.wait("stream PCM"));
        // While the connection is open.
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while ((*sink)->totals().chunks < 4 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(20ms);
        }
        const iclforge::sink::Sink::Totals open = (*sink)->totals();
        CHECK(open.connections == 1);
        CHECK(open.streams == 1);
        CHECK(open.chunks == 4);
        CHECK(open.frames == 4 * 480);
    }
    // Once it has closed, what it counted is still counted.
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while ((*sink)->totals().connections != 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(20ms);
    }
    const iclforge::sink::Sink::Totals closed = (*sink)->totals();
    CHECK(closed.connections == 0);
    CHECK(closed.streams == 1);
    CHECK(closed.chunks == 4);
    CHECK(closed.frames == 4 * 480);
}

TEST_CASE("sink: a server pairs by the dynamic code the sink shows, and is then admitted", "[sink][websocket]") {
    const fs::path dir = scratch("pairing");
    Recording recording;
    FakeOutputs outputs(recording);
    Lines log;
    iclforge::sink::Options options = options_in(dir);
    options.unpaired_access = false;
    options.code_method = iclforge::sink::CodeMethod::kDynamic;
    auto sink = iclforge::sink::Sink::start(std::move(options), outputs, log);
    REQUIRE(sink.has_value());

    const iclforge::sendspin::noise::KeyPair identity = generated();
    Dialled server(**sink, identity);
    REQUIRE(server.pair(m::PairMethod::kDynamicCode, m::CodeFormat::kDigits, 6));
    REQUIRE(server.events.wait([&] { return server.events.code_wanted; }));
    REQUIRE(log.wait("PAIRING CODE "));
    std::string shown = log.code().value_or("");
    std::erase(shown, '-');
    REQUIRE(shown.size() == 6);

    // The code the sink shows is the one that pairs: a round with it ends in a record on both sides.
    REQUIRE(server.call(
        [&] { return server.server.enter_code(iclforge::sendspin::pairing_flow::Code{shown}); }));
    CHECK(log.wait("paired with server "));
    CHECK(server.events.wait([&] { return server.events.paired; }));
}

TEST_CASE("sink: the store keeps the identity, the pairing PSK and the records between runs", "[sink][store]") {
    const fs::path dir = scratch("store") / "state";
    Key32 server_a{};
    Key32 psk_a{};
    server_a.fill(0x11);
    psk_a.fill(0x22);
    Key32 server_b{};
    Key32 psk_b{};
    server_b.fill(0x33);
    psk_b.fill(0x44);

    Key32 identity_key{};
    Key32 pairing_psk{};
    {
        auto store = iclforge::sink::Store::open(dir);
        REQUIRE(store.has_value());
        identity_key = (*store)->identity().public_key();
        pairing_psk = (*store)->pairing_psk();
        CHECK((*store)->records() == 0);
        CHECK((*store)->add_record(server_a, psk_a));
        CHECK((*store)->add_record(server_b, psk_b));
        (*store)->set_last_playback(server_b);
        // A record for a server already held replaces it.
        CHECK((*store)->add_record(server_a, psk_b));
        CHECK((*store)->records() == 2);
    }
    {
        auto store = iclforge::sink::Store::open(dir);
        REQUIRE(store.has_value());
        CHECK((*store)->identity().public_key() == identity_key);
        CHECK((*store)->pairing_psk() == pairing_psk);
        CHECK((*store)->records() == 2);
        CHECK((*store)->last_playback() == std::optional<Key32>{server_b});
        (*store)->remove_record(server_a);
        CHECK((*store)->records() == 1);
    }
    {
        auto store = iclforge::sink::Store::open(dir);
        REQUIRE(store.has_value());
        CHECK((*store)->records() == 1);
    }
}

TEST_CASE("sink: a key's hex text goes back and forth, and only whole keys do", "[sink][store]") {
    Key32 key{};
    for (std::size_t index = 0; index < key.size(); ++index) {
        key[index] = static_cast<std::uint8_t>(index * 5U);
    }
    const std::string text = iclforge::sink::hex_of(key);
    CHECK(text.size() == 64);
    CHECK(iclforge::sink::key_from_hex(text) == std::optional<Key32>{key});
    CHECK_FALSE(iclforge::sink::key_from_hex(text.substr(1)).has_value());
    CHECK_FALSE(iclforge::sink::key_from_hex(text + "00").has_value());
    std::string bad = text;
    bad[10] = 'g';
    CHECK_FALSE(iclforge::sink::key_from_hex(bad).has_value());
}
