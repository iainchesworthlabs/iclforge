#include "frame_device.hpp"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "iclforge/audio/monitor.hpp"
#include "iclforge/audio/passthrough.hpp"

namespace iclforge::hearth::player {

NullDevice::NullDevice(std::uint64_t capacity_frames) : capacity_(capacity_frames) {}

bool NullDevice::open(std::uint32_t sample_rate, std::uint16_t channels) {
    if (sample_rate == 0 || channels == 0) {
        return false;
    }
    rate_ = sample_rate;
    channels_ = channels;
    open_ = true;
    at_ = Clock::now();
    queued_ = 0.0;
    starved_ = false;
    underruns_ = 0;
    return true;
}

void NullDevice::close() { open_ = false; }

void NullDevice::advance() const {
    const Clock::time_point now = Clock::now();
    const double played = std::chrono::duration<double>(now - at_).count() * rate_;
    at_ = now;
    if (played >= queued_) {
        // Ran dry: one underrun for the stretch, not one for every look at it.
        if (!starved_ && queued_ > 0.0) {
            ++underruns_;
            starved_ = true;
        }
        queued_ = 0.0;
    } else {
        queued_ -= played;
    }
}

bool NullDevice::submit(std::span<const float> interleaved) {
    if (!open_) {
        return false;
    }
    advance();
    const std::uint64_t frames = interleaved.size() / channels_;
    if (queued_ + static_cast<double>(frames) > static_cast<double>(capacity_)) {
        return false;
    }
    queued_ += static_cast<double>(frames);
    starved_ = false;
    return true;
}

std::uint64_t NullDevice::pending_frames() const {
    if (!open_) {
        return 0;
    }
    advance();
    return static_cast<std::uint64_t>(queued_);
}

void NullDevice::flush() {
    advance();
    queued_ = 0.0;
    // Dropping the queue is not the device running dry.
    starved_ = true;
}

namespace {

class AudioDevice final : public FrameDevice {
   public:
    explicit AudioDevice(std::string id) : id_(std::move(id)) {}
    ~AudioDevice() override { sink_.stop(); }

    [[nodiscard]] bool open(std::uint32_t sample_rate, std::uint16_t channels) override {
        sink_.stop();
        channels_ = channels;
        return sink_.start(id_, sample_rate, channels).has_value();
    }
    void close() override { sink_.stop(); }
    [[nodiscard]] bool submit(std::span<const float> interleaved) override { return sink_.submit(interleaved); }
    [[nodiscard]] std::uint64_t pending_frames() const override {
        const std::optional<audio::MonitorPosition> position = sink_.position();
        return position ? position->frames_queued + position->latency_frames : 0;
    }
    void flush() override { sink_.flush(); }
    [[nodiscard]] std::uint64_t underruns() const override { return sink_.stats().underruns; }
    [[nodiscard]] bool running() const override { return sink_.running(); }

   private:
    std::string id_;
    std::uint16_t channels_ = 0;
    audio::MonitorSink sink_;
};

}  // namespace

std::vector<DeviceEntry> list_devices() {
    std::vector<DeviceEntry> entries;
    const auto devices = audio::enumerate_render_devices();
    if (!devices) {
        return entries;
    }
    for (const audio::RenderDeviceInfo& device : *devices) {
        entries.push_back({.id = device.id, .name = device.name, .is_default = device.is_default});
    }
    return entries;
}

std::unique_ptr<FrameDevice> make_audio_device(std::string id) { return std::make_unique<AudioDevice>(std::move(id)); }

}  // namespace iclforge::hearth::player
