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
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QString>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "iclforge/pitch/synth.hpp"
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

// A singer's voice for the notes of the tune that end by `to_ms` (ribbon()'s flat start, end, MIDI
// triples), `shift` semitones away from it: what a microphone would deliver, with the item's time 0
// at the first sample.
std::vector<float> sing(const QVariantList& notes, double to_ms, double shift) {
    std::vector<iclforge::pitch::VoiceNote> voice;
    for (int i = 0; i + 2 < notes.size(); i += 3) {
        const double start = notes[i].toDouble();
        const double end = notes[i + 1].toDouble();
        if (end > to_ms) {
            break;
        }
        voice.push_back({start / 1000.0, (end - start) / 1000.0, notes[i + 2].toDouble() + shift});
    }
    iclforge::pitch::VoiceOptions options;
    options.sample_rate = 48000;
    options.lead_s = 0.0;
    options.tail_s = 0.2;
    options.vibrato_cents = 20.0;
    options.snr_db = 30.0;
    options.seed = 7;
    std::vector<float> samples = iclforge::pitch::synthesize_voice(voice, options).samples;
    // To the end of the stretch, so that the input never looks stalled after the last note.
    samples.resize(std::max(samples.size(), static_cast<std::size_t>(to_ms * 48.0)), 0.0F);
    return samples;
}

// Plays the item for `milliseconds` with `samples` at the microphone, 20 ms at a time, as the
// window's timer would: the position is told, the microphone read. Takes a second more of the
// clock each time so that one scenario follows another.
void perform(Rig& rig, std::vector<float> samples, int milliseconds) {
    const std::int64_t base = rig.now + 1'000'000'000LL;
    rig.input->script(std::move(samples), base);
    rig.now = base;
    rig.input->set_now_ns(base);
    rig.controller->anchor(0, true);
    for (int t = 20; t <= milliseconds; t += 20) {
        rig.now = base + static_cast<std::int64_t>(t) * 1'000'000LL;
        rig.input->set_now_ns(rig.now);
        rig.controller->anchor(t, true);
        rig.controller->tick();
    }
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

TEST_CASE("microphone controller: a line's three levels fall at 45 and 75 percent",
          "[hearth][microphone][score]") {
    CHECK(MicrophoneController::levelFor(100) == 3);
    CHECK(MicrophoneController::levelFor(75) == 3);
    CHECK(MicrophoneController::levelFor(74) == 2);
    CHECK(MicrophoneController::levelFor(45) == 2);
    CHECK(MicrophoneController::levelFor(44) == 1);
    CHECK(MicrophoneController::levelFor(0) == 1);
}

TEST_CASE("microphone controller: a performance is drawn, marked line by line and summed up",
          "[hearth][microphone][score]") {
    Room room;
    application();
    auto rig = make_rig();
    rig->controller->setHeadphones(true);
    rig->controller->load(example_stream(), "m");
    REQUIRE(wait_for([&] { return rig->controller->referenceStatus() == "ready"; }, 60000));

    // --- the tune as the ribbon asks for it: notes in the window, nothing of the singer yet -----
    const QVariantMap tune = rig->controller->ribbon(0, 20000);
    CHECK(tune.value("hasReference").toBool());
    const QVariantList notes = tune.value("notes").toList();
    REQUIRE(notes.size() >= 3 * 6);
    CHECK(notes.size() % 3 == 0);
    CHECK(tune.value("lowMidi").toDouble() < tune.value("highMidi").toDouble());
    CHECK(tune.value("trace").toList().isEmpty());
    for (int i = 0; i + 2 < notes.size(); i += 3) {
        CHECK(notes[i].toDouble() < 20000.0);  // starts inside the window
        CHECK(notes[i + 1].toDouble() > 0.0);  // and ends after its start
        CHECK(notes[i + 1].toDouble() > notes[i].toDouble());
    }
    // A window further on holds other notes, and none of these.
    const QVariantList later = rig->controller->ribbon(40000, 50000).value("notes").toList();
    for (int i = 0; i + 2 < later.size(); i += 3) {
        CHECK(later[i + 1].toDouble() > 40000.0);
        CHECK(later[i].toDouble() < 50000.0);
    }
    // A line is the first three notes.
    const qlonglong line_start = static_cast<qlonglong>(notes[0].toDouble());
    const qlonglong line_end = static_cast<qlonglong>(notes[7].toDouble());
    REQUIRE(line_end > line_start);

    // --- a singer on the tune
    // ----------------------------------------------------------------------
    REQUIRE(rig->controller->start());
    // A mark binds to the revision, which has to move when the score does.
    const int revision_before = rig->controller->scoreRevision();
    perform(*rig, sing(notes, 20000, 0.0), 20000);
    CHECK(rig->controller->scoreRevision() > revision_before);
    const QVariantMap good = rig->controller->lineResult(line_start, line_end);
    CHECK(good.value("scored").toBool());
    CHECK(!good.value("withheld").toBool());
    CHECK(!good.value("notSung").toBool());
    CHECK(good.value("percent").toInt() >= 75);
    CHECK(good.value("level").toInt() == 3);
    // And the ribbon holds the voice, most of it within the tolerance.
    const QVariantList trace =
        rig->controller->ribbon(line_start - 300, line_end + 300).value("trace").toList();
    REQUIRE(trace.size() >= 3 * 20);
    int voiced = 0;
    int hit = 0;
    for (int i = 0; i + 2 < trace.size(); i += 3) {
        if (trace[i + 1].toDouble() >= 0.0) {
            ++voiced;
            hit += trace[i + 2].toDouble() > 0.0 ? 1 : 0;
        }
    }
    CHECK(voiced > 10);
    CHECK(hit > voiced * 8 / 10);
    // A stretch with no note of the tune in it has no mark, which is not a zero.
    const QVariantMap none = rig->controller->lineResult(10'000'000, 10'001'000);
    CHECK(!none.value("scored").toBool());
    CHECK(none.value("percent").toInt() == -1);
    // The score and the summary agree, and the summary is kept when it is turned off.
    rig->controller->stop();
    const QVariantMap summary_good = rig->controller->summary();
    REQUIRE(summary_good.contains("percent"));
    CHECK(summary_good.value("percent").toInt() > 0);
    CHECK(summary_good.value("reference").toString() == "the melody guide (M)");
    CHECK(summary_good.value("toleranceCents").toDouble() == Catch::Approx(100.0));
    CHECK(summary_good.value("hit").toInt() <= summary_good.value("counted").toInt());
    // Off, there is no line to mark.
    CHECK(!rig->controller->lineResult(line_start, line_end).value("scored").toBool());

    // --- the same singer a tritone away: not on the tune, and it says so
    // ------------------------------
    REQUIRE(rig->controller->start());
    CHECK(rig->controller->summary().isEmpty());  // a new performance has no summary yet
    perform(*rig, sing(notes, 20000, -6.0), 20000);
    const QVariantMap bad = rig->controller->lineResult(line_start, line_end);
    CHECK(bad.value("scored").toBool());
    CHECK(!bad.value("notSung").toBool());
    CHECK(bad.value("percent").toInt() < 25);
    CHECK(bad.value("level").toInt() == 1);
    // The ribbon draws it as off the tune: few of its frames are within the tolerance.
    {
        const QVariantList away =
            rig->controller->ribbon(line_start - 300, line_end + 300).value("trace").toList();
        int away_voiced = 0;
        int away_hit = 0;
        for (int i = 0; i + 2 < away.size(); i += 3) {
            if (away[i + 1].toDouble() >= 0.0) {
                ++away_voiced;
                away_hit += away[i + 2].toDouble() > 0.0 ? 1 : 0;
            }
        }
        CHECK(away_voiced > 10);
        CHECK(away_hit < away_voiced * 2 / 10);
    }
    rig->controller->stop();
    CHECK(rig->controller->summary().value("percent").toInt() <
          summary_good.value("percent").toInt());

    // --- an octave below is right, and is drawn on the tune's octave
    // ---------------------------------
    REQUIRE(rig->controller->start());
    perform(*rig, sing(notes, 20000, -12.0), 20000);
    const QVariantMap octave = rig->controller->lineResult(line_start, line_end);
    CHECK(octave.value("scored").toBool());
    CHECK(octave.value("level").toInt() == 3);
    {
        const QVariantMap window = rig->controller->ribbon(line_start - 300, line_end + 300);
        const QVariantList below = window.value("trace").toList();
        const double low = window.value("lowMidi").toDouble();
        const double high = window.value("highMidi").toDouble();
        int below_voiced = 0;
        int below_hit = 0;
        bool on_the_tunes_octave = true;
        for (int i = 0; i + 2 < below.size(); i += 3) {
            const double midi = below[i + 1].toDouble();
            if (midi >= 0.0) {
                ++below_voiced;
                below_hit += below[i + 2].toDouble() > 0.0 ? 1 : 0;
                on_the_tunes_octave = on_the_tunes_octave && midi >= low && midi <= high;
            }
        }
        CHECK(below_voiced > 10);
        CHECK(below_hit > below_voiced * 8 / 10);
        CHECK(on_the_tunes_octave);
    }
    rig->controller->stop();

    // --- a singer who does not sing: "not sung", not a bad score, and no summary
    // -------------------------
    REQUIRE(rig->controller->start());
    perform(*rig, std::vector<float>(48000 * 20, 0.0F), 20000);
    const QVariantMap silent = rig->controller->lineResult(line_start, line_end);
    CHECK(silent.value("scored").toBool());
    CHECK(silent.value("notSung").toBool());
    CHECK(silent.value("percent").toInt() == -1);
    CHECK(silent.value("level").toInt() == 0);
    rig->controller->stop();
    CHECK(rig->controller->summary().isEmpty());

    // --- without the headphones answer there is no number at all, in a line or in a summary
    // --------------
    rig->controller->setHeadphones(false);
    REQUIRE(rig->controller->start());
    perform(*rig, sing(notes, 20000, 0.0), 20000);
    const QVariantMap withheld = rig->controller->lineResult(line_start, line_end);
    CHECK(withheld.value("withheld").toBool());
    CHECK(!withheld.value("scored").toBool());
    CHECK(withheld.value("percent").toInt() == -1);
    rig->controller->stop();
    CHECK(rig->controller->summary().isEmpty());
}

TEST_CASE("microphone controller: how close counts is a preset, and kept; whose key is not",
          "[hearth][microphone][score]") {
    Room room;
    application();
    {
        auto rig = make_rig();
        CHECK(rig->controller->toleranceCents() == 100);
        CHECK(!rig->controller->ownKey());
        // A value is snapped to the nearest preset: half a semitone, a semitone, a whole tone.
        rig->controller->setToleranceCents(120);
        CHECK(rig->controller->toleranceCents() == 100);
        rig->controller->setToleranceCents(175);
        CHECK(rig->controller->toleranceCents() == 200);
        rig->controller->setToleranceCents(10);
        CHECK(rig->controller->toleranceCents() == 50);
        rig->controller->setOwnKey(true);
        CHECK(rig->controller->ownKey());
        // Nothing sung yet: no key to allow for.
        CHECK(rig->controller->keyShiftCents() == Catch::Approx(0.0));
    }
    // The tolerance is a preference and is kept; judging against one's own key is chosen each
    // time, so that it is seen to be on.
    auto again = make_rig();
    CHECK(again->controller->toleranceCents() == 50);
    CHECK(!again->controller->ownKey());
}

TEST_CASE("microphone controller: a sharp singer is judged by the tolerance, or by their own key",
          "[hearth][microphone][score]") {
    Room room;
    application();
    auto rig = make_rig();
    rig->controller->setHeadphones(true);
    rig->controller->load(example_stream(), "m");
    REQUIRE(wait_for([&] { return rig->controller->referenceStatus() == "ready"; }, 60000));
    const QVariantList notes = rig->controller->ribbon(0, 20000).value("notes").toList();
    REQUIRE(notes.size() >= 3 * 6);
    const auto line_start = static_cast<qlonglong>(notes[0].toDouble());
    const auto line_end = static_cast<qlonglong>(notes[7].toDouble());

    // A singer a semitone and a half sharp of the tune, all the way through (150 cents).
    REQUIRE(rig->controller->start());
    perform(*rig, sing(notes, 20000, 1.5), 20000);
    const auto percent = [&] {
        return rig->controller->lineResult(line_start, line_end).value("percent").toInt();
    };

    // A semitone is not enough room, half a semitone less so, and a whole tone is.
    rig->controller->setToleranceCents(100);
    CHECK(percent() < 25);
    rig->controller->setToleranceCents(50);
    CHECK(percent() < 25);
    rig->controller->setToleranceCents(200);
    CHECK(percent() >= 75);
    rig->controller->setToleranceCents(100);

    // Against their own key, the same singer is on the tune's shape; the figure allowed for is
    // shown.
    CHECK(rig->controller->keyShiftCents() == Catch::Approx(0.0));
    rig->controller->setOwnKey(true);
    CHECK(rig->controller->keyShiftCents() == Catch::Approx(150.0).margin(35.0));
    CHECK(percent() >= 75);
    // And the summary says so.
    rig->controller->stop();
    const QVariantMap summary = rig->controller->summary();
    REQUIRE(summary.contains("percent"));
    CHECK(summary.value("ownKey").toBool());
    CHECK(summary.value("keyShiftCents").toDouble() == Catch::Approx(150.0).margin(35.0));
    CHECK(summary.value("toleranceCents").toDouble() == Catch::Approx(100.0));

    // A new performance has its own key: none until it is sung, and then taken from the first
    // sung notes as they come, when it was turned on before.
    REQUIRE(rig->controller->start());
    CHECK(rig->controller->ownKey());
    CHECK(rig->controller->keyShiftCents() == Catch::Approx(0.0));
    perform(*rig, sing(notes, 20000, 1.5), 20000);
    CHECK(rig->controller->keyShiftCents() == Catch::Approx(150.0).margin(35.0));
    CHECK(percent() >= 75);

    // Another item is another tune, sung from its first notes: the key is taken again, and the
    // choice to be judged by it stays.
    rig->controller->load(plain_stream(), "m");
    CHECK(rig->controller->ownKey());
    CHECK(rig->controller->keyShiftCents() == Catch::Approx(0.0));

    // And off again when it is turned off.
    rig->controller->setOwnKey(false);
    CHECK(!rig->controller->ownKey());
    CHECK(rig->controller->keyShiftCents() == Catch::Approx(0.0));
    rig->controller->stop();

    // A few notes are too few to take a key from: the summary does not say one was used.
    rig->controller->load(example_stream(), "m");
    REQUIRE(wait_for([&] { return rig->controller->referenceStatus() == "ready"; }, 60000));
    rig->controller->setOwnKey(true);
    REQUIRE(rig->controller->start());
    perform(*rig, sing(notes, notes[1].toDouble(), 1.5), 150);
    CHECK(rig->controller->keyShiftCents() == Catch::Approx(0.0));
    rig->controller->stop();
    const QVariantMap brief = rig->controller->summary();
    REQUIRE(brief.contains("percent"));
    CHECK(!brief.value("ownKey").toBool());
    CHECK(brief.value("keyShiftCents").toDouble() == Catch::Approx(0.0));
}

TEST_CASE("microphone controller: a take needs the microphone, is its own switch, and is kept",
          "[hearth][microphone][take]") {
    Room room;
    application();
    QTemporaryDir takes;
    REQUIRE(takes.isValid());
    auto rig = make_rig();
    rig->controller->setTakesFolder(takes.path());
    CHECK(QDir::fromNativeSeparators(rig->controller->takesFolder()) ==
          QDir::fromNativeSeparators(takes.path()));

    // Not with the microphone off.
    CHECK(!rig->controller->startRecording());
    CHECK(rig->controller->takeError().contains("Turn the microphone on"));
    CHECK(!rig->controller->recording());

    REQUIRE(rig->controller->start());
    CHECK(!rig->controller->recording());  // on is not recording
    REQUIRE(rig->controller->startRecording());
    CHECK(rig->controller->recording());
    CHECK(rig->controller->on());

    // A second of what the microphone delivered.
    rig->now += 1'000'000'000LL;
    rig->input->set_now_ns(rig->now);
    rig->controller->tick();

    rig->controller->stopRecording();
    CHECK(!rig->controller->recording());
    CHECK(rig->controller->on());  // the microphone stays on
    const QVariantMap take = rig->controller->lastTake();
    REQUIRE(take.contains("path"));
    CHECK(QFileInfo::exists(take.value("path").toString()));
    CHECK(take.value("name").toString().endsWith(".flac"));
    CHECK(take.value("seconds").toDouble() > 0.5);
    QString sidecar = take.value("path").toString();
    sidecar.chop(5);
    CHECK(QFileInfo::exists(sidecar + ".take.json"));

    // Deleting it removes both.
    CHECK(rig->controller->deleteLastTake());
    CHECK(!QFileInfo::exists(take.value("path").toString()));
    CHECK(!QFileInfo::exists(sidecar + ".take.json"));
    CHECK(rig->controller->lastTake().isEmpty());
}

TEST_CASE("microphone controller: turning the microphone off ends a take and keeps it",
          "[hearth][microphone][take]") {
    Room room;
    application();
    QTemporaryDir takes;
    REQUIRE(takes.isValid());
    auto rig = make_rig();
    rig->controller->setTakesFolder(takes.path());
    REQUIRE(rig->controller->start());
    REQUIRE(rig->controller->startRecording());
    rig->now += 500'000'000;
    rig->input->set_now_ns(rig->now);
    rig->controller->tick();
    rig->controller->stop();
    CHECK(!rig->controller->recording());
    CHECK(rig->controller->state() == "off");
    const QVariantMap take = rig->controller->lastTake();
    REQUIRE(take.contains("path"));
    CHECK(QFileInfo::exists(take.value("path").toString()));
}

TEST_CASE("microphone controller: the takes in the folder are listed, and removed one by one",
          "[hearth][microphone][take]") {
    Room room;
    application();
    QTemporaryDir takes;
    REQUIRE(takes.isValid());
    QTemporaryDir elsewhere;
    REQUIRE(elsewhere.isValid());

    const auto put = [](const QString& dir, const QString& name, const QByteArray& bytes) {
        QFile file(QDir(dir).filePath(name));
        REQUIRE(file.open(QIODevice::WriteOnly));
        file.write(bytes);
    };
    // A take is its file and its description; the description is what is listed.
    const auto make = [&](const QString& dir, const QString& stem, const QString& created,
                          double seconds, const QString& title, qlonglong start) {
        put(dir, stem + ".flac", "fLaC");
        QJsonObject item;
        item.insert("title", title);
        QJsonObject doc;
        doc.insert("format", "iclforge-take-1");
        doc.insert("audio", stem + ".flac");
        doc.insert("seconds", seconds);
        doc.insert("created_utc", created);
        doc.insert("item", item);
        doc.insert("item_start_ms", start >= 0 ? QJsonValue(start) : QJsonValue(QJsonValue::Null));
        put(dir, stem + ".take.json", QJsonDocument(doc).toJson());
    };
    make(takes.path(), "Old - 2020-01-01 090000", "2020-01-01T09:00:00Z", 61.5, "Old", 1200);
    make(takes.path(), "New - 2020-03-01 211500", "2020-03-01T21:15:00Z", 213.0, "New", -1);
    make(takes.path(), "Mid - 2020-02-01 120000", "2020-02-01T12:00:00Z", 30.0, "Mid", 0);
    // Not takes of this folder: a FLAC with no description, a description of a file that is not
    // there, a description that is not JSON, one that names a file outside the folder, and a
    // take in another folder.
    put(takes.path(), "stray.flac", "fLaC");
    put(takes.path(), "gone.take.json",
        R"({"format":"iclforge-take-1","audio":"gone.flac","seconds":1,"created_utc":"2020-04-01T00:00:00Z"})");
    put(takes.path(), "broken.take.json", "this is not json");
    // Names a take that exists, in another folder, by its whole path.
    const QByteArray away =
        "{\"format\":\"iclforge-take-1\",\"audio\":\"" +
        QDir(elsewhere.path()).filePath("Other - 2020-06-01 100000.flac").toUtf8() +
        "\",\"seconds\":1,\"created_utc\":\"2020-05-01T00:00:00Z\"}";
    put(takes.path(), "away.take.json", away);
    // A description in some other format is not a take of this program's.
    put(takes.path(), "alien.flac", "fLaC");
    put(takes.path(), "alien.take.json",
        R"({"format":"someone-elses-1","audio":"alien.flac","seconds":1,"created_utc":"2020-07-01T00:00:00Z"})");
    make(elsewhere.path(), "Other - 2020-06-01 100000", "2020-06-01T10:00:00Z", 5.0, "Other", 0);

    auto rig = make_rig();
    rig->controller->setTakesFolder(takes.path());
    const QVariantList listed = rig->controller->takes();
    REQUIRE(listed.size() == 3);
    CHECK(listed[0].toMap().value("title").toString() == "New");
    CHECK(listed[1].toMap().value("title").toString() == "Mid");
    CHECK(listed[2].toMap().value("title").toString() == "Old");
    CHECK(listed[0].toMap().value("startMs").toLongLong() == -1);
    CHECK(listed[1].toMap().value("startMs").toLongLong() == 0);
    CHECK(listed[2].toMap().value("startMs").toLongLong() == 1200);
    CHECK(listed[2].toMap().value("seconds").toDouble() == Catch::Approx(61.5));
    CHECK(listed[0].toMap().value("name").toString() == "New - 2020-03-01 211500.flac");

    // Removing one removes its file and its description, and only that.
    const QString mid = listed[1].toMap().value("path").toString();
    CHECK(rig->controller->deleteTake(mid));
    CHECK(!QFileInfo::exists(mid));
    CHECK(!QFileInfo::exists(QDir(takes.path()).filePath("Mid - 2020-02-01 120000.take.json")));
    CHECK(rig->controller->takes().size() == 2);
    CHECK(QFileInfo::exists(QDir(takes.path()).filePath("New - 2020-03-01 211500.flac")));

    // A path that is not in the list is not touched: another folder's take, a stray file.
    const QString other = QDir(elsewhere.path()).filePath("Other - 2020-06-01 100000.flac");
    CHECK(!rig->controller->deleteTake(other));
    CHECK(QFileInfo::exists(other));
    CHECK(!rig->controller->deleteTake(QDir(takes.path()).filePath("stray.flac")));
    CHECK(QFileInfo::exists(QDir(takes.path()).filePath("stray.flac")));
    CHECK(!rig->controller->deleteTake(QDir(takes.path()).filePath("../stray.flac")));

    // A take made now joins the list, and is the newest.
    REQUIRE(rig->controller->start());
    REQUIRE(rig->controller->startRecording());
    rig->now += 1'000'000'000LL;
    rig->input->set_now_ns(rig->now);
    rig->controller->tick();
    rig->controller->stopRecording();
    const QVariantList after = rig->controller->takes();
    REQUIRE(after.size() == 3);
    CHECK(after[0].toMap().value("path").toString() ==
          rig->controller->lastTake().value("path").toString());
    CHECK(after[0].toMap().value("seconds").toDouble() > 0.5);

    // Removing the last take leaves the list and the card's last take.
    CHECK(rig->controller->deleteTake(after[0].toMap().value("path").toString()));
    CHECK(rig->controller->lastTake().isEmpty());
    CHECK(rig->controller->takes().size() == 2);

    // A folder with none lists none.
    rig->controller->setTakesFolder(elsewhere.path() + "/nothing-here");
    CHECK(rig->controller->takes().isEmpty());

    // Only the newest fifty are listed.
    QTemporaryDir many;
    REQUIRE(many.isValid());
    for (int i = 1; i <= 55; ++i) {
        const QString day = QString::number(i).rightJustified(2, '0');
        make(many.path(), "Many " + day, "2020-02-" + day + "T10:00:00Z", 1.0, "Many", 0);
    }
    rig->controller->setTakesFolder(many.path());
    REQUIRE(rig->controller->takes().size() == 50);
    CHECK(rig->controller->takes().first().toMap().value("name").toString() == "Many 55.flac");
    CHECK(rig->controller->takes().last().toMap().value("name").toString() == "Many 06.flac");
}

TEST_CASE("microphone controller: a device that goes away during a take ends it and keeps it",
          "[hearth][microphone][take]") {
    Room room;
    application();
    QTemporaryDir takes;
    REQUIRE(takes.isValid());
    auto rig = make_rig();
    rig->controller->setTakesFolder(takes.path());
    REQUIRE(rig->controller->start());
    REQUIRE(rig->controller->startRecording());
    rig->now += 500'000'000;
    rig->input->set_now_ns(rig->now);
    rig->controller->tick();
    rig->input->unplug();
    rig->now += 100'000'000;
    rig->controller->tick();
    CHECK(rig->controller->state() == "error");
    CHECK(!rig->controller->recording());
    const QVariantMap take = rig->controller->lastTake();
    REQUIRE(take.contains("path"));
    CHECK(QFileInfo::exists(take.value("path").toString()));
}

TEST_CASE("microphone controller: a take the disk cannot hold ends itself, is kept, and says why",
          "[hearth][microphone][take]") {
    Room room;
    application();
    QTemporaryDir takes;
    REQUIRE(takes.isValid());
    auto rig = make_rig();
    rig->controller->setTakesFolder(takes.path());
    // Room for the header and a few frames of silence.
    rig->controller->limit_take_bytes_for_test(100);
    REQUIRE(rig->controller->start());
    REQUIRE(rig->controller->startRecording());
    rig->now += 1'000'000'000LL;
    rig->input->set_now_ns(rig->now);
    for (int i = 0; i < 5; ++i) {
        rig->controller->tick();
    }
    CHECK(!rig->controller->recording());
    CHECK(rig->controller->on());  // the microphone stays on
    CHECK(rig->controller->takeError().contains("disk"));
    const QVariantMap take = rig->controller->lastTake();
    REQUIRE(take.contains("path"));
    CHECK(QFileInfo::exists(take.value("path").toString()));
    CHECK(QFileInfo(take.value("path").toString()).size() <= 100);
    QString sidecar = take.value("path").toString();
    sidecar.chop(5);
    CHECK(QFileInfo::exists(sidecar + ".take.json"));
    // A take begun again with the limit lifted is whole.
    rig->controller->limit_take_bytes_for_test(0);
    REQUIRE(rig->controller->startRecording());
    CHECK(rig->controller->recording());
    CHECK(rig->controller->takeError().isEmpty());
    rig->controller->stopRecording();
}

TEST_CASE("microphone controller: a take is not written over, nor deleted while another runs",
          "[hearth][microphone][take]") {
    Room room;
    application();
    QTemporaryDir takes;
    REQUIRE(takes.isValid());
    auto rig = make_rig();
    rig->controller->setTakesFolder(takes.path());
    REQUIRE(rig->controller->start());
    REQUIRE(rig->controller->startRecording());
    rig->now += 500'000'000;
    rig->input->set_now_ns(rig->now);
    rig->controller->tick();
    rig->controller->stopRecording();
    const QString first = rig->controller->lastTake().value("path").toString();
    REQUIRE(QFileInfo::exists(first));
    const qint64 first_size = QFileInfo(first).size();

    // A second take at once, which is the same second: its own file, and the first is not touched.
    REQUIRE(rig->controller->startRecording());
    CHECK(!rig->controller->deleteLastTake());  // not while one is being made
    CHECK(QFileInfo::exists(first));
    rig->controller->stopRecording();
    const QString second = rig->controller->lastTake().value("path").toString();
    CHECK(second != first);
    CHECK(QFileInfo::exists(second));
    CHECK(QFileInfo::exists(first));
    CHECK(QFileInfo(first).size() == first_size);
}

TEST_CASE("microphone controller: the takes folder is kept, and is not made until a take starts",
          "[hearth][microphone][take]") {
    Room room;
    application();
    QTemporaryDir parent;
    REQUIRE(parent.isValid());
    const QString chosen = QDir(parent.path()).filePath("My takes");
    {
        auto rig = make_rig();
        rig->controller->setTakesFolder(QUrl::fromLocalFile(chosen).toString());
        CHECK(!QFileInfo::exists(chosen));
    }
    auto again = make_rig();
    CHECK(QDir::fromNativeSeparators(again->controller->takesFolder()) ==
          QDir::fromNativeSeparators(chosen));
    CHECK(!QFileInfo::exists(chosen));
    REQUIRE(again->controller->start());
    REQUIRE(again->controller->startRecording());
    CHECK(QFileInfo::exists(chosen));
    again->controller->stopRecording();
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

TEST_CASE("microphone controller: a voice file stands in for the input, sung along as the item is heard",
          "[hearth][microphone][voice-file]") {
    Room room;
    application();
    auto rig = make_rig();
    QString why;
    REQUIRE(rig->controller->use_voice_file(
        QString::fromStdString((std::filesystem::path{ICLFORGE_KARAOKE_EXAMPLE_DIR} / "parts" /
                                "vocal-1-lead.wav")
                                   .string()),
        &why));
    CHECK(why.isEmpty());
    // The input there is is the file, listed under its own name, and nothing has opened it: the
    // scripted device the rig was made with is not in use, and was never started.
    const QVariantList devices = rig->controller->devices();
    REQUIRE(devices.size() == 1);
    CHECK(devices[0].toMap().value("name").toString() == "vocal-1-lead.wav");
    CHECK(rig->controller->state() == "off");

    rig->controller->setHeadphones(true);
    rig->controller->load(example_stream(), "m");
    REQUIRE(wait_for([&] { return rig->controller->referenceStatus() == "ready"; }, 60000));
    REQUIRE(rig->controller->start());

    // play() is called a second on, and the output is heard to start 350 ms after that: a file that
    // began with play() would be 350 ms early, which a pitch score would take for a flat singer.
    const std::int64_t base = rig->now + 1'000'000'000LL;
    // Through the whole song, which is 16.5 s: the score is of all of the tune's time, so a
    // performance that is half way through is half a score.
    for (int t = 0; t <= 17000; t += 20) {
        rig->now = base + static_cast<std::int64_t>(t) * 1'000'000LL;
        const int heard = std::max(0, t - 350);
        rig->controller->anchor(heard, heard > 0);
        rig->controller->tick();
        REQUIRE(rig->controller->state() == "on");
    }
    const QVariantMap score = rig->controller->score();
    CHECK(!score.value("withheld").toBool());
    CHECK(score.value("percent").toInt() >= 85);
    CHECK(score.value("coveragePercent").toInt() >= 90);
    // The voice was in time with the item as it was heard: not early by the 350 ms the output took
    // to start, which is what a file begun with play() would have been.
    REQUIRE(score.contains("lagMs"));
    CHECK(std::abs(score.value("lagMs").toInt()) <= 40);
    CHECK(!score.value("timingEstimated").toBool());
    // Each of the example's four lines, 0, 4, 8 and 12 s in, was sung on the tune and is marked so.
    for (const auto& line : {std::pair{0, 4000}, std::pair{4000, 8000}, std::pair{8000, 12000},
                             std::pair{12000, 16500}}) {
        const QVariantMap mark = rig->controller->lineResult(line.first, line.second);
        INFO("the line from " << line.first << " ms");
        CHECK(mark.value("scored").toBool());
        CHECK(mark.value("percent").toInt() >= 75);
        CHECK(mark.value("level").toInt() == 3);
    }
    // The ribbon has the voice, on the tune.
    const QVariantList trace = rig->controller->ribbon(0, 4000).value("trace").toList();
    CHECK(trace.size() >= 3 * 100);

    // Ended, it is a summary and the input is closed.
    rig->controller->stop();
    CHECK(rig->controller->summary().value("percent").toInt() >= 85);
    CHECK(rig->controller->summary().value("reference").toString() == "the melody guide (M)");
}

TEST_CASE("microphone controller: a voice file that is not one is refused and nothing changes",
          "[hearth][microphone][voice-file]") {
    Room room;
    application();
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString words = QDir(dir.path()).filePath("words.wav");
    {
        QFile file(words);
        REQUIRE(file.open(QIODevice::WriteOnly));
        file.write("these are words, not audio");
    }
    auto rig = make_rig();
    QString why;
    CHECK(!rig->controller->use_voice_file(words, &why));
    CHECK(why.contains("words.wav"));
    // The input it had is the one it still has.
    rig->controller->refreshDevices();
    CHECK(rig->controller->devices().size() == 2);
    CHECK(rig->controller->start());
    CHECK(rig->input->starts() == 1);
}
