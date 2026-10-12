#include "single_instance.hpp"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLocalSocket>
#include <QTimer>

#include <memory>
#include <optional>

// See single_instance.hpp.

namespace iclforge::hearth::ui {

namespace {

// A hand-over is a handful of paths. A peer that sends more than this, or more files than any
// queue is built for, or never finishes its line, is dropped rather than waited on.
constexpr qsizetype kMaxMessageBytes = 1 << 20;
constexpr qsizetype kMaxFiles = 4096;
constexpr int kLineTimeoutMs = 5000;

struct Message {
    QStringList files;
    QString page;
};

// One line of JSON: an object with a non-empty "open" array of non-empty strings, and an
// optional "page". Anything else is not a hand-over.
[[nodiscard]] std::optional<Message> parse_message(const QByteArray& line) {
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::nullopt;
    }
    const QJsonObject object = document.object();
    const QJsonValue open = object.value(QLatin1String("open"));
    if (!open.isArray()) {
        return std::nullopt;
    }
    Message out;
    for (const QJsonValue& value : open.toArray()) {
        if (!value.isString() || value.toString().isEmpty() || out.files.size() >= kMaxFiles) {
            return std::nullopt;
        }
        out.files.push_back(value.toString());
    }
    if (out.files.isEmpty()) {
        return std::nullopt;
    }
    const QString page = object.value(QLatin1String("page")).toString();
    if (SingleInstance::known_pages().contains(page)) {
        out.page = page;
    }
    return out;
}

// What one connection has said so far, kept for as long as its handlers are connected.
struct Line {
    QByteArray data;
    bool done = false;
};

}  // namespace

SingleInstance::SingleInstance(QString name, QObject* parent)
    : QObject(parent),
      name_(std::move(name)),
      lock_(QDir(QDir::tempPath()).filePath(name_ + QStringLiteral(".lock"))),
      server_(this) {
    // Only the account that runs this Hearth may hand it anything.
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&server_, &QLocalServer::newConnection, this, &SingleInstance::on_connection);
}

QString SingleInstance::default_name() {
    const QByteArray digest =
        QCryptographicHash::hash(QDir::homePath().toUtf8(), QCryptographicHash::Sha1).toHex();
    return QStringLiteral("iclforge-hearth-") + QString::fromLatin1(digest.left(16));
}

QStringList SingleInstance::known_pages() {
    return {QStringLiteral("play"),  QStringLiteral("media"),   QStringLiteral("speakers"),
            QStringLiteral("decoder"), QStringLiteral("network"), QStringLiteral("settings"),
            QStringLiteral("karaoke")};
}

bool SingleInstance::hand_over(const QString& name, const QStringList& files, const QString& page,
                               int timeout_ms) {
    if (files.isEmpty()) {
        return false;
    }
    QLocalSocket socket;
    socket.connectToServer(name, QIODevice::ReadWrite);
    if (!socket.waitForConnected(timeout_ms)) {
        return false;
    }
    QJsonObject object;
    object.insert(QLatin1String("open"), QJsonArray::fromStringList(files));
    if (!page.isEmpty()) {
        object.insert(QLatin1String("page"), page);
    }
    QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    payload.append('\n');
    if (socket.write(payload) != payload.size() || !socket.waitForBytesWritten(timeout_ms)) {
        return false;
    }
    // What is already buffered first: waitForReadyRead() only reports bytes that arrive while it
    // waits, and the answer can have come in during the write above (found as a flake, in
    // roughly one run in twenty, on Windows).
    QByteArray reply;
    for (;;) {
        reply.append(socket.readAll());
        if (reply.contains('\n')) {
            break;
        }
        if (!socket.waitForReadyRead(timeout_ms)) {
            reply.append(socket.readAll());
            break;
        }
    }
    return reply.startsWith("ok");
}

bool SingleInstance::listen() {
    // A live Hearth holds the lock. One that did not close leaves its lock file behind, and
    // QLockFile lets go of it when the process that wrote it is gone.
    if (!lock_.tryLock(0)) {
        return false;
    }
    // Nothing live serves the name now, so what a Hearth that did not close left under it (a
    // socket file on Unix; a pipe goes with its process) is stale.
    QLocalServer::removeServer(name_);
    if (server_.listen(name_)) {
        return true;
    }
    lock_.unlock();
    return false;
}

void SingleInstance::on_connection() {
    while (QLocalSocket* socket = server_.nextPendingConnection()) {
        socket->setParent(this);
        const auto line = std::make_shared<Line>();
        connect(socket, &QLocalSocket::readyRead, socket, [this, socket, line] {
            if (line->done) {
                socket->readAll();
                return;
            }
            line->data.append(socket->readAll());
            if (line->data.size() > kMaxMessageBytes) {
                line->done = true;
                socket->abort();
                return;
            }
            const qsizetype end = line->data.indexOf('\n');
            if (end < 0) {
                return;
            }
            line->done = true;
            const std::optional<Message> message = parse_message(line->data.left(end));
            // Said before the news is passed on: the sender is waiting for it. The sender
            // closes the connection when it has read it (`disconnected` below deletes the
            // socket). Not closed from this end: a pipe closed by its server can drop what the
            // client has not read yet, which is the one thing it is there to deliver. A peer
            // that stays is cut off by the timer below.
            socket->write(message ? "ok\n" : "error\n");
            if (message) {
                emit received(message->files, message->page);
            }
        });
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        // A peer that connects and never finishes a line.
        QTimer::singleShot(kLineTimeoutMs, socket, [socket] { socket->abort(); });
    }
}

}  // namespace iclforge::hearth::ui
