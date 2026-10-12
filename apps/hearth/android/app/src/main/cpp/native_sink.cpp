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

class DiscardingBursts final : public sink::BurstStreamOutput {
   public:
    bool start(const sendspin::player::StreamStart& /*stream*/) override {
        ++counts_.streams;
        return true;
    }
    void clear() override {}
    void end() override {}
    void write(const sendspin::BurstChunk& /*chunk*/, std::int64_t /*local_time*/) override { ++counts_.units; }
    void set_karaoke(sendspin::player::Karaoke /*reproduction*/, sendspin::player::KaraokeVocals /*vocals*/,
                     const std::array<double, 3>& /*levels_db*/) override {}
    [[nodiscard]] sink::OutputCounts counts() const override { return counts_; }
    [[nodiscard]] const std::filesystem::path& file() const override { return no_file_; }
    [[nodiscard]] const std::optional<sendspin::player::DecoderReport>& decoder() const override { return decoder_; }

   private:
    sink::OutputCounts counts_;
    std::filesystem::path no_file_;
    std::optional<sendspin::player::DecoderReport> decoder_;
};

class DiscardingOutputs final : public sink::Outputs {
   public:
    [[nodiscard]] std::unique_ptr<sink::StreamOutput> stream(std::uint64_t /*connection*/) override {
        return std::make_unique<DiscardingStream>();
    }
    [[nodiscard]] std::unique_ptr<sink::BurstStreamOutput> bursts(std::uint64_t /*connection*/) override {
        return std::make_unique<DiscardingBursts>();
    }
};

}  // namespace

class NativeSink::Impl {
   public:
    mutable std::mutex mutex;
    // The log and the outputs outlive the sink, which refers to both.
    LogBuffer log;
    DiscardingOutputs outputs;
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
    // player@v1 in PCM, and the extension role's three data types: what the output takes is the
    // next phase's. Phase 1b's sink is a skeleton that takes either and drops it.
    options.codecs = {sendspin::messages::Codec::kPcm};
    options.extension_role = true;
    options.buffer_capacity = 8ULL * 1024 * 1024;

    std::expected<std::unique_ptr<sink::Sink>, std::string> started =
        sink::Sink::start(std::move(options), impl_->outputs, impl_->log);
    if (!started) {
        return started.error();
    }
    const std::lock_guard lock(impl_->mutex);
    impl_->sink = std::move(*started);
    return {};
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

}  // namespace iclforge::hearth_android
