#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "iclforge/render/layout.hpp"
#include "item_source.hpp"
#include "pcm_sink.hpp"
#include "player.hpp"
#include "session.hpp"
#include "stream_decoder.hpp"
#include "truehd_test_support.hpp"

// TrueHD through the player (planning/truehd-parity-hearth-player.md, H1): a queue of a TrueHD item
// plays to a fake device and ends. The stream is the library's encoder's, so the PCM that went in
// is the oracle: the codec is lossless.

namespace {

using namespace iclforge::hearth;
using namespace iclforge::hearth::testing;
namespace th = iclforge::truehd;

// A stereo room whose slots are the stream's channels, L then R.
iclforge::render::OutputLayout stereo_room() {
    const std::array<th::Channel, 2> channels{th::Channel::kL, th::Channel::kR};
    return layout_of(channels);
}

// A device whose clock the test runs, recording what it was given.
class FakeDevice final : public PcmSink {
public:
    struct Log {
        std::vector<std::vector<float>> kept{};
        std::uint64_t submitted = 0;
        std::uint64_t heard = 0;
        std::uint64_t clock = 0;
        std::uint32_t opens = 0;
        std::uint32_t flushes = 0;
        bool open = false;
    };
    FakeDevice(std::shared_ptr<Log> log, std::size_t capacity)
        : log_(std::move(log)), capacity_(capacity) {}

    std::expected<OpenOutputFormat, std::string> open(const Format& format) override {
        ++log_->opens;
        log_->open = true;
        log_->submitted = log_->heard = log_->clock = 0;
        for (auto& slot : log_->kept) {
            slot.clear();
        }
        width_ = static_cast<std::uint16_t>(format.layout.slots());
        return OpenOutputFormat{.sample_rate = format.sample_rate,
                                .channels = width_,
                                .mode = OutputMode::kLocalPcm};
    }
    void close() override { log_->open = false; }
    [[nodiscard]] bool is_open() const override { return log_->open; }
    bool submit(std::span<const std::span<const float>> slots, std::size_t frames) override {
        if (!log_->open || log_->submitted - log_->heard + frames > capacity_) {
            return false;
        }
        REQUIRE(slots.size() == width_);
        log_->kept.resize(std::max(log_->kept.size(), slots.size()));
        for (std::size_t slot = 0; slot < slots.size(); ++slot) {
            REQUIRE(slots[slot].size() == frames);
            log_->kept[slot].insert(log_->kept[slot].end(), slots[slot].begin(), slots[slot].end());
        }
        log_->submitted += frames;
        return true;
    }
    [[nodiscard]] std::optional<iclforge::audio::MonitorPosition> position() const override {
        if (!log_->open) {
            return std::nullopt;
        }
        return iclforge::audio::MonitorPosition{.frames_played = log_->clock,
                                                .frames_queued = log_->submitted - log_->heard,
                                                .latency_frames = 0};
    }
    void flush() override {
        ++log_->flushes;
        log_->submitted = log_->heard = log_->clock = 0;
        for (auto& slot : log_->kept) {
            slot.clear();
        }
    }
    bool pause() override { return log_->open; }
    bool resume() override { return log_->open; }

private:
    std::shared_ptr<Log> log_;
    std::size_t capacity_;
    std::uint16_t width_ = 0;
};

void advance(FakeDevice::Log& log, std::uint64_t frames) {
    if (!log.open) {
        return;
    }
    log.heard += std::min(log.submitted - log.heard, frames);
    log.clock += frames;
}

struct Library {
    std::map<std::string, std::vector<std::byte>> files;
    [[nodiscard]] ItemLoader loader() const {
        return [this](const std::string& path) -> std::expected<LoadedItem, std::string> {
            const auto found = files.find(path);
            if (found == files.end()) {
                return std::unexpected("no such file: " + path);
            }
            return LoadedItem{.source = std::make_shared<MemoryByteSource>(found->second),
                              .truehd = true};
        };
    }
};

QueueItem item(const std::string& path) {
    QueueItem entry;
    entry.path = path;
    entry.title = path;
    return entry;
}

bool play_out(Player& player, FakeDevice::Log& log, std::size_t period = 480) {
    for (int step = 0; step < 400000; ++step) {
        player.pump();
        advance(log, period);
        if (player.transport().state() == TransportState::kStopped && !log.open) {
            return true;
        }
    }
    return false;
}

// What the device was given, as 24-bit samples, interleaved.
std::vector<std::int32_t> heard_by(const FakeDevice::Log& log) {
    std::vector<std::int32_t> out;
    const std::size_t frames = log.kept.empty() ? 0 : log.kept.front().size();
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (const auto& slot : log.kept) {
            out.push_back(static_cast<std::int32_t>(slot[frame] * 8388608.0F));
        }
    }
    return out;
}

}  // namespace

TEST_CASE("truehd player: a queue of a TrueHD item plays to its end, and the device is given the stream's samples",
          "[hearth][truehd][player]") {
    const MadeStream made = make_stream(2, 48000);
    Library library;
    library.files["one.thd"] = made.bytes;
    auto log = std::make_shared<FakeDevice::Log>();
    Player player(std::make_unique<FakeDevice>(log, 8192), library.loader(), stereo_room());
    player.queue().add(item("one.thd"));
    player.play();
    REQUIRE(play_out(player, *log));

    // The queue row knows the item: its duration, its rate, its channels, and that it is TrueHD.
    const ItemFacts& facts = player.queue().items()[0].facts;
    CHECK(facts.codec == ItemCodec::kTruehd);
    REQUIRE(facts.duration.has_value());
    CHECK(facts.duration->count() == 1000);
    CHECK(facts.sample_rate == 48000);
    CHECK(facts.channels == 2);

    CHECK(log->opens == 1);
    CHECK(log->kept.front().size() == 48000);
    CHECK(heard_by(*log) == made.pcm);
}
