#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "engine.hpp"
#include "fake_devices.hpp"
#include "fake_services.hpp"

// What Engine::start() answers, over a scripted machine.
//
// It used to answer `{}` every time: it spawned the worker and returned,
// while everything that can actually fail - the output stage, the first
// enumeration, opening a sink - happened on that thread afterwards. A
// machine with no audio endpoint at all therefore reported a running
// engine with no error, the window's status strip had nothing to say, and
// the "the engine did not start here" skip in the QML suites was dead
// code. These cases hold the refusal open: start() now waits for the
// worker to say which it is, and says so.
//
// Every case runs one-block frames (`low_latency`), which is not about
// latency here: it makes the frame loop's first pass 5 ms rather than 32,
// so the first probe's verdict reaches start() well inside its deadline
// even on a loaded host.

using namespace iclforge::crucible;
using namespace iclforge::crucible::testing;

namespace {

EngineConfig config_over(std::shared_ptr<FakeDevices> devices) {
    EngineConfig config;
    config.devices = std::move(devices);
    config.sessions = std::make_shared<FakeSessionMonitor>();
    config.foreground = std::make_shared<FakeForeground>();
    config.low_latency = true;
    return config;
}

// The engine publishes its status every other frame, so a fresh one has
// counted nothing yet; this waits for it rather than sleeping a guess.
bool frames_flow(const Engine& engine, std::chrono::milliseconds within) {
    const auto deadline = std::chrono::steady_clock::now() + within;
    while (std::chrono::steady_clock::now() < deadline) {
        if (engine.status().frames_encoded > 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

// Frames flowing says nothing about the probe: engine.cpp encodes and
// submits every frame regardless of output mode, so frames_flow() above can
// answer true before the first probe has ever been applied. And start()
// itself does not promise the probe is in - past kProbeDeadline (400 ms,
// engine.cpp) it reports started anyway rather than block the caller on a
// platform whose probe is slow, on the understanding that the mode reaches
// the status snapshot once the probe lands, whenever that turns out to be
// (test_engine.cpp:212 waits on the same status field for the same reason).
// This polls rather than reading status() once, so a probe that is merely
// running late - which is what the deadline exists to tolerate, and more
// likely on a loaded host - is not read as this fixture never carrying
// anything.
bool mode_resolved(const Engine& engine, std::chrono::milliseconds within) {
    const auto deadline = std::chrono::steady_clock::now() + within;
    while (std::chrono::steady_clock::now() < deadline) {
        if (engine.status().mode != OutputMode::kNone) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

}  // namespace

TEST_CASE("engine start: a machine with no render endpoint is refused, not started",
          "[crucible][engine]") {
    auto devices = std::make_shared<FakeDevices>();  // nothing to enumerate
    Engine engine(config_over(devices));

    const auto started = engine.start();
    REQUIRE_FALSE(started.has_value());
    // The policy's own sentence, not the deadline's: this refusal is the
    // first probe reporting, which is what distinguishes it from a worker
    // that never came up.
    CHECK(started.error().find("no render endpoint can carry any mode") != std::string::npos);
    // Nothing was opened to play into, which is the state the sentence
    // above describes. `status().running` is deliberately not asserted
    // here: it reports the worker's own lifecycle, and a refusal reports
    // rather than stopping the loop.
    {
        const std::lock_guard lock(devices->mutex);
        CHECK(devices->burst_sinks.empty());
        CHECK(devices->pcm_sinks.empty());
        CHECK(devices->object_sinks.empty());
    }
    engine.stop();
}

TEST_CASE("engine start: an endpoint that can carry the stream starts and runs frames",
          "[crucible][engine]") {
    auto devices = std::make_shared<FakeDevices>();
    devices->devices = {realtek_default(), null_sink(), hdmi_avr()};
    Engine engine(config_over(devices));

    REQUIRE(engine.start().has_value());
    CHECK(frames_flow(engine, std::chrono::milliseconds(2000)));
    CHECK(mode_resolved(engine, std::chrono::milliseconds(2000)));
    engine.stop();
}

TEST_CASE("engine start: a sink that refuses to open is a refusal with the sink's reason",
          "[crucible][engine]") {
    auto devices = std::make_shared<FakeDevices>();
    devices->devices = {realtek_default(), null_sink(), hdmi_avr()};
    devices->refuse_next_start = true;  // the burst sink the AVR would get
    Engine engine(config_over(devices));

    const auto started = engine.start();
    REQUIRE_FALSE(started.has_value());
    CHECK(started.error().find("could not start: refused by the test") != std::string::npos);
    // The sink was asked and said no, which is a different shape of refusal
    // from the case above: one was offered and refused, rather than none
    // being there to offer.
    {
        const std::lock_guard lock(devices->mutex);
        REQUIRE(devices->burst_sinks.size() == 1);
        CHECK_FALSE(devices->burst_sinks[0]->started);
    }
    engine.stop();
}

TEST_CASE("engine start: starting a running engine is refused without disturbing it",
          "[crucible][engine]") {
    auto devices = std::make_shared<FakeDevices>();
    devices->devices = {realtek_default(), null_sink(), hdmi_avr()};
    Engine engine(config_over(devices));

    REQUIRE(engine.start().has_value());
    const auto again = engine.start();
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error() == "already running");
    CHECK(frames_flow(engine, std::chrono::milliseconds(2000)));
    engine.stop();
}

TEST_CASE("engine start: a refused engine can be started again once the machine has an endpoint",
          "[crucible][engine]") {
    auto devices = std::make_shared<FakeDevices>();
    Engine refused(config_over(devices));
    REQUIRE_FALSE(refused.start().has_value());
    refused.stop();

    // The same fakes, with something to play into this time: nothing about
    // the first refusal is sticky.
    devices->devices = {realtek_default(), null_sink(), hdmi_avr()};
    Engine engine(config_over(devices));
    REQUIRE(engine.start().has_value());
    CHECK(frames_flow(engine, std::chrono::milliseconds(2000)));
    engine.stop();
}

TEST_CASE("engine start: the same engine restarted answers for this run, not the last one",
          "[crucible][engine]") {
    auto devices = std::make_shared<FakeDevices>();
    Engine engine(config_over(devices));

    // Refused: nothing to play into.
    REQUIRE_FALSE(engine.start().has_value());
    engine.stop();

    // The AVR arrives while the engine is stopped. A second start() has to
    // wait for the new worker rather than hand back the verdict the last
    // one left behind.
    devices->devices = {realtek_default(), null_sink(), hdmi_avr()};
    const auto started = engine.start();
    INFO("second start() said: " << (started ? std::string("started") : started.error()));
    REQUIRE(started.has_value());
    CHECK(frames_flow(engine, std::chrono::milliseconds(2000)));
    engine.stop();

    // And the other way round, so a stale success cannot pass either: the
    // endpoints go away, and the restart refuses.
    devices->devices.clear();
    const auto after = engine.start();
    REQUIRE_FALSE(after.has_value());
    CHECK(after.error().find("no render endpoint can carry any mode") != std::string::npos);
    engine.stop();
}

TEST_CASE("engine start: a probe the last run left unapplied is not applied to this one",
          "[crucible][engine]") {
    auto devices = std::make_shared<FakeDevices>();
    Engine engine(config_over(devices));

    // Refused, and the frame loop keeps running after a refusal: it is still
    // watching the machine for an endpoint to appear.
    REQUIRE_FALSE(engine.start().has_value());

    // A second enumeration, caught in flight. It has read the list - empty -
    // and is parked, so whatever happens next, what it will report is a
    // machine with nothing on it. On a host with a device watcher this is
    // just what a stop() racing a hot-plug event meets.
    const auto parked_before = devices->enumerations_parked();
    devices->hold_enumerations();
    engine.reprobe();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (devices->enumerations_parked() == parked_before && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (devices->enumerations_parked() == parked_before) {
        // A gate left shut would hang the engine's destructor on the join.
        devices->release_enumerations();
    }
    REQUIRE(devices->enumerations_parked() > parked_before);

    // Stop with that enumeration still parked. stop() asks the frame loop to
    // leave and the loop's last act is to join the probe, so the probe can
    // only land after the loop has made its final pass over the results.
    // The pause is for the loop to reach the join: the gate opening any
    // sooner could let a frame apply the answer, and the case would pass
    // against the bug it is here for. One-block frames are 5 ms.
    std::thread stopper([&engine] { engine.stop(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    {
        const std::lock_guard lock(devices->mutex);
        devices->devices = {realtek_default(), null_sink(), hdmi_avr()};
    }
    devices->release_enumerations();
    stopper.join();

    // The machine has an endpoint now, and the engine is started again. What
    // the parked enumeration left behind describes the machine as it was in
    // the last run; this run asks for its own first probe, and its answer is
    // that one's.
    const auto started = engine.start();
    INFO("start() said: " << (started ? std::string("started") : started.error()));
    REQUIRE(started.has_value());
    CHECK(mode_resolved(engine, std::chrono::milliseconds(2000)));
    engine.stop();
}
