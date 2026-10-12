#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration>

#include <memory>
#include <vector>

#include "cdg_frame_source.hpp"
#include "media_session.hpp"

// Lyrics for the application a person has selected (planning/karaoke-phase2.md, W13b, D9): an
// `.lrc` file imported against one application of the room, and a strip that shows the line being
// sung. A second QML singleton beside CrucibleController, so that crucible_controller.cpp does not
// grow. What it wraps is libs/lyrics, the Qt-free reader and cursor that Hearth's window shares.
//
// Where the position comes from (D9). An application that publishes a media session (the Windows
// transport controls, MPRIS on Linux, macOS's now-playing information: media_session.hpp) is
// followed from it: the player's own position, carried forward between reads, so a pause and a seek
// in it are seen. Where it publishes none, or the platform cannot say, the person presses Start as
// the song begins and the strip runs on this process's monotonic clock from there; a nudge moves
// the words earlier or later whichever the origin is. The by-hand source says that it does not see
// a pause or a seek, and the strip says so.
//
// Three things are kept honest. A session that goes away does not make the words jump: they carry
// on from where they were, by hand, and say so. A session whose track changes (its title, artist
// or length) is not followed against the lyrics of the last one: they wait for the person. And a
// platform that cannot read sessions says why, instead of reading as "nothing is playing".
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

    // --- the position from the operating system's media session ---------------------------------

    // What application `app` is, so that its session can be found: the name the room shows, its
    // executable's path and its platform id (a Windows packaged application's user model id, a
    // Linux desktop entry). The strip says it whenever it is given an application.
    Q_INVOKABLE void setApplication(int app, const QString& name, const QString& imagePath, const QString& appId);

    // "session": the position is the application's media session where it has one; "hand": the
    // person's Start. An application with lyrics is in "session" until Start is pressed.
    Q_INVOKABLE QString positionSource(int app) const;
    // Where that stands: "none" (no lyrics to follow); "following" (a session has it); "waiting"
    // (session mode, none matched yet: press Start to begin by hand); "no-timeline" (a session
    // matched that publishes no position); "changed" (the session's track is not the one the lyrics
    // were bound to); "unavailable" (this platform cannot read sessions, see
    // sessionsUnavailableReason()); "lost" (it had a session and it went: now following the clock
    // from where it was); "hand" (the person started it).
    Q_INVOKABLE QString sessionState(int app) const;
    // The matched session's name, or its id when it has none; empty with none.
    Q_INVOKABLE QString sessionLabel(int app) const;
    // The matched session's track title and artist, as "Title - Artist"; empty with none.
    Q_INVOKABLE QString sessionTrack(int app) const;
    // Follow the media session again after a by-hand start: the best match for the application
    // when `sessionId` is empty, otherwise that session, held until it goes. Binds the lyrics to
    // the track it is playing, which is how "changed" is got out of.
    Q_INVOKABLE void useSession(int app, const QString& sessionId = QString());
    // Every session as last read: id, name, title, artist, state ("playing", "paused", "stopped"),
    // hasPosition and matched (it is the one `app`'s), for a picker. `app` -1: none matched.
    Q_INVOKABLE QVariantList sessions(int app = -1) const;
    // Whether this platform can read sessions, and when it cannot, why in one sentence.
    Q_INVOKABLE bool sessionsAvailable() const;
    Q_INVOKABLE QString sessionsUnavailableReason() const;

    // Applies a read of the sessions as of `nowMs`: what the monitor gives it on the window's
    // thread, and what a test gives it (applySessionList) in place of a machine's own music.
    void applySessions(const std::vector<iclforge::crucible::ui::MediaSession>& read, qlonglong nowMs);
    // The same from maps: id, name, executable, desktopId, title, artist, durationMs, hasPosition,
    // positionMs, rate and playback ("playing", "paused", "stopped"); the position is as of `nowMs`.
    Q_INVOKABLE void applySessionList(const QVariantList& read, qlonglong nowMs);

    // Start: the song begins now (or at `nowMs`), by hand. Start again to begin it again.
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
    // The sessions read, or what can be read, changed: the picker rereads.
    void sessionsChanged();

private:
    struct State;

    // Starts or stops the thread that reads the sessions: it runs while an application with
    // lyrics is waiting on one.
    void refresh_monitor();

    std::unique_ptr<State> state_;
};
