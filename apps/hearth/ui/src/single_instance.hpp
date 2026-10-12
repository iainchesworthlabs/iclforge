#pragma once

#include <QLocalServer>
#include <QLockFile>
#include <QObject>
#include <QString>
#include <QStringList>

// One Hearth to play, however many times a file is opened: a second `hearth <file>` (a second
// double-click on a stream) hands its files to the Hearth that is already running and goes,
// and the running one queues them, plays the first and comes forward. Without it every
// double-click starts another engine on the same output, and since a launch plays, two of
// them play over each other.
//
// The hand-over is a local socket (a named pipe on Windows) that only the same account can
// open, and one line of JSON each way: `{"open": [paths...], "page": "karaoke"}` answered with
// `ok` or `error`. The paths are absolute (the sender's working directory is not the receiver's)
// and `page` is optional. Nothing here starts a window or reads a setting: main.cpp decides
// which launches take part (those that play, and every one of them listens - a capture does
// neither), and what the files do once they arrive.
namespace iclforge::hearth::ui {

class SingleInstance final : public QObject {
    Q_OBJECT

public:
    explicit SingleInstance(QString name, QObject* parent = nullptr);

    // The name every Hearth of one person's account shares, and no other account's: a Windows
    // pipe name is machine-wide, so two people on one machine would otherwise share it. The
    // home directory tells the accounts apart.
    [[nodiscard]] static QString default_name();

    // The pages of the window a hand-over may name (Main.qml's `page`). The dialogs `--page`
    // also takes (firstrun, shortcuts, about, licences) are not pages and are not carried.
    [[nodiscard]] static QStringList known_pages();

    // Gives `files` (and `page`, if it is one of known_pages()) to the Hearth listening under
    // `name`, and waits up to `timeout_ms` for each step of it to say it has them. True when it
    // did, and the caller has nothing left to do. False when nobody listens or the one that does
    // does not answer, and the caller runs as itself. A plain function and not a member, with no
    // event loop needed, so a caller on any thread may use it.
    [[nodiscard]] static bool hand_over(const QString& name, const QStringList& files,
                                        const QString& page = {}, int timeout_ms = 2000);

    // Starts listening under the name. False when a live Hearth already does (which stays the
    // one that is served), or when it cannot be done at all: the caller runs on without it.
    // Ownership of the name is a lock file (QLockFile, which also lets go of one a Hearth that
    // did not close left behind): a Windows pipe name can be served by two servers at once, so
    // the pipe alone would not say who owns it. Holding the lock is also what makes it safe to
    // remove a socket file left behind on Unix before listening.
    [[nodiscard]] bool listen();

signals:
    // A hand-over arrived: absolute paths, at least one, and a page of the window or empty.
    void received(const QStringList& files, const QString& page);

private:
    void on_connection();

    QString name_;
    QLockFile lock_;
    QLocalServer server_;
};

}  // namespace iclforge::hearth::ui
