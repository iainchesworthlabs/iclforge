#include "audio_link.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace iclforge::hearth_android {

namespace {

namespace ac = sendspin::player;

[[nodiscard]] std::optional<audio::BitstreamFormat> format_of(ac::DataType type) {
    switch (type) {
        case ac::DataType::kAc3:
            return audio::BitstreamFormat::kAc3;
        case ac::DataType::kEac3:
            return audio::BitstreamFormat::kEac3;
        case ac::DataType::kAc4:
        case ac::DataType::kTruehd:  // access units, not IEC 61937 bursts: this link does not take them
            break;
    }
    return std::nullopt;
}

}  // namespace

// A link to the shared track, for one output.
class SharedAudio::Link final : public sink::BurstLink {
   public:
    Link(SharedAudio& audio, std::uint64_t id) : audio_(&audio), id_(id) {}

    ~Link() override { close(); }

    [[nodiscard]] std::expected<void, std::string> open(const sink::LinkFormat& format) override {
        const std::optional<audio::BitstreamFormat> wanted = format_of(format.data_type);
        if (!wanted) {
            return std::unexpected(std::string("this route does not carry that format"));
        }
        const std::lock_guard lock(audio_->mutex_);
        if (audio_->track_.running()) {
            audio_->track_.stop();
        }
        audio_->owner_ = 0;
        const std::expected<void, audio::PassthroughError> started =
            audio_->track_.start({}, format.sample_rate, *wanted);
        if (!started) {
            return std::unexpected(std::string(audio::describe(started.error())));
        }
        audio_->owner_ = id_;
        return {};
    }

    void close() override {
        const std::lock_guard lock(audio_->mutex_);
        if (audio_->owner_ != id_) {
            return;
        }
        audio_->track_.stop();
        audio_->owner_ = 0;
    }

    [[nodiscard]] bool is_open() const override {
        const std::lock_guard lock(audio_->mutex_);
        return audio_->owner_ == id_ && audio_->track_.running();
    }

    [[nodiscard]] bool submit(std::span<const std::byte> burst) override {
        const std::lock_guard lock(audio_->mutex_);
        return audio_->owner_ == id_ && audio_->track_.submit(burst);
    }

    [[nodiscard]] std::optional<sink::LinkPosition> position() const override {
        const std::lock_guard lock(audio_->mutex_);
        if (audio_->owner_ != id_) {
            return std::nullopt;
        }
        const std::optional<audio::MonitorPosition> position = audio_->track_.position();
        if (!position) {
            return std::nullopt;
        }
        return sink::LinkPosition{.frames_played = position->frames_played,
                                  .frames_queued = position->frames_queued};
    }

    void flush() override {
        const std::lock_guard lock(audio_->mutex_);
        if (audio_->owner_ == id_) {
            audio_->track_.flush();
        }
    }

    void pause(bool paused) override {
        const std::lock_guard lock(audio_->mutex_);
        if (audio_->owner_ != id_) {
            return;
        }
        if (paused) {
            static_cast<void>(audio_->track_.pause());
        } else {
            static_cast<void>(audio_->track_.resume());
        }
    }

    [[nodiscard]] std::uint64_t underruns() const override {
        const std::lock_guard lock(audio_->mutex_);
        return audio_->owner_ == id_ ? audio_->track_.stats().underruns : 0;
    }

   private:
    SharedAudio* audio_;
    std::uint64_t id_;
};

std::unique_ptr<sink::BurstLink> SharedAudio::make_link() {
    std::uint64_t id = 0;
    {
        const std::lock_guard lock(mutex_);
        id = next_id_++;
    }
    return std::make_unique<Link>(*this, id);
}

std::vector<sendspin::player::DataType> SharedAudio::routes() const {
    std::vector<sendspin::player::DataType> types;
    const std::expected<std::vector<audio::RenderDeviceInfo>, audio::PassthroughError> devices =
        audio::enumerate_render_devices(48000);
    if (!devices) {
        return types;
    }
    for (const audio::RenderDeviceInfo& device : *devices) {
        if (device.supports_ac3_passthrough &&
            std::ranges::find(types, sendspin::player::DataType::kAc3) == types.end()) {
            types.push_back(sendspin::player::DataType::kAc3);
        }
        if (device.supports_eac3_passthrough &&
            std::ranges::find(types, sendspin::player::DataType::kEac3) == types.end()) {
            types.push_back(sendspin::player::DataType::kEac3);
        }
    }
    return types;
}

}  // namespace iclforge::hearth_android
