#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

// Where hearth-player's decoded audio goes (docs/hearth/player.md): an output device at one rate
// and channel count, behind the least the scheduler needs of it. The WASAPI device
// (audio_device.hpp) is one; the null device below is another, which plays in real time to
// nowhere, so the loopback test and a machine with no speakers run the whole path.

namespace iclforge::hearth::player {

class FrameDevice {
   public:
    FrameDevice() = default;
    virtual ~FrameDevice() = default;
    FrameDevice(const FrameDevice&) = delete;
    FrameDevice& operator=(const FrameDevice&) = delete;
    FrameDevice(FrameDevice&&) = delete;
    FrameDevice& operator=(FrameDevice&&) = delete;

    // Opens the device for `channels` of float samples at `sample_rate`; false if it cannot.
    [[nodiscard]] virtual bool open(std::uint32_t sample_rate, std::uint16_t channels) = 0;
    virtual void close() = 0;
    // Queues interleaved frames; false, taking none, when the device's queue is full.
    [[nodiscard]] virtual bool submit(std::span<const float> interleaved) = 0;
    // The frames handed over and not heard yet: the device's queue and whatever delay the platform
    // adds after it. A frame submitted now is heard this many frames from now.
    [[nodiscard]] virtual std::uint64_t pending_frames() const = 0;
    // Drops what was queued and not yet played.
    virtual void flush() = 0;
    // Periods the device played with nothing queued, since open().
    [[nodiscard]] virtual std::uint64_t underruns() const = 0;
    // The device is open and has not gone away.
    [[nodiscard]] virtual bool running() const = 0;
};

// A device that consumes what it is given at the stream's rate, by the steady clock, and keeps
// nothing. Its queue holds `capacity_frames`.
class NullDevice final : public FrameDevice {
   public:
    explicit NullDevice(std::uint64_t capacity_frames = 48000);

    [[nodiscard]] bool open(std::uint32_t sample_rate, std::uint16_t channels) override;
    void close() override;
    [[nodiscard]] bool submit(std::span<const float> interleaved) override;
    [[nodiscard]] std::uint64_t pending_frames() const override;
    void flush() override;
    [[nodiscard]] std::uint64_t underruns() const override { return underruns_; }
    [[nodiscard]] bool running() const override { return open_; }

   private:
    using Clock = std::chrono::steady_clock;
    // Moves the play position to now.
    void advance() const;

    std::uint64_t capacity_;
    std::uint32_t rate_ = 0;
    std::uint16_t channels_ = 0;
    bool open_ = false;
    // Mutable: pending_frames() is a question about the time, which has passed.
    mutable Clock::time_point at_{};
    mutable double queued_ = 0.0;
    mutable bool starved_ = false;
    mutable std::uint64_t underruns_ = 0;
};

struct DeviceEntry {
    std::string id;
    std::string name;
    bool is_default = false;
};

// The machine's render endpoints, none where this build has no audio backend.
[[nodiscard]] std::vector<DeviceEntry> list_devices();
// The WASAPI (or the platform's) shared-mode device `id` names; empty for the default endpoint.
[[nodiscard]] std::unique_ptr<FrameDevice> make_audio_device(std::string id);

}  // namespace iclforge::hearth::player
