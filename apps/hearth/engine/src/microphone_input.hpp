#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

// Where a singer's microphone comes from (planning/karaoke-microphone.md, MI1 to MI6): a seam over
// iclforge::audio::Capture, so that the session built on it is tested without a device and a window
// can be tested without a microphone.
//
// The input opens nothing until start() is called, which is the person's own action
// (MI2: a microphone is never on because something kept a preference).

namespace iclforge::hearth {

struct InputDevice {
    std::string id;
    std::string name;
    bool is_default = false;
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
};

// Samples delivered since the last read, interleaved, with when the first was captured.
struct InputBlock {
    std::vector<float> samples;
    // On std::chrono::steady_clock's clock, in nanoseconds: the playback position's clock.
    std::int64_t first_time_ns = 0;
    // True when the platform gave no capture times and this is the time the block was read less
    // its own length, which is later than the truth by whatever the device held back.
    bool time_estimated = false;
};

class MicrophoneInput {
   public:
    virtual ~MicrophoneInput() = default;

    // The inputs the machine has (microphones and line ins; not loopbacks), the default marked.
    [[nodiscard]] virtual std::vector<InputDevice> devices() = 0;

    // Opens `device_id` (empty: the default input). The error is a sentence for the page.
    [[nodiscard]] virtual std::expected<void, std::string> start(const std::string& device_id) = 0;
    virtual void stop() = 0;

    // False before start(), after stop(), and once the device has gone away under the stream.
    [[nodiscard]] virtual bool running() const = 0;
    [[nodiscard]] virtual std::uint32_t sample_rate() const = 0;
    [[nodiscard]] virtual std::uint16_t channels() const = 0;
    // The packet period the platform says it gave, in frames; 0 where it does not say.
    [[nodiscard]] virtual std::uint32_t period_frames() const { return 0; }

    // Everything captured since the last call. False when there is nothing new.
    virtual bool read(InputBlock& out) = 0;
};

// The machine's own input, over iclforge::audio::Capture.
[[nodiscard]] std::unique_ptr<MicrophoneInput> make_device_input();

// A scripted input for tests: plays `samples` (mono) as a microphone that starts at
// `first_time_ns`, and delivers what has been "captured" by the time the test says it is. No
// device, no thread.
class ScriptedMicrophoneInput final : public MicrophoneInput {
   public:
    ScriptedMicrophoneInput(std::vector<float> samples, std::uint32_t sample_rate,
                            std::int64_t first_time_ns, std::vector<InputDevice> devices = {})
        : samples_(std::move(samples)),
          rate_(sample_rate),
          first_time_ns_(first_time_ns),
          devices_(std::move(devices)) {}

    // The time up to which samples have been captured.
    void set_now_ns(std::int64_t now_ns) { now_ns_ = now_ns; }
    // Makes the device go away, as a pulled USB cable does.
    void unplug() { plugged_ = false; }
    // Makes the next start() refuse with `reason`.
    void refuse(std::string reason) { refusal_ = std::move(reason); }
    // The device still runs but delivers nothing, as a stalled driver does.
    void stall(bool stalled) { stalled_ = stalled; }
    [[nodiscard]] const std::string& last_device_id() const { return last_device_; }
    [[nodiscard]] std::size_t starts() const { return starts_; }

    [[nodiscard]] std::vector<InputDevice> devices() override { return devices_; }

    [[nodiscard]] std::expected<void, std::string> start(const std::string& device_id) override {
        if (!refusal_.empty()) {
            return std::unexpected(refusal_);
        }
        ++starts_;
        last_device_ = device_id;
        running_ = plugged_;
        read_ = 0;
        return {};
    }

    void stop() override { running_ = false; }
    [[nodiscard]] bool running() const override { return running_ && plugged_; }
    [[nodiscard]] std::uint32_t sample_rate() const override { return rate_; }
    [[nodiscard]] std::uint16_t channels() const override { return 1; }
    [[nodiscard]] std::uint32_t period_frames() const override { return rate_ / 100; }

    bool read(InputBlock& out) override {
        if (!running() || stalled_) {
            return false;
        }
        const double seconds = static_cast<double>(now_ns_ - first_time_ns_) / 1e9;
        const std::size_t available =
            seconds <= 0.0
                ? 0
                : std::min(samples_.size(),
                           static_cast<std::size_t>(seconds * static_cast<double>(rate_)));
        if (available <= read_) {
            return false;
        }
        out.samples.assign(samples_.begin() + static_cast<std::ptrdiff_t>(read_),
                           samples_.begin() + static_cast<std::ptrdiff_t>(available));
        out.first_time_ns =
            first_time_ns_ + static_cast<std::int64_t>(static_cast<double>(read_) * 1e9 /
                                                       static_cast<double>(rate_));
        out.time_estimated = false;
        read_ = available;
        return true;
    }

   private:
    std::vector<float> samples_;
    std::uint32_t rate_;
    std::int64_t first_time_ns_;
    std::vector<InputDevice> devices_;
    std::int64_t now_ns_ = 0;
    std::size_t read_ = 0;
    bool running_ = false;
    bool plugged_ = true;
    bool stalled_ = false;
    std::string refusal_;
    std::string last_device_;
    std::size_t starts_ = 0;
};

}  // namespace iclforge::hearth
