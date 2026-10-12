#pragma once

#include <QMetaType>
#include <QObject>
#include <QString>

#include <memory>
#include <vector>

#include "media_session.hpp"

class QThread;

// The operating system's media sessions, polled off the window's thread (planning/karaoke-phase2.md,
// D9). Reading them is an inter-process call (media_session.hpp), so it runs on a thread of its
// own, on a timer, and what it read comes back to the window's thread as one list. Nothing polls
// while nothing wants the answer: setWanted(false) stops the timer, which is the state of every
// run in which no lyrics were imported.
//
// The reader is a seam. Production gives it platform_media_sessions(); a test gives it a scripted
// reader, or none, so that a developer's own music does not move a test.
class MediaMonitor : public QObject {
    Q_OBJECT

public:
    // How often a wanted monitor reads: often enough that a seek in the player is followed within a
    // line's time, rarely enough to cost nothing. A position between reads is carried forward.
    static constexpr int kPollIntervalMs = 500;

    // `reader` null: the platform's. `intervalMs` is the poll period.
    explicit MediaMonitor(std::shared_ptr<iclforge::crucible::ui::MediaSessions> reader = nullptr,
                          int intervalMs = kPollIntervalMs, QObject* parent = nullptr);
    ~MediaMonitor() override;

    MediaMonitor(const MediaMonitor&) = delete;
    MediaMonitor& operator=(const MediaMonitor&) = delete;

    // Whether anything wants the sessions now. true starts the thread and its timer (and reads at
    // once); false stops both and waits for the thread.
    void setWanted(bool wanted);
    [[nodiscard]] bool wanted() const { return wanted_; }

    // What the platform says about its own ability to read, as it said when this was asked.
    [[nodiscard]] iclforge::crucible::ui::MediaSessionSupport support() const;

signals:
    // A read finished: every session the system had, on the window's thread.
    void sessionsRead(const std::vector<iclforge::crucible::ui::MediaSession>& sessions);

private:
    std::shared_ptr<iclforge::crucible::ui::MediaSessions> reader_;
    int interval_ms_;
    bool wanted_ = false;
    // The thread and its worker exist while wanted.
    QThread* thread_ = nullptr;
    QObject* worker_ = nullptr;
};

Q_DECLARE_METATYPE(std::vector<iclforge::crucible::ui::MediaSession>)
