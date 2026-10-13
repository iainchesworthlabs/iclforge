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
    // How close a frame must be to the tune to count, in cents: one of the presets 50, 100 and 200
    // (a value is snapped to the nearest). Kept. 100 is a semitone.
    Q_PROPERTY(
        int toleranceCents READ toleranceCents WRITE setToleranceCents NOTIFY toleranceChanged)
    // Score against the singer's own key (planning/karaoke-microphone.md, "What a score means"):
    // the median of how far their first voiced frames were from the tune is taken off every frame,
    // so that a person who sings consistently flat or sharp is judged on the shape of the melody.
    // Off at every start of the program: a person should see that it is on. `keyShiftCents` is the
    // figure in force (0 until it has been taken, which needs 20 sung frames).
    Q_PROPERTY(bool ownKey READ ownKey WRITE setOwnKey NOTIFY ownKeyChanged)
    Q_PROPERTY(double keyShiftCents READ keyShiftCents NOTIFY ownKeyChanged)

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
    // The takes in that folder, newest first, at most 50: each a map of `path`, `name`, `folder`,
    // `seconds`, `created` (UTC), `title` (the item's) and `startMs` (where in the item it began,
    // -1 when that was not known). Made from the descriptions beside the files, so listing opens
    // no audio: a file with no description of this program's is not a take. Read again by
    // refreshTakes(), and when a take is kept or deleted or the folder is changed.
    Q_PROPERTY(QVariantList takes READ takes NOTIFY takesChanged)

    // The live view: voiced, hz, referenceHz, centsFromReference (absent where there is no note or
    // the singer is not voiced), itemMs.
    Q_PROPERTY(QVariantMap live READ live NOTIFY liveChanged)
    // The score: withheld, percent (0 to 100, -1 for none), coveragePercent (how much of the
    // reference's voiced time the singer was voiced for), meanCents, lagMs and keyOffsetCents
    // (absent until there is enough), counted, hit, unplaced, timingEstimated.
    Q_PROPERTY(QVariantMap score READ score NOTIFY scoreChanged)
    // A number that changes whenever the score does, which a line's mark binds to: lineResult()
    // is a function of the performance so far, and a binding follows only what it reads.
    Q_PROPERTY(int scoreRevision READ scoreRevision NOTIFY scoreChanged)
    // What the last performance came to, kept when the microphone is turned off, the item ends or
    // the device goes (M5): the same fields as `score`, plus `reference` (what it was scored
    // against) and `toleranceCents`. Empty until one has been scored, and when the person had not
    // said they wore headphones, since that score is never shown. Cleared by the next start().
    Q_PROPERTY(QVariantMap summary READ summary NOTIFY summaryChanged)

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

    // The input the machine's own is replaced with, for the Qt Quick suites alone
    // (apps/hearth/ui/tests/qml_test_main.cpp). The QML engine makes this singleton through the
    // first constructor above, which has nowhere to be handed an input, and a suite that opens
    // no person's microphone needs it to be a script: HearthController::set_test_outputs() is
    // the same seam for the speakers. Set it before the singleton is first used; empty (the
    // shipped window, always) means the machine's own.
    static void set_test_input_factory(
        std::function<std::unique_ptr<iclforge::hearth::MicrophoneInput>()> make);

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
    [[nodiscard]] int toleranceCents() const { return tolerance_cents_; }
    void setToleranceCents(int cents);
    [[nodiscard]] bool ownKey() const { return own_key_; }
    void setOwnKey(bool on);
    [[nodiscard]] double keyShiftCents() const { return key_locked_ ? key_shift_cents_ : 0.0; }
    // A test seam: a take's file may hold this many bytes, and the take then ends by itself as
    // it does when a disk fills. 0 is none; nothing but the tests sets it.
    void limit_take_bytes_for_test(std::uint64_t bytes);
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
    [[nodiscard]] int scoreRevision() const { return score_revision_; }
    [[nodiscard]] QVariantMap summary() const { return summary_; }
    [[nodiscard]] bool recording() const { return recording_; }
    [[nodiscard]] double takeSeconds() const { return take_seconds_; }
    [[nodiscard]] QString takeError() const { return take_error_; }
    [[nodiscard]] QVariantMap lastTake() const { return last_take_; }
    [[nodiscard]] QVariantList takes() const { return takes_; }
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
    // Reads the folder again.
    Q_INVOKABLE void refreshTakes();
    // Removes one take of the list, its file and its description. False for a path that is not
    // in the list: another folder's file, or one that was never a take, is never touched.
    Q_INVOKABLE bool deleteTake(const QString& path);
    // Where takes go: a folder URL from a dialog, or a path. Kept.
    Q_INVOKABLE void setTakesFolder(const QString& urlOrPath);
    // The takes folder a capture names (main.cpp, --shot and --smoke-shot): the default's own
    // shape under a home that is nobody's - C:/Users/you/Documents/Hearth takes on Windows - so
    // that a picture of the card carries no account's name. Nothing is made there: a capture
    // never turns a microphone on.
    [[nodiscard]] static QString capture_takes_folder();
    // Keeps that folder in the settings store in use, which a capture has just pointed at a
    // scratch one of its own. Called before the controller that reads it exists.
    static void keep_capture_takes_folder();

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

    // What a view of the last few seconds is drawn from (M5), for [fromMs, toMs] of the item:
    //   hasReference  false when there is nothing to score against;
    //   notes         the reference's notes in the window, flat: start, end, MIDI note number;
    //   trace         the singer's placed frames in the window, flat: item time, MIDI note number
    //                 (-1 where not voiced) and 1 when within the tolerance of the reference, else
    //                 0. A voice an octave from the tune is drawn on the tune's octave, as it is
    //                 scored, so the line is where the notes are.
    // The window is asked for by the view, so nothing is built while no ribbon is shown.
    Q_INVOKABLE QVariantMap ribbon(qlonglong fromMs, qlonglong toMs) const;

    // A line's result once it has been sung, for [startMs, endMs) of the item (the line's own
    // times with the lyrics' delay added): scored (false when the reference has no note in it, or
    // there is no reference: a line like that has no mark, not a zero), withheld (the headphones
    // answer is missing), notSung (the singer was voiced for under a fifth of its notes' time:
    // shown as such and not as a bad score), percent, level (1 to 3: under 45, under 75, 75 and
    // over: presentation, not measurement) and counted.
    Q_INVOKABLE QVariantMap lineResult(qlonglong startMs, qlonglong endMs) const;
    // The level (1 to 3) a line of `percent` is marked with: the same three bands for every line.
    [[nodiscard]] static int levelFor(int percent);

    // What the 20 ms timer does: read the microphone, measure and score it, and tell the page.
    // Public so that a test with its own clock can drive it.
    void tick();

   signals:
    void stateChanged();
    void devicesChanged();
    void deviceChanged();
    void headphonesChanged();
    void alignmentChanged();
    void toleranceChanged();
    void ownKeyChanged();
    void meterChanged();
    void referenceChanged();
    void liveChanged();
    void scoreChanged();
    void recordingChanged();
    void takesFolderChanged();
    void takesChanged();
    void summaryChanged();

   private:
    struct State;
    static constexpr int kMaxAlignmentMs = 1000;

    void publish(bool force);
    void fail(const QString& text);
    void remember_take(const iclforge::hearth::TakeInfo& info);
    // Keeps what the performance came to, before the session forgets it.
    void capture_summary();
    // Hands the tolerance and the key shift in force to the session.
    void apply_score_options();
    // Takes the singer's key from what has been sung, once, when it is asked for.
    void lock_key(const std::optional<double>& offset_cents);
    // A new performance or another item has its own key.
    void unlock_key();

    std::unique_ptr<State> state_data_;
    QTimer timer_;

    QString state_ = QStringLiteral("off");
    QString error_text_;
    QVariantList devices_;
    QString device_id_;
    bool headphones_ = false;
    int tolerance_cents_ = 100;
    bool own_key_ = false;
    bool key_locked_ = false;
    double key_shift_cents_ = 0.0;
    int input_delay_ms_ = 0;
    int offset_ms_ = 0;
    double level_ = 0.0;
    double level_db_ = -120.0;
    bool clipping_ = false;
    double silent_seconds_ = 0.0;
    // The silence the card was last told of (meterChanged): it is told again when it has grown by
    // half a second. Not measured against silent_seconds_, which is read every 100 ms and so is
    // never half a second from the one before.
    double silent_announced_ = 0.0;
    QString reference_status_ = QStringLiteral("none");
    QString reference_text_;
    QVariantMap live_;
    QVariantMap score_;
    int score_revision_ = 0;
    QVariantMap summary_;
    bool recording_ = false;
    double take_seconds_ = 0.0;
    QString take_error_;
    QVariantMap last_take_;
    QVariantList takes_;
    QString takes_folder_;
    QString item_path_;
    QString prefer_;
    int ticks_ = 0;
};

}  // namespace iclforge::hearth::ui
