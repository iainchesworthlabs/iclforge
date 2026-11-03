#pragma once

#include <QObject>
#include <QString>
#include <QUrl>
#include <QtQmlIntegration>

#include <atomic>
#include <cstdint>

#include "background_jobs.hpp"

// The Forge window's TrueHD encode (planning/truehd-parity-forge.md, row U1): a WAV coded losslessly
// into a bare .thd stream by the encode `forge truehd-encode` runs (apps/shared/media/src/
// truehd_encode_core.hpp). It is its own surface, reached from the header like Open stream and QC,
// and deliberately not a fourth entry of the encoder workbench's codec list: the workbench plans an
// AC-3 family encode (bit rate, coding mode, metadata, a channel layout chosen by the operator),
// none of which a lossless TrueHD encode has, and its `plan::Codec` belongs to libs/ac3. The WAV's
// own channel mask names the speakers, as it does for the command line.
//
// The run is off the GUI thread and can be stopped; cliLine is the command that does the same.
// Bare .thd only: the containers (Matroska, MP4, a Blu-ray transport stream) are `forge truehd-encode
// in.wav out.mkv` and `forge mkv`, `mp4`, `ts` of the stream.
class TruehdEncodeController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QString sourcePath READ sourcePath NOTIFY sourceChanged)
    Q_PROPERTY(QString outputPath READ outputPath NOTIFY outputChanged)
    Q_PROPERTY(bool verify READ verify WRITE setVerify NOTIFY optionsChanged)
    Q_PROPERTY(bool standardDownmix READ standardDownmix WRITE setStandardDownmix NOTIFY
                   optionsChanged)
    Q_PROPERTY(bool canEncode READ canEncode NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    // 0 to 1 while a run is going.
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    // What the last successful run wrote, as one line, and why the last run failed. One of the two
    // is empty.
    Q_PROPERTY(QString summary READ summary NOTIFY resultChanged)
    Q_PROPERTY(QString error READ error NOTIFY resultChanged)
    // The `forge` command that does what the window would, quoted as a shell takes it.
    Q_PROPERTY(QString cliLine READ cliLine NOTIFY cliLineChanged)

   public:
    explicit TruehdEncodeController(QObject* parent = nullptr);
    ~TruehdEncodeController() override;

    [[nodiscard]] QString sourcePath() const { return source_; }
    [[nodiscard]] QString outputPath() const { return output_; }
    [[nodiscard]] bool verify() const { return verify_; }
    [[nodiscard]] bool standardDownmix() const { return downmix_; }
    [[nodiscard]] bool canEncode() const { return !busy_ && !source_.isEmpty() && !output_.isEmpty(); }
    [[nodiscard]] bool busy() const { return busy_; }
    [[nodiscard]] double progress() const { return progress_; }
    [[nodiscard]] QString summary() const { return summary_; }
    [[nodiscard]] QString error() const { return error_; }
    [[nodiscard]] QString cliLine() const;

    void setVerify(bool on);
    void setStandardDownmix(bool on);

    // The WAV to code. An output beside it, named for it with .thd, is proposed when none is chosen.
    Q_INVOKABLE void chooseSource(const QUrl& url);
    Q_INVOKABLE void chooseOutput(const QUrl& url);
    Q_INVOKABLE void encode();
    // Asks the run to stop at its next chunk; nothing is written.
    Q_INVOKABLE void cancel();
    // Forgets the source, the output and the last result, for a project that brings its own.
    Q_INVOKABLE void clear();

   signals:
    void sourceChanged();
    void outputChanged();
    void optionsChanged();
    void stateChanged();
    void progressChanged();
    void resultChanged();
    void cliLineChanged();

   private:
    QString source_;
    QString output_;
    bool verify_ = false;
    bool downmix_ = false;
    bool busy_ = false;
    double progress_ = 0.0;
    QString summary_;
    QString error_;
    std::atomic<bool> stop_{false};
    forge_gui::BackgroundJobs jobs_;
};
