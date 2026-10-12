#include "media_monitor.hpp"

#include <QThread>
#include <QTimer>

#include <utility>

using iclforge::crucible::ui::MediaSession;
using iclforge::crucible::ui::MediaSessions;
using iclforge::crucible::ui::MediaSessionSupport;

namespace {

// The object that lives on the reading thread: a timer, and a read each time it fires.
class Worker : public QObject {
    Q_OBJECT

public:
    Worker(std::shared_ptr<MediaSessions> reader, int interval_ms) : reader_(std::move(reader)), interval_ms_(interval_ms) {}

public slots:
    void begin() {
        timer_ = new QTimer(this);
        timer_->setInterval(interval_ms_);
        connect(timer_, &QTimer::timeout, this, &Worker::poll);
        timer_->start();
        poll();
    }

    void end() {
        if (timer_ != nullptr) {
            timer_->stop();
        }
    }

    void poll() {
        if (reader_ != nullptr) {
            emit read(reader_->read());
        }
    }

signals:
    void read(const std::vector<MediaSession>& sessions);

private:
    std::shared_ptr<MediaSessions> reader_;
    int interval_ms_;
    QTimer* timer_ = nullptr;
};

}  // namespace

MediaMonitor::MediaMonitor(std::shared_ptr<MediaSessions> reader, int intervalMs, QObject* parent)
    : QObject(parent),
      reader_(reader != nullptr ? std::move(reader) : iclforge::crucible::ui::platform_media_sessions()),
      interval_ms_(intervalMs) {
    qRegisterMetaType<std::vector<MediaSession>>("std::vector<iclforge::crucible::ui::MediaSession>");
}

MediaMonitor::~MediaMonitor() {
    setWanted(false);
}

void MediaMonitor::setWanted(bool wanted) {
    if (wanted == wanted_) {
        return;
    }
    wanted_ = wanted;
    if (wanted) {
        thread_ = new QThread(this);
        thread_->setObjectName(QStringLiteral("crucible-media-sessions"));
        auto* worker = new Worker(reader_, interval_ms_);
        worker_ = worker;
        worker->moveToThread(thread_);
        // Queued by default across threads: the list is copied into the window's thread's queue.
        connect(worker, &Worker::read, this, &MediaMonitor::sessionsRead);
        connect(thread_, &QThread::started, worker, &Worker::begin);
        connect(thread_, &QThread::finished, worker, &QObject::deleteLater);
        thread_->start();
    } else if (thread_ != nullptr) {
        // The timer is stopped on its own thread before the loop leaves it.
        QMetaObject::invokeMethod(static_cast<Worker*>(worker_), &Worker::end, Qt::BlockingQueuedConnection);
        thread_->quit();
        thread_->wait();
        delete thread_;
        thread_ = nullptr;
        worker_ = nullptr;
    }
}

MediaSessionSupport MediaMonitor::support() const {
    return reader_ != nullptr ? reader_->support() : MediaSessionSupport{false, "No media session reader."};
}

#include "media_monitor.moc"
