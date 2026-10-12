// The hand-over between two Hearths (apps/hearth/ui/src/single_instance.hpp): a second launch
// with files gives them to the first and the first says it has them; a name that is served is not
// taken; and what is not a hand-over opens nothing. Every case runs on a name of its own, never
// SingleInstance::default_name(): a Hearth running on the machine that runs these cases must not
// be handed their files.

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QLocalSocket>
#include <QString>
#include <QStringList>
#include <QThread>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <functional>
#include <thread>

#include "single_instance.hpp"

using iclforge::hearth::ui::SingleInstance;

namespace {

// The sockets need an event loop on the thread that listens; the test binary has no
// application of its own.
void ensure_application() {
    if (QCoreApplication::instance() != nullptr) {
        return;
    }
    static int argc = 1;
    static char name[] = "hearth_controller_tests";
    static char* argv[] = {name, nullptr};
    static QCoreApplication application(argc, argv);
}

QString unique_name() {
    static std::atomic<int> counter{0};
    return QStringLiteral("iclforge-hearth-test-%1-%2")
        .arg(QCoreApplication::applicationPid())
        .arg(counter.fetch_add(1));
}

// Spins the listener's event loop until `done` or the time is up.
bool pump_until(const std::function<bool()>& done, int timeout_ms = 8000) {
    QElapsedTimer timer;
    timer.start();
    while (!done()) {
        if (timer.elapsed() > timeout_ms) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    return true;
}

// The sender blocks until it is answered, and the listener answers from the event loop: the
// sender is another thread, as it is another process in life.
struct Sender {
    std::atomic<bool> finished{false};
    bool result = false;
    std::thread thread;

    Sender(const QString& name, const QStringList& files, const QString& page = {}) {
        thread = std::thread([this, name, files, page] {
            result = SingleInstance::hand_over(name, files, page, 4000);
            finished = true;
        });
    }
    ~Sender() {
        if (thread.joinable()) {
            thread.join();
        }
    }
};

// A raw line, for what a Hearth would never send. Returns what came back, or empty.
QByteArray send_raw(const QString& name, const QByteArray& bytes) {
    QLocalSocket socket;
    socket.connectToServer(name, QIODevice::ReadWrite);
    if (!socket.waitForConnected(4000)) {
        return {};
    }
    socket.write(bytes);
    socket.waitForBytesWritten(4000);
    // Buffered bytes first: waitForReadyRead() only sees what arrives while it waits.
    QByteArray reply;
    for (;;) {
        reply.append(socket.readAll());
        if (reply.contains('\n') || !socket.waitForReadyRead(4000)) {
            break;
        }
    }
    reply.append(socket.readAll());
    return reply;
}

struct RawSender {
    std::atomic<bool> finished{false};
    QByteArray reply;
    std::thread thread;

    RawSender(const QString& name, const QByteArray& bytes) {
        thread = std::thread([this, name, bytes] {
            reply = send_raw(name, bytes);
            finished = true;
        });
    }
    ~RawSender() {
        if (thread.joinable()) {
            thread.join();
        }
    }
};

}  // namespace

TEST_CASE("a second Hearth hands its files to the first, in order, and is told they arrived",
          "[hearth][single-instance]") {
    ensure_application();
    const QString name = unique_name();
    SingleInstance first(name);
    REQUIRE(first.listen());

    QStringList files;
    QString page;
    int arrivals = 0;
    QObject::connect(&first, &SingleInstance::received,
                     [&](const QStringList& got, const QString& got_page) {
                         files = got;
                         page = got_page;
                         ++arrivals;
                     });

    Sender sender(name, {QStringLiteral("C:/Music/a song.ac3"), QStringLiteral("/tmp/b.ec3")});
    REQUIRE(pump_until([&] { return sender.finished.load(); }));
    CHECK(sender.result);
    REQUIRE(pump_until([&] { return arrivals > 0; }));
    CHECK(arrivals == 1);
    REQUIRE(files.size() == 2);
    CHECK(files[0] == QStringLiteral("C:/Music/a song.ac3"));
    CHECK(files[1] == QStringLiteral("/tmp/b.ec3"));
    CHECK(page.isEmpty());
}

TEST_CASE("the page travels with the files when it is one of the window's pages",
          "[hearth][single-instance]") {
    ensure_application();
    const QString name = unique_name();
    SingleInstance first(name);
    REQUIRE(first.listen());

    QString page = QStringLiteral("unset");
    int arrivals = 0;
    QObject::connect(&first, &SingleInstance::received, [&](const QStringList&, const QString& got) {
        page = got;
        ++arrivals;
    });

    {
        Sender sender(name, {QStringLiteral("song.ac3")}, QStringLiteral("karaoke"));
        REQUIRE(pump_until([&] { return sender.finished.load() && arrivals == 1; }));
        CHECK(sender.result);
    }
    CHECK(page == QStringLiteral("karaoke"));

    // A dialog `--page` also takes is not a page: the files still arrive, without it.
    {
        Sender sender(name, {QStringLiteral("song.ac3")}, QStringLiteral("firstrun"));
        REQUIRE(pump_until([&] { return sender.finished.load() && arrivals == 2; }));
        CHECK(sender.result);
    }
    CHECK(page.isEmpty());
}

TEST_CASE("with nobody listening, a hand-over says so and nothing waits", "[hearth][single-instance]") {
    ensure_application();
    QElapsedTimer timer;
    timer.start();
    CHECK_FALSE(SingleInstance::hand_over(unique_name(), {QStringLiteral("song.ac3")}));
    CHECK(timer.elapsed() < 2000);
    // And no files is nothing to hand over, whoever listens.
    CHECK_FALSE(SingleInstance::hand_over(unique_name(), {}));
}

TEST_CASE("a name that is being served is not taken, and its server still answers",
          "[hearth][single-instance]") {
    ensure_application();
    const QString name = unique_name();
    SingleInstance first(name);
    REQUIRE(first.listen());
    int arrivals = 0;
    QObject::connect(&first, &SingleInstance::received,
                     [&](const QStringList&, const QString&) { ++arrivals; });

    // A second Hearth that only listens (no files to hand over) finds the name served, probes
    // it, finds it live, and leaves it alone: taking the name would leave the first unreachable.
    SingleInstance second(name);
    CHECK_FALSE(second.listen());

    Sender sender(name, {QStringLiteral("still.ac3")});
    REQUIRE(pump_until([&] { return sender.finished.load() && arrivals == 1; }));
    CHECK(sender.result);
}

TEST_CASE("a lock left by a Hearth that is gone does not keep the name", "[hearth][single-instance]") {
    ensure_application();
    const QString name = unique_name();
    // What QLockFile writes, with no host (one it takes for this machine's): the owner's
    // process id and its application. A process id that no process has is a Hearth that did not
    // close. (With a host it must be spelt as QLockFile spells it, which differs by platform:
    // upper case on Windows.)
    QFile stale(QDir(QDir::tempPath()).filePath(name + QStringLiteral(".lock")));
    REQUIRE(stale.open(QIODevice::WriteOnly));
    stale.write("2147483000\nhearth\n");
    stale.close();

    SingleInstance next(name);
    CHECK(next.listen());
}

TEST_CASE("what is not a hand-over is refused and opens nothing", "[hearth][single-instance]") {
    ensure_application();
    const QString name = unique_name();
    SingleInstance first(name);
    REQUIRE(first.listen());
    int arrivals = 0;
    QObject::connect(&first, &SingleInstance::received,
                     [&](const QStringList&, const QString&) { ++arrivals; });

    for (const char* bad : {"not json\n", "[1,2]\n", "{}\n", "{\"open\":[]}\n", "{\"open\":[\"\"]}\n",
                            "{\"open\":[3]}\n", "{\"open\":\"song.ac3\"}\n"}) {
        RawSender sender(name, QByteArray(bad));
        REQUIRE(pump_until([&] { return sender.finished.load(); }));
        INFO(bad);
        CHECK(sender.reply.startsWith("error"));
    }
    CHECK(arrivals == 0);

    // One that is refused does not stop the next from being served.
    Sender sender(name, {QStringLiteral("after.ac3")});
    REQUIRE(pump_until([&] { return sender.finished.load() && arrivals == 1; }));
    CHECK(sender.result);
}

TEST_CASE("a message past the size limit is dropped, and the server carries on",
          "[hearth][single-instance]") {
    ensure_application();
    const QString name = unique_name();
    SingleInstance first(name);
    REQUIRE(first.listen());
    int arrivals = 0;
    QObject::connect(&first, &SingleInstance::received,
                     [&](const QStringList&, const QString&) { ++arrivals; });

    // Two mebibytes and no end of line.
    RawSender flood(name, QByteArray(2 * 1024 * 1024, 'x'));
    REQUIRE(pump_until([&] { return flood.finished.load(); }));
    CHECK(arrivals == 0);

    Sender sender(name, {QStringLiteral("after.ac3")});
    REQUIRE(pump_until([&] { return sender.finished.load() && arrivals == 1; }));
    CHECK(sender.result);
}

TEST_CASE("one account's name is stable, and every account's is its own", "[hearth][single-instance]") {
    const QString name = SingleInstance::default_name();
    CHECK(name == SingleInstance::default_name());
    CHECK(name.startsWith(QStringLiteral("iclforge-hearth-")));
    // A pipe or socket name: no separators.
    CHECK_FALSE(name.contains(QLatin1Char('/')));
    CHECK_FALSE(name.contains(QLatin1Char('\\')));
    CHECK(SingleInstance::known_pages().contains(QStringLiteral("karaoke")));
}
