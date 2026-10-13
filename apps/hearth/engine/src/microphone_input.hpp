#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
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

    // Where the item is heard to be, as of `now_ns` on the steady clock: told on every anchor of
    // the session the input belongs to, open or not. A device has no use for it. An input that is a
    // recording sung along to the item (VoiceFileInput) places its samples by it.
    virtual void heard_position(std::int64_t position_ms, bool playing, std::int64_t now_ns) {
        static_cast<void>(position_ms);
        static_cast<void>(playing);
        static_cast<void>(now_ns);
    }
};

// The machine's own input, over iclforge::audio::Capture.
[[nodiscard]] std::unique_ptr<MicrophoneInput> make_device_input();

// A voice read from a file: one channel (the file's channels averaged), at the file's rate.
struct VoiceRecording {
    std::vector<float> samples;
    std::uint32_t sample_rate = 0;
    // What the input is called where inputs are listed: the file's own name.
    std::string name;
};

// Reads a WAV file as a voice. The error is a sentence for the person who named the file.
[[nodiscard]] std::expected<VoiceRecording, std::string> read_voice_file(
    const std::filesystem::path& path);

// A recording as the microphone, for a capture that has no singer (`hearth --sing`; planning/
// karaoke-microphone.md, MI2). Opening it opens no device, so the rule that only a person's press
// opens a microphone is not bent: nothing here can hear a room.
//
// It is sung along to the item as the item is HEARD, not started with it. A device's samples are
// stamped with the time they were captured and the score places them by the heard position, so a
// recording that began when play() was called would be early by the output's latency, and by what
// the engine takes to start. Told where the item is (heard_position(), which the session passes on
// from its anchor), the input finds the moment the item's time 0 sounded and delivers each sample
// of the recording at the moment the item's own time reaches it. Sung that way a voice that is in
// time with the item reads as in time, whatever the output adds; and while the item is held, or
// before it is known where it is, or after the recording ends, what arrives is silence, so the
// stream is as unbroken as a device's and is never taken for a stalled one.
class VoiceFileInput final : public MicrophoneInput {
   public:
    // `clock` is the steady clock's nanoseconds: the one the session is run by.
    VoiceFileInput(VoiceRecording recording, std::function<std::int64_t()> clock);

    [[nodiscard]] std::vector<InputDevice> devices() override;
    [[nodiscard]] std::expected<void, std::string> start(const std::string& device_id) override;
    void stop() override { running_ = false; }
    [[nodiscard]] bool running() const override { return running_; }
    [[nodiscard]] std::uint32_t sample_rate() const override { return recording_.sample_rate; }
    [[nodiscard]] std::uint16_t channels() const override { return 1; }
    [[nodiscard]] std::uint32_t period_frames() const override {
        return recording_.sample_rate / 100;
    }
    bool read(InputBlock& out) override;
    void heard_position(std::int64_t position_ms, bool playing, std::int64_t now_ns) override;

    // On the steady clock, when the item's time 0 sounded as far as is known; nothing until the
    // item has been heard for long enough to say.
    [[nodiscard]] std::optional<std::int64_t> origin_ns() const { return origin_ns_; }

   private:
    VoiceRecording recording_;
    std::function<std::int64_t()> clock_;
    bool running_ = false;
    bool playing_ = false;
    std::optional<std::int64_t> origin_ns_{};
    // The clock's reading at the end of the last block read.
    std::int64_t last_ns_ = 0;
};

// A scripted input for tests: plays `samples` (mono) as a microphone that starts at
// `first_time_ns`, and delivers what has been "captured" by the time the test says it is. No
// device, no thread.
class ScriptedMicrophoneInput final : public MicrophoneInput {
   public:
    // `samples` is mono, or interleaved when `channels` is more than one.
    ScriptedMicrophoneInput(std::vector<float> samples, std::uint32_t sample_rate,
                            std::int64_t first_time_ns, std::vector<InputDevice> devices = {},
                            std::uint16_t channels = 1)
        : samples_(std::move(samples)),
          rate_(sample_rate),
          first_time_ns_(first_time_ns),
          devices_(std::move(devices)),
          channels_(std::max<std::uint16_t>(channels, 1)) {}

    // The time up to which samples have been captured.
    void set_now_ns(std::int64_t now_ns) { now_ns_ = now_ns; }
    // A new script: other samples, the first captured at `first_time_ns`, played from the start.
    void script(std::vector<float> samples, std::int64_t first_time_ns) {
        samples_ = std::move(samples);
        first_time_ns_ = first_time_ns;
        read_ = 0;
    }
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
    [[nodiscard]] std::uint16_t channels() const override { return channels_; }
    [[nodiscard]] std::uint32_t period_frames() const override { return rate_ / 100; }

    bool read(InputBlock& out) override {
        if (!running() || stalled_) {
            return false;
        }
        const double seconds = static_cast<double>(now_ns_ - first_time_ns_) / 1e9;
        // In frames: one sample of every channel.
        const std::size_t available =
            seconds <= 0.0
                ? 0
                : std::min(samples_.size() / channels_,
                           static_cast<std::size_t>(seconds * static_cast<double>(rate_)));
        if (available <= read_) {
            return false;
        }
        out.samples.assign(samples_.begin() + static_cast<std::ptrdiff_t>(read_ * channels_),
                           samples_.begin() + static_cast<std::ptrdiff_t>(available * channels_));
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
    std::uint16_t channels_ = 1;
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
