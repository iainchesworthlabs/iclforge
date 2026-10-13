#include "microphone_input.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>

#include "iclforge/audio/capture.hpp"
#include "iclforge/audio/capture_timeline.hpp"
#include "iclforge/base/wav.hpp"

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

// How much of the clock a single read answers for, at most: a read after a long stall is a block
// of that length, not an allocation the size of the stall.
constexpr std::int64_t kMaxBlockNs = 1'000'000'000LL;
// An item that has been heard for less than this has no position worth taking the origin from: it
// may be held at 0 while the output starts, which reads as "playing" with the clock not yet moving.
constexpr std::int64_t kMinHeardMs = 100;
// A change of origin larger than this is the item having been moved (a seek, or a hold and a
// restart) and the voice follows it; a smaller one is the heard position being reported late.
constexpr std::int64_t kRebaseNs = 100'000'000LL;

[[nodiscard]] std::int64_t floor_div(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) {
        --q;
    }
    return q;
}

}  // namespace

std::unique_ptr<MicrophoneInput> make_device_input() {
    return std::make_unique<DeviceInput>();
}

std::expected<VoiceRecording, std::string> read_voice_file(const std::filesystem::path& path) {
    const auto wav = base::read_wav(path.string());
    if (!wav) {
        return std::unexpected("The voice file " + path.filename().string() +
                               " could not be read as a WAV file: " +
                               std::string{base::describe(wav.error())} + ".");
    }
    if (wav->channels.empty() || wav->frame_count() == 0 || wav->sample_rate == 0) {
        return std::unexpected("The voice file " + path.filename().string() + " has no audio in it.");
    }
    VoiceRecording out;
    out.sample_rate = wav->sample_rate;
    out.name = path.filename().string();
    const std::size_t frames = wav->frame_count();
    out.samples.resize(frames);
    const auto channels = static_cast<float>(wav->channels.size());
    for (std::size_t i = 0; i < frames; ++i) {
        float sum = 0.0F;
        for (const std::vector<float>& channel : wav->channels) {
            sum += channel[i];
        }
        out.samples[i] = sum / channels;
    }
    return out;
}

VoiceFileInput::VoiceFileInput(VoiceRecording recording, std::function<std::int64_t()> clock)
    : recording_(std::move(recording)), clock_(std::move(clock)) {}

std::vector<InputDevice> VoiceFileInput::devices() {
    return {{"voice-file", recording_.name.empty() ? std::string{"A recorded voice"} : recording_.name,
             true, recording_.sample_rate, 1}};
}

std::expected<void, std::string> VoiceFileInput::start(const std::string& device_id) {
    static_cast<void>(device_id);  // there is one input, and it is this file
    running_ = true;
    last_ns_ = clock_();
    return {};
}

void VoiceFileInput::heard_position(std::int64_t position_ms, bool playing, std::int64_t now_ns) {
    playing_ = playing;
    if (!playing || position_ms < kMinHeardMs) {
        return;
    }
    // When the item's time 0 sounded, if the position is exact. It is reported a little late (it
    // is read off the engine a poll ago), which makes this later than the truth and never earlier,
    // so of the readings that agree to within a seek the earliest is the best.
    const std::int64_t candidate = now_ns - position_ms * 1'000'000LL;
    if (!origin_ns_ || std::llabs(candidate - *origin_ns_) > kRebaseNs) {
        origin_ns_ = candidate;
        return;
    }
    origin_ns_ = std::min(*origin_ns_, candidate);
}

bool VoiceFileInput::read(InputBlock& out) {
    if (!running_ || recording_.sample_rate == 0) {
        return false;
    }
    const std::int64_t now = clock_();
    const std::int64_t from = std::max(last_ns_, now - kMaxBlockNs);
    const auto rate = static_cast<std::int64_t>(recording_.sample_rate);
    const std::int64_t frames = (now - from) * rate / 1'000'000'000LL;
    if (frames <= 0) {
        return false;
    }
    out.first_time_ns = from;
    out.time_estimated = false;
    out.samples.assign(static_cast<std::size_t>(frames), 0.0F);
    // The next block begins where this one's last frame ends, so no fraction of a frame is lost.
    last_ns_ = from + frames * 1'000'000'000LL / rate;
    if (!playing_ || !origin_ns_) {
        return true;  // held, or not yet known where the item is: the room is quiet
    }
    // Which frame of the recording is sounding at the block's first frame, and so for every frame
    // after it: the recording runs at the rate the block does.
    const std::int64_t first = floor_div((from - *origin_ns_) * rate, 1'000'000'000LL);
    const auto size = static_cast<std::int64_t>(recording_.samples.size());
    for (std::int64_t i = 0; i < frames; ++i) {
        const std::int64_t at = first + i;
        if (at >= 0 && at < size) {
            out.samples[static_cast<std::size_t>(i)] = recording_.samples[static_cast<std::size_t>(at)];
        }
    }
    return true;
}

}  // namespace iclforge::hearth
