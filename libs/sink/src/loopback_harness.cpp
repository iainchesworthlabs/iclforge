#include "iclforge/sink/loopback_harness.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "iclforge/ac3/io/elementary.hpp"
#include "iclforge/containers/iec61937/iec61937.hpp"
#include "iclforge/sendspin/handshake.hpp"
#include "iclforge/sendspin/iclforge_player.hpp"
#include "iclforge/sendspin/messages.hpp"
#include "iclforge/sendspin/noise.hpp"
#include "iclforge/sendspin/pairing_messages.hpp"
#include "iclforge/sendspin/server_session.hpp"
#include "iclforge/sendspin/session_driver.hpp"
#include "iclforge/sendspin/websocket.hpp"
#include "iclforge/sink/passthrough.hpp"

namespace iclforge::sink::harness {

namespace {

namespace iec = containers::iec61937;
namespace ac = sendspin::player;
namespace m = sendspin::messages;
namespace hs = sendspin::handshake;
using sendspin::crypto::Key32;

constexpr std::uint32_t kSampleRate = 48000;
// How long after the last burst's time the run waits before it looks at the device: past the slot
// the burst is written in, which the output fills a few bursts early.
constexpr std::int64_t lead_after = 400'000;

[[nodiscard]] std::uint64_t fnv1a(std::uint64_t hash, std::span<const std::byte> bytes) {
    for (const std::byte b : bytes) {
        hash ^= static_cast<std::uint64_t>(std::to_integer<unsigned>(b));
        hash *= 0x100000001B3ULL;
    }
    return hash;
}
constexpr std::uint64_t kFnvBasis = 0xCBF29CE484222325ULL;

// --- the bursts the host would send, and the chunks they are --------------------------------------

struct HostBurst {
    // The whole burst as the packer wrote it.
    std::vector<std::byte> burst;
    std::uint16_t pc = 0;
    std::uint16_t pd = 0;
    // The elementary-stream bytes the chunk carries.
    std::vector<std::uint8_t> payload;
    // The programme frame of its first sample.
    std::int64_t frame = 0;
};

[[nodiscard]] std::uint16_t word_at(const std::vector<std::byte>& words, std::size_t at) {
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(words[at]) |
                                      (std::to_integer<unsigned>(words[at + 1]) << 8U));
}

// The stream's bursts as hearth-testserver and the engine make them: an AC-3 syncframe is a burst of
// its own, and E-AC-3 access units are packed until a burst's worth (Eac3BurstPacker); the chunk
// carries the units the burst holds, and the Pc and Pd the packer wrote.
[[nodiscard]] std::expected<std::vector<HostBurst>, std::string> pack(std::span<const std::byte> bytes,
                                                                      int passes, std::size_t max_bursts,
                                                                      ac::DataType& type) {
    const std::expected<ac3::io::ScannedStream, ac3::io::ScanError> scanned = ac3::io::scan(bytes);
    if (!scanned) {
        return std::unexpected(std::string("the stream is not AC-3 or E-AC-3"));
    }
    if (scanned->sample_rate != ac3::SampleRate::k48000) {
        return std::unexpected(std::string("only 48 kHz streams are played"));
    }
    const bool eac3 = scanned->kind != ac3::io::StreamKind::kAc3;
    type = eac3 ? ac::DataType::kEac3 : ac::DataType::kAc3;
    const std::uint64_t pass_samples = ac3::io::stream_duration_samples(*scanned);

    std::vector<HostBurst> out;
    iec::Eac3BurstPacker packer;
    for (int pass = 0; pass < passes; ++pass) {
        std::vector<std::uint8_t> payload;
        std::optional<std::int64_t> first_frame;
        for (std::size_t index = 0; index < scanned->access_units.size(); ++index) {
            const std::span<const std::byte> unit = scanned->access_units[index];
            const std::optional<ac3::io::AccessUnitTiming> timing = ac3::io::access_unit_timing(*scanned, index);
            if (!timing) {
                return std::unexpected(std::string("an access unit has no timing"));
            }
            if (!first_frame) {
                first_frame = static_cast<std::int64_t>(pass_samples * static_cast<std::uint64_t>(pass) +
                                                        timing->start_sample);
            }
            for (const std::byte b : unit) {
                payload.push_back(std::to_integer<std::uint8_t>(b));
            }
            std::optional<std::vector<std::byte>> burst;
            if (!eac3) {
                std::expected<std::vector<std::byte>, iec::WrapError> wrapped = iec::wrap_frame(unit);
                if (!wrapped) {
                    return std::unexpected(std::string("an AC-3 frame could not be wrapped"));
                }
                burst = std::move(*wrapped);
            } else {
                std::expected<std::optional<std::vector<std::byte>>, iec::WrapError> packed = packer.push(unit);
                if (!packed) {
                    return std::unexpected(std::string("an E-AC-3 access unit could not be packed"));
                }
                burst = std::move(*packed);
            }
            if (burst) {
                if (max_bursts != 0 && out.size() >= max_bursts) {
                    return out;
                }
                HostBurst host;
                host.pc = word_at(*burst, 4);
                host.pd = word_at(*burst, 6);
                host.burst = std::move(*burst);
                host.payload = std::move(payload);
                host.frame = *first_frame;
                out.push_back(std::move(host));
                payload.clear();
                first_frame.reset();
            }
        }
    }
    return out;
}

// --- the device ----------------------------------------------------------------------------------

// What the Shield's AudioTrack did in the spikes (planning/hearth-android-findings.md, F12), on the
// real clock, and every burst it is handed written down: it does not move until 8 bursts are queued,
// starts about 200 ms after, takes a burst per period, and reports its position in 20 ms steps.
class ModelDevice final : public BurstLink {
   public:
    explicit ModelDevice(std::function<std::int64_t()> now_us) : now_us_(std::move(now_us)) {}

    [[nodiscard]] std::expected<void, std::string> open(const LinkFormat& format) override {
        const std::lock_guard lock(mutex_);
        open_ = true;
        format_ = format;
        reset_locked();
        return {};
    }
    void close() override {
        const std::lock_guard lock(mutex_);
        open_ = false;
    }
    [[nodiscard]] bool is_open() const override {
        const std::lock_guard lock(mutex_);
        return open_;
    }
    [[nodiscard]] bool submit(std::span<const std::byte> burst) override {
        const std::lock_guard lock(mutex_);
        if (!open_) {
            return false;
        }
        const std::int64_t now = now_us_();
        if (accepted_.size() - taken_bursts_locked(now) >= kCapacity) {
            return false;
        }
        accepted_.emplace_back(burst.begin(), burst.end());
        all_.emplace_back(burst.begin(), burst.end());
        if (!started_ && accepted_.size() >= kPrime) {
            started_ = true;
            start_us_ = now + kStartLatencyUs;
        }
        return true;
    }
    [[nodiscard]] std::optional<LinkPosition> position() const override {
        const std::lock_guard lock(mutex_);
        if (!open_) {
            return std::nullopt;
        }
        const std::uint64_t played = played_frames_locked(now_us_());
        const std::uint64_t total = accepted_.size() * static_cast<std::uint64_t>(format_.frames_per_burst);
        return LinkPosition{.frames_played = played, .frames_queued = total > played ? total - played : 0};
    }
    void flush() override {
        const std::lock_guard lock(mutex_);
        reset_locked();
    }
    void pause(bool /*paused*/) override {}
    [[nodiscard]] std::uint64_t underruns() const override {
        const std::lock_guard lock(mutex_);
        return 0;
    }

    // Every burst handed over, in order.
    [[nodiscard]] std::vector<std::vector<std::byte>> everything() const {
        const std::lock_guard lock(mutex_);
        return all_;
    }

   private:
    static constexpr std::size_t kCapacity = 8;
    static constexpr std::size_t kPrime = 8;
    static constexpr std::int64_t kStartLatencyUs = 200'000;
    static constexpr std::int64_t kStepUs = 20'000;

    void reset_locked() {
        accepted_.clear();
        started_ = false;
        start_us_ = 0;
    }
    [[nodiscard]] std::uint64_t played_frames_locked(std::int64_t now) const {
        if (!started_ || now <= start_us_) {
            return 0;
        }
        const std::int64_t steps = (now - start_us_) / kStepUs;
        const std::uint64_t frames = static_cast<std::uint64_t>(steps) * kStepUs * format_.sample_rate / 1'000'000ULL;
        return std::min<std::uint64_t>(frames, accepted_.size() * static_cast<std::uint64_t>(format_.frames_per_burst));
    }
    [[nodiscard]] std::size_t taken_bursts_locked(std::int64_t now) const {
        return static_cast<std::size_t>(played_frames_locked(now) / format_.frames_per_burst);
    }

    std::function<std::int64_t()> now_us_;
    mutable std::mutex mutex_;
    bool open_ = false;
    bool started_ = false;
    std::int64_t start_us_ = 0;
    LinkFormat format_;
    // What is queued on the device now, and everything it was ever handed (across flushes).
    std::vector<std::vector<std::byte>> accepted_;
    std::vector<std::vector<std::byte>> all_;
};

class DiscardingStream final : public StreamOutput {
   public:
    bool start(const m::PlayerStream& /*stream*/) override { return true; }
    void clear() override {}
    void end() override {}
    void write(std::span<const std::uint8_t> /*frame*/, std::int64_t /*local_time*/) override {}
    [[nodiscard]] OutputCounts counts() const override { return {}; }
    [[nodiscard]] const std::filesystem::path& file() const override { return none_; }

   private:
    std::filesystem::path none_;
};

class HarnessOutputs final : public Outputs {
   public:
    HarnessOutputs(ModelDevice& device, std::function<std::int64_t()> now_us)
        : device_(&device), now_us_(std::move(now_us)) {}

    [[nodiscard]] std::unique_ptr<StreamOutput> stream(std::uint64_t /*connection*/) override {
        return std::make_unique<DiscardingStream>();
    }
    [[nodiscard]] std::unique_ptr<BurstStreamOutput> bursts(std::uint64_t /*connection*/) override {
        auto output = std::make_unique<PassthroughOutput>(*device_, now_us_);
        output->run();
        const std::lock_guard lock(mutex_);
        last_ = output.get();
        return output;
    }

    // What the last connection's output has done; valid while that connection is open.
    [[nodiscard]] OutputCounts counts() const {
        const std::lock_guard lock(mutex_);
        return last_ != nullptr ? last_->counts() : OutputCounts{};
    }

   private:
    ModelDevice* device_;
    std::function<std::int64_t()> now_us_;
    mutable std::mutex mutex_;
    PassthroughOutput* last_ = nullptr;
};

class QuietLog final : public Log {
   public:
    void line(std::string_view /*text*/) override {}
};

// --- the server ------------------------------------------------------------------------------------

class Keys final : public hs::ServerKeyring {
   public:
    [[nodiscard]] hs::PskChoice choose(const Key32& /*client_key*/) const override { return hs::sentinel_choice(); }
};

class Events final : public sendspin::ServerListener {
   public:
    void on_hello(const m::ClientHello& /*hello*/) override { update([&] { ++hellos; }); }
    void on_state(const m::ClientState& state) override { update([&] { available = state.available; }); }
    void on_goodbye(m::GoodbyeReason /*reason*/) override {}
    void on_leave() override {}
    void on_pairing_held_back(const std::optional<std::string>& /*message*/) override {}
    void on_pairing_code_wanted() override {}
    bool on_paired(const Key32& /*client_key*/, const Key32& /*psk*/) override { return true; }
    void on_pairing_ended(std::optional<sendspin::pairing_messages::AbortReason> /*reason*/) override {}

    template <class Predicate>
    bool wait(Predicate&& predicate, std::chrono::microseconds timeout) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, std::forward<Predicate>(predicate));
    }

    int hellos = 0;
    bool available = false;

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

}  // namespace

Result play_through(std::span<const std::byte> stream, const Options& options) {
    Result result;

    ac::DataType type = ac::DataType::kAc3;
    std::expected<std::vector<HostBurst>, std::string> packed = pack(stream, options.passes, options.max_bursts, type);
    if (!packed) {
        result.error = packed.error();
        return result;
    }
    const std::vector<HostBurst>& bursts = *packed;
    result.sent = bursts.size();
    if (bursts.empty()) {
        result.error = "the stream has no burst in it";
        return result;
    }
    std::uint64_t sent_hash = kFnvBasis;
    for (const HostBurst& burst : bursts) {
        sent_hash = fnv1a(sent_hash, burst.burst);
    }
    result.sent_hash = sent_hash;

    // The sink, with the pass-through output over a model of a device.
    const sendspin::SteadyClock clock;
    const auto now = [&clock] { return clock.now_us(); };
    ModelDevice device(now);
    HarnessOutputs outputs(device, now);
    QuietLog log;
    sink::Options sink_options;
    sink_options.name = "Loopback harness sink";
    sink_options.product_name = "iclforge-sink-harness";
    sink_options.address = "127.0.0.1";
    sink_options.port = 0;
    sink_options.state_directory = options.state_directory;
    sink_options.advertise = false;
    sink_options.codecs = {m::Codec::kPcm};
    sink_options.unpaired_access = true;
    sink_options.data_types = {ac::DataType::kAc3, ac::DataType::kEac3};
    sink_options.required_lead_time_ms = 800;
    std::expected<std::unique_ptr<sink::Sink>, std::string> started =
        sink::Sink::start(std::move(sink_options), outputs, log);
    if (!started) {
        result.error = "the sink did not start: " + started.error();
        return result;
    }
    std::unique_ptr<sink::Sink> sink = std::move(*started);

    const auto began = std::chrono::steady_clock::now();
    const auto since = [](std::chrono::steady_clock::time_point from) {
        return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - from).count();
    };
    // A server, dialled at it.
    const std::optional<sendspin::noise::KeyPair> identity = sendspin::noise::KeyPair::generate();
    if (!identity) {
        result.error = "no identity for the server could be made";
        return result;
    }
    Keys keys;
    Events events;
    sendspin::ServerSession server(
        {.identity = *identity, .name = "Loopback harness server", .languages = {}, .max_message_bytes = 1 << 22},
        keys, events, clock);
    auto dialled =
        sendspin::transport::websocket::connect("ws://127.0.0.1:" + std::to_string(sink->port()) + "/sendspin");
    if (!dialled) {
        result.error = "the server could not dial the sink";
        return result;
    }
    sendspin::SessionDriver driver(std::move(*dialled),
                                   {.receive = [&](const auto& frame) { return server.receive(frame); },
                                    .tick = [&] { return server.tick(); },
                                    .next_tick_us = [&] { return server.next_tick_us(); },
                                    .ended = {}});
    driver.start();
    const auto budget = std::chrono::microseconds(options.timeout_us);
    if (!events.wait([&] { return events.hellos >= 1; }, budget)) {
        result.error = "the sink did not answer the server's hello";
        return result;
    }
    if (!driver.call([&] {
                  return server.activate({.activities = {m::Activity::kPlayback},
                                          .active_roles = std::vector<std::string>{std::string(ac::kRole)},
                                          .pairing = std::nullopt});
              }).has_value()) {
        result.error = "the server could not activate the sink for playback";
        return result;
    }
    if (!events.wait([&] { return events.available; }, budget)) {
        result.error = "the sink did not become available";
        return result;
    }
    if (!driver.call([&] {
                  return server.start_burst_stream({.data_type = type, .sample_rate = kSampleRate});
              }).has_value()) {
        result.error = "the server could not start the burst stream";
        return result;
    }

    result.setup_us = since(began);
    const auto played_from = std::chrono::steady_clock::now();
    // The bursts, each sent `lead_us` before its time. The first is due a lead and a moment from now,
    // so that it is sent a moment from now.
    const std::int64_t origin = clock.now_us() + options.lead_us + 200'000;
    const auto steady_origin = std::chrono::steady_clock::now();
    std::int64_t last_due = origin;
    for (const HostBurst& burst : bursts) {
        const std::int64_t due = origin + burst.frame * 1'000'000 / kSampleRate;
        last_due = due;
        const auto send_at = steady_origin + std::chrono::microseconds(due - origin + 200'000);
        std::this_thread::sleep_until(send_at);
        const auto sent = driver.call([&] { return server.send_burst(due, burst.pc, burst.pd, burst.payload); });
        if (!sent.has_value()) {
            result.error = "the server could not send burst " + std::to_string(&burst - bursts.data());
            return result;
        }
    }

    // Until the last of them has been due, and a little after.
    std::this_thread::sleep_until(
        steady_origin + std::chrono::microseconds(last_due - origin + options.lead_us + 200'000 + lead_after));
    result.counts = outputs.counts();
    result.play_us = since(played_from);
    const auto closing_from = std::chrono::steady_clock::now();
    static_cast<void>(driver.call([&] { return server.end_burst_stream(); }));
    driver.close();
    driver.join();
    sink.reset();
    result.teardown_us = since(closing_from);

    // What the device was handed, less the null bursts.
    const std::expected<std::vector<std::byte>, iec::WrapError> null = iec::null_burst(bursts.front().burst.size());
    const std::vector<std::vector<std::byte>> handed = device.everything();
    std::vector<const std::vector<std::byte>*> written;
    for (const std::vector<std::byte>& burst : handed) {
        if (null && burst == *null) {
            ++result.nulls;
        } else {
            written.push_back(&burst);
        }
    }
    result.written = written.size();
    std::uint64_t written_hash = kFnvBasis;
    for (const auto* burst : written) {
        written_hash = fnv1a(written_hash, *burst);
    }
    result.written_hash = written_hash;
    result.first_difference = bursts.size();
    result.equal = written.size() == bursts.size();
    for (std::size_t index = 0; index < std::min(written.size(), bursts.size()); ++index) {
        if (*written[index] != bursts[index].burst) {
            result.equal = false;
            result.first_difference = index;
            break;
        }
    }
    if (result.equal) {
        result.first_difference = bursts.size();
    } else if (result.first_difference == bursts.size()) {
        result.first_difference = std::min(written.size(), bursts.size());
    }
    return result;
}

}  // namespace iclforge::sink::harness
