// The media sessions of Linux (apps/crucible/ui/src/media_session.hpp): MPRIS 2, which the players
// that show up in a desktop's media controls publish on the D-Bus session bus as
// `org.mpris.MediaPlayer2.<name>` (a second instance of one player adds `.instance<pid>`), object
// `/org/mpris/MediaPlayer2`, interface `org.mpris.MediaPlayer2.Player`.
//
// Read with QtDBus, on the thread read() is called from; the window never makes these calls. The
// position is `Position`, in microseconds, which MPRIS says is not announced by a property change:
// it is read here each time and carried forward at the rate between reads. The bus daemon says
// which process owns a name, so an application is matched to a session by process id too.
//
// The other half of "exactly one of two readers is compiled" is no_media_sessions.cpp, for a build
// whose Qt has no D-Bus module.

#include "media_session.hpp"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace iclforge::crucible::ui {

namespace {

constexpr auto kBusPrefix = "org.mpris.MediaPlayer2.";
constexpr auto kObjectPath = "/org/mpris/MediaPlayer2";
constexpr auto kPlayerInterface = "org.mpris.MediaPlayer2.Player";
constexpr auto kRootInterface = "org.mpris.MediaPlayer2";
constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";
// A player that does not answer is not waited for: a media key and a status line must not stall on
// one hung player.
constexpr int kCallTimeoutMs = 400;

[[nodiscard]] std::int64_t steady_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// `GetAll` of one interface of a player: its properties, or empty when it did not answer.
[[nodiscard]] QVariantMap get_all(QDBusConnection& bus, const QString& name, const char* interface) {
    QDBusMessage call = QDBusMessage::createMethodCall(name, QString::fromLatin1(kObjectPath),
                                                       QString::fromLatin1(kPropertiesInterface),
                                                       QStringLiteral("GetAll"));
    call << QString::fromLatin1(interface);
    const QDBusMessage reply = bus.call(call, QDBus::Block, kCallTimeoutMs);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        return {};
    }
    return qdbus_cast<QVariantMap>(reply.arguments().constFirst().value<QDBusArgument>());
}

// A D-Bus integer of whichever width a player used, or `fallback`.
[[nodiscard]] qlonglong as_integer(const QVariant& value, qlonglong fallback) {
    bool ok = false;
    const qlonglong out = value.toLongLong(&ok);
    return ok ? out : fallback;
}

class Mpris final : public MediaSessions {
public:
    [[nodiscard]] std::vector<MediaSession> read() override {
        std::vector<MediaSession> out;
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (!bus.isConnected() || bus.interface() == nullptr) {
            return out;
        }
        const QDBusReply<QStringList> names = bus.interface()->registeredServiceNames();
        if (!names.isValid()) {
            return out;
        }
        const std::int64_t now = steady_now_ms();
        for (const QString& name : names.value()) {
            if (!name.startsWith(QLatin1String(kBusPrefix))) {
                continue;
            }
            const QVariantMap player = get_all(bus, name, kPlayerInterface);
            if (player.isEmpty()) {
                continue;
            }
            MediaSession session;
            session.id = name.toStdString();
            session.executable = executable_of_mpris_bus_name(session.id);

            const QVariantMap root = get_all(bus, name, kRootInterface);
            session.name = root.value(QStringLiteral("Identity")).toString().toStdString();
            session.desktop_id = root.value(QStringLiteral("DesktopEntry")).toString().toStdString();
            const QDBusReply<uint> pid = bus.interface()->servicePid(name);
            session.pid = pid.isValid() ? pid.value() : 0;

            const QString status = player.value(QStringLiteral("PlaybackStatus")).toString();
            session.playback = status == QLatin1String("Playing")  ? MediaPlayback::kPlaying
                               : status == QLatin1String("Paused") ? MediaPlayback::kPaused
                                                                   : MediaPlayback::kStopped;
            const double rate = player.value(QStringLiteral("Rate"), 1.0).toDouble();
            session.rate = rate > 0.0 ? rate : 1.0;

            const QVariant metadata_value = player.value(QStringLiteral("Metadata"));
            const QVariantMap metadata = metadata_value.canConvert<QDBusArgument>()
                                             ? qdbus_cast<QVariantMap>(metadata_value.value<QDBusArgument>())
                                             : metadata_value.toMap();
            session.title = metadata.value(QStringLiteral("xesam:title")).toString().toStdString();
            session.artist = metadata.value(QStringLiteral("xesam:artist")).toStringList().join(QStringLiteral(", ")).toStdString();
            // mpris:length is in microseconds (a player may send it as a signed or unsigned integer).
            session.duration_ms = as_integer(metadata.value(QStringLiteral("mpris:length")), 0) / 1000;

            if (player.contains(QStringLiteral("Position"))) {
                session.has_position = true;
                session.position_ms = as_integer(player.value(QStringLiteral("Position")), 0) / 1000;
                session.read_at_ms = now;
            }
            out.push_back(std::move(session));
        }
        return out;
    }

    [[nodiscard]] MediaSessionSupport support() const override {
        MediaSessionSupport out;
        out.available = QDBusConnection::sessionBus().isConnected();
        if (!out.available) {
            out.reason = "There is no D-Bus session bus to read the media players from.";
        }
        return out;
    }
};

}  // namespace

std::shared_ptr<MediaSessions> platform_media_sessions() {
    return std::make_shared<Mpris>();
}

}  // namespace iclforge::crucible::ui
