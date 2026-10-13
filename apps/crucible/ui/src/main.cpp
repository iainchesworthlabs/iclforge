// crucible: the Crucible's window (docs/platforms/windows-demo.md,
// "UI"). Everything that is not the window lives in ../../engine/src; this file
// only stands the QML up, applies the language, and offers one debugging
// aid: `--shot <path.png>` grabs the window after it has settled and quits,
// the way forge-gui's smoke modes do, so a headless check can see the screen;
// `--page settings` (or output, room, room3d; about, licences or firstrun for
// that dialog over the room) picks the page it shows first,
// `--place Name=x,y,z` positions a listed application before the capture,
// `--lyrics Name=<file>[@<time>]` selects that application, imports the
// lyrics file against it and, with a time (`5.75` seconds, or `0:05.75` as an
// .lrc writes it), starts the lyric strip by hand so that the song is at that
// time when the window is grabbed, which is what a picture of the line being
// sung needs (without a time the strip follows the application's media
// session, whatever that says), and `--language ar` runs the window in one of
// the languages it ships so a capture can show that language and its layout.
// A `--shot` run never shows the first-run dialog unless `--page firstrun`
// asked for it, so a capture against a fresh settings store is clean. A
// capture whose `--lyrics` could not be shown (no such application, a file
// that could not be read, a time given to a file with no times) takes no
// picture and exits with code 3, so that a picture is never taken of what was
// not asked for.

#include <QFont>
#include <QFontDatabase>
#include <QApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QMessageLogContext>
#include <QVariant>
#include <QVariantMap>

#include "iclforge/base/detail/profiling.hpp"

#include "capture_lyrics.hpp"
#include "crucible_controller.hpp"
#include "diagnostics.hpp"
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QtQml>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QString>
#include <QIcon>
#include <QTimer>

#include "app_entry.hpp"
#include "app_icon_provider.hpp"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>
#include <QUrl>

#include "language_manager.hpp"
#include "lyrics_controller.hpp"
#include "settings_migration.hpp"

namespace {

// How long a capture waits, after the last thing it applied, before it grabs the window: the
// placement glides and the views settle. `--lyrics` starts the strip so that the song is at its
// time at the end of this wait.
constexpr int kSettleMs = 1500;
// What a capture exits with when it was asked for lyrics it could not show.
constexpr int kLyricsNotShownExit = 3;

// Qt's own messages - this file's --shot and --place lines, a QML warning,
// a font that failed to register - into the diagnostics ring
// (engine/diagnostics.hpp), so a saved report carries them: on a
// WIN32_EXECUTABLE they otherwise reach only an attached debugger. Debug
// output is left out; it is Qt's own chatter and the ring holds 512 lines.
// The handler runs on whatever thread emitted the message, possibly during
// teardown, so it converts the string, hands it to note() (which takes the
// ring's lock and returns) and chains to the handler that was there before;
// nothing in it may itself log, since that would recurse.
QtMessageHandler g_previous_handler = nullptr;

void forward_to_diagnostics(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    if (type != QtDebugMsg) {
        const char* level = type == QtInfoMsg       ? "info: "
                            : type == QtWarningMsg  ? "warning: "
                            : type == QtCriticalMsg ? "critical: "
                                                    : "fatal: ";
        iclforge::crucible::process_diagnostics().note(level + message.toStdString());
    }
    if (g_previous_handler != nullptr) {
        g_previous_handler(type, context, message);
    }
}

// Ties Tracy's frame view to real Qt Quick presentation - "UI", not the bare
// ICLFORGE_FRAME_MARK(), because the engine loop (engine.cpp) already marks the
// default frame set once per captured/encoded audio frame; two unrelated
// cadences on one frame set would interleave into a single meaningless graph.
// Direct, not queued: frameSwapped can fire on the scene graph's own render
// thread (the default "threaded" loop, though this window forces "basic"
// below), and the mark should timestamp the swap itself, not whenever the
// window's own GUI thread later gets around to a queued call - Tracy's
// FrameMark is documented safe to call from whichever thread produces the
// frame, matching how the engine loop already calls it off the GUI thread.
void mark_frames_for_tracy(QQuickWindow* window) {
    QObject::connect(window, &QQuickWindow::frameSwapped, window,
                     [] { ICLFORGE_FRAME_MARK_NAMED("UI"); }, Qt::DirectConnection);
}

}  // namespace

int main(int argc, char** argv) {
    // Render on the GUI thread. With the threaded loop the window's frames
    // are produced on a render thread that paints while Windows moves the
    // window, so a drag translates the frame a step behind the cursor and
    // the motion judders. The basic loop renders in step with the event
    // loop, which Windows' move loop drives, so the window follows the
    // cursor smoothly. A person can still choose another loop with the
    // environment variable.
    if (qEnvironmentVariableIsEmpty("QSG_RENDER_LOOP")) {
        qputenv("QSG_RENDER_LOOP", QByteArrayLiteral("basic"));
    }
    // QApplication, not QGuiApplication: the tray icon (Qt.labs.platform,
    // Main.qml) is QSystemTrayIcon underneath, a Widgets class. Windows
    // tolerated the narrower application object because its native menu
    // path needs no widgets; Linux refused it and the tray was absent.
    QApplication app(argc, argv);
    // The ring exists from here, so its clock starts with the window and
    // every later message lands in it.
    iclforge::crucible::process_diagnostics().note("crucible started");
    g_previous_handler = qInstallMessageHandler(forward_to_diagnostics);
    QGuiApplication::setApplicationName(QStringLiteral("Crucible"));
    QGuiApplication::setOrganizationName(QStringLiteral("iclforge"));
    // What a person saved under the old names (organisation ac3forge, application Crucible, and
    // the desktop demo's DesktopAtmos before it) is copied to the store Crucible has now, once,
    // before anything constructs a controller (settings_migration.hpp has the rules). The four-
    // argument QSettings constructor it uses honours QSettings::setDefaultFormat(), which keeps
    // the QML tests' INI-in-a-temporary-directory isolation.
    iclforge::settings_migration::migrate_program(iclforge::settings_migration::Program::kCrucible);
    // The window and taskbar icon; the .exe's own icon comes from the
    // resource script CMake generates.
    QIcon app_icon;
    app_icon.addFile(QStringLiteral(":/icons/iclforge-32.png"));
    app_icon.addFile(QStringLiteral(":/icons/iclforge-256.png"));
    QGuiApplication::setWindowIcon(app_icon);
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Crucible"));
    // The GUI app's faces (Archivo, and Noto Sans for the scripts it does
    // not cover), registered before the engine loads so the Theme's
    // font probe finds them, Archivo made the application default so every
    // control uses it; the language manager swaps the family for Arabic,
    // Hebrew and Yiddish.
    for (const auto* face : {":/fonts/Archivo-Regular.ttf", ":/fonts/Archivo-Medium.ttf",
                             ":/fonts/Archivo-SemiBold.ttf", ":/fonts/Archivo-ExtraBold.ttf",
                             ":/fonts/NotoSansArabic.ttf", ":/fonts/NotoSansHebrew.ttf"}) {
        if (QFontDatabase::addApplicationFont(QLatin1String(face)) < 0) {
            qWarning("could not register bundled font %s", face);
        }
    }
    QFont default_font = QGuiApplication::font();
    default_font.setFamily(QStringLiteral("Archivo"));
    QGuiApplication::setFont(default_font);
    // Every control is drawn by the QML in this module, on the Theme's
    // tokens; the Basic style is the one that gets out of the way.
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    // The tray keeps the engine alive after the window closes, so the app
    // must not quit with its last window.
    QGuiApplication::setQuitOnLastWindowClosed(false);

    QString shot_path;
    QString page;
    QString language;
    QStringList placements;
    QStringList lyrics_specs;
    const QStringList args = QCoreApplication::arguments();
    for (qsizetype i = 1; i + 1 < args.size(); ++i) {
        if (args[i] == QLatin1String("--shot")) {
            shot_path = args[i + 1];
        } else if (args[i] == QLatin1String("--page")) {
            page = args[i + 1];  // room, output or settings
        } else if (args[i] == QLatin1String("--place")) {
            placements.push_back(args[i + 1]);  // name=x,y,z, applied before the shot
        } else if (args[i] == QLatin1String("--lyrics")) {
            lyrics_specs.push_back(args[i + 1]);  // name=file[@time], imported before the shot
        } else if (args[i] == QLatin1String("--language")) {
            language = args[i + 1];  // en, fr, de, es, ar, he, yi
        }
    }
    // A capture in one language, with the person's own settings left
    // alone: ICLFORGE_GUI_LOCALE is the override LanguageManager already reads
    // ahead of the saved key (apps/shared/preferences/src/language_manager.cpp), so setting it
    // here shows the window in that language without writing language/code
    // to the store the way setLanguage() would. An unsupported code is
    // ignored by the manager and the run falls back to the saved language.
    if (!language.isEmpty()) {
        qputenv("ICLFORGE_GUI_LOCALE", language.toUtf8());
    }

    QQmlApplicationEngine engine;
    // The GUI's own language manager, pointed at this app's translation
    // files: system locale by default, a saved override when the user chose
    // one (docs/forge/gui/localisation.md).
    LanguageManager language_manager(app, engine, QStringLiteral("crucible"));
    language_manager.applyInitialLanguage();
    // A singleton instance rather than a context property: QML compiled
    // ahead of time resolves a registered type, where an unqualified
    // context name came back null at first evaluation. Under its own URI,
    // not the module's: registering a type into Crucible by hand marks
    // that module as registered and the module's own types (CrucibleController)
    // then never get registered at load.
    qmlRegisterSingletonInstance("CrucibleLanguage", 1, 0, "LanguageManager",
                                 &language_manager);

    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.addImageProvider(QStringLiteral("appicon"), new iclforge::crucible::ui::AppIconProvider);
    engine.loadFromModule("Crucible", "Main");
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }
    if (auto* root_window = qobject_cast<QQuickWindow*>(engine.rootObjects().first())) {
        mark_frames_for_tracy(root_window);
    }
    // A capture never shows the first-run dialog it did not ask for:
    // Main.qml reads this one event-loop turn later, after main() has had
    // its say. `--page firstrun` opens the dialog over the Room page.
    if (!shot_path.isEmpty()) {
        engine.rootObjects().first()->setProperty("suppressFirstRun", true);
    }
    if (page == QLatin1String("firstrun")) {
        engine.rootObjects().first()->setProperty("page", QStringLiteral("room"));
        QMetaObject::invokeMethod(engine.rootObjects().first(), "openFirstRun");
        page.clear();
    }
    if (page == QLatin1String("room3d")) {  // the room with its 3D view up
        engine.rootObjects().first()->setProperty("roomThreeD", true);
        page = QStringLiteral("room");
    }
    // `--page about` opens the About box over the Room page, for a capture;
    // `--page licences` opens the notices view the same way.
    if (page == QLatin1String("about")) {
        engine.rootObjects().first()->setProperty("page", QStringLiteral("room"));
        QMetaObject::invokeMethod(engine.rootObjects().first(), "openAbout");
    } else if (page == QLatin1String("licences")) {
        engine.rootObjects().first()->setProperty("page", QStringLiteral("room"));
        QMetaObject::invokeMethod(engine.rootObjects().first(), "openLicences");
    } else if (!page.isEmpty()) {
        engine.rootObjects().first()->setProperty("page", page);
    }

    // `--place Name=x,y,z`, once the engine has listed the sessions: the
    // application whose display name matches is positioned there, so a
    // capture can show a populated room without a hand on the mouse. The
    // list arrives when the engine's first refresh does, which is not at a
    // fixed time, so this polls until every named application has been
    // placed (or gives up after six seconds) rather than guessing once.
    //
    // `--lyrics Name=<file>[@<time>]` is found the same way: the application
    // is selected in the room (Main.qml's selectApplication, which the lyric
    // strip follows) and the file imported against it. The strip is started
    // only when the polling has ended, so that the song is at the time at the
    // grab and not at the moment the file was read.
    auto* poll = new QTimer(&app);
    auto pending = std::make_shared<QStringList>(placements);
    auto tries = std::make_shared<int>(0);
    struct LyricsJob {
        iclforge::crucible::ui::CaptureLyrics request;
        int app = -1;  // the application's id, once it is listed
        bool imported = false;
    };
    auto lyrics_jobs = std::make_shared<std::vector<LyricsJob>>();
    // Set when a request could not be shown: the grab is then not taken, and the exit code says so.
    auto lyrics_failed = std::make_shared<bool>(false);
    for (const QString& spec : lyrics_specs) {
        if (auto request = iclforge::crucible::ui::parse_capture_lyrics(spec.toStdString())) {
            lyrics_jobs->push_back(LyricsJob{std::move(*request)});
        } else {
            qWarning("--lyrics: %s is not Name=<file> or Name=<file>@<time>", qPrintable(spec));
            *lyrics_failed = true;
        }
    }
    auto take_shot = [&engine, shot_path, lyrics_failed] {
        if (shot_path.isEmpty()) {
            return;
        }
        if (*lyrics_failed) {
            qWarning("--shot: no picture taken, the lyrics asked for could not be shown");
            QCoreApplication::exit(kLyricsNotShownExit);
            return;
        }
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        int code = 1;
        if (window != nullptr) {
            qInfo("--shot: page %s", qPrintable(window->property("page").toString()));
            if (auto* loader = window->findChild<QObject*>(QStringLiteral("room3dLoader"))) {
                if (auto* room = loader->property("item").value<QObject*>()) {
                    QMetaObject::invokeMethod(room, "debugPick");
                }
            }
            code = window->grabWindow().save(shot_path) ? 0 : 2;
        }
        QCoreApplication::exit(code);
    };
    QObject::connect(poll, &QTimer::timeout, &app, [&engine, &app, poll, pending, tries, take_shot, shot_path,
                                                    lyrics_jobs, lyrics_failed] {
        auto* controller = engine.singletonInstance<CrucibleController*>("Crucible", "CrucibleController");
        auto* lyrics = lyrics_jobs->empty() ? nullptr
                                            : engine.singletonInstance<CrucibleLyrics*>("Crucible", "CrucibleLyrics");
        if (controller != nullptr) {
            for (auto it = pending->begin(); it != pending->end();) {
                const QString& spec = *it;
                const auto eq = spec.indexOf(QLatin1Char('='));
                // A trailing ",split" asks for the pair.
                const bool split = spec.endsWith(QLatin1String(",split"), Qt::CaseInsensitive);
                QString coords = spec.mid(eq + 1);
                if (split) {
                    coords.chop(6);
                }
                const QStringList xyz = coords.split(QLatin1Char(','));
                bool done = eq < 0 || xyz.size() != 3;  // malformed: drop it
                if (!done) {
                    for (QObject* object : controller->apps()) {
                        auto* entry = qobject_cast<iclforge::crucible::ui::AppEntry*>(object);
                        if (entry != nullptr && entry->name().compare(spec.left(eq), Qt::CaseInsensitive) == 0) {
                            const int app = entry->app();
                            if (split) {
                                controller->setSplit(app, true);
                            }
                            controller->position(app, xyz[0].toDouble(), xyz[1].toDouble(), xyz[2].toDouble());
                            qInfo("--place: %s -> app %d at (%s)", qPrintable(spec.left(eq)), app, qPrintable(coords));
                            done = true;
                        }
                    }
                }
                it = done ? pending->erase(it) : it + 1;
            }
        }
        if (controller != nullptr && lyrics != nullptr) {
            for (LyricsJob& job : *lyrics_jobs) {
                if (job.imported) {
                    continue;
                }
                const QString wanted = QString::fromStdString(job.request.application);
                for (QObject* object : controller->apps()) {
                    auto* entry = qobject_cast<iclforge::crucible::ui::AppEntry*>(object);
                    if (entry == nullptr || entry->name().compare(wanted, Qt::CaseInsensitive) != 0) {
                        continue;
                    }
                    job.app = entry->app();
                    const QVariant selected(job.app);
                    QMetaObject::invokeMethod(engine.rootObjects().first(), "selectApplication",
                                              Q_ARG(QVariant, selected));
                    lyrics->importFile(job.app, QString::fromStdString(job.request.file));
                    job.imported = true;
                    break;
                }
            }
        }
        ++*tries;
        const bool lyrics_found = std::all_of(lyrics_jobs->begin(), lyrics_jobs->end(),
                                              [](const LyricsJob& job) { return job.imported; });
        if ((pending->isEmpty() && lyrics_found) || *tries >= 24) {
            for (const QString& spec : *pending) {
                qWarning("--place: no application named %s after %d polls", qPrintable(spec.left(spec.indexOf(QLatin1Char('=')))), *tries);
            }
            // The strip starts now, so that the song is where the option said when the window is
            // grabbed, after the wait below.
            const qlonglong until_grab = shot_path.isEmpty() ? 0 : kSettleMs;
            for (const LyricsJob& job : *lyrics_jobs) {
                const QString name = QString::fromStdString(job.request.application);
                if (!job.imported || lyrics == nullptr) {
                    qWarning("--lyrics: no application named %s after %d polls", qPrintable(name), *tries);
                    *lyrics_failed = true;
                } else if (lyrics->status(job.app) == QLatin1String("error")) {
                    qWarning("--lyrics: %s: %s", qPrintable(name), qPrintable(lyrics->errorText(job.app)));
                    *lyrics_failed = true;
                } else if (!job.request.at_ms) {
                    qInfo("--lyrics: %s -> app %d, %s, left to its media session", qPrintable(name), job.app,
                          qPrintable(lyrics->fileName(job.app)));
                } else {
                    lyrics->startAt(job.app, iclforge::crucible::ui::capture_start_ms(
                                                 lyrics->nowMs(), *job.request.at_ms, until_grab));
                    if (lyrics->running(job.app)) {
                        qInfo("--lyrics: %s -> app %d, %s, the song at %lld ms when the picture is taken",
                              qPrintable(name), job.app, qPrintable(lyrics->fileName(job.app)),
                              static_cast<long long>(*job.request.at_ms));
                    } else {
                        qWarning("--lyrics: %s: %s has no times to start from, so no line is being sung",
                                 qPrintable(name), qPrintable(lyrics->fileName(job.app)));
                        *lyrics_failed = true;
                    }
                }
            }
            poll->stop();
            // Let the placement glide and the views settle before the grab.
            QTimer::singleShot(shot_path.isEmpty() ? 0 : kSettleMs, &app, take_shot);
        }
    });
    if (!placements.isEmpty() || !lyrics_specs.isEmpty() || !shot_path.isEmpty()) {
        poll->start(250);
    }

    return QGuiApplication::exec();
}
