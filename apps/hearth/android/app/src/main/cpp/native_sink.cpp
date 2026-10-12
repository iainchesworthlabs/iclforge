#include "native_sink.hpp"

#include <android/log.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "audio_link.hpp"
#include "iclforge/lyrics/stage.hpp"
#include "iclforge/sendspin/iclforge_player.hpp"
#include "iclforge/sendspin/session_driver.hpp"
#include "iclforge/sink/lyric_frame.hpp"
#include "iclforge/sink/passthrough.hpp"
#include "iclforge/sink/sink.hpp"

namespace iclforge::hearth_android {

namespace {

constexpr const char* kTag = "HearthSink";
// The most lines kept for the screen to read: it polls, and a pairing code is one line.
constexpr std::size_t kMaxKeptLines = 400;

// What the sink says, kept for the screen and written to logcat.
class LogBuffer final : public sink::Log {
   public:
    void line(std::string_view text) override {
        __android_log_print(ANDROID_LOG_INFO, kTag, "%.*s", static_cast<int>(text.size()), text.data());
        const std::lock_guard lock(mutex_);
        lines_.emplace_back(text);
        if (lines_.size() > kMaxKeptLines) {
            lines_.pop_front();
        }
    }

    [[nodiscard]] std::vector<std::string> drain() {
        const std::lock_guard lock(mutex_);
        std::vector<std::string> out(lines_.begin(), lines_.end());
        lines_.clear();
        return out;
    }

   private:
    std::mutex mutex_;
    std::deque<std::string> lines_;
};

// Counts a stream and drops it. The counters are what the sink's totals and its role state report.
class DiscardingStream final : public sink::StreamOutput {
   public:
    bool start(const sendspin::messages::PlayerStream& /*stream*/) override {
        ++counts_.streams;
        return true;
    }
    void clear() override {}
    void end() override {}
    void write(std::span<const std::uint8_t> frame, std::int64_t /*local_time*/) override {
        ++counts_.units;
        // PCM at 16 bits and two channels is the only format a stream is accepted in, which is the
        // sink's own state to say; a frame here is four bytes.
        counts_.frames += frame.size() / 4;
    }
    [[nodiscard]] sink::OutputCounts counts() const override { return counts_; }
    [[nodiscard]] const std::filesystem::path& file() const override { return no_file_; }

   private:
    sink::OutputCounts counts_;
    std::filesystem::path no_file_;
};

// The extension role's output: the stream as it is, to the receiver, through the process's one
// AudioTrack. It keeps its link in a base, so that the link is made before the output that refers to it
// and outlives it, and tells the app's outputs when it is made and gone, for audio focus.
class AppOutputs;

struct LinkHolder {
    explicit LinkHolder(std::unique_ptr<sink::BurstLink> held) : link(std::move(held)) {}
    std::unique_ptr<sink::BurstLink> link;
};

class AppBurstOutput final : private LinkHolder, public sink::PassthroughOutput {
   public:
    AppBurstOutput(AppOutputs& owner, std::unique_ptr<sink::BurstLink> link);
    ~AppBurstOutput() override;

   private:
    AppOutputs* owner_;
};

class AppOutputs final : public sink::Outputs {
   public:
    explicit AppOutputs(SharedAudio& audio) : audio_(&audio) {}

    [[nodiscard]] std::unique_ptr<sink::StreamOutput> stream(std::uint64_t /*connection*/) override {
        return std::make_unique<DiscardingStream>();
    }
    [[nodiscard]] std::unique_ptr<sink::BurstStreamOutput> bursts(std::uint64_t /*connection*/) override {
        auto output = std::make_unique<AppBurstOutput>(*this, audio_->make_link());
        output->run();
        return output;
    }

    // Another source holds the device (audio focus), or has given it back.
    void set_paused(bool paused) {
        const std::lock_guard lock(mutex_);
        paused_ = paused;
        for (AppBurstOutput* output : live_) {
            output->set_paused(paused);
        }
    }

    // Whether any connection has a stream open on the link: what audio focus is taken for.
    [[nodiscard]] bool streaming() const {
        const std::lock_guard lock(mutex_);
        return std::ranges::any_of(live_, [](const AppBurstOutput* output) { return output->streaming(); });
    }

    void add(AppBurstOutput* output) {
        const std::lock_guard lock(mutex_);
        live_.push_back(output);
        if (paused_) {
            output->set_paused(true);
        }
    }
    void remove(AppBurstOutput* output) {
        const std::lock_guard lock(mutex_);
        std::erase(live_, output);
    }

   private:
    SharedAudio* audio_;
    mutable std::mutex mutex_;
    std::vector<AppBurstOutput*> live_;
    bool paused_ = false;
};

[[nodiscard]] std::int64_t steady_now_us() {
    static const sendspin::SteadyClock clock;
    return clock.now_us();
}

[[nodiscard]] sink::PassthroughOptions app_options() {
    sink::PassthroughOptions options;
    options.note = [](std::string_view text) {
        __android_log_print(ANDROID_LOG_INFO, "HearthPassthrough", "%.*s", static_cast<int>(text.size()), text.data());
    };
    return options;
}

AppBurstOutput::AppBurstOutput(AppOutputs& owner, std::unique_ptr<sink::BurstLink> link)
    : LinkHolder(std::move(link)), PassthroughOutput(*this->link, steady_now_us, app_options()), owner_(&owner) {
    owner_->add(this);
}

AppBurstOutput::~AppBurstOutput() {
    owner_->remove(this);
}

}  // namespace

class NativeSink::Impl {
   public:
    mutable std::mutex mutex;
    // The log, the device and the outputs outlive the sink, which refers to the first and the last.
    LogBuffer log;
    SharedAudio audio;
    AppOutputs outputs{audio};
    std::unique_ptr<sink::Sink> sink;
};

NativeSink::NativeSink() : impl_(std::make_unique<Impl>()) {}

NativeSink::~NativeSink() {
    stop();
}

std::string NativeSink::start(const std::string& state_directory, const std::string& name, std::uint16_t port) {
    stop();
    sink::Options options;
    options.name = name;
    options.product_name = "hearth-android";
    options.address = "0.0.0.0";
    options.port = port;
    options.state_directory = state_directory;
    // NsdManager advertises (the service's own), and a pairing code is shown, not typed on the box.
    options.advertise = false;
    options.code_method = sink::CodeMethod::kDynamic;
    options.unpaired_access = false;
    // The coded stream goes to the receiver as it is, in the formats the route takes when a server
    // dials. player@v1's PCM is listed and dropped: the phase that decodes (planning/
    // hearth-android.md, phase 4) and the one that plays it (5) are not this one's.
    options.codecs = {sendspin::messages::Codec::kPcm};
    options.extension_role = true;
    SharedAudio* audio = &impl_->audio;
    options.data_types_now = [audio] { return audio->routes(); };
    options.buffer_capacity = 8ULL * 1024 * 1024;
    // The first burst can be placed once the track has been opened, eight bursts queued (it does not
    // move before that) and its position has begun to report: about 0.5 s on a Shield. A server that
    // starts a stream with the lead the test sink asks for (0.5 s) has its first bursts due before
    // that, and they are dropped; 0.8 s leaves the margin that the first run showed was needed.
    options.required_lead_time_ms = 800;
    // The words of a karaoke item are held and followed on the sink's own clock, and drawn
    // (lyric_frame()): the most a server may send one message of.
    options.lyrics_max_bytes = sendspin::player::kMaxLyricsBytes;
    // What the server says is playing (metadata@v1), so an item with no lyrics still has a title on
    // the screen (D7). The sink keeps it per connection and Sink::lyric_frame() reads it.
    options.other_roles = {"metadata@v1"};
    // What the remote may ask the server for (D8): the reproduction and which vocals.
    options.requests = {"karaoke", "karaoke_vocals"};

    std::expected<std::unique_ptr<sink::Sink>, std::string> started =
        sink::Sink::start(std::move(options), impl_->outputs, impl_->log);
    if (!started) {
        return started.error();
    }
    const std::lock_guard lock(impl_->mutex);
    impl_->sink = std::move(*started);
    return {};
}

void NativeSink::set_external_source(bool external) {
    const std::lock_guard lock(impl_->mutex);
    if (impl_->sink) {
        impl_->sink->set_external_source(external);
    }
    impl_->outputs.set_paused(external);
}

bool NativeSink::streaming() const {
    return impl_->outputs.streaming();
}

void NativeSink::stop() {
    std::unique_ptr<sink::Sink> stopping;
    {
        const std::lock_guard lock(impl_->mutex);
        stopping = std::move(impl_->sink);
    }
    // Destroyed outside the lock: its threads may be reporting through the log.
    stopping.reset();
}

bool NativeSink::running() const {
    const std::lock_guard lock(impl_->mutex);
    return impl_->sink != nullptr;
}

std::uint16_t NativeSink::port() const {
    const std::lock_guard lock(impl_->mutex);
    return impl_->sink ? impl_->sink->port() : std::uint16_t{0};
}

std::string NativeSink::client_id() const {
    const std::lock_guard lock(impl_->mutex);
    return impl_->sink ? impl_->sink->client_id() : std::string{};
}

SinkTotals NativeSink::totals() const {
    const std::lock_guard lock(impl_->mutex);
    SinkTotals out;
    if (!impl_->sink) {
        return out;
    }
    const sink::Sink::Totals totals = impl_->sink->totals();
    out.connections = totals.connections;
    out.streams = totals.streams;
    out.chunks = totals.chunks;
    out.frames = totals.frames;
    out.burst_streams = totals.burst_streams;
    out.bursts = totals.bursts;
    out.burst_frames = totals.burst_frames;
    return out;
}

std::vector<std::string> NativeSink::drain_log() {
    return impl_->log.drain();
}

void NativeSink::request_vocals(std::int32_t vocals) {
    namespace player = sendspin::player;
    player::Request request;
    request.decoder.karaoke = player::Karaoke::kCapable;
    request.decoder.karaoke_vocals = vocals == 0   ? player::KaraokeVocals::kNone
                                     : vocals == 1 ? player::KaraokeVocals::kV1
                                     : vocals == 2 ? player::KaraokeVocals::kV2
                                                   : player::KaraokeVocals::kBoth;
    const std::lock_guard lock(impl_->mutex);
    if (impl_->sink) {
        impl_->sink->send_request(request);
    }
}

std::string NativeSink::status_json() const {
    sink::Sink::Status status;
    {
        const std::lock_guard lock(impl_->mutex);
        if (impl_->sink) {
            status = impl_->sink->status();
        }
    }
    std::string out = "{\"connections\":[";
    for (std::size_t i = 0; i < status.connections.size(); ++i) {
        const sink::Sink::Status::Peer& peer = status.connections[i];
        if (i != 0) {
            out.push_back(',');
        }
        const char* activity = peer.activity == sink::Sink::Status::Activity::kPlayback  ? "playback"
                               : peer.activity == sink::Sink::Status::Activity::kPairing ? "pairing"
                                                                                         : "nothing";
        // How the stream leaves the sink: "passthrough" or "pcm" while one the output took is open
        // (the role's `output` state, D6), null otherwise.
        const std::string output = peer.output
                                       ? "\"" + std::string(sendspin::player::output_name(*peer.output)) + "\""
                                       : std::string("null");
        out += "{\"id\":" + std::to_string(peer.id) + ",\"address\":" + sink::json_quote(peer.address) +
               ",\"server_name\":" + sink::json_quote(peer.server_name) + ",\"activity\":\"" + activity +
               "\",\"streaming\":" + (peer.streaming ? "true" : "false") + ",\"output\":" + output + "}";
    }
    out += "],\"pairing_code\":";
    out += status.pairing_code ? sink::json_quote(*status.pairing_code) : "null";
    out += ",\"paired_servers\":" + std::to_string(status.paired_servers) + "}";
    return out;
}

std::string NativeSink::lyric_frame_json(std::int32_t delay_ms) const {
    const std::lock_guard lock(impl_->mutex);
    if (!impl_->sink) {
        return sink::lyric_frame_json(sink::LyricFrame{});
    }
    return sink::lyric_frame_json(impl_->sink->lyric_frame(delay_ms, sink::kScreenStageOptions));
}

}  // namespace iclforge::hearth_android
