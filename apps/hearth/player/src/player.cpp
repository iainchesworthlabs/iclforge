#include "hearth_player.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "iclforge/render/layout.hpp"
#include "iclforge/sendspin/codec.hpp"
#include "iclforge/sendspin/messages.hpp"
#include "iclforge/sendspin/session_driver.hpp"
#include "iclforge/sink/decoding_output.hpp"

namespace iclforge::hearth::player {

namespace {

[[nodiscard]] std::int64_t steady_now_us() { return sendspin::SteadyClock{}.now_us(); }

[[nodiscard]] std::string env(const char* name) {
    const char* const value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

}  // namespace

// What every connection's outputs say about themselves, for the status line.
class PlayerOutputs final : public sink::Outputs {
   public:
    PlayerOutputs(PlayerOptions options, DeviceFactory device) : options_(std::move(options)), device_(std::move(device)) {
        if (!device_) {
            const std::string id = options_.device;
            device_ = [id]() -> std::unique_ptr<FrameDevice> {
                if (id == "null") {
                    return std::make_unique<NullDevice>();
                }
                return make_audio_device(id);
            };
        }
    }

    [[nodiscard]] std::unique_ptr<sink::StreamOutput> stream(std::uint64_t connection) override;
    [[nodiscard]] std::unique_ptr<sink::BurstStreamOutput> bursts(std::uint64_t connection) override;

    void note(const std::string& state, const std::string& codec, std::int32_t channels) {
        const std::lock_guard lock(mutex_);
        status_.state = state;
        if (!codec.empty()) {
            status_.codec = codec;
            status_.channels = channels;
        }
    }
    void note_decoder(std::int32_t objects, bool placed, std::int32_t channels) {
        const std::lock_guard lock(mutex_);
        status_.objects = objects;
        status_.objects_placed = placed;
        status_.channels = channels;
    }
    void watch(const ScheduledPcmSink* sink, bool add) {
        const std::lock_guard lock(mutex_);
        if (add) {
            sinks_.push_back(sink);
        } else {
            std::erase(sinks_, sink);
        }
    }
    void invalid(std::uint64_t count) {
        const std::lock_guard lock(mutex_);
        status_.invalid += count;
    }

    [[nodiscard]] PlayerStatus status() const {
        const std::lock_guard lock(mutex_);
        PlayerStatus status = status_;
        for (const ScheduledPcmSink* sink : sinks_) {
            const PlayStats stats = sink->stats();
            status.play.underruns += stats.underruns;
            status.play.corrected += stats.corrected;
            status.play.measured += stats.measured;
            status.play.dropped_frames += stats.dropped_frames;
            status.play.silence_frames += stats.silence_frames;
            status.play.pending_frames += stats.pending_frames;
            status.play.sync_error_us = stats.sync_error_us;
            if (std::llabs(stats.worst_sync_error_us) > std::llabs(status.play.worst_sync_error_us)) {
                status.play.worst_sync_error_us = stats.worst_sync_error_us;
            }
        }
        return status;
    }

    [[nodiscard]] std::unique_ptr<ScheduledPcmSink> make_sink() {
        ScheduledPcmSink::Options sink_options;
        sink_options.gain = static_cast<float>(std::pow(10.0, options_.gain_db / 20.0));
        return std::make_unique<ScheduledPcmSink>(device_(), steady_now_us, sink_options);
    }

   private:
    PlayerOptions options_;
    DeviceFactory device_;
    mutable std::mutex mutex_;
    PlayerStatus status_;
    std::vector<const ScheduledPcmSink*> sinks_;
};

namespace {

// player@v1: PCM, FLAC or Opus decoded to float and played.
class PcmStreamOutput final : public sink::StreamOutput {
   public:
    PcmStreamOutput(PlayerOutputs& board, std::unique_ptr<ScheduledPcmSink> sink)
        : board_(&board), sink_(std::move(sink)) {
        board_->watch(sink_.get(), true);
    }
    ~PcmStreamOutput() override { board_->watch(sink_.get(), false); }

    bool start(const sendspin::messages::PlayerStream& stream) override {
        ++streams_;
        stream_frames_ = 0;
        decoder_ = sendspin::codec::make_decoder(stream);
        if (!decoder_ || stream.format.channels < 1 || stream.format.sample_rate < 1 ||
            !sink_->open(static_cast<std::uint32_t>(stream.format.sample_rate),
                         static_cast<std::uint16_t>(stream.format.channels))) {
            decoder_.reset();
            return false;
        }
        channels_ = static_cast<std::size_t>(stream.format.channels);
        board_->note("playing", "pcm", stream.format.channels);
        board_->note_decoder(0, false, stream.format.channels);
        return true;
    }
    void clear() override {
        sink_->clear(stream_frames_);
        board_->note("cleared", {}, 0);
    }
    void end() override { board_->note("idle", {}, 0); }
    void write(std::span<const std::uint8_t> frame, std::int64_t local_time) override {
        ++chunks_;
        if (!decoder_) {
            return;
        }
        const std::optional<std::vector<std::int32_t>> decoded = decoder_->decode(frame);
        if (!decoded) {
            ++undecodable_;
            board_->invalid(1);
            return;
        }
        const std::size_t frames = decoded->size() / channels_;
        const double scale = std::ldexp(1.0, decoder_->bit_depth() - 1);
        samples_.resize(frames * channels_);
        for (std::size_t i = 0; i < samples_.size(); ++i) {
            samples_[i] = static_cast<float>(static_cast<double>((*decoded)[i]) / scale);
        }
        sink_->burst(local_time, stream_frames_, frames);
        sink_->write(samples_);
        stream_frames_ += frames;
        frames_ += frames;
    }
    [[nodiscard]] sink::OutputCounts counts() const override {
        const PlayStats stats = sink_->stats();
        return {.streams = streams_, .units = chunks_, .frames = frames_, .underruns = stats.underruns,
                .late = stats.corrected, .dropped = undecodable_};
    }
    [[nodiscard]] const std::filesystem::path& file() const override { return sink_->file(); }

   private:
    PlayerOutputs* board_;
    std::unique_ptr<ScheduledPcmSink> sink_;
    std::unique_ptr<sendspin::codec::Decoder> decoder_;
    std::vector<float> samples_;
    std::size_t channels_ = 2;
    std::uint64_t chunks_ = 0;
    std::uint64_t frames_ = 0;
    std::uint64_t undecodable_ = 0;
    std::uint64_t stream_frames_ = 0;
    std::uint32_t streams_ = 0;
};

// _iclforge_player@v1: the bursts decoded by libs/sink, objects rendered, folded to stereo.
class PlayerBurstOutput final : public sink::DecodingBurstOutput {
   public:
    // `pcm` is the sink the decoding output owns; `raw` is the same, kept for the counts.
    PlayerBurstOutput(PlayerOutputs& board, std::unique_ptr<ScheduledPcmSink> pcm, ScheduledPcmSink* raw)
        : sink::DecodingBurstOutput(*render::OutputLayout::parse("2.0"), std::move(pcm)), board_(&board), pcm_(raw) {
        board_->watch(pcm_, true);
    }
    ~PlayerBurstOutput() override { board_->watch(pcm_, false); }

    bool start(const sendspin::player::StreamStart& stream) override {
        const bool started = sink::DecodingBurstOutput::start(stream);
        board_->note(started ? "playing" : "refused",
                     stream.data_type == sendspin::player::DataType::kAc3 ? "ac3" : "eac3", 2);
        return started;
    }
    void clear() override {
        sink::DecodingBurstOutput::clear();
        board_->note("cleared", {}, 0);
    }
    void end() override {
        sink::DecodingBurstOutput::end();
        board_->note("idle", {}, 0);
    }
    void write(const sendspin::BurstChunk& chunk, std::int64_t local_time) override {
        const std::uint64_t before = undecodable();
        sink::DecodingBurstOutput::write(chunk, local_time);
        if (undecodable() > before) {
            board_->invalid(undecodable() - before);
        }
        if (const auto& report = decoder()) {
            board_->note_decoder(report->objects, report->objects_placed, 2);
        }
    }
    [[nodiscard]] sink::OutputCounts counts() const override {
        sink::OutputCounts counts = sink::DecodingBurstOutput::counts();
        const PlayStats stats = pcm_->stats();
        counts.underruns = stats.underruns;
        counts.late = stats.corrected;
        counts.dropped = undecodable();
        return counts;
    }

   private:
    PlayerOutputs* board_;
    ScheduledPcmSink* pcm_;
};

}  // namespace

std::unique_ptr<sink::StreamOutput> PlayerOutputs::stream(std::uint64_t /*connection*/) {
    return std::make_unique<PcmStreamOutput>(*this, make_sink());
}

std::unique_ptr<sink::BurstStreamOutput> PlayerOutputs::bursts(std::uint64_t /*connection*/) {
    std::unique_ptr<ScheduledPcmSink> pcm = make_sink();
    ScheduledPcmSink* const raw = pcm.get();
    return std::make_unique<PlayerBurstOutput>(*this, std::move(pcm), raw);
}

std::expected<std::unique_ptr<Player>, std::string> Player::start(PlayerOptions options, PlayerLog& log,
                                                                  DeviceFactory device) {
    if (options.name.empty()) {
        options.name = default_player_name();
    }
    auto outputs = std::make_unique<PlayerOutputs>(options, std::move(device));
    std::unique_ptr<Player> player(new Player(std::move(outputs)));
    auto core = sink::Sink::start(std::move(static_cast<sink::Options&>(options)), *player->outputs_, log);
    if (!core) {
        return std::unexpected(std::move(core.error()));
    }
    player->core_ = std::move(*core);
    return player;
}

Player::Player(std::unique_ptr<PlayerOutputs> outputs) : outputs_(std::move(outputs)) {}
Player::~Player() = default;

std::uint16_t Player::port() const { return core_->port(); }
std::string Player::client_id() const { return core_->client_id(); }
std::string Player::pairing_token() const { return core_->pairing_token(); }
void Player::open_window() { core_->open_window(); }
void Player::reset_rounds() { core_->reset_rounds(); }
void Player::cancel_pairing() { core_->cancel_pairing(); }
sink::Sink::Totals Player::totals() const { return core_->totals(); }
PlayerStatus Player::status() const { return outputs_->status(); }

std::string Player::status_line() const {
    const PlayerStatus s = status();
    return fmt::format("{} {} ch={} objects={}{} bursts_pending={}f underruns={} invalid={} sync={:+.1f}ms worst={:+.1f}ms "
                       "corrected={}",
                       s.state, s.codec.empty() ? "-" : s.codec, s.channels, s.objects,
                       s.objects > 0 ? (s.objects_placed ? " placed" : " not-placed") : "", s.play.pending_frames,
                       s.play.underruns, s.invalid, static_cast<double>(s.play.sync_error_us) / 1000.0,
                       static_cast<double>(s.play.worst_sync_error_us) / 1000.0, s.play.corrected);
}

std::string default_player_name() {
    std::string host = env("COMPUTERNAME");
    if (host.empty()) {
        host = env("HOSTNAME");
    }
    if (host.empty()) {
        host = "pc";
    }
    std::ranges::transform(host, host.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return "hearth-pc-" + host;
}

std::filesystem::path default_state_directory() {
    std::filesystem::path base;
    if (const std::string local = env("LOCALAPPDATA"); !local.empty()) {
        base = local;
    } else if (const std::string xdg = env("XDG_DATA_HOME"); !xdg.empty()) {
        base = xdg;
    } else if (const std::string home = env("HOME"); !home.empty()) {
        base = std::filesystem::path(home) / ".local" / "share";
    } else {
        base = std::filesystem::current_path();
    }
    return base / "hearth-player";
}

}  // namespace iclforge::hearth::player
