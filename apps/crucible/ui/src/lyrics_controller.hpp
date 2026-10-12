#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration>

#include <memory>

#include "cdg_frame_source.hpp"

// Lyrics for the application a person has selected (planning/karaoke-phase2.md, W13b, D9): an
// `.lrc` file imported against one application of the room, and a strip that shows the line being
// sung. A second QML singleton beside CrucibleController, so that crucible_controller.cpp does not
// grow. What it wraps is libs/lyrics, the Qt-free reader and cursor that Hearth's window shares.
//
// Where the position comes from. Crucible taps an application's audio and knows nothing of a
// track: a search of this program and libs/audio finds no media session (MPRIS, the Windows
// transport controls, macOS's now-playing), so the song's position is not known. This is the
// start-and-nudge source D9 names for where there is none: the person presses Start as the song
// begins, the strip runs on this process's monotonic clock from there, and a nudge moves the
// words earlier or later. It is honest about being manual - a pause or a seek in the player is not
// seen - and says so on the page. The media-session sources (three platform back ends) are a later
// change that replaces the origin and keeps everything else.
//
// A CD+G disc (planning/karaoke-phase2.md, W13f and D11) is imported the same way and is followed by
// the same clock and nudge, but it carries a picture and no text: status() says "picture", lines()
// is empty and the strip draws it with the shared CdgPicture, asking pictureFrame(app, position).
//
// Nothing here owns a timer: the strip asks cursor(app, nowMs()) on its own, and every function
// that depends on the time takes it as an argument, so a test can set the clock.
class CrucibleLyrics : public iclforge::apps::CdgFrameSource {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    // How far a nudge may take the words either way, in milliseconds.
    static constexpr int kMaxNudgeMs = 10000;

    explicit CrucibleLyrics(QObject* parent = nullptr);
    ~CrucibleLyrics() override;

    CrucibleLyrics(const CrucibleLyrics&) = delete;
    CrucibleLyrics& operator=(const CrucibleLyrics&) = delete;

    // This process's monotonic clock, in milliseconds: what Start and the strip are timed on.
    Q_INVOKABLE qlonglong nowMs() const;

    // Reads `.lrc` text from `fileUrl` (a file: URL or a path) for application `app`, replacing
    // what it had. False, with errorText(app), when it could not be read or has no lyrics; the
    // application then has no lyrics. `durationMs` is the song's length for the last line's end,
    // 0 when it is not known.
    Q_INVOKABLE bool importFile(int app, const QString& fileUrl, qlonglong durationMs = 0);
    // The same from text already in hand: what importFile() does after it has read the file, and
    // what a test gives it. `name` is shown as the file's name.
    Q_INVOKABLE bool importText(int app, const QString& name, const QString& text, qlonglong durationMs = 0);
    // Forgets an application's lyrics and its clock.
    Q_INVOKABLE void clear(int app);
    // Forgets every application that is not in `liveApps` (a list of application ids), so a
    // session that has left does not leave lyrics behind for the next to be given its id.
    Q_INVOKABLE void prune(const QVariantList& liveApps);

    // "none": nothing imported (and no error); "timed": a file with times; "plain": a file with no
    // times, shown whole and not followed; "picture": a CD+G disc (a `.cdg` file), whose words are
    // pixels and which is followed as a picture; "error": a file that could not be read.
    Q_INVOKABLE QString status(int app) const;
    Q_INVOKABLE QString fileName(int app) const;
    Q_INVOKABLE QString errorText(int app) const;
    // Each line: text, startMs, endMs, wordTimed and words (text, startMs, endMs, gap), as the
    // Hearth window's controller gives them. Empty for an application with no lyrics.
    Q_INVOKABLE QVariantList lines(int app) const;

    // The picture of application `key`'s CD+G disc at `positionMs` of the song, with the nudge taken
    // off: 300 by 216, 0xAARRGGBB. A null image for an application without a disc. This is what
    // the shared CdgPicture asks, and `key` is the application's id. A position before the song
    // has begun is the first picture.
    [[nodiscard]] QImage pictureFrame(int key, qlonglong positionMs) override;

    // Start: the song begins now (or at `nowMs`). Start again to begin it again.
    Q_INVOKABLE void start(int app);
    Q_INVOKABLE void startAt(int app, qlonglong nowMs);
    // Stops the strip following: the lyrics stay.
    Q_INVOKABLE void stop(int app);
    Q_INVOKABLE bool running(int app) const;

    // The words shown later by this many milliseconds (negative: earlier), held to +/-kMaxNudgeMs.
    Q_INVOKABLE int nudgeMs(int app) const;
    Q_INVOKABLE void setNudgeMs(int app, int ms);
    Q_INVOKABLE void nudge(int app, int deltaMs);

    // The song's position at `nowMs`: time since Start, never negative; -1 until it is started.
    Q_INVOKABLE qlonglong positionMs(int app, qlonglong nowMs) const;
    // Where that position is, with the nudge taken off: line (-1 before the first and while not
    // running), inLine, word (-1 for none), fraction through that word, positionMs, and fills -
    // for a word-timed current line one number per word: 1 sung, between 0 and 1 being sung, 0
    // not yet. The same keys as KaraokeController.cursor() in the Hearth window.
    Q_INVOKABLE QVariantMap cursor(int app, qlonglong nowMs) const;

signals:
    // An application's lyrics, clock or nudge changed; the strip for it rereads.
    void changed(int app);

private:
    struct State;
    std::unique_ptr<State> state_;
};
