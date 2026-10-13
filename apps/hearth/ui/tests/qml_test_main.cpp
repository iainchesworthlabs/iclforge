#include <QtQuickTest/quicktest.h>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QObject>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QWindow>
#include <QQuickStyle>
#include <QClipboard>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QMimeData>
#include <QPointF>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantMap>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

#include "hearth_controller.hpp"
#include "language_manager.hpp"
#include "microphone_controller.hpp"
#include "microphone_input.hpp"
#include "network_controller.hpp"
#include "test_room.hpp"

// Qt Quick Test entry point for the Hearth window: runs every tst_*.qml under
// QUICK_TEST_SOURCE_DIR against the REAL HearthController the embedded
// Hearth module registers - the same rule apps/forge/gui/tests and
// apps/crucible/ui/tests follow, and for the same reason: a parallel fake API
// is a second thing the real one could silently disagree with. Unlike
// CrucibleController, HearthController is QML_SINGLETON, so no manual
// qmlRegisterSingletonInstance() is needed here to reach it - the embedded
// module registers it itself, the same as Main.qml gets it.
//
// Hearth has no tray, no icon provider and no scripted-machine TestServices
// double, so this file is still shorter than apps/crucible/ui/tests/
// qml_test_main.cpp's own. It DOES need a LanguageManager now: Settings.qml
// imports HearthLanguage, and unlike HearthController that singleton
// is not QML_SINGLETON-registered by the module - main.cpp registers the
// instance by hand, so the suite has to as well or Settings.qml will not
// load here.

namespace {

// What a suite scripts the microphone to do, shared between TestServices (which a suite calls) and
// the input the controller was handed (which reads it when the microphone is turned on). Both are
// used from the GUI thread alone: the controller polls its input from a timer there.
struct MicrophoneScript {
    std::vector<float> samples;  // mono, RealtimeScriptedInput::kRate; empty is silence
    std::string refusal;         // non-empty: the next start() fails with this sentence
    int starts = 0;
};

// A microphone that is a script and runs on the real clock, for the Qt Quick suites: the controller
// reads the steady clock, so this delivers what has been "captured" by that clock's now, from the
// moment it is turned on - ScriptedMicrophoneInput (microphone_input.hpp) is told the time by
// whoever tests it, and a window has no one to tell it. No device is opened and none is listed
// but this one.
class RealtimeScriptedInput final : public iclforge::hearth::MicrophoneInput {
public:
    static constexpr std::uint32_t kRate = 48000;

    explicit RealtimeScriptedInput(std::shared_ptr<MicrophoneScript> script)
        : script_(std::move(script)),
          inner_({}, kRate, 0,
                 {{.id = "test-microphone", .name = "Test microphone", .is_default = true,
                   .sample_rate = kRate, .channels = 1}}) {}

    [[nodiscard]] std::vector<iclforge::hearth::InputDevice> devices() override {
        return inner_.devices();
    }
    [[nodiscard]] std::expected<void, std::string> start(const std::string& device_id) override {
        ++script_->starts;
        const std::int64_t now = steady_now_ns();
        // A script that was never set delivers a minute of silence.
        inner_.script(script_->samples.empty() ? std::vector<float>(kRate * 60U, 0.0F)
                                               : script_->samples,
                      now);
        inner_.set_now_ns(now);
        inner_.refuse(script_->refusal);
        return inner_.start(device_id);
    }
    void stop() override { inner_.stop(); }
    [[nodiscard]] bool running() const override { return inner_.running(); }
    [[nodiscard]] std::uint32_t sample_rate() const override { return inner_.sample_rate(); }
    [[nodiscard]] std::uint16_t channels() const override { return inner_.channels(); }
    [[nodiscard]] std::uint32_t period_frames() const override { return inner_.period_frames(); }
    bool read(iclforge::hearth::InputBlock& out) override {
        inner_.set_now_ns(steady_now_ns());
        return inner_.read(out);
    }

private:
    // The clock MicrophoneController reads (microphone_controller.cpp's steady_now_ns()).
    [[nodiscard]] static std::int64_t steady_now_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    std::shared_ptr<MicrophoneScript> script_;
    iclforge::hearth::ScriptedMicrophoneInput inner_;
};

// The fakes a suite can hand the REAL controllers (test_room.hpp), registered
// as the HearthTest singleton - apps/crucible/ui/tests/qml_test_main.cpp's
// own TestServices is the pattern this follows, and for the same reason: a
// suite that wants to see the engine actually play needs somewhere to play
// into, and neither a CI container nor a developer's desk should be it.
//
// useFakeRoom() must run before the first HearthController.start() in the
// process (start() reads the seam once - HearthController::set_test_outputs()'s
// own comment), so a suite calls it from its initTestCase(). A suite that never
// calls it sees the machine exactly as before, which is what the four
// original suites were written against. One process per suite (one ctest
// entry each), so a fake room in one cannot reach another.
class TestServices : public QObject {
    Q_OBJECT

public:
    TestServices(QQmlEngine& engine, std::shared_ptr<MicrophoneScript> microphone)
        : engine_(&engine), microphone_(std::move(microphone)) {}

    // What the microphone every suite has delivers from the moment it is turned on
    // (RealtimeScriptedInput): a tone of `hz` at `amplitude` (1.0 is full scale, which reads as
    // clipping) for `seconds`, or silence for 0 Hz. The default is silence. There is no other
    // microphone in this binary: the machine's own is never opened, and its inputs are never
    // listed (MicrophoneController::set_test_input_factory()).
    Q_INVOKABLE void scriptMicrophone(double hz, double amplitude, double seconds) {
        constexpr double kRate = RealtimeScriptedInput::kRate;
        const auto count = static_cast<std::size_t>(std::max(0.0, seconds) * kRate);
        std::vector<float> samples(count, 0.0F);
        if (hz > 0.0) {
            for (std::size_t i = 0; i < count; ++i) {
                samples[i] = static_cast<float>(
                    amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / kRate));
            }
        }
        microphone_->samples = std::move(samples);
    }
    // The next turning on is refused with `reason` (empty: it is not).
    Q_INVOKABLE void refuseMicrophone(const QString& reason) {
        microphone_->refusal = reason.toStdString();
    }
    // How many times the microphone has been turned on in this process.
    Q_INVOKABLE int microphoneStarts() const { return microphone_->starts; }

    // Three endpoints: a default 5.1 set of speakers, a stereo pair of
    // headphones, and a 7.1 receiver that says it takes AC-3/E-AC-3
    // passthrough (the picker's second section). False when the controller
    // cannot be reached, or when start() has already run - too late for the
    // seam to matter, and a suite should know rather than silently get the
    // machine.
    Q_INVOKABLE bool useFakeRoom() {
        auto* controller = hearth();
        if (controller == nullptr) {
            return false;
        }
        if (room_) {
            return true;
        }
        room_ = iclforge::hearth::uitest::make_room(standard_endpoints());
        controller->set_test_outputs(iclforge::hearth::uitest::outputs_for(room_));
        return true;
    }

    // A second HearthController over the settings the first has written, which
    // is what a restart of the window finds: made and started as the window
    // starts one, before anything plays, and over a room of its own - the three
    // endpoints above, less the ids in `without` (a receiver switched off or
    // unplugged since), and with a second receiver, "fake-other-receiver", when
    // `otherReceiver` is set: an output that takes a bitstream and is not the
    // one that was chosen. It plays into that room, not this one, so a suite
    // reads it through its own properties (state, outputFormat,
    // currentDeviceId, outputReason, errorText, passthroughDeviceId,
    // outputGroupName). Null before useFakeRoom(). closeWindow() ends it: a case
    // closes the one it started, so that nothing of it is still running, or
    // still listening for the quit, at the next.
    Q_INVOKABLE QObject* startAnotherWindow(const QStringList& without = {}, bool otherReceiver = false) {
        if (!room_) {
            return nullptr;
        }
        std::vector<iclforge::hearth::uitest::FakeEndpoint> endpoints;
        for (auto& endpoint : standard_endpoints()) {
            if (!without.contains(QString::fromStdString(endpoint.id))) {
                endpoints.push_back(std::move(endpoint));
            }
        }
        if (otherReceiver) {
            endpoints.push_back({.id = "fake-other-receiver", .name = "Other receiver", .is_default = false,
                                 .channels = 8, .passthrough = true});
        }
        AnotherWindow another;
        another.room = iclforge::hearth::uitest::make_room(std::move(endpoints));
        another.window = std::make_unique<iclforge::hearth::ui::HearthController>();
        // Ours, not the script engine's to collect when the call returns.
        QQmlEngine::setObjectOwnership(another.window.get(), QQmlEngine::CppOwnership);
        another.window->set_test_outputs(iclforge::hearth::uitest::outputs_for(another.room));
        another.window->start();
        QObject* started = another.window.get();
        windows_.push_back(std::move(another));
        return started;
    }

    Q_INVOKABLE void closeWindow(QObject* window) {
        std::erase_if(windows_, [window](const AnotherWindow& another) { return another.window.get() == window; });
    }

    // The device clock's rate against real time: 1.0 by default, faster for
    // a suite that wants an item to play to its end without waiting it out.
    Q_INVOKABLE void setClockSpeed(double speed) {
        if (room_) {
            iclforge::hearth::uitest::set_speed(*room_, speed);
        }
    }

    // What the fake device has been asked to do: { open, paused, endpoint,
    // sampleRate, channels, opens, framesHeard, framesSubmitted, peak } -
    // empty before useFakeRoom().
    Q_INVOKABLE QVariantMap device() const {
        if (!room_) {
            return {};
        }
        const iclforge::hearth::uitest::RoomReading reading =
            iclforge::hearth::uitest::read(*room_);
        return {{QStringLiteral("open"), reading.open},
                {QStringLiteral("paused"), reading.paused},
                {QStringLiteral("endpoint"), QString::fromStdString(reading.endpoint)},
                {QStringLiteral("sampleRate"), reading.sample_rate},
                {QStringLiteral("channels"), reading.channels},
                {QStringLiteral("opens"), reading.opens},
                {QStringLiteral("framesHeard"), static_cast<double>(reading.frames_heard)},
                {QStringLiteral("framesSubmitted"), static_cast<double>(reading.frames_submitted)},
                {QStringLiteral("peak"), reading.peak}};
    }

    // What the room's IEC 61937 link - the output "Test receiver" takes a
    // bitstream on - has been asked to do: { open, paused, endpoint, format,
    // sampleRate, opens, bursts, badBursts, framesHeard, dataType,
    // payloadBytes } (test_room.hpp's LinkReading) - empty before
    // useFakeRoom().
    Q_INVOKABLE QVariantMap link() const {
        if (!room_) {
            return {};
        }
        const iclforge::hearth::uitest::LinkReading reading =
            iclforge::hearth::uitest::read_link(*room_);
        return {{QStringLiteral("open"), reading.open},
                {QStringLiteral("paused"), reading.paused},
                {QStringLiteral("endpoint"), QString::fromStdString(reading.endpoint)},
                {QStringLiteral("format"), QString::fromStdString(reading.format)},
                {QStringLiteral("sampleRate"), reading.sample_rate},
                {QStringLiteral("opens"), reading.opens},
                {QStringLiteral("bursts"), static_cast<double>(reading.bursts)},
                {QStringLiteral("badBursts"), static_cast<double>(reading.bad_bursts)},
                {QStringLiteral("framesHeard"), static_cast<double>(reading.frames_heard)},
                {QStringLiteral("dataType"), QString::fromStdString(reading.data_type)},
                {QStringLiteral("payloadBytes"), static_cast<double>(reading.payload_bytes)}};
    }

    // Whether what the link has carried since it last opened is the start of
    // the files at `paths`, one after another, byte for byte - or, with
    // `whole`, every byte of them (test_room.hpp's link_carried()).
    Q_INVOKABLE bool linkCarried(const QStringList& paths, bool whole = false) const {
        if (!room_) {
            return false;
        }
        std::vector<std::string> files;
        files.reserve(static_cast<std::size_t>(paths.size()));
        for (const QString& path : paths) {
            files.push_back(path.toStdString());
        }
        return iclforge::hearth::uitest::link_carried(*room_, files, whole);
    }

    // Whether the room's endpoint `id` says it takes a bitstream from here on
    // (test_room.hpp's set_passthrough()): a receiver switched on, or off.
    Q_INVOKABLE bool setEndpointPassthrough(const QString& id, bool takes) {
        return room_ && iclforge::hearth::uitest::set_passthrough(*room_, id.toStdString(), takes);
    }

    // The frames toneLevelDb() reads over (test_room.hpp's kToneWindow).
    Q_INVOKABLE double toneWindowFrames() const {
        return static_cast<double>(iclforge::hearth::uitest::kToneWindow);
    }

    Q_INVOKABLE void resetPeak() {
        if (room_) {
            iclforge::hearth::uitest::reset_peak(*room_);
        }
    }

    // The level of the tone at `hz` in rendered slot `slot`, in dBFS, over the
    // last samples the engine handed the fake device (test_room.hpp's
    // tone_level_db()); -Infinity before there are enough.
    Q_INVOKABLE double toneLevelDb(int slot, double hz) const {
        if (!room_ || slot < 0) {
            return -std::numeric_limits<double>::infinity();
        }
        return iclforge::hearth::uitest::tone_level_db(*room_, static_cast<std::size_t>(slot), hz);
    }

    // Writes an AC-4 stream of tones of `kind` (test_room.hpp's
    // write_ac4_stream()) into this process's scratch folder as `name`, and
    // returns its path - empty if it could not be written.
    Q_INVOKABLE QString writeAc4Stream(const QString& name, const QString& kind) {
        if (!scratch_.isValid()) {
            return {};
        }
        const QString path = QDir(scratch_.path()).filePath(name);
        std::string error;
        if (!iclforge::hearth::uitest::write_ac4_stream(path.toStdString(), kind.toStdString(),
                                                   &error)) {
            qWarning("writeAc4Stream: %s", error.c_str());
            return {};
        }
        return path;
    }

    // Writes an AC-3 stream of tones of `kind` (test_room.hpp's write_ac3_stream())
    // into this process's scratch folder as `name`, and returns its path - empty if
    // it could not be written.
    Q_INVOKABLE QString writeAc3Stream(const QString& name, const QString& kind) {
        if (!scratch_.isValid()) {
            return {};
        }
        const QString path = QDir(scratch_.path()).filePath(name);
        std::string error;
        if (!iclforge::hearth::uitest::write_ac3_stream(path.toStdString(), kind.toStdString(),
                                                   &error)) {
            qWarning("writeAc3Stream: %s", error.c_str());
            return {};
        }
        return path;
    }

    // Writes `text` into this process's scratch folder as `name` - UTF-8, or one
    // byte a character (Latin-1) when `latin1`, which is how a file that is not
    // UTF-8 is made - and returns its path, or empty if it could not be written.
    // For a lyrics file beside a stream the suite wrote under the same stem.
    Q_INVOKABLE QString writeTextFile(const QString& name, const QString& text, bool latin1 = false) {
        if (!scratch_.isValid()) {
            return {};
        }
        const QString path = QDir(scratch_.path()).filePath(name);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            qWarning("writeTextFile: cannot open %s", qPrintable(path));
            return {};
        }
        const QByteArray bytes = latin1 ? text.toLatin1() : text.toUtf8();
        if (file.write(bytes) != bytes.size()) {
            return {};
        }
        return path;
    }

    // Writes the bytes `base64` decodes to into this process's scratch folder as `name` and
    // returns its path, or empty if it could not be written: a file that is not text, such as a
    // CD+G picture (the suite builds the bytes with String.fromCharCode and Qt.btoa).
    Q_INVOKABLE QString writeBinaryFile(const QString& name, const QString& base64) {
        if (!scratch_.isValid()) {
            return {};
        }
        const QString path = QDir(scratch_.path()).filePath(name);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            qWarning("writeBinaryFile: cannot open %s", qPrintable(path));
            return {};
        }
        const QByteArray bytes = QByteArray::fromBase64(base64.toLatin1());
        if (file.write(bytes) != bytes.size()) {
            return {};
        }
        return path;
    }

    // Copies `source` (a path, or a file: URL) into this process's scratch
    // folder as `name`, so a suite can queue several distinctly named items
    // from the repository's small golden streams, and returns the copy's
    // path - empty if the copy failed. `folder` (optional) puts it in a
    // subfolder of its own, for Add folder.
    Q_INVOKABLE QString stageFixture(const QString& source, const QString& name, const QString& folder = {}) {
        if (!scratch_.isValid()) {
            return {};
        }
        const QString from = source.startsWith(QStringLiteral("file:")) ? QUrl(source).toLocalFile() : source;
        QDir dir(scratch_.path());
        if (!folder.isEmpty()) {
            dir.mkpath(folder);
            dir.cd(folder);
        }
        const QString to = dir.filePath(name);
        QFile::remove(to);
        return QFile::copy(from, to) ? to : QString();
    }

    // A fresh folder under the scratch folder, for a suite's own output files
    // (a diagnostics export, a media JSON export).
    Q_INVOKABLE QString scratchPath(const QString& name) const { return QDir(scratch_.path()).filePath(name); }

    // What the controller has written under `key` into the app's settings (a
    // fresh read of the file, so what a restart would find), or null.
    Q_INVOKABLE QVariant storedSetting(const QString& key) const {
        const QSettings settings(QSettings::defaultFormat(), QSettings::UserScope,
                                 QStringLiteral("iclforge"), QStringLiteral("Hearth"));
        return settings.value(key);
    }

    Q_INVOKABLE bool removeFile(const QString& path) const {
        return QFile::remove(path.startsWith(QStringLiteral("file:")) ? QUrl(path).toLocalFile() : path);
    }

    Q_INVOKABLE QString readTextFile(const QString& path) const {
        QFile file(path.startsWith(QStringLiteral("file:")) ? QUrl(path).toLocalFile() : path);
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        return QString::fromUtf8(file.readAll());
    }

    // The first QObject descendant of `root` whose `property` equals
    // `value` - for a non-visual object such as a QtQuick.Dialogs FileDialog,
    // which a ScrollView-rooted page (Settings.qml) parents without listing
    // in any QML list property a suite could walk, and which carries no
    // objectName for TestCase.findChild().
    Q_INVOKABLE QObject* findByProperty(QObject* root, const QString& property, const QVariant& value) const {
        if (root == nullptr) {
            return nullptr;
        }
        const QByteArray name = property.toUtf8();
        for (QObject* child : root->findChildren<QObject*>()) {
            const QVariant found = child->property(name.constData());
            if (found.isValid() && found == value) {
                return child;
            }
        }
        return nullptr;
    }

    // The clipboard's text, for a suite checking a Copy button.
    Q_INVOKABLE QString clipboardText() const { return QGuiApplication::clipboard()->text(); }

    // Starts an in-process Hearth test sink (apps/hearth/testsink) named
    // `name` on loopback and hands it to NetworkController's own
    // NetworkSinks as a found service - starting NetworkController first if
    // no suite code has yet. `acceptSettings`: the sink lists the Settings
    // command and applies one (SinkOptions::accept_settings). Every sink
    // started stays up for the process; the code/log calls below read the
    // most recent one. `lyricsBytes` above 0: the sink states a size of lyric
    // document it holds and lists the lyric commands. Returns the empty
    // string on success, otherwise why not.
    Q_INVOKABLE QString startTestSink(const QString& name, bool acceptSettings = false, int lyricsBytes = 0) {
        auto* network = this->network();
        if (network == nullptr) {
            return QStringLiteral("NetworkController is not registered");
        }
        network->start();
        if (network->sinks_for_test() == nullptr) {
            return QStringLiteral("NetworkController did not start its Sendspin host");
        }
        std::string error;
        auto sink = iclforge::hearth::uitest::start_test_sink(
            name.toStdString(),
            QDir(scratch_.path()).filePath(QStringLiteral("sink-%1").arg(sinks_.size())).toStdString(),
            acceptSettings, &error, static_cast<std::uint32_t>(lyricsBytes > 0 ? lyricsBytes : 0));
        if (!sink) {
            return QString::fromStdString(error);
        }
        iclforge::hearth::uitest::announce(*network->sinks_for_test(), *sink);
        sinks_.push_back(std::move(sink));
        return {};
    }

    // The code the test sink printed for the attempt under way, digits only -
    // what a person would read off the sink's console and type into the
    // Network page's code boxes. Empty until the sink has printed one.
    Q_INVOKABLE QString testSinkCode() const {
        return sinks_.empty() ? QString()
                              : QString::fromStdString(iclforge::hearth::uitest::pairing_code(*sinks_.back()));
    }

    Q_INVOKABLE QStringList testSinkLog() const {
        QStringList lines;
        if (!sinks_.empty()) {
            for (const std::string& line : iclforge::hearth::uitest::log_lines(*sinks_.back())) {
                lines.push_back(QString::fromStdString(line));
            }
        }
        return lines;
    }

    // A key press and release delivered to `target`'s own window (an item,
    // or a window itself). TestCase.keyClick() always sends to the test
    // case's own window, never to a second top-level one - so a suite that
    // creates Main.qml's ApplicationWindow needs this to reach that
    // window's Shortcuts (Ctrl+1..6, F1, Escape) or a text field inside it.
    // QTest::keyClick() on a QWindow sends the ShortcutOverride first, the
    // way a real key press arrives, so a Shortcut in that window fires
    // exactly as it would for a person - provided the window is the focus
    // window (Qt Quick's WindowShortcut context requires it), which the
    // caller arranges with requestActivate().
    Q_INVOKABLE bool keyClick(QObject* target, int key, int modifiers = 0) {
        QWindow* window = window_of(target);
        if (window == nullptr) {
            return false;
        }
        QTest::keyClick(window, static_cast<Qt::Key>(key), Qt::KeyboardModifiers(modifiers));
        return true;
    }

    // Each character of `text` typed into `target`'s window in turn.
    Q_INVOKABLE bool keyClicks(QObject* target, const QString& text) {
        QWindow* window = window_of(target);
        if (window == nullptr) {
            return false;
        }
        for (const QChar c : text) {
            QTest::keyClick(window, c.toLatin1());
        }
        return true;
    }

    // `paths` dropped on the middle of `target`, the way a file manager
    // drops them: a DragEnter, a DragMove and a Drop, each carrying the
    // files as file: URLs (text/uri-list), sent to `target`'s window, which
    // delivers them to whichever DropArea is under that point exactly as it
    // would a real drag. QtTest has no drag-and-drop call of its own, and an
    // internal Drag.active drag carries no URLs, so this is the only way to
    // reach PlayPage.qml's DropArea headlessly with real files. True when a
    // drop target under that point took the drag (accepted the DragEnter) -
    // PlayPage's own onDropped does not accept the Drop itself, which a
    // file manager does not need either.
    Q_INVOKABLE bool dropFiles(QObject* target, const QStringList& paths) {
        auto* item = qobject_cast<QQuickItem*>(target);
        if (item == nullptr || item->window() == nullptr) {
            return false;
        }
        QMimeData mime;
        QList<QUrl> urls;
        for (const QString& path : paths) {
            urls.push_back(QUrl::fromLocalFile(path));
        }
        mime.setUrls(urls);
        const QPointF at = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
        QWindow* window = item->window();
        QDragEnterEvent enter(at.toPoint(), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &enter);
        QDragMoveEvent move(at.toPoint(), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &move);
        QDropEvent drop(at, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window, &drop);
        return enter.isAccepted();
    }

private:
    [[nodiscard]] static QWindow* window_of(QObject* target) {
        if (auto* window = qobject_cast<QWindow*>(target)) {
            return window;
        }
        if (auto* item = qobject_cast<QQuickItem*>(target)) {
            return item->window();
        }
        return nullptr;
    }

    [[nodiscard]] iclforge::hearth::ui::HearthController* hearth() const {
        return engine_->singletonInstance<iclforge::hearth::ui::HearthController*>(QStringLiteral("Hearth"),
                                                                              QStringLiteral("HearthController"));
    }
    [[nodiscard]] iclforge::hearth::ui::NetworkController* network() const {
        return engine_->singletonInstance<iclforge::hearth::ui::NetworkController*>(QStringLiteral("Hearth"),
                                                                               QStringLiteral("NetworkController"));
    }

    // The room useFakeRoom() makes and a window started from it by
    // startAnotherWindow() sees: a default 5.1 set of speakers, a stereo pair
    // of headphones, and a 7.1 receiver that takes a bitstream.
    [[nodiscard]] static std::vector<iclforge::hearth::uitest::FakeEndpoint> standard_endpoints() {
        return {
            {.id = "fake-speakers", .name = "Test speakers", .is_default = true, .channels = 6},
            {.id = "fake-headphones", .name = "Test headphones", .is_default = false, .channels = 2},
            {.id = "fake-receiver", .name = "Test receiver", .is_default = false, .channels = 8, .passthrough = true},
        };
    }

    // One window startAnotherWindow() made, and the room it plays into. The
    // window goes first: its sinks hold the room.
    struct AnotherWindow {
        std::shared_ptr<iclforge::hearth::uitest::FakeRoom> room;
        std::unique_ptr<iclforge::hearth::ui::HearthController> window;
    };

    QQmlEngine* engine_ = nullptr;
    std::shared_ptr<MicrophoneScript> microphone_;
    QTemporaryDir scratch_;
    std::shared_ptr<iclforge::hearth::uitest::FakeRoom> room_;
    std::vector<AnotherWindow> windows_;
    std::vector<std::shared_ptr<iclforge::hearth::uitest::TestSinkHost>> sinks_;
};

// Mirrors DeskIsolation (apps/crucible/ui/tests/qml_test_main.cpp) and
// apps/forge/gui/tests: real organisation/application names plus a QTemporaryDir
// settings path, so HearthController's QSettings (organisation "iclforge",
// application "Hearth", the shipped app's own - hearth_controller.cpp's
// constructor) read and write a store that is empty at start and gone at
// exit rather than the developer's own. HearthController::start() is called
// explicitly by whichever test needs a live engine (there is no
// Component.onCompleted running it here, unlike Main.qml), so a suite that
// never calls it never opens a device at all.
class HearthTestIsolation : public QObject {
    Q_OBJECT

public slots:
    void applicationAvailable() {
        QCoreApplication::setOrganizationName(QStringLiteral("iclforge"));
        QCoreApplication::setApplicationName(QStringLiteral("Hearth"));
        QQuickStyle::setStyle(QStringLiteral("Basic"));
        // The family's own typeface, registered and made the default exactly as src/main.cpp
        // does it, from the same files (tests/CMakeLists.txt embeds them in this binary), so a
        // layout suite measures the glyphs a person sees whatever fonts the machine has. Without
        // it a Windows machine with no QT_QPA_FONTDIR has no font at all: Qt.fontFamilies() is
        // empty and every glyph is a square as wide as its pixel size, about twice an Archivo
        // one. The list is src/main.cpp's own; a face that is missing here is a face the suites
        // never see, so a change to one is a change to both.
        for (const auto* face : {":/fonts/Archivo-Regular.ttf", ":/fonts/Archivo-Medium.ttf",
                                 ":/fonts/Archivo-SemiBold.ttf", ":/fonts/Archivo-ExtraBold.ttf",
                                 ":/fonts/MaterialSymbolsSharp-Regular.ttf",
                                 ":/fonts/NotoSansArabic.ttf", ":/fonts/NotoSansHebrew.ttf"}) {
            if (QFontDatabase::addApplicationFont(QLatin1String(face)) < 0) {
                qWarning("could not register bundled font %s", face);
            }
        }
        QFont default_font = QGuiApplication::font();
        default_font.setFamily(QStringLiteral("Archivo"));
        QGuiApplication::setFont(default_font);
        scratch_.emplace();
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, scratch_->path());
        // Documents (where Save diagnostics... suggests writing) and every
        // other standard location under ~/.qttest rather than the
        // developer's own folders; created, since a save into a missing
        // folder would fail for a reason that has nothing to do with the UI.
        QStandardPaths::setTestModeEnabled(true);
        QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
        // Before any suite starts the network (Main.qml's own
        // Component.onCompleted does, in every suite that opens it): a
        // suite's sinks are the loopback test sinks startTestSink() hands
        // NetworkSinks itself. Browsing too, every suite would dial the real
        // sinks on whatever network it runs on - CI's self-hosted runners sit
        // on someone's home network - and ask for a firewall exception this
        // binary cannot finish (NetworkController::set_network_discovery()'s
        // own comment).
        iclforge::hearth::ui::NetworkController::set_network_discovery(false);
    }

    void qmlEngineAvailable(QQmlEngine* engine) {
        // Same URI and reasoning as main.cpp's own registration: its own,
        // not the module's, so registering a type by hand cannot mark
        // Hearth registered and stop HearthController registering.
        language_manager_ = std::make_unique<LanguageManager>(
            *qGuiApp, *engine, QStringLiteral("hearth"));
        qmlRegisterSingletonInstance("HearthLanguage", 1, 0, "LanguageManager",
                                     language_manager_.get());
        // Before the microphone singleton is first used, which is when the QML makes it: every
        // suite has the scripted microphone, so none of them lists or opens a person's.
        iclforge::hearth::ui::MicrophoneController::set_test_input_factory(
            [script = microphone_script_] { return std::make_unique<RealtimeScriptedInput>(script); });
        test_services_ = std::make_unique<TestServices>(*engine, microphone_script_);
        qmlRegisterSingletonInstance("HearthTest", 1, 0, "TestServices", test_services_.get());
    }

private:
    std::optional<QTemporaryDir> scratch_;
    std::shared_ptr<MicrophoneScript> microphone_script_ = std::make_shared<MicrophoneScript>();
    std::unique_ptr<LanguageManager> language_manager_;
    std::unique_ptr<TestServices> test_services_;
};

}  // namespace

QUICK_TEST_MAIN_WITH_SETUP(hearth, HearthTestIsolation)

#include "qml_test_main.moc"
