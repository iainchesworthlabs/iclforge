#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "engine.hpp"
#include "fake_devices.hpp"
#include "fake_services.hpp"
#include "iclforge/containers/iec61937/mat.hpp"
#include "iclforge/truehd/decoder/decoder.hpp"

// The bed's LFE, from a tap to the receiver (planning/truehd-parity-crucible.md,
// question 14). A surround application's LFE channel used to be dropped by the
// bed mixer, so the engine's 5.1 bed went out with an empty LFE in every mode
// that reads it. The engine now hands the encoder the sum of the bed taps' LFE
// as its `lfe` span (AtmosEncoder::encode_frame's third argument), and the
// TrueHD leg, which reads the encoder's bed, carries it.
//
// The oracle is the same lossless one test_output_stage_truehd.cpp uses: unwrap
// the MAT bursts the sink received, decode them with iclforge::truehd, and look
// at the channel that is the LFE. FakeTap plays a full-scale sine on every
// channel it is opened with, and the engine opens it as wide as the silent
// device is (8 here), so the LFE channel of the 7.1 tap is a loud tone and
// every other source of LFE in the engine (objects' lfe_send) is silent.

using namespace iclforge::crucible;
using namespace iclforge::crucible::testing;

namespace {

namespace mat = iclforge::containers::iec61937;

using Clock = std::chrono::steady_clock;

constexpr std::size_t kChannels = 6;
constexpr std::size_t kLfeInWaveOrder = 3;  // L R C LFE Ls Rs: the order the TrueHD decoder writes

}  // namespace

TEST_CASE("crucible engine: a surround application's LFE reaches the TrueHD stream",
          "[crucible][engine][truehd]") {
    auto devices = std::make_shared<FakeDevices>();
    devices->record_bursts = true;
    devices->devices = {realtek_default(), null_sink(), hdmi_avr_truehd()};
    auto sessions = std::make_shared<FakeSessionMonitor>();
    AppSession player;
    player.app = 1001U;
    player.name = "player";
    player.active = true;
    player.has_window = true;
    sessions->set_apps({player});

    EngineConfig config;
    config.devices = devices;
    config.sessions = sessions;
    config.foreground = std::make_shared<FakeForeground>();
    config.null_sink_substring = "Crucible Silent Output";
    config.pinned = OutputMode::kTrueHd51;
    Engine engine(config);
    REQUIRE(engine.start().has_value());

    // Long enough for the encoder's first restart interval and its 75 ms hold, and the pre-roll,
    // to have gone by: the carrier holds a few hundred milliseconds of decodable audio.
    std::shared_ptr<SinkRecord> sink;
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    while (Clock::now() < deadline) {
        {
            const std::lock_guard<std::mutex> lock(devices->mutex);
            if (!devices->burst_sinks.empty()) {
                sink = devices->burst_sinks.front();
            }
        }
        if (sink) {
            const std::lock_guard<std::mutex> lock(sink->mutex);
            if (sink->submits >= 80) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(sink);
    engine.stop();
    CHECK(engine.status().mode == OutputMode::kTrueHd51);

    std::vector<std::byte> carrier;
    {
        const std::lock_guard<std::mutex> lock(sink->mutex);
        REQUIRE(sink->submits >= 80);
        CHECK(sink->format == BurstFormat::kTrueHd);
        carrier = sink->carrier;
    }
    auto units = mat::unwrap_mat_stream(carrier);
    REQUIRE(units.has_value());

    iclforge::truehd::Decoder decoder;
    std::int64_t lfe_peak = 0;
    std::int64_t front_peak = 0;
    std::size_t frames = 0;
    for (const auto& unit : *units) {
        auto decoded = decoder.decode(unit);
        REQUIRE(decoded.has_value());
        if (!decoded->has_value()) {
            continue;
        }
        REQUIRE((*decoded)->channels == static_cast<int>(kChannels));
        const auto& pcm = (*decoded)->pcm;
        for (std::size_t i = 0; i + kChannels <= pcm.size(); i += kChannels) {
            lfe_peak = std::max<std::int64_t>(lfe_peak, std::llabs(static_cast<std::int64_t>(pcm[i + kLfeInWaveOrder])));
            front_peak = std::max<std::int64_t>(front_peak, std::llabs(static_cast<std::int64_t>(pcm[i])));
            ++frames;
        }
    }
    REQUIRE(frames > 20000);  // about half a second of decoded audio
    // The fronts carry the tap's tone, so the decode is not silence for some other reason...
    CHECK(front_peak > 1'000'000);  // of 8'388'607 at full scale
    // ...and the LFE carries its own. Before the bed's LFE was fed it was exactly zero.
    CHECK(lfe_peak > 1'000'000);
}
