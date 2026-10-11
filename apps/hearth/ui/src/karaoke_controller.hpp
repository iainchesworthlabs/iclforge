#pragma once

#include <QImage>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration>

#include <cstddef>
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
    // and not followed; "picture": no lyrics file, and a `name.cdg` beside the
    // item, the picture of a CD+G disc (planning/karaoke-phase2.md, W13f; it
    // carries no text, so it is shown and not read); "error": a file that could
    // not be read, with errorText.
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

    // ---- timing the lyrics (planning/karaoke-phase2.md, W6) -------------------------
    // A session over a copy of the item's words, in which a person presses a
    // key as each line and then each word is sung; saving writes the item's
    // `.lrc`. The displayed lyrics above are not touched until a save, so the
    // lyric view never shows an estimate as sung.
    //
    // Whether a session is open.
    Q_PROPERTY(bool timing READ timing NOTIFY timingChanged)
    // The copy being timed. Each line: text, startMs, endMs, stamped (a person
    // or the file gave it a time), wordTimed and words - each with text
    // (spacing removed), gap, startMs, endMs and tapped (a person or the file
    // gave it its time; false is an estimate, to be drawn as a guess).
    Q_PROPERTY(QVariantList timingLines READ timingLines NOTIFY timingChanged)
    // The first line with no time yet, for the line pass; -1 when every line
    // has one.
    Q_PROPERTY(int nextLine READ nextLine NOTIFY timingChanged)
    Q_PROPERTY(int stampedLines READ stampedLines NOTIFY timingChanged)
    // Edited since it was opened or last saved.
    Q_PROPERTY(bool timingDirty READ timingDirty NOTIFY timingChanged)
    // Why a session would not open or a save failed; empty otherwise.
    Q_PROPERTY(QString timingError READ timingError NOTIFY timingChanged)
    // How long after the sound a press comes, taken off every press so that
    // the stored time is the sound's. Kept across starts, 0 to maxReactionMs.
    Q_PROPERTY(int reactionMs READ reactionMs WRITE setReactionMs NOTIFY reactionChanged)
    Q_PROPERTY(int maxReactionMs READ maxReactionMs CONSTANT)
    // The singers' names as the file being timed will carry them (`[v1:]` and `[v2:]`, planning/
    // karaoke-phase2.md, W11c): what the file said, then what was typed. They are the file's, so
    // they are saved with it; the names typed on the Karaoke page are for the run and are not.
    // Brackets and control characters are taken out, since a tag ends at the first `]`. Empty
    // while nothing is being timed.
    Q_PROPERTY(QString timingFirstName READ timingFirstName WRITE setTimingFirstName NOTIFY timingChanged)
    Q_PROPERTY(QString timingSecondName READ timingSecondName WRITE setTimingSecondName NOTIFY timingChanged)

public:
    static constexpr int kMaxDelayMs = 1000;
    static constexpr int kMaxReactionMs = 500;
    static constexpr int kDefaultReactionMs = 120;

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

    [[nodiscard]] bool timing() const { return timing_; }
    [[nodiscard]] QVariantList timingLines() const { return timing_lines_; }
    [[nodiscard]] int nextLine() const { return next_line_; }
    [[nodiscard]] int stampedLines() const { return stamped_lines_; }
    [[nodiscard]] bool timingDirty() const { return timing_dirty_; }
    [[nodiscard]] QString timingError() const { return timing_error_; }
    [[nodiscard]] int reactionMs() const { return reaction_ms_; }
    [[nodiscard]] int maxReactionMs() const { return kMaxReactionMs; }
    void setReactionMs(int reaction_ms);
    [[nodiscard]] QString timingFirstName() const;
    [[nodiscard]] QString timingSecondName() const;
    void setTimingFirstName(const QString& name);
    void setTimingSecondName(const QString& name);

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

    // Opens a session over the loaded lyrics, or over `textFile` (a file URL
    // or a path: a plain text file, or a `.lrc`) when given. False, with
    // timingError, when nothing is playing or there are no words to time.
    Q_INVOKABLE bool beginTiming(const QString& textFile = QString());
    // Closes the session, discarding what was not saved.
    Q_INVOKABLE void endTiming();
    // The line pass: the next line with no time starts at `positionMs` (the
    // engine's heard position) less the reaction time. False when there is
    // none left, or no session.
    Q_INVOKABLE bool tapLine(qlonglong positionMs);
    // Gives a line a new start (a press on one already stamped), its words
    // moving with it.
    Q_INVOKABLE bool restampLine(int line, qlonglong positionMs);
    // The word pass: every stamped line with text and no words gets the
    // estimate. How many lines were given one.
    Q_INVOKABLE int estimateWords();
    // A word's start from a press, held between its neighbours.
    Q_INVOKABLE bool tapWord(int line, int word, qlonglong positionMs);
    // Moves a line (`word` < 0) or a word by `deltaMs`, as a nudge.
    Q_INVOKABLE bool nudge(int line, int word, int deltaMs);
    // Who sings line `line` of the file being timed (a duet, W11): "" for no one, "v1", "v2" or
    // "both". The line's time and words are not touched. False, changing nothing, when nothing is
    // being timed, the line does not exist or the singer is another word.
    Q_INVOKABLE bool setLineSinger(int line, const QString& singer);
    // Writes the item's `.lrc` (an existing one is kept once as `.lrc.bak`) and
    // reloads it as the lyrics. An empty string, or why not.
    Q_INVOKABLE QString saveTiming();
    // The picture of a "picture" status at `positionMs`, with the delay taken off: 300 by 216, the
    // border drawn, 0xAARRGGBB. A null image when there is no picture. CdgPicture asks for it each
    // time the position moves; only the position decides what it is, so a seek and a pause are
    // only another position, and the screen replays from the start when it goes back.
    [[nodiscard]] QImage pictureFrame(qlonglong positionMs);

signals:
    void lyricsChanged();
    void delayChanged();
    void timingChanged();
    void reactionChanged();
    void singerNamesChanged();

private:
    struct State;

    void clear_lyrics();
    void refresh_timing();
    [[nodiscard]] qlonglong adjusted(qlonglong position_ms) const;

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

    bool timing_ = false;
    bool timing_dirty_ = false;
    QVariantList timing_lines_;
    int next_line_ = -1;
    int stamped_lines_ = 0;
    QString timing_error_;
    int reaction_ms_ = kDefaultReactionMs;
    // The last frame handed out and how many packets it had applied: a position that has not
    // reached the next packet (they come 300 a second, the view asks 60) is the same picture.
    QImage frame_;
    std::size_t frame_applied_ = 0;
};

}  // namespace iclforge::hearth::ui
