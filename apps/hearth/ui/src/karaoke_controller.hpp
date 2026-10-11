#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration>

#include <memory>

// The lyrics of the item playing (planning/karaoke-design.md, K5): a third QML
// singleton beside HearthController and NetworkController, so that
// hearth_controller.cpp does not grow. What it wraps is apps/hearth/engine/src/
// iclforge/lyrics/lyrics.hpp, the Qt-free reader and cursor; this class turns a file into
// what a view binds to and answers "where is the singer" for a position.
//
// Nothing here is polled and nothing here owns a clock: the view that shows the
// words keeps one (LyricClock.qml), because a word's fill moves far more often
// than HearthController's poll, and asks cursor() with its reading each frame.
// load() is told which item is playing by whoever shows lyrics (LyricView.qml),
// and is a no-op for an item it already has.
namespace iclforge::hearth::ui {

class KaraokeController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // "none": no `name.lrc` beside the item (or nothing playing); "timed": a
    // file whose lines have times; "plain": a file with no times, shown whole
    // and not followed; "error": a file that could not be read, with errorText.
    Q_PROPERTY(QString status READ status NOTIFY lyricsChanged)
    // The file's name, and for "error" the reason as the reader wrote it. Empty
    // while there is no file.
    Q_PROPERTY(QString fileName READ fileName NOTIFY lyricsChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY lyricsChanged)
    // The name the file would have, for the "no lyrics" sentence: the item's
    // own, with `.lrc`.
    Q_PROPERTY(QString expectedName READ expectedName NOTIFY lyricsChanged)
    Q_PROPERTY(QString title READ title NOTIFY lyricsChanged)
    Q_PROPERTY(QString artist READ artist NOTIFY lyricsChanged)
    // Whether any line carries word times. A timed file without them is
    // followed line by line, and no word times are invented.
    Q_PROPERTY(bool wordTimed READ wordTimed NOTIFY lyricsChanged)
    // Each line: text, startMs, endMs, wordTimed, and words - each with text
    // (spacing removed), startMs, endMs and gap (whether it was followed by
    // spacing). Empty unless the status is "timed" or "plain".
    Q_PROPERTY(QVariantList lines READ lines NOTIFY lyricsChanged)
    Q_PROPERTY(int lineCount READ lineCount NOTIFY lyricsChanged)
    // A duet (planning/karaoke-phase2.md, W11b). Each line also carries `singer`: "" for a
    // line that says nothing, else "v1", "v2" or "both". hasSingers is whether any does.
    Q_PROPERTY(bool hasSingers READ hasSingers NOTIFY lyricsChanged)
    // The two singers' names as a person typed them: for this run of the app, not for a song
    // and not kept (D7), and empty until typed. A typed name comes before the file's
    // `[v1:]` / `[v2:]` tag, which comes before "V1" and "V2".
    Q_PROPERTY(QString firstSingerName READ firstSingerName WRITE setFirstSingerName NOTIFY singerNamesChanged)
    Q_PROPERTY(QString secondSingerName READ secondSingerName WRITE setSecondSingerName NOTIFY singerNamesChanged)
    // What the file calls them, and what a view shows: the typed name, else the file's, else
    // the vocal's own name.
    Q_PROPERTY(QString fileFirstSingerName READ fileFirstSingerName NOTIFY lyricsChanged)
    Q_PROPERTY(QString fileSecondSingerName READ fileSecondSingerName NOTIFY lyricsChanged)
    Q_PROPERTY(QString firstSingerLabel READ firstSingerLabel NOTIFY singerNamesChanged)
    Q_PROPERTY(QString secondSingerLabel READ secondSingerLabel NOTIFY singerNamesChanged)
    // The singer's nudge, in milliseconds, added to the file's own [offset:]:
    // positive shows the words later. Kept across starts, clamped to
    // +/-kMaxDelayMs.
    Q_PROPERTY(int delayMs READ delayMs WRITE setDelayMs NOTIFY delayChanged)
    Q_PROPERTY(int maxDelayMs READ maxDelayMs CONSTANT)

public:
    static constexpr int kMaxDelayMs = 1000;

    explicit KaraokeController(QObject* parent = nullptr);
    ~KaraokeController() override;

    KaraokeController(const KaraokeController&) = delete;
    KaraokeController& operator=(const KaraokeController&) = delete;

    [[nodiscard]] QString status() const { return status_; }
    [[nodiscard]] QString fileName() const { return file_name_; }
    [[nodiscard]] QString errorText() const { return error_text_; }
    [[nodiscard]] QString expectedName() const { return expected_name_; }
    [[nodiscard]] QString title() const { return title_; }
    [[nodiscard]] QString artist() const { return artist_; }
    [[nodiscard]] bool wordTimed() const { return word_timed_; }
    [[nodiscard]] QVariantList lines() const { return lines_; }
    [[nodiscard]] int lineCount() const { return static_cast<int>(lines_.size()); }
    [[nodiscard]] int delayMs() const { return delay_ms_; }
    [[nodiscard]] int maxDelayMs() const { return kMaxDelayMs; }
    void setDelayMs(int delay_ms);
    [[nodiscard]] bool hasSingers() const { return has_singers_; }
    [[nodiscard]] QString firstSingerName() const { return first_name_; }
    [[nodiscard]] QString secondSingerName() const { return second_name_; }
    [[nodiscard]] QString fileFirstSingerName() const { return file_first_name_; }
    [[nodiscard]] QString fileSecondSingerName() const { return file_second_name_; }
    [[nodiscard]] QString firstSingerLabel() const;
    [[nodiscard]] QString secondSingerLabel() const;
    void setFirstSingerName(const QString& name);
    void setSecondSingerName(const QString& name);

    // Reads the lyrics of the item at `itemPath` (the queue's path), whose
    // length is `durationMs` (0 while it is not known). The same item and
    // length again changes nothing. An empty path means nothing is playing.
    Q_INVOKABLE void load(const QString& itemPath, qlonglong durationMs);

    // Where `positionMs` is, with the delay taken off: line (-1 before the
    // first and for a plain file), inLine, word (-1 for none), fraction through
    // that word, positionMs (the position as the words see it), and fills - for
    // a word-timed current line one number per word: 1 sung, between 0 and 1
    // being sung, 0 not yet. A pure function of the lyrics, the delay and the
    // position, so a seek and a pause are only another position.
    Q_INVOKABLE QVariantMap cursor(qlonglong positionMs) const;

signals:
    void lyricsChanged();
    void delayChanged();
    void singerNamesChanged();

private:
    struct State;

    void clear_lyrics();

    std::unique_ptr<State> state_;
    QString status_ = QStringLiteral("none");
    QString file_name_;
    QString error_text_;
    QString expected_name_;
    QString title_;
    QString artist_;
    bool word_timed_ = false;
    bool has_singers_ = false;
    QString first_name_;
    QString second_name_;
    QString file_first_name_;
    QString file_second_name_;
    QVariantList lines_;
    QString item_path_;
    qlonglong duration_ms_ = 0;
    int delay_ms_ = 0;
};

}  // namespace iclforge::hearth::ui
