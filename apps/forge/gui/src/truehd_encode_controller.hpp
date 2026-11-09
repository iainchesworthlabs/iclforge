#pragma once

#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>
#include <QtQmlIntegration>

#include <atomic>
#include <cstdint>

#include "background_jobs.hpp"

// The Forge window's TrueHD encode: a WAV coded losslessly
// into a bare .thd stream by the encode `forge truehd-encode` runs (apps/shared/media/src/
// truehd_encode_core.hpp). It is its own surface, reached from the header like Open stream and QC,
// and deliberately not a fourth entry of the encoder workbench's codec list: the workbench plans an
// AC-3 family encode (bit rate, coding mode, metadata, a channel layout chosen by the operator),
// none of which a lossless TrueHD encode has, and its `plan::Codec` belongs to libs/ac3. The WAV's
// own channel mask names the speakers, as it does for the command line.
//
// The run is off the GUI thread and can be stopped; cliLine is the command that does the same.
// The output is a bare .thd stream, or Matroska (.mkv), MP4 (.mp4) or a Blu-ray transport stream
// (.m2ts, .ts), whichever its name asks for: the same writers `forge truehd-encode in.wav out.mkv`
// uses (apps/shared/media/src/truehd_wrap.hpp).
class TruehdEncodeController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QString sourcePath READ sourcePath NOTIFY sourceChanged)
    Q_PROPERTY(QString outputPath READ outputPath NOTIFY outputChanged)
    Q_PROPERTY(bool verify READ verify WRITE setVerify NOTIFY optionsChanged)
    Q_PROPERTY(bool standardDownmix READ standardDownmix WRITE setStandardDownmix NOTIFY
                   optionsChanged)
    // The other `truehd-*` options of the command line, by name (restart, peak-rate, rate-check,
    // bitrate, major-sync, fir-order, matrix, stereo-mode, downmix-gains) and the channel meaning of
    // TH spec §14.1 (dialnorm-2ch, mix-level-6ch, source-format-8ch, control-2ch, drc-start-up-gain,
    // ...: core::meaning_option_names), each as the text the command line takes; empty is the
    // encoder's own default (zero for the channel meaning). The channel meaning is provisional, no
    // oracle until the DEE set (libs/truehd/ERRATA.md, questions 51 to 55). optionErrors holds, for each text that is not a
    // value, the sentence that says what the option takes.
    Q_PROPERTY(QVariantMap options READ options NOTIFY optionsChanged)
    Q_PROPERTY(QVariantMap optionErrors READ optionErrors NOTIFY optionsChanged)
    Q_PROPERTY(QStringList optionNames READ optionNames CONSTANT)
    // Several sources, as `src=`, `map=` and `offset=` of the command line: the sources after the
    // first WAV, the map saying where each channel of each source goes (the places are TrueHD's
    // channel names), and for each source (the first WAV is source 0) a start offset in seconds.
    // mapError is the sentence for a map the command line would refuse, or for sources without one.
    Q_PROPERTY(QStringList extraSources READ extraSources NOTIFY sourcesChanged)
    Q_PROPERTY(QString mapText READ mapText NOTIFY sourcesChanged)
    Q_PROPERTY(QString mapError READ mapError NOTIFY sourcesChanged)
    Q_PROPERTY(QStringList offsetTexts READ offsetTexts NOTIFY sourcesChanged)
    Q_PROPERTY(QStringList offsetErrors READ offsetErrors NOTIFY sourcesChanged)
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
    [[nodiscard]] QVariantMap options() const;
    [[nodiscard]] QVariantMap optionErrors() const { return option_errors_; }
    [[nodiscard]] static QStringList optionNames();
    [[nodiscard]] QStringList extraSources() const { return extra_sources_; }
    [[nodiscard]] QString mapText() const { return map_text_; }
    [[nodiscard]] QString mapError() const { return map_error_; }
    [[nodiscard]] QStringList offsetTexts() const { return offset_text_; }
    [[nodiscard]] QStringList offsetErrors() const { return offset_error_; }
    [[nodiscard]] bool canEncode() const {
        return !busy_ && !source_.isEmpty() && !output_.isEmpty() && option_errors_.isEmpty() &&
               map_error_.isEmpty() && !has_offset_error();
    }
    [[nodiscard]] bool busy() const { return busy_; }
    [[nodiscard]] double progress() const { return progress_; }
    [[nodiscard]] QString summary() const { return summary_; }
    [[nodiscard]] QString error() const { return error_; }
    [[nodiscard]] QString cliLine() const;

    void setVerify(bool on);
    void setStandardDownmix(bool on);

    // Sets one of the options above; the text is kept as typed, trimmed, and checked by the rules of
    // the command line (apps/shared/media/src/truehd_encode_core.hpp). Back to the defaults with
    // resetOptions.
    Q_INVOKABLE void setOption(const QString& name, const QString& text);
    Q_INVOKABLE void resetOptions();
    // The WAV to code. An output beside it, named for it with .thd, is proposed when none is chosen.
    Q_INVOKABLE void chooseSource(const QUrl& url);
    Q_INVOKABLE void chooseOutput(const QUrl& url);
    // The sources after the first WAV (src=), the map (map=) and a source's start offset in seconds
    // (offset=<index>:<seconds>, source 0 the first WAV; the text is kept as typed and checked by the
    // rules of the command line). A source that is added gets no offset; one that is removed takes
    // its offset with it.
    Q_INVOKABLE void addSource(const QUrl& url);
    Q_INVOKABLE void removeSource(int extraIndex);
    Q_INVOKABLE void setMap(const QString& text);
    Q_INVOKABLE void setOffset(int sourceIndex, const QString& text);
    Q_INVOKABLE void encode();
    // Asks the run to stop at its next chunk; nothing is written.
    Q_INVOKABLE void cancel();
    // Forgets the source, the output and the last result, for a project that brings its own.
    Q_INVOKABLE void clear();

   signals:
    void sourceChanged();
    void sourcesChanged();
    void outputChanged();
    void optionsChanged();
    void stateChanged();
    void progressChanged();
    void resultChanged();
    void cliLineChanged();

   private:
    void sources_changed();
    void check_map();
    [[nodiscard]] bool has_offset_error() const;

    QString source_;
    QString output_;
    QStringList extra_sources_;
    QString map_text_;
    QString map_error_;
    QStringList offset_text_;   // by source, the first WAV being 0; kept as long as the sources
    QStringList offset_error_;  // by source, empty where the text is a value
    bool verify_ = false;
    bool downmix_ = false;
    QMap<QString, QString> option_text_;  // by name, only the ones with a text
    QVariantMap option_errors_;           // by name, the sentence for each text that is refused
    bool busy_ = false;
    double progress_ = 0.0;
    QString summary_;
    QString error_;
    std::atomic<bool> stop_{false};
    forge_gui::BackgroundJobs jobs_;
};
