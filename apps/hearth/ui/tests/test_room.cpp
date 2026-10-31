#include "test_room.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <numbers>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <thread>
#include <utility>

#include "iclforge/audio/monitor.hpp"
#include "iclforge/audio/passthrough.hpp"
#include "iclforge/audio/pcm_output.hpp"
#include "iclforge/audio/speakers.hpp"
#include "iclforge/containers/iec61937/iec61937.hpp"
#include "iclforge/render/layout.hpp"
#include "iclforge/render/routing.hpp"
#include "iclforge/sendspin/discovery.hpp"
#include "iclforge/ac3/core/eac3_tables.hpp"
#include "iclforge/ac3/encoder/eac3_frame.hpp"
#include "iclforge/ac3/encoder/encoder.hpp"
#include "iclforge/ac3/meta/bsi.hpp"
#include "iclforge/ac4/encoder/encoder.hpp"
#include "bitstream_sink.hpp"
#include "network_sinks.hpp"
#include "output_selector.hpp"
#include "pcm_sink.hpp"
#include "sink.hpp"
#include "test_outputs.hpp"
#include "transport.hpp"

namespace iclforge::hearth::uitest {

// The room: its endpoints, and the one device the engine has open among them
// - a PcmSink is one open stream at a time, whichever endpoint it names, the
// way DeviceSink wraps one iclforge::audio::PcmOutput. Everything is under one
// lock: the engine thread submits and reads the position, the clock thread
// plays, and the GUI thread reads the totals a suite asserts on. The shape is
// apps/hearth/engine/tests/test_engine.cpp's ClockedDevice, plus what the Speakers page
// and the output picker read off a real device (routing, name, id, mask).
class FakeRoom {
public:
    explicit FakeRoom(std::vector<FakeEndpoint> endpoints)
        : endpoints_(std::move(endpoints)),
          clock_([this](const std::stop_token& stop) {
              // The device's time is the steady clock's, read at each wake: a sleep of one
              // millisecond lasts a timer tick, 15.6 ms on Windows by default, so counting the
              // sleeps ran this device at a sixteenth of real time there and made every wait on
              // its frames cost sixteen times its audio (the AC-4 decoder page's suite took 298 s
              // of its 300 on the Windows leg, 31 s on Linux).
              auto last = std::chrono::steady_clock::now();
              while (!stop.stop_requested()) {
                  std::this_thread::sleep_for(std::chrono::milliseconds(1));
                  const auto now = std::chrono::steady_clock::now();
                  tick(std::chrono::duration<double, std::milli>(now - last).count());
                  last = now;
              }
          }) {}

    ~FakeRoom() {
        clock_.request_stop();
        clock_.join();
    }

    FakeRoom(const FakeRoom&) = delete;
    FakeRoom& operator=(const FakeRoom&) = delete;
    FakeRoom(FakeRoom&&) = delete;
    FakeRoom& operator=(FakeRoom&&) = delete;

    // A copy, taken under the lock: a suite can change what an endpoint takes
    // (set_passthrough()) while the engine and the window read the room.
    [[nodiscard]] std::vector<FakeEndpoint> endpoints() const {
        const std::scoped_lock lock(mutex_);
        return endpoints_;
    }

    // Whether `id` says it takes a bitstream from here on: a receiver switched
    // on, or off. What is open stays open, as a real link does until it is told.
    bool set_passthrough(const std::string& id, bool takes) {
        const std::scoped_lock lock(mutex_);
        for (FakeEndpoint& endpoint : endpoints_) {
            if (endpoint.id == id) {
                endpoint.passthrough = takes;
                return true;
            }
        }
        return false;
    }

    std::expected<OpenOutputFormat, std::string> open(const PcmSink::Format& format) {
        const std::scoped_lock lock(mutex_);
        const FakeEndpoint* endpoint = find_locked(format.endpoint_id);
        if (endpoint == nullptr) {
            return std::unexpected("The output could not be opened: no endpoint " + format.endpoint_id + ".");
        }
        open_ = true;
        paused_ = false;
        endpoint_ = *endpoint;
        sample_rate_ = format.sample_rate;
        width_ = endpoint->channels > 0 ? endpoint->channels : static_cast<std::uint16_t>(format.layout.slots());
        mask_ = endpoint->speakers != 0 ? endpoint->speakers : audio::default_speakers(width_);
        routing_ = audio::speaker_routing(format.layout, mask_, width_);
        submitted_ = 0;
        heard_ = 0;
        clock_frames_ = 0;
        carry_ = 0.0;
        ++opens_;
        recent_.assign(format.layout.slots(), std::vector<float>(kToneWindow, 0.0F));
        recent_at_ = 0;
        recent_filled_ = 0;
        return OpenOutputFormat{.sample_rate = format.sample_rate, .channels = width_, .mode = OutputMode::kLocalPcm};
    }

    void close() {
        const std::scoped_lock lock(mutex_);
        open_ = false;
    }

    [[nodiscard]] bool is_open() const {
        const std::scoped_lock lock(mutex_);
        return open_;
    }

    bool submit(std::span<const std::span<const float>> slots, std::size_t frames) {
        const std::scoped_lock lock(mutex_);
        if (!open_ || submitted_ - heard_ + frames > kCapacity) {
            return false;
        }
        for (const std::span<const float> slot : slots) {
            for (std::size_t n = 0; n < frames && n < slot.size(); ++n) {
                peak_ = std::max(peak_, static_cast<double>(std::fabs(slot[n])));
            }
        }
        // The last kToneWindow samples of each slot, for tone_level_db().
        for (std::size_t n = 0; n < frames; ++n) {
            for (std::size_t s = 0; s < recent_.size(); ++s) {
                recent_[s][recent_at_] =
                    s < slots.size() && n < slots[s].size() ? slots[s][n] : 0.0F;
            }
            recent_at_ = (recent_at_ + 1) % kToneWindow;
        }
        recent_filled_ = std::min(recent_filled_ + frames, kToneWindow);
        submitted_ += frames;
        submitted_total_ += frames;
        return true;
    }

    [[nodiscard]] double tone_level_db(std::size_t slot, double hz) const {
        const std::scoped_lock lock(mutex_);
        if (slot >= recent_.size() || recent_filled_ < kToneWindow || sample_rate_ == 0) {
            return -std::numeric_limits<double>::infinity();
        }
        const double w = 2.0 * std::numbers::pi * hz / static_cast<double>(sample_rate_);
        std::complex<double> sum{};
        double weights = 0.0;
        for (std::size_t k = 0; k < kToneWindow; ++k) {
            const double hann =
                0.5 - (0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(k) /
                                      static_cast<double>(kToneWindow - 1)));
            const float sample = recent_[slot][(recent_at_ + k) % kToneWindow];
            sum +=
                hann * static_cast<double>(sample) * std::polar(1.0, -w * static_cast<double>(k));
            weights += hann;
        }
        return 20.0 * std::log10(std::abs(2.0 * sum / weights));
    }

    [[nodiscard]] std::optional<audio::MonitorPosition> position() const {
        const std::scoped_lock lock(mutex_);
        if (!open_) {
            return std::nullopt;
        }
        return audio::MonitorPosition{
            .frames_played = clock_frames_, .frames_queued = submitted_ - heard_, .latency_frames = 0};
    }

    void flush() {
        const std::scoped_lock lock(mutex_);
        submitted_ = 0;
        heard_ = 0;
        clock_frames_ = 0;
    }

    bool pause() {
        const std::scoped_lock lock(mutex_);
        paused_ = true;
        return open_;
    }

    bool resume() {
        const std::scoped_lock lock(mutex_);
        paused_ = false;
        return open_;
    }

    bool set_routing(const render::Routing& routing) {
        const std::scoped_lock lock(mutex_);
        if (!open_ || routing.outputs() != width_) {
            return false;
        }
        routing_ = routing;
        return true;
    }

    [[nodiscard]] render::Routing routing() const {
        const std::scoped_lock lock(mutex_);
        return open_ ? routing_ : render::Routing{};
    }

    [[nodiscard]] std::string device_name() const {
        const std::scoped_lock lock(mutex_);
        return open_ ? endpoint_.name : std::string();
    }

    [[nodiscard]] std::string device_id() const {
        const std::scoped_lock lock(mutex_);
        return open_ ? endpoint_.id : std::string();
    }

    [[nodiscard]] std::uint32_t speaker_mask() const {
        const std::scoped_lock lock(mutex_);
        return open_ ? mask_ : 0;
    }

    [[nodiscard]] RoomReading reading() const {
        const std::scoped_lock lock(mutex_);
        return RoomReading{.open = open_,
                           .paused = paused_,
                           .endpoint = open_ ? endpoint_.id : std::string(),
                           .sample_rate = open_ ? sample_rate_ : 0,
                           .channels = open_ ? width_ : std::uint16_t{0},
                           .opens = opens_,
                           .frames_heard = heard_total_,
                           .frames_submitted = submitted_total_,
                           .peak = peak_};
    }

    void reset_peak() {
        const std::scoped_lock lock(mutex_);
        peak_ = 0.0;
    }

    void set_speed(double speed) {
        const std::scoped_lock lock(mutex_);
        speed_ = std::max(0.0, speed);
    }

    // --- the link: the room's IEC 61937 output, on an endpoint that takes one.
    // A BitstreamSink is one open link at a time, as the PCM device above is
    // one open stream; apps/hearth/engine/tests/test_player_bitstream.cpp's
    // FakeLink is the shape, with the room's clock. Every burst is read back
    // as a receiver would read it (BurstReader), so a suite can hold what the
    // window sent to the bytes of the file.

    std::expected<OpenOutputFormat, std::string> open_link(const BitstreamSink::Format& format) {
        const std::scoped_lock lock(mutex_);
        const FakeEndpoint* endpoint = find_locked(format.endpoint_id);
        if (endpoint == nullptr || !endpoint->passthrough) {
            return std::unexpected("The output would not take " +
                                   std::string(audio::format_name(format.format)) +
                                   " over IEC 61937: no such endpoint " + format.endpoint_id + ".");
        }
        link_open_ = true;
        link_paused_ = false;
        link_endpoint_ = *endpoint;
        link_format_ = format.format;
        link_rate_ = format.sample_rate;
        link_submitted_ = 0;
        link_heard_ = 0;
        link_clock_ = 0;
        link_carry_ = 0.0;
        ++link_opens_;
        link_reader_ = containers::iec61937::BurstReader{};
        link_payload_.clear();
        return OpenOutputFormat{.sample_rate = format.sample_rate,
                                .channels = 2,
                                .mode = OutputMode::kBitstream,
                                .stream = format.format};
    }

    void close_link() {
        const std::scoped_lock lock(mutex_);
        link_open_ = false;
    }

    [[nodiscard]] bool link_is_open() const {
        const std::scoped_lock lock(mutex_);
        return link_open_;
    }

    bool submit_burst(std::span<const std::byte> burst) {
        const std::scoped_lock lock(mutex_);
        constexpr std::uint64_t kBurstFrames = 1536;
        if (!link_open_ || (link_submitted_ - link_heard_) / kBurstFrames + 1 > kLinkCapacityBursts) {
            return false;
        }
        const std::size_t expected = link_format_ == audio::BitstreamFormat::kEac3
                                         ? containers::iec61937::kEac3BurstBytes
                                         : containers::iec61937::kBurstBytes;
        if (burst.size() != expected || !link_reader_.push(burst, link_payload_).has_value()) {
            // Taken all the same, and counted: refused, the player would
            // offer the same burst at every pump.
            ++link_bad_bursts_;
        }
        ++link_bursts_;
        link_submitted_ += kBurstFrames;
        return true;
    }

    [[nodiscard]] std::optional<audio::MonitorPosition> link_position() const {
        const std::scoped_lock lock(mutex_);
        if (!link_open_) {
            return std::nullopt;
        }
        return audio::MonitorPosition{.frames_played = link_clock_,
                                      .frames_queued = link_submitted_ - link_heard_,
                                      .latency_frames = 0};
    }

    void flush_link() {
        const std::scoped_lock lock(mutex_);
        link_submitted_ = 0;
        link_heard_ = 0;
        link_clock_ = 0;
    }

    bool pause_link() {
        const std::scoped_lock lock(mutex_);
        link_paused_ = true;
        return link_open_;
    }

    bool resume_link() {
        const std::scoped_lock lock(mutex_);
        link_paused_ = false;
        return link_open_;
    }

    [[nodiscard]] LinkReading link_reading() const {
        const std::scoped_lock lock(mutex_);
        LinkReading reading;
        reading.open = link_open_;
        reading.paused = link_paused_;
        reading.endpoint = link_open_ ? link_endpoint_.id : std::string();
        reading.format = link_open_ ? std::string(audio::format_name(link_format_)) : std::string();
        reading.sample_rate = link_open_ ? link_rate_ : 0;
        reading.opens = link_opens_;
        reading.bursts = link_bursts_;
        reading.bad_bursts = link_bad_bursts_;
        reading.frames_heard = link_heard_total_;
        if (const auto type = link_reader_.data_type()) {
            reading.data_type = std::string(containers::iec61937::data_type_name(*type));
        }
        reading.payload_bytes = link_payload_.size();
        return reading;
    }

    // Whether what the link has carried since it last opened is the start of
    // `stream` - or, with `whole`, all of it: the elementary stream's own
    // bytes, in order, none changed.
    [[nodiscard]] bool link_carried(std::span<const std::byte> stream, bool whole) const {
        const std::scoped_lock lock(mutex_);
        if (link_payload_.empty() || link_payload_.size() > stream.size() ||
            (whole && link_payload_.size() != stream.size())) {
            return false;
        }
        return std::equal(link_payload_.begin(), link_payload_.end(), stream.begin());
    }

private:
    // Frames the device holds before submit() refuses - a third of a second
    // at 48 kHz, enough for the engine's own pump budget to keep it fed.
    static constexpr std::uint64_t kCapacity = 16384;

    // `milliseconds` of the device's own clock: what it holds is heard
    // first, and the clock runs on whether or not there is anything left to
    // hear, as a real device does through an underrun.
    void tick(double milliseconds) {
        const std::scoped_lock lock(mutex_);
        if (link_open_ && !link_paused_ && link_rate_ != 0) {
            // The link's clock counts the content's frames, as
            // iclforge::audio::PassthroughSink::position() does.
            link_carry_ += static_cast<double>(link_rate_) * speed_ * milliseconds / 1000.0;
            const auto frames = static_cast<std::uint64_t>(link_carry_);
            link_carry_ -= static_cast<double>(frames);
            const std::uint64_t now = std::min(link_submitted_ - link_heard_, frames);
            link_heard_ += now;
            link_heard_total_ += now;
            link_clock_ += frames;
        }
        if (!open_ || paused_ || sample_rate_ == 0) {
            return;
        }
        carry_ += static_cast<double>(sample_rate_) * speed_ * milliseconds / 1000.0;
        const auto frames = static_cast<std::uint64_t>(carry_);
        carry_ -= static_cast<double>(frames);
        const std::uint64_t now = std::min(submitted_ - heard_, frames);
        heard_ += now;
        heard_total_ += now;
        clock_frames_ += frames;
    }

    // An empty id is the engine's "the sink's own", which for a real device
    // is the enumeration's default - the same reading here.
    const FakeEndpoint* find_locked(const std::string& id) const {
        for (const FakeEndpoint& endpoint : endpoints_) {
            if (id.empty() ? endpoint.is_default : endpoint.id == id) {
                return &endpoint;
            }
        }
        return id.empty() && !endpoints_.empty() ? endpoints_.data() : nullptr;
    }

    std::vector<FakeEndpoint> endpoints_;
    mutable std::mutex mutex_;
    bool open_ = false;
    bool paused_ = false;
    FakeEndpoint endpoint_{};
    std::uint32_t sample_rate_ = 0;
    std::uint16_t width_ = 0;
    std::uint32_t mask_ = 0;
    render::Routing routing_{};
    std::uint64_t submitted_ = 0;
    std::uint64_t heard_ = 0;
    std::uint64_t heard_total_ = 0;
    std::uint64_t submitted_total_ = 0;
    std::uint64_t clock_frames_ = 0;
    std::uint32_t opens_ = 0;
    double carry_ = 0.0;
    double speed_ = 1.0;
    double peak_ = 0.0;
    // Each rendered slot's last kToneWindow samples as submitted, a ring
    // written at recent_at_.
    std::vector<std::vector<float>> recent_;
    std::size_t recent_at_ = 0;
    std::size_t recent_filled_ = 0;

    // Bursts the link holds before submit_burst() refuses: a quarter of a
    // second, as the device's kCapacity is of its own.
    static constexpr std::uint64_t kLinkCapacityBursts = 8;
    bool link_open_ = false;
    bool link_paused_ = false;
    FakeEndpoint link_endpoint_{};
    audio::BitstreamFormat link_format_ = audio::BitstreamFormat::kAc3;
    std::uint32_t link_rate_ = 0;
    std::uint64_t link_submitted_ = 0;
    std::uint64_t link_heard_ = 0;
    std::uint64_t link_heard_total_ = 0;
    std::uint64_t link_clock_ = 0;
    double link_carry_ = 0.0;
    std::uint32_t link_opens_ = 0;
    std::uint64_t link_bursts_ = 0;
    std::uint64_t link_bad_bursts_ = 0;
    // Since the link last opened: the reader its bursts went through, and the
    // elementary stream they carried.
    containers::iec61937::BurstReader link_reader_;
    std::vector<std::byte> link_payload_;

    // Last, so it starts after, and is joined before, everything it reads.
    std::jthread clock_;
};

namespace {

// The engine owns its PcmSink outright, the room outlives it (TestServices
// holds it for the process): this forwards every call to the room.
class RoomSink final : public PcmSink {
public:
    explicit RoomSink(std::shared_ptr<FakeRoom> room) : room_(std::move(room)) {}

    std::expected<OpenOutputFormat, std::string> open(const Format& format) override { return room_->open(format); }
    void close() override { room_->close(); }
    [[nodiscard]] bool is_open() const override { return room_->is_open(); }
    bool submit(std::span<const std::span<const float>> slots, std::size_t frames) override {
        return room_->submit(slots, frames);
    }
    [[nodiscard]] std::optional<audio::MonitorPosition> position() const override { return room_->position(); }
    void flush() override { room_->flush(); }
    bool pause() override { return room_->pause(); }
    bool resume() override { return room_->resume(); }
    bool set_routing(const render::Routing& routing) override { return room_->set_routing(routing); }
    [[nodiscard]] render::Routing routing() const override { return room_->routing(); }
    [[nodiscard]] std::string device_name() const override { return room_->device_name(); }
    [[nodiscard]] std::string device_id() const override { return room_->device_id(); }
    [[nodiscard]] std::uint32_t speaker_mask() const override { return room_->speaker_mask(); }

private:
    std::shared_ptr<FakeRoom> room_;
};

// The engine's passthrough sink over the room's link, as RoomSink is its PCM
// sink over the room's device.
class RoomLink final : public BitstreamSink {
public:
    explicit RoomLink(std::shared_ptr<FakeRoom> room) : room_(std::move(room)) {}

    std::expected<OpenOutputFormat, std::string> open(const Format& format) override {
        return room_->open_link(format);
    }
    void close() override { room_->close_link(); }
    [[nodiscard]] bool is_open() const override { return room_->link_is_open(); }
    bool submit(std::span<const std::byte> burst) override { return room_->submit_burst(burst); }
    [[nodiscard]] std::optional<audio::MonitorPosition> position() const override {
        return room_->link_position();
    }
    void flush() override { room_->flush_link(); }
    bool pause() override { return room_->pause_link(); }
    bool resume() override { return room_->resume_link(); }

private:
    std::shared_ptr<FakeRoom> room_;
};

audio::RenderDeviceInfo device_info(const FakeEndpoint& endpoint) {
    audio::RenderDeviceInfo info;
    info.id = endpoint.id;
    info.name = endpoint.name;
    info.is_default = endpoint.is_default;
    info.supports_ac3_passthrough = endpoint.passthrough;
    info.supports_eac3_passthrough = endpoint.passthrough;
    // A receiver that takes a bitstream takes TrueHD's MAT link too, as the picker's row shows.
    info.supports_mat_passthrough = endpoint.passthrough;
    info.supports_exclusive_pcm = true;
    info.channels = endpoint.channels;
    info.speakers = endpoint.speakers != 0 ? endpoint.speakers : audio::default_speakers(endpoint.channels);
    info.sample_rates = {44100, 48000};
    return info;
}

}  // namespace

std::shared_ptr<FakeRoom> make_room(std::vector<FakeEndpoint> endpoints) {
    return std::make_shared<FakeRoom>(std::move(endpoints));
}

std::shared_ptr<iclforge::hearth::ui::TestOutputs> outputs_for(
    const std::shared_ptr<FakeRoom>& room) {
    auto outputs = std::make_shared<iclforge::hearth::ui::TestOutputs>();
    outputs->make_pcm = [room] { return std::make_unique<RoomSink>(room); };
    outputs->make_bitstream = [room] { return std::make_unique<RoomLink>(room); };
    outputs->endpoints = [room](std::uint32_t /*sample_rate*/) {
        std::vector<EndpointReading> readings;
        for (const FakeEndpoint& endpoint : room->endpoints()) {
            readings.push_back(EndpointReading{.device = device_info(endpoint)});
        }
        return readings;
    };
    outputs->enumerate = [room]() -> decltype(audio::enumerate_render_devices()) {
        std::vector<audio::RenderDeviceInfo> devices;
        for (const FakeEndpoint& endpoint : room->endpoints()) {
            devices.push_back(device_info(endpoint));
        }
        return devices;
    };
    return outputs;
}

RoomReading read(const FakeRoom& room) {
    return room.reading();
}

LinkReading read_link(const FakeRoom& room) {
    return room.link_reading();
}

bool link_carried(const FakeRoom& room, const std::vector<std::string>& paths, bool whole) {
    std::vector<char> bytes;
    for (const std::string& path : paths) {
        std::ifstream file(std::filesystem::path(path), std::ios::binary);
        if (!file) {
            return false;
        }
        bytes.insert(bytes.end(), std::istreambuf_iterator<char>(file),
                     std::istreambuf_iterator<char>());
    }
    return room.link_carried(std::as_bytes(std::span<const char>(bytes)), whole);
}

void reset_peak(FakeRoom& room) {
    room.reset_peak();
}

bool set_passthrough(FakeRoom& room, const std::string& id, bool takes) {
    return room.set_passthrough(id, takes);
}

void set_speed(FakeRoom& room, double speed) {
    room.set_speed(speed);
}

double tone_level_db(const FakeRoom& room, std::size_t slot, double hz) {
    return room.tone_level_db(slot, hz);
}

// --- AC-4 tone streams -----------------------------------------------------

namespace {

constexpr int kToneRate = 48000;
constexpr std::size_t kToneSamples = 20 * 48000;
constexpr double kToneAmplitude = 0.1;

[[nodiscard]] std::vector<float> tone(double hz) {
    std::vector<float> out(kToneSamples);
    for (std::size_t n = 0; n < out.size(); ++n) {
        out[n] = static_cast<float>(kToneAmplitude * std::sin(2.0 * std::numbers::pi * hz *
                                                              static_cast<double>(n) / kToneRate));
    }
    return out;
}

[[nodiscard]] iclforge::ac4::EncoderConfig tones_config() {
    iclforge::ac4::EncoderConfig config;
    config.channels = 6;
    config.bitrate_kbps = 384;
    config.dialnorm_db = -24.0;
    config.downmix = iclforge::ac4::DownmixConfig{.loro_centre_db = -1.5,
                                        .loro_surround_db = -4.5,
                                        .ltrt_centre_db = -3.0,
                                        .ltrt_surround_db = -6.0,
                                        .lfe_db = -4.5,
                                        .preferred = iclforge::ac4::PreferredDownmix::kLtRt,
                                        .loro_correction_db2 = std::nullopt,
                                        .ltrt_correction_db2 = std::nullopt};
    config.dialogue = iclforge::ac4::DialogueConfig{};
    config.dialogue->max_gain_db = 9;
    return config;
}

[[nodiscard]] iclforge::ac4::EncoderConfig presentations_config() {
    iclforge::ac4::EncoderConfig config;
    config.bitrate_kbps = 256;
    iclforge::ac4::SubstreamConfig music;
    music.channels = 2;
    music.bitrate_kbps = 128;
    music.content = iclforge::ac4::ContentClassifier::kMusicAndEffects;
    iclforge::ac4::SubstreamConfig dialogue;
    dialogue.channels = 1;
    dialogue.bitrate_kbps = 64;
    dialogue.content = iclforge::ac4::ContentClassifier::kDialogue;
    dialogue.language = "en";
    dialogue.dialogue_mix = iclforge::ac4::DialogueMix{.max_gain_db = 6, .pan_degrees = {}};
    iclforge::ac4::SubstreamConfig described;
    described.channels = 1;
    described.bitrate_kbps = 48;
    described.content = iclforge::ac4::ContentClassifier::kVisuallyImpaired;
    described.language = "qad";
    config.substreams = {music, dialogue, described};
    iclforge::ac4::PresentationConfig plain;
    plain.config = 0;
    plain.substreams = {0, 1};
    plain.presentation_id = 1;
    iclforge::ac4::PresentationConfig with_description;
    with_description.config = 3;
    with_description.substreams = {0, 1, 2};
    with_description.presentation_id = 2;
    config.presentations = {plain, with_description};
    return config;
}

}  // namespace

bool write_ac4_stream(const std::string& path, const std::string& kind, std::string* error) {
    const auto fail = [error](std::string why) {
        if (error != nullptr) {
            *error = std::move(why);
        }
        return false;
    };
    iclforge::ac4::EncoderConfig config;
    std::vector<std::vector<float>> input;
    if (kind == "tones") {
        config = tones_config();
        for (const double hz : {440.0, 620.0, 800.0, 90.0, 1030.0, 1270.0}) {
            input.push_back(tone(hz));
        }
    } else if (kind == "presentations") {
        config = presentations_config();
        for (const double hz : {331.0, 457.0, 1117.0, 1531.0}) {
            input.push_back(tone(hz));
        }
    } else {
        return fail("no such kind of stream: " + kind);
    }
    auto encoder = iclforge::ac4::Encoder::create(config);
    if (!encoder) {
        return fail(std::string{iclforge::ac4::Encoder::refusal_reason(config)});
    }
    std::ofstream out(std::filesystem::path(path), std::ios::binary);
    if (!out) {
        return fail("could not open " + path);
    }
    const auto write = [&out](const std::vector<iclforge::ac4::EncodedFrame>& frames) {
        for (const iclforge::ac4::EncodedFrame& frame : frames) {
            const std::vector<std::byte> wrapped =
                iclforge::ac4::sync_frame(frame.raw_ac4_frame, false);
            out.write(reinterpret_cast<const char*>(wrapped.data()),
                      static_cast<std::streamsize>(wrapped.size()));
        }
    };
    const std::vector<std::span<const float>> views(input.begin(), input.end());
    const auto frames = encoder->encode(views);
    if (!frames) {
        return fail("the encoder refused the input");
    }
    write(*frames);
    const auto rest = encoder->flush();
    if (!rest) {
        return fail("the encoder could not flush");
    }
    write(*rest);
    return out.good() ? true : fail("could not write " + path);
}

namespace {

// An E-AC-3 programme of `kind` ("karaoke-3/2" or "karaoke-3/2+rear") as the AC-3 case's tones,
// one independent substream flagged bsmod 7 with a 3/2 bed and an LFE; "+rear" adds the rear
// pair in a dependent substream (a 7.1 programme, which Annex C has no vocals for).
bool write_eac3_stream(const std::string& path, const std::string& kind, std::string* error) {
    const auto fail = [error](std::string why) {
        if (error != nullptr) {
            *error = std::move(why);
        }
        return false;
    };
    const bool rear = kind == "karaoke-3/2+rear";
    if (kind != "karaoke-3/2" && !rear) {
        return fail("no such kind of E-AC-3 stream: " + kind);
    }
    iclforge::ac3::eac3::FrameConfig bed;
    bed.sample_rate = iclforge::ac3::SampleRate::k48000;
    bed.bitrate_kbps = 448;
    bed.acmod = iclforge::ac3::Acmod::k3_2;
    bed.lfe = true;
    bed.info = iclforge::ac3::meta::BsiInfo{.bsmod = iclforge::ac3::meta::BitstreamMode::kVoiceOverOrKaraoke};
    iclforge::ac3::eac3::AccessUnitConfig config;
    config.independent = bed;
    if (rear) {
        iclforge::ac3::eac3::FrameConfig pair;
        pair.sample_rate = iclforge::ac3::SampleRate::k48000;
        pair.bitrate_kbps = 320;
        pair.acmod = iclforge::ac3::Acmod::k2_2;
        pair.chanmap = iclforge::ac3::eac3::chanmap::k71Rear;
        config.dependents.push_back(pair);
    }
    iclforge::ac3::eac3::AccessUnitEncoder encoder{config};
    const auto channels = static_cast<std::size_t>(encoder.channel_count());
    // The bed's six, as the AC-3 case has them, then the dependent's four.
    constexpr std::array<double, 10> kHz = {440.0, 800.0, 620.0, 1030.0, 1270.0, 90.0,
                                            1500.0, 1700.0, 1900.0, 2100.0};
    std::vector<std::vector<float>> input;
    for (std::size_t c = 0; c < channels; ++c) {
        input.push_back(tone(kHz[c]));
    }
    std::ofstream out(std::filesystem::path(path), std::ios::binary);
    if (!out) {
        return fail("could not open " + path);
    }
    const std::size_t frame = iclforge::ac3::kSamplesPerFrame;
    const std::size_t frames = (10 * 48000) / frame;
    for (std::size_t f = 0; f < frames; ++f) {
        std::vector<std::span<const float>> views;
        for (const auto& channel : input) {
            views.emplace_back(channel.data() + (f * frame), frame);
        }
        const auto unit = encoder.encode_access_unit(views);
        if (!unit) {
            return fail("the encoder refused an access unit");
        }
        out.write(reinterpret_cast<const char*>(unit->bytes.data()),
                  static_cast<std::streamsize>(unit->bytes.size()));
    }
    return out.good() ? true : fail("could not write " + path);
}

}  // namespace

bool write_ac3_stream(const std::string& path, const std::string& kind, std::string* error) {
    const auto fail = [error](std::string why) {
        if (error != nullptr) {
            *error = std::move(why);
        }
        return false;
    };
    if (kind.starts_with("eac3-")) {
        return write_eac3_stream(path, kind.substr(5), error);
    }
    const bool karaoke = kind.starts_with("karaoke-");
    if (!karaoke && !kind.starts_with("main-")) {
        return fail("no such kind of stream: " + kind);
    }
    const std::string mode = kind.substr(kind.find('-') + 1);
    iclforge::ac3::EncoderConfig config;
    config.sample_rate = iclforge::ac3::SampleRate::k48000;
    config.bitrate_kbps = 448;
    if (mode == "3/2") {
        config.acmod = iclforge::ac3::Acmod::k3_2;
        config.lfe = true;
    } else if (mode == "2/1") {
        config.acmod = iclforge::ac3::Acmod::k2_1;
    } else {
        return fail("no such coding mode: " + mode);
    }
    config.info.bsmod = karaoke ? iclforge::ac3::meta::BitstreamMode::kVoiceOverOrKaraoke
                                : iclforge::ac3::meta::BitstreamMode::kCompleteMain;
    iclforge::ac3::FrameEncoder encoder{config};
    const auto channels = static_cast<std::size_t>(encoder.channel_count());
    // Coded order: L, C, R, Ls, Rs, then the LFE.
    constexpr std::array<double, 6> kHz = {440.0, 800.0, 620.0, 1030.0, 1270.0, 90.0};
    std::vector<std::vector<float>> input;
    for (std::size_t c = 0; c < channels; ++c) {
        input.push_back(tone(kHz[c]));
    }
    std::ofstream out(std::filesystem::path(path), std::ios::binary);
    if (!out) {
        return fail("could not open " + path);
    }
    const std::size_t frame = iclforge::ac3::kSamplesPerFrame;
    const std::size_t frames = (10 * 48000) / frame;
    for (std::size_t f = 0; f < frames; ++f) {
        std::vector<std::span<const float>> views;
        for (const auto& channel : input) {
            views.emplace_back(channel.data() + (f * frame), frame);
        }
        const auto encoded = encoder.encode_frame(views);
        if (!encoded) {
            return fail("the encoder refused a frame");
        }
        out.write(reinterpret_cast<const char*>(encoded->data()),
                  static_cast<std::streamsize>(encoded->size()));
    }
    return out.good() ? true : fail("could not write " + path);
}

// --- the test sink ---------------------------------------------------------

class TestSinkHost {
public:
    class Log final : public testsink::SinkLog {
    public:
        void line(std::string_view text) override {
            const std::scoped_lock lock(mutex_);
            lines_.emplace_back(text);
        }
        [[nodiscard]] std::vector<std::string> lines() const {
            const std::scoped_lock lock(mutex_);
            return lines_;
        }

    private:
        mutable std::mutex mutex_;
        std::vector<std::string> lines_;
    };

    std::string name;
    Log log;
    std::unique_ptr<testsink::Sink> sink;
};

std::shared_ptr<TestSinkHost> start_test_sink(const std::string& name, const std::string& directory,
                                              bool accept_settings, std::string* error,
                                              std::uint32_t lyrics_max_bytes) {
    auto host = std::make_shared<TestSinkHost>();
    host->name = name;
    testsink::SinkOptions options;
    options.name = name;
    options.address = "127.0.0.1";
    options.port = 0;
    options.state_directory = std::filesystem::path(directory) / "state";
    options.output_directory = std::filesystem::path(directory) / "out";
    options.advertise = false;
    options.code_method = testsink::CodeMethod::kDynamic;
    // A 5.1 sink, so its own Speakers tab has more than a stereo pair to
    // route and trim - the default 7.1.4 would do too, but twelve rows is
    // more page than a fast test needs to draw.
    options.layout = "5.1";
    options.accept_settings = accept_settings;
    options.lyrics_max_bytes = lyrics_max_bytes;
    auto started = testsink::Sink::start(options, host->log);
    if (!started.has_value()) {
        if (error != nullptr) {
            *error = started.error();
        }
        return nullptr;
    }
    host->sink = std::move(*started);
    return host;
}

void announce(iclforge::hearth::NetworkSinks& sinks, const TestSinkHost& host) {
    sinks.on_found(sendspin::discovery::Service{.instance = host.name,
                                                .host = "127.0.0.1",
                                                .addresses = {"127.0.0.1"},
                                                .port = host.sink->port(),
                                                .txt = {{.key = "path", .value = "/sendspin"}}});
}

std::string pairing_code(const TestSinkHost& host) {
    const std::vector<std::string> lines = host.log.lines();
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        const std::size_t at = it->find("PAIRING CODE ");
        if (at != std::string::npos) {
            std::string digits;
            for (const char c : it->substr(at + 13)) {
                if (c >= '0' && c <= '9') {
                    digits.push_back(c);
                }
            }
            return digits;
        }
    }
    return {};
}

std::vector<std::string> log_lines(const TestSinkHost& host) {
    return host.log.lines();
}

std::uint32_t connections(const TestSinkHost& host) {
    return host.sink->totals().connections;
}

}  // namespace iclforge::hearth::uitest
