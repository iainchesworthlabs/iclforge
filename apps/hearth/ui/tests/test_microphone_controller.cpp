// MicrophoneController (planning/karaoke-microphone.md, M4): what the Karaoke page's microphone
// card binds to. The pipeline under it (the input, the meter, the placing of a voice, the score) is
// tested where it lives (apps/hearth/engine/tests/test_microphone_session.cpp and
// test_performance.cpp); what is held here is what this class adds for QML: that nothing opens
// until a person asks, that a kept choice opens nothing, what the page is told when a device will
// not open or goes away, the part a singer is scored against, and that the reference is read off
// the thread it is asked from.

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QSettings>
#include <QString>
#include <QTemporaryDir>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "microphone_controller.hpp"
#include "microphone_input.hpp"

using iclforge::hearth::InputDevice;
using iclforge::hearth::ScriptedMicrophoneInput;
using iclforge::hearth::ui::MicrophoneController;

namespace {

constexpr std::int64_t kT0 = 5'000'000'000'000LL;

QCoreApplication& application() {
    static int argc = 1;
    static char name[] = "hearth-controller-tests";
    static char* argv[] = {name, nullptr};
    static QCoreApplication app(argc, argv);
    return app;
}

// A settings folder of its own, so that what one case keeps is not read by the next.
struct Room {
    QTemporaryDir settings;

    Room() {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    }
};

struct Rig {
    ScriptedMicrophoneInput* input = nullptr;  // owned by the controller
    std::int64_t now = kT0;
    std::unique_ptr<MicrophoneController> controller;
};

std::unique_ptr<Rig> make_rig() {
    auto rig = std::make_unique<Rig>();
    std::vector<float> silence(48000, 0.0F);
    auto input = std::make_unique<ScriptedMicrophoneInput>(
        std::move(silence), 48000, kT0,
        std::vector<InputDevice>{{"mic-a", "Headset", false, 48000, 1},
                                 {"mic-b", "Desk microphone", true, 48000, 2}});
    rig->input = input.get();
    Rig* raw = rig.get();
    rig->controller =
        std::make_unique<MicrophoneController>(std::move(input), [raw]() { return raw->now; });
    return rig;
}

// Runs the event loop until `done` or `timeout_ms` passes: the reference is read on a thread and
// arrives as a queued call.
template <class Done>
bool wait_for(Done done, int timeout_ms) {
    application();
    QElapsedTimer timer;
    timer.start();
    while (!done()) {
        if (timer.elapsed() > timeout_ms) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

QString example_stream() {
    return QString::fromStdString(
        (std::filesystem::path{ICLFORGE_KARAOKE_EXAMPLE_DIR} / "auld-lang-syne.ac3").string());
}

QString plain_stream() {
    const std::filesystem::path root = std::filesystem::path{ICLFORGE_KARAOKE_EXAMPLE_DIR}
                                           .parent_path()
                                           .parent_path()
                                           .parent_path();
    return QString::fromStdString(
        (root / "firmware" / "hearth-sink" / "www" / "ac3-20.ac3").string());
}

}  // namespace

TEST_CASE("microphone controller: it is off, and listing the inputs opens nothing",
          "[hearth][microphone]") {
    Room room;
    application();
    auto rig = make_rig();
    CHECK(rig->controller->state() == "off");
    CHECK(!rig->controller->on());
    CHECK(rig->controller->devices().isEmpty());
    rig->controller->refreshDevices();
    const QVariantList devices = rig->controller->devices();
    REQUIRE(devices.size() == 2);
    CHECK(devices[0].toMap().value("name").toString() == "Headset");
    CHECK(devices[1].toMap().value("isDefault").toBool());
    CHECK(rig->input->starts() == 0);
    CHECK(rig->controller->score().value("withheld").toBool());
}

TEST_CASE("microphone controller: a kept choice opens nothing", "[hearth][microphone]") {
    Room room;
    application();
    {
        QSettings kept(QSettings::defaultFormat(), QSettings::UserScope, "iclforge", "Hearth");
        kept.setValue("microphone/deviceId", "mic-a");
        kept.setValue("microphone/headphones", true);
        kept.setValue("microphone/inputDelayMs", 130);
        kept.setValue("microphone/offsetMs", -40);
    }
    auto rig = make_rig();
    CHECK(rig->controller->deviceId() == "mic-a");
    CHECK(rig->controller->headphones());
    CHECK(rig->controller->inputDelayMs() == 130);
    CHECK(rig->controller->offsetMs() == -40);
    // Everything it kept, and it is still off.
    CHECK(rig->controller->state() == "off");
    CHECK(rig->input->starts() == 0);
}

TEST_CASE("microphone controller: only start opens the input, and stop closes it",
          "[hearth][microphone]") {
    Room room;
    application();
    auto rig = make_rig();
    rig->controller->setDeviceId("mic-a");
    REQUIRE(rig->controller->start());
    CHECK(rig->controller->state() == "on");
    CHECK(rig->controller->on());
    CHECK(rig->input->last_device_id() == "mic-a");
    CHECK(rig->input->running());

    rig->controller->stop();
    CHECK(rig->controller->state() == "off");
    CHECK(!rig->input->running());
}

TEST_CASE("microphone controller: an input that will not open says why and stays off",
          "[hearth][microphone]") {
    Room room;
    application();
    auto rig = make_rig();
    rig->input->refuse("The microphone is in use by another application.");
    CHECK(!rig->controller->start());
    CHECK(rig->controller->state() == "error");
    CHECK(rig->controller->errorText() == "The microphone is in use by another application.");
    CHECK(!rig->controller->on());
    // Pressing stop clears the error: it is off again.
    rig->controller->stop();
    CHECK(rig->controller->state() == "off");
    CHECK(rig->controller->errorText().isEmpty());
}

TEST_CASE("microphone controller: a device that goes away is named and closed",
          "[hearth][microphone]") {
    Room room;
    application();
    auto rig = make_rig();
    REQUIRE(rig->controller->start());
    rig->input->unplug();
    rig->now += 100'000'000;
    rig->controller->tick();
    CHECK(rig->controller->state() == "error");
    CHECK(rig->controller->errorText().contains("went away"));
    CHECK(!rig->input->running());
}

TEST_CASE("microphone controller: an input that stops delivering is reported",
          "[hearth][microphone]") {
    Room room;
    application();
    auto rig = make_rig();
    REQUIRE(rig->controller->start());
    rig->input->stall(true);
    rig->now += 500'000'000;
    rig->controller->tick();
    CHECK(rig->controller->state() == "on");
    rig->now += 4'000'000'000LL;
    rig->controller->tick();
    CHECK(rig->controller->state() == "error");
    CHECK(rig->controller->errorText().contains("stopped delivering"));
}

TEST_CASE("microphone controller: the part to score against is the one the singer has muted",
          "[hearth][microphone]") {
    CHECK(MicrophoneController::preferredPart("capable", "none") == "v1");
    CHECK(MicrophoneController::preferredPart("capable", "v1") == "v2");
    CHECK(MicrophoneController::preferredPart("capable", "v2") == "v1");
    CHECK(MicrophoneController::preferredPart("capable", "both") == "m");
    CHECK(MicrophoneController::preferredPart("aware", "none") == "m");
    CHECK(MicrophoneController::preferredPart("asCoded", "v1") == "m");
}

TEST_CASE("microphone controller: the alignment is clamped and kept", "[hearth][microphone]") {
    Room room;
    application();
    {
        auto rig = make_rig();
        rig->controller->setInputDelayMs(5000);
        rig->controller->setOffsetMs(-5000);
        CHECK(rig->controller->inputDelayMs() == MicrophoneController::maxAlignmentMs());
        CHECK(rig->controller->offsetMs() == -MicrophoneController::maxAlignmentMs());
        rig->controller->setInputDelayMs(95);
        rig->controller->setOffsetMs(30);
        rig->controller->setHeadphones(true);
    }
    auto again = make_rig();
    CHECK(again->controller->inputDelayMs() == 95);
    CHECK(again->controller->offsetMs() == 30);
    CHECK(again->controller->headphones());
}

TEST_CASE("microphone controller: the reference is read from the karaoke stream, off this thread",
          "[hearth][microphone]") {
    Room room;
    application();
    auto rig = make_rig();

    rig->controller->load(example_stream(), "m");
    CHECK(rig->controller->referenceStatus() == "loading");
    REQUIRE(wait_for([&] { return rig->controller->referenceStatus() != "loading"; }, 30000));
    CHECK(rig->controller->referenceStatus() == "ready");
    CHECK(rig->controller->referenceText() == "the melody guide (M)");

    // Another part asked for: read again.
    rig->controller->load(example_stream(), "v2");
    REQUIRE(wait_for([&] { return rig->controller->referenceText() == "vocal 2 (V2)"; }, 30000));
    CHECK(rig->controller->referenceStatus() == "ready");

    // A stream that is not karaoke has no melody part, and the page is told why.
    rig->controller->load(plain_stream(), "m");
    REQUIRE(wait_for([&] { return rig->controller->referenceStatus() == "none"; }, 30000));
    CHECK(rig->controller->referenceText().contains("not a karaoke stream"));

    // Nothing playing.
    rig->controller->load("", "");
    CHECK(rig->controller->referenceStatus() == "none");
}

TEST_CASE("microphone controller: the item ending closes the microphone", "[hearth][microphone]") {
    Room room;
    application();
    auto rig = make_rig();
    rig->controller->load(plain_stream(), "m");
    REQUIRE(rig->controller->start());
    CHECK(rig->controller->on());
    rig->controller->load("", "");
    CHECK(rig->controller->state() == "off");
    CHECK(!rig->input->running());
}
