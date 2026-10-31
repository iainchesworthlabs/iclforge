#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "frame_device.hpp"
#include "platform/process.hpp"
#include "hearth_player.hpp"
#include "scheduled_sink.hpp"

// hearth-player (apps/hearth/player): the scheduler that plays each burst when the server said, on
// a device that is a model and a clock that is a variable; and the whole player, over loopback,
// played E-AC-3 bursts by hearth-testserver into the null device.

namespace {

namespace fs = std::filesystem;
namespace hp = iclforge::hearth::player;

constexpr std::uint32_t kRate = 48000;
constexpr std::uint16_t kChannels = 2;

// A device that keeps what it was given and plays it at the time the test sets.
class ModelDevice final : public hp::FrameDevice {
   public:
    bool open(std::uint32_t /*sample_rate*/, std::uint16_t /*channels*/) override {
        is_open = true;
        ++opens;
        return true;
    }
    void close() override { is_open = false; }
    bool submit(std::span<const float> interleaved) override {
        if (full) {
            return false;
        }
        frames += interleaved.size() / kChannels;
        for (const float sample : interleaved) {
            if (sample == 0.0F) {
                ++zero_samples;
            }
        }
        return true;
    }
    [[nodiscard]] std::uint64_t pending_frames() const override { return frames > played ? frames - played : 0; }
    void flush() override {
        played = frames;
        ++flushes;
    }
    [[nodiscard]] std::uint64_t underruns() const override { return 0; }
    [[nodiscard]] bool running() const override { return is_open; }

    bool is_open = false;
    bool full = false;
    int opens = 0;
    int flushes = 0;
    std::uint64_t frames = 0;
    std::uint64_t played = 0;
    std::uint64_t zero_samples = 0;
};

struct Rig {
    Rig() {
        auto owned = std::make_unique<ModelDevice>();
        device = owned.get();
        hp::ScheduledPcmSink::Options options;
        options.thread = false;
        options.settle_bursts = 1;
        sink = std::make_unique<hp::ScheduledPcmSink>(std::move(owned), [this] { return now_us; }, options);
        REQUIRE(sink->open(kRate, kChannels));
    }

    // One burst of 1536 frames of 0.5, due at `due_us`.
    void burst(std::int64_t due_us, std::uint64_t first_frame) {
        sink->burst(due_us, first_frame, 1536);
        sink->write(std::vector<float>(1536 * kChannels, 0.5F));
    }
    // The device plays what it holds until now, as a clock would.
    void advance(std::int64_t to_us) {
        owed += static_cast<double>(to_us - now_us) * skew * kRate / 1'000'000.0;
        const auto whole = static_cast<std::uint64_t>(owed);
        owed -= static_cast<double>(whole);
        device->played = std::min(device->frames, device->played + whole);
        now_us = to_us;
    }

    // The device's clock against the sink's: below 1 it is slow, as a sound card's crystal can be.
    double skew = 1.0;
    double owed = 0.0;
    std::int64_t now_us = 1'000'000;
    ModelDevice* device = nullptr;
    std::unique_ptr<hp::ScheduledPcmSink> sink;
};

}  // namespace

TEST_CASE("hearth player: nothing reaches the device before its horizon", "[hearth][player]") {
    Rig rig;
    rig.burst(rig.now_us + 1'000'000, 0);
    CHECK_FALSE(rig.sink->pump());
    CHECK(rig.device->frames == 0);
    CHECK(rig.sink->stats().pending_frames == 1536);
    rig.advance(rig.now_us + 900'000);
    CHECK(rig.sink->pump());
    CHECK(rig.sink->idle());
}

TEST_CASE("hearth player: the first burst is placed by silence and later bursts need none", "[hearth][player]") {
    Rig rig;
    const std::int64_t due = rig.now_us + 20'000;
    rig.burst(due, 0);
    rig.burst(due + 32'000, 1536);
    rig.burst(due + 64'000, 3072);
    CHECK(rig.sink->pump());
    const hp::PlayStats stats = rig.sink->stats();
    CHECK(stats.measured == 3);
    CHECK(stats.corrected == 0);
    CHECK(stats.silence_frames == 20 * kRate / 1000);
    CHECK(stats.dropped_frames == 0);
    CHECK(stats.worst_sync_error_us == 0);
    CHECK(rig.device->frames == (20 * kRate / 1000) + (3 * 1536));
    CHECK(rig.device->zero_samples == static_cast<std::uint64_t>(stats.silence_frames) * kChannels);
}

TEST_CASE("hearth player: a burst that is late is trimmed to its time and counted", "[hearth][player]") {
    Rig rig;
    const std::int64_t due = rig.now_us + 50'000;
    rig.burst(due, 0);
    REQUIRE(rig.sink->pump());
    // The second burst is sent for 32 ms after the first, but arrives 28 ms after it should have
    // been playing: the device holds nothing, so it would be heard at "now".
    rig.advance(due + 60'000);
    rig.burst(due + 32'000, 1536);
    REQUIRE(rig.sink->pump());
    const hp::PlayStats stats = rig.sink->stats();
    CHECK(stats.corrected == 1);
    CHECK(stats.dropped_frames > 0);
    CHECK(stats.sync_error_us > 25'000);
}

TEST_CASE("hearth player: clear drops what is waiting and flushes the device", "[hearth][player]") {
    Rig rig;
    rig.burst(rig.now_us + 80'000, 0);
    REQUIRE(rig.sink->pump());
    rig.burst(rig.now_us + 5'000'000, 1536);
    rig.sink->clear(3072);
    CHECK(rig.device->flushes == 1);
    CHECK(rig.sink->idle());
    // The stream carries on from the next burst, placed afresh.
    rig.burst(rig.now_us + 100'000, 3072);
    CHECK(rig.sink->pump());
    CHECK(rig.sink->stats().corrected == 0);
}

TEST_CASE("hearth player: a full device is waited on, not dropped", "[hearth][player]") {
    Rig rig;
    rig.burst(rig.now_us + 20'000, 0);
    rig.device->full = true;
    CHECK_FALSE(rig.sink->pump());
    CHECK_FALSE(rig.sink->idle());
    rig.device->full = false;
    CHECK(rig.sink->pump());
    CHECK(rig.sink->idle());
}

TEST_CASE("hearth player: the null device refuses frames past its capacity and reports its queue", "[hearth][player]") {
    hp::NullDevice device(4800);
    REQUIRE(device.open(kRate, kChannels));
    CHECK(device.submit(std::vector<float>(4000 * kChannels, 0.0F)));
    CHECK_FALSE(device.submit(std::vector<float>(2000 * kChannels, 0.0F)));
    CHECK(device.pending_frames() <= 4000);
    device.flush();
    CHECK(device.pending_frames() == 0);
    CHECK(device.underruns() == 0);
}

TEST_CASE("hearth player: the name and the state directory are the computer's, not the repository's", "[hearth][player]") {
    CHECK(hp::default_player_name().starts_with("hearth-pc-"));
    CHECK(hp::default_state_directory().filename() == "hearth-player");
}

TEST_CASE("hearth player: hearth-testserver plays demo.ec3 to it over loopback with nothing invalid or late",
          "[hearth][player][websocket]") {
    class Quiet final : public hp::PlayerLog {
       public:
        void line(std::string_view /*text*/) override {}
    } log;
    const fs::path scratch = fs::path{ICLFORGE_TEST_SCRATCH_DIR} / ("hearth_player_" + iclforge::test::platform::process_id());
    fs::remove_all(scratch);
    hp::PlayerOptions options;
    options.name = "Loopback player";
    options.address = "127.0.0.1";
    options.port = 0;
    options.advertise = false;
    options.state_directory = scratch / "state";
    options.device = "null";
    auto player = hp::Player::start(options, log);
    REQUIRE(player.has_value());

    const fs::path report = scratch / "report.json";
    const std::string command = std::string("\"") + ICLFORGE_TESTSERVER_EXE + "\" --play \"" + ICLFORGE_DEMO_EC3 +
                                "\" --player ws://127.0.0.1:" + std::to_string((*player)->port()) +
                                "/sendspin --token " + (*player)->pairing_token() + " --state \"" +
                                (scratch / "server").string() + "\" --timeout 20 --report \"" + report.string() +
                                "\" > \"" + (scratch / "server.log").string() + "\" 2>&1";
    fs::create_directories(scratch);
    CHECK(iclforge::test::platform::run_shell(command) == 0);

    const auto totals = (*player)->totals();
    CHECK(totals.burst_streams >= 1);
    CHECK(totals.bursts > 100);
    const hp::PlayerStatus status = (*player)->status();
    CHECK(status.invalid == 0);
    CHECK(status.play.underruns == 0);
    CHECK(status.play.corrected == 0);
    CHECK(status.codec == "eac3");
    CHECK(status.objects > 0);
    CHECK(status.objects_placed == false);  // folded: a stereo layout has no heights to place into
}

TEST_CASE("hearth player: a stream that ended lets the device go once it has played out", "[hearth][player]") {
    Rig rig;
    rig.burst(rig.now_us + 20'000, 0);
    REQUIRE(rig.sink->pump());
    rig.sink->close();
    rig.sink->pump();
    CHECK(rig.device->is_open);  // the last frames are still queued in it
    rig.advance(rig.now_us + 200'000);
    rig.sink->pump();
    CHECK_FALSE(rig.device->is_open);
}

TEST_CASE("hearth player: a device clock that runs slow is followed, not left to drift", "[hearth][player]") {
    // 300 ppm slow: over two minutes the queue grows by 36 ms, more than the coarse correction's
    // tolerance, and the drift must be taken up in small steps well before that.
    Rig rig;
    rig.skew = 1.0 - 300e-6;
    const std::int64_t first = rig.now_us + 20'000;
    const std::int64_t end = first + 120'000'000;
    std::uint64_t next = 0;
    while (rig.now_us < end) {
        while (first + static_cast<std::int64_t>(next) * 32'000 - rig.now_us < 500'000) {
            rig.burst(first + static_cast<std::int64_t>(next) * 32'000, next * 1536);
            ++next;
        }
        rig.advance(rig.now_us + 4'000);
        rig.sink->pump();
    }
    const hp::PlayStats stats = rig.sink->stats();
    CHECK(stats.measured > 3000);
    CHECK(std::llabs(stats.worst_sync_error_us) < 8'000);
    CHECK(stats.trimmed_frames > 0);
}
