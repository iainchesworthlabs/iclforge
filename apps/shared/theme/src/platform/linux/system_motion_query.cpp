#include "system_motion_query.hpp"

#include "system_motion_parse.hpp"

#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

namespace iclforge::theme {

namespace {

// GNOME and what shares its schema keep this in GSettings. The command-line
// tool is asked rather than libgio linked: this is read when the window is
// activated, not per frame, and a machine without the tool (KDE on its own,
// a container) answers empty, which is "not reduced". The wait is bounded so
// a stuck settings daemon cannot hold the window's activation.
QString gnome_enable_animations() {
    QProcess process;
    process.start(QStringLiteral("gsettings"),
                  {QStringLiteral("get"), QStringLiteral("org.gnome.desktop.interface"),
                   QStringLiteral("enable-animations")});
    if (!process.waitForFinished(250) || process.exitStatus() != QProcess::NormalExit ||
        process.exitCode() != 0) {
        process.kill();
        return {};
    }
    return QString::fromUtf8(process.readAllStandardOutput());
}

// KDE keeps its animation speed in kdeglobals, a plain INI file. Read as a
// file, so no KDE library is needed and no process is started.
QString kde_animation_duration_factor() {
    const QString path =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/kdeglobals");
    const QSettings settings(path, QSettings::IniFormat);
    return settings.value(QStringLiteral("KDE/AnimationDurationFactor")).toString();
}

}  // namespace

bool query_system_reduced_motion() {
    return reduced_from_gsettings_enable_animations(gnome_enable_animations().toStdString()) ||
           reduced_from_kde_animation_duration_factor(kde_animation_duration_factor().toStdString());
}

}  // namespace iclforge::theme
