#include "microphone_input.hpp"

#include <chrono>
#include <optional>
#include <string>

#include "iclforge/audio/capture.hpp"
#include "iclforge/audio/capture_timeline.hpp"

namespace iclforge::hearth {

namespace {

// Roughly three quarters of a second of stereo at 48 kHz: the consumer drains every few tens of
// milliseconds, so this is room for a UI thread that stalls, not a buffer anyone should fill.
constexpr std::size_t kRingSamples = 1U << 16;

class DeviceInput final : public MicrophoneInput {
   public:
    [[nodiscard]] std::vector<InputDevice> devices() override {
        std::vector<InputDevice> out;
        const auto found = audio::enumerate_devices();
        if (!found) {
            return out;
        }
        for (const audio::DeviceInfo& d : *found) {
            if (d.kind != audio::DeviceKind::kInput) {
                continue;
            }
            out.push_back({d.id, d.name, d.is_default, d.sample_rate, d.channels});
        }
        return out;
    }

    [[nodiscard]] std::expected<void, std::string> start(const std::string& device_id) override {
        capture_.stop();
        audio::CaptureOptions options;
        options.ring_capacity_samples = kRingSamples;
        const auto started = capture_.start(device_id, audio::DeviceKind::kInput, options);
        if (!started) {
            return std::unexpected(std::string{audio::describe(started.error())});
        }
        timeline_.emplace(capture_.sample_rate());
        frames_read_ = 0;
        return {};
    }

    void stop() override {
        capture_.stop();
        timeline_.reset();
    }

    [[nodiscard]] bool running() const override { return capture_.running(); }
    [[nodiscard]] std::uint32_t sample_rate() const override { return capture_.sample_rate(); }
    [[nodiscard]] std::uint16_t channels() const override { return capture_.channels(); }
    [[nodiscard]] std::uint32_t period_frames() const override {
        return capture_.timing().period_frames;
    }

    bool read(InputBlock& out) override {
        if (!capture_.running() || !timeline_) {
            return false;
        }
        if (audio::StampRing* ring = capture_.stamps()) {
            audio::CaptureStamp stamp;
            while (ring->read(std::span<audio::CaptureStamp>(&stamp, 1)) == 1) {
                timeline_->add(stamp);
            }
        }
        audio::RingBuffer* samples = capture_.buffer();
        const std::size_t channels = std::max<std::size_t>(capture_.channels(), 1);
        const std::size_t whole = samples->available() / channels * channels;
        if (whole == 0) {
            return false;
        }
        out.samples.resize(whole);
        samples->read(out.samples);
        const std::uint64_t first_frame = frames_read_;
        frames_read_ += whole / channels;

        const auto time = timeline_->time_ns_of_frame(first_frame);
        if (time) {
            out.first_time_ns = *time;
            out.time_estimated = false;
        } else {
            // No stamps on this platform: when the block was read, less its own length.
            const auto now = std::chrono::steady_clock::now().time_since_epoch();
            const std::int64_t now_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
            const std::int64_t length_ns =
                static_cast<std::int64_t>(whole / channels) * 1'000'000'000LL /
                static_cast<std::int64_t>(std::max<std::uint32_t>(capture_.sample_rate(), 1));
            out.first_time_ns = now_ns - length_ns;
            out.time_estimated = true;
        }
        return true;
    }

   private:
    audio::Capture capture_;
    std::optional<audio::CaptureTimeline> timeline_;
    std::uint64_t frames_read_ = 0;
};

}  // namespace

std::unique_ptr<MicrophoneInput> make_device_input() {
    return std::make_unique<DeviceInput>();
}

}  // namespace iclforge::hearth
