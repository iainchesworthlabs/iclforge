#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration>

#include <cstdint>
#include <functional>
#include <memory>

// A singer's microphone, for the Karaoke page (planning/karaoke-microphone.md, M4): a fourth QML
// singleton beside HearthController, NetworkController and KaraokeController, so that
// hearth_controller.cpp does not grow. What it wraps is the engine's MicrophoneSession (the input,
// the meter, the Performance against a reference); this class turns it into what a card binds to,
// and holds the rules of the page that are not the engine's:
//
//   * it is OFF at start, always (MI2): a kept choice of device is a preference and opens nothing,
//     and only start() - a person's press - opens a microphone;
//   * it is closed again by stop(), by the item ending (an empty item path), and by the loss of the
//     device, which it names;
//   * nothing is stored: the samples are measured and dropped (a recording is a later, separate
//     switch);
//   * the score is shown only when the person has said they wear headphones (SC5), because through
//     open speakers the microphone hears the backing and the number reads as perfect singing.
//
// Not polled by the engine: a 20 ms timer runs while the microphone is on, and the position is
// handed in by whoever shows it (the card), as KaraokeController's is.
namespace iclforge::hearth {
class MicrophoneInput;
struct TakeInfo;
}

namespace iclforge::hearth::ui {

class MicrophoneController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // "off", "on" or "error"; errorText says why for "error" (a device that would not open, or
    // went away, or stopped delivering).
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool on READ on NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)

    // The inputs the machine has: id, name, isDefault. Read when asked (refreshDevices()), not
    // watched: a person who plugs one in presses the button.
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    // The input chosen, "" for the machine's default. Kept as a preference; opens nothing.
    Q_PROPERTY(QString deviceId READ deviceId WRITE setDeviceId NOTIFY deviceChanged)
    // What the person says of their equipment. Kept.
    Q_PROPERTY(bool headphones READ headphones WRITE setHeadphones NOTIFY headphonesChanged)
    // The alignment, in milliseconds: how long after a sound reaches the microphone it is stamped
    // (declared, measured with the calibration, or 0), and where the person has moved it. Kept.
    Q_PROPERTY(int inputDelayMs READ inputDelayMs WRITE setInputDelayMs NOTIFY alignmentChanged)
    Q_PROPERTY(int offsetMs READ offsetMs WRITE setOffsetMs NOTIFY alignmentChanged)
    Q_PROPERTY(int maxAlignmentMs READ maxAlignmentMs CONSTANT)

    // The meter: 0 to 1 over -60 to 0 dBFS, whether a sample reached full scale in the last
    // second, and for how long the input has been all zeros (a muted microphone).
    Q_PROPERTY(double level READ level NOTIFY meterChanged)
    Q_PROPERTY(double levelDb READ levelDb NOTIFY meterChanged)
    Q_PROPERTY(bool clipping READ clipping NOTIFY meterChanged)
    Q_PROPERTY(double silentSeconds READ silentSeconds NOTIFY meterChanged)

    // What the singer is scored against. "none": nothing playing, or not karaoke (referenceText
    // says why); "loading": being read; "ready": referenceText names the part ("the melody guide
    // (M)").
    Q_PROPERTY(QString referenceStatus READ referenceStatus NOTIFY referenceChanged)
    Q_PROPERTY(QString referenceText READ referenceText NOTIFY referenceChanged)

    // A take (M6): the dry voice kept as a FLAC with a description beside it. Its own switch, not
    // the microphone's (RC1): recording and "Microphone on" are two indicators. The folder is the
    // person's choice, kept; it is never made until a take is started.
    Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)
    Q_PROPERTY(double takeSeconds READ takeSeconds NOTIFY recordingChanged)
    Q_PROPERTY(QString takeError READ takeError NOTIFY recordingChanged)
    // The last take: name, path, folder, seconds; empty before one.
    Q_PROPERTY(QVariantMap lastTake READ lastTake NOTIFY recordingChanged)
    Q_PROPERTY(QString takesFolder READ takesFolder NOTIFY takesFolderChanged)

    // The live view: voiced, hz, referenceHz, centsFromReference (absent where there is no note or
    // the singer is not voiced), itemMs.
    Q_PROPERTY(QVariantMap live READ live NOTIFY liveChanged)
    // The score: withheld, percent (0 to 100, -1 for none), coveragePercent (how much of the
    // reference's voiced time the singer was voiced for), meanCents, lagMs and keyOffsetCents
    // (absent until there is enough), counted, hit, unplaced, timingEstimated.
    Q_PROPERTY(QVariantMap score READ score NOTIFY scoreChanged)

   public:
    // The machine's own input.
    explicit MicrophoneController(QObject* parent = nullptr);
    // An input of the caller's, for a test that has no device.
    explicit MicrophoneController(std::unique_ptr<iclforge::hearth::MicrophoneInput> input,
                                  QObject* parent = nullptr);
    // And a clock of the caller's (the steady clock's nanoseconds), so that a test says what time
    // it is.
    MicrophoneController(std::unique_ptr<iclforge::hearth::MicrophoneInput> input,
                         std::function<std::int64_t()> clock, QObject* parent = nullptr);
    ~MicrophoneController() override;

    MicrophoneController(const MicrophoneController&) = delete;
    MicrophoneController& operator=(const MicrophoneController&) = delete;

    [[nodiscard]] QString state() const { return state_; }
    [[nodiscard]] bool on() const { return state_ == QLatin1String("on"); }
    [[nodiscard]] QString errorText() const { return error_text_; }
    [[nodiscard]] QVariantList devices() const { return devices_; }
    [[nodiscard]] QString deviceId() const { return device_id_; }
    void setDeviceId(const QString& id);
    [[nodiscard]] bool headphones() const { return headphones_; }
    void setHeadphones(bool headphones);
    [[nodiscard]] int inputDelayMs() const { return input_delay_ms_; }
    [[nodiscard]] int offsetMs() const { return offset_ms_; }
    [[nodiscard]] static int maxAlignmentMs() { return kMaxAlignmentMs; }
    void setInputDelayMs(int ms);
    void setOffsetMs(int ms);
    [[nodiscard]] double level() const { return level_; }
    [[nodiscard]] double levelDb() const { return level_db_; }
    [[nodiscard]] bool clipping() const { return clipping_; }
    [[nodiscard]] double silentSeconds() const { return silent_seconds_; }
    [[nodiscard]] QString referenceStatus() const { return reference_status_; }
    [[nodiscard]] QString referenceText() const { return reference_text_; }
    [[nodiscard]] QVariantMap live() const { return live_; }
    [[nodiscard]] QVariantMap score() const { return score_; }
    [[nodiscard]] bool recording() const { return recording_; }
    [[nodiscard]] double takeSeconds() const { return take_seconds_; }
    [[nodiscard]] QString takeError() const { return take_error_; }
    [[nodiscard]] QVariantMap lastTake() const { return last_take_; }
    [[nodiscard]] QString takesFolder() const { return takes_folder_; }

    // Lists the inputs again.
    Q_INVOKABLE void refreshDevices();

    // Opens the chosen input: a person's press, and the only thing that does. False, with
    // errorText, when it would not open.
    Q_INVOKABLE bool start();
    // Closes it and forgets what was sung.
    Q_INVOKABLE void stop();
    // Starts the score again from nothing (a new take of the same item).
    Q_INVOKABLE void restartScore();

    // Starts a take: the microphone must be on. False, with takeError, when the file could not be
    // made. The name is the item's and the time.
    Q_INVOKABLE bool startRecording();
    // Ends it and keeps it; lastTake says where.
    Q_INVOKABLE void stopRecording();
    // Removes the last take and its description.
    Q_INVOKABLE bool deleteLastTake();
    // Opens the folder in the file manager.
    Q_INVOKABLE void showTakes();
    // Where takes go: a folder URL from a dialog, or a path. Kept.
    Q_INVOKABLE void setTakesFolder(const QString& urlOrPath);

    // The item playing, and which part to take the reference from ("m", "v1", "v2" or "" for the
    // first present). An empty path closes the microphone: nothing is playing to sing along to.
    // The same item and part again changes nothing.
    Q_INVOKABLE void load(const QString& itemPath, const QString& prefer);

    // The part to prefer for a reproduction and a choice of vocals: the one the singer has
    // muted, which is the one they are taking over (SC6). "capable" with none: V1; with V1: V2;
    // with V2: V1; otherwise (both, aware, as coded) the guide melody.
    Q_INVOKABLE static QString preferredPart(const QString& reproduction, const QString& vocals);

    // The engine's heard position, as of now. Called whenever it is polled.
    Q_INVOKABLE void anchor(qlonglong positionMs, bool playing);

    // What the 20 ms timer does: read the microphone, measure and score it, and tell the page.
    // Public so that a test with its own clock can drive it.
    void tick();

   signals:
    void stateChanged();
    void devicesChanged();
    void deviceChanged();
    void headphonesChanged();
    void alignmentChanged();
    void meterChanged();
    void referenceChanged();
    void liveChanged();
    void scoreChanged();
    void recordingChanged();
    void takesFolderChanged();

   private:
    struct State;
    static constexpr int kMaxAlignmentMs = 1000;

    void publish(bool force);
    void fail(const QString& text);
    void remember_take(const iclforge::hearth::TakeInfo& info);

    std::unique_ptr<State> state_data_;
    QTimer timer_;

    QString state_ = QStringLiteral("off");
    QString error_text_;
    QVariantList devices_;
    QString device_id_;
    bool headphones_ = false;
    int input_delay_ms_ = 0;
    int offset_ms_ = 0;
    double level_ = 0.0;
    double level_db_ = -120.0;
    bool clipping_ = false;
    double silent_seconds_ = 0.0;
    QString reference_status_ = QStringLiteral("none");
    QString reference_text_;
    QVariantMap live_;
    QVariantMap score_;
    bool recording_ = false;
    double take_seconds_ = 0.0;
    QString take_error_;
    QVariantMap last_take_;
    QString takes_folder_;
    QString item_path_;
    QString prefer_;
    int ticks_ = 0;
};

}  // namespace iclforge::hearth::ui
