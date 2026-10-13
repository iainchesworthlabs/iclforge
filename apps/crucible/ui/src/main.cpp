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
// `--size 1480x1100` opens the window at that size, for a picture of a page
// whose rails run on below the 1480x820 it opens at (the Lyrics block of the
// Room's right rail starts below the fold of the default window).
//
// A `--shot` run is hermetic where it is asked to be. It never reads or writes
// the person's own settings: it runs on a scratch store of its own, which
// `--setting group/key=value` seeds (`behaviour/showSilentApps=false`), the way
// Hearth's and the Forge GUI's captures do. And `--app Name[,idle|silent|
// background]` puts the window over a scripted machine (scripted_machine.hpp):
// the applications named, one pair of speakers and no silent device, with
// nothing opened on the computer and nothing of its own listed, which is what
// the documentation's pictures are taken over.
// A `--shot` run never shows the first-run dialog unless `--page firstrun`
// asked for it, so a capture against a fresh settings store is clean. A
// capture that could not show what it was asked to (a `--place` or `--lyrics`
// for an application that is not listed, a lyrics file that could not be read,
// a time given to a file with no times, an `--app` that is not Name or Name
// with one of the words above) takes no picture and exits with code 3, so that
// a picture is never taken of what was not asked for.

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
#include <QTemporaryDir>
#include <QIcon>
#include <QTimer>

#include "app_entry.hpp"
#include "app_icon_provider.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>
#include <vector>
#include <QUrl>

#include "language_manager.hpp"
#include "lyrics_controller.hpp"
#include "platform_services.hpp"
#include "scripted_machine.hpp"
#include "settings_migration.hpp"
#include "window_memory.hpp"

namespace {

// How long a capture waits, after the last thing it applied, before it grabs the window: the
// placement glides and the views settle. `--lyrics` starts the strip so that the song is at its
// time at the end of this wait.
constexpr int kSettleMs = 1500;
// What a capture exits with when it could not show what it was asked to.
constexpr int kCaptureNotAsAskedExit = 3;

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
    QStringList app_specs;
    QStringList setting_specs;
    int window_width = 0;
    int window_height = 0;
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
        } else if (args[i] == QLatin1String("--app")) {
            app_specs.push_back(args[i + 1]);  // Name[,idle|silent|background], on a scripted machine
        } else if (args[i] == QLatin1String("--setting")) {
            setting_specs.push_back(args[i + 1]);  // group/key=value, in the capture's scratch store
        } else if (args[i] == QLatin1String("--size")) {
            // width x height in pixels; anything else is said and left out
            const QStringList parts = args[i + 1].toLower().split(QLatin1Char('x'));
            bool width_ok = false;
            bool height_ok = false;
            const int width = parts.size() == 2 ? parts[0].toInt(&width_ok) : 0;
            const int height = parts.size() == 2 ? parts[1].toInt(&height_ok) : 0;
            if (width_ok && height_ok && width >= 320 && width <= 8192 && height >= 320 && height <= 8192) {
                window_width = width;
                window_height = height;
            } else {
                qWarning("--size: %s is not <width>x<height> in pixels, from 320 to 8192", qPrintable(args[i + 1]));
            }
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

    // A capture does not touch the person's own settings, the way Hearth's and the Forge GUI's
    // do not: CrucibleController builds its QSettings from QSettings::defaultFormat() (the QML
    // tests isolate themselves the same way), so pointing the process-wide default at an INI
    // file in a directory of its own, before anything reads, reaches it with no change to the
    // controller. The directory has to outlive the run, so it is kept in scope here.
    std::optional<QTemporaryDir> shot_settings_scratch;
    if (!shot_path.isEmpty()) {
        shot_settings_scratch.emplace();
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, shot_settings_scratch->path());
    }
    // What a person saved under the old names (organisation ac3forge, application Crucible, and
    // the desktop demo's DesktopAtmos before it) is copied to the store Crucible has now, once,
    // before anything constructs a controller (settings_migration.hpp has the rules). The four-
    // argument QSettings constructor it uses honours QSettings::setDefaultFormat(), which keeps
    // the QML tests' INI-in-a-temporary-directory isolation, and a capture's: it has just been
    // pointed at an empty directory, so there is nothing to copy.
    iclforge::settings_migration::migrate_program(iclforge::settings_migration::Program::kCrucible);

    // `--setting group/key=value`: what the capture's store holds before anything reads it. Only
    // a capture has a store of its own, so only a capture takes one; a value is text, which is
    // how the INI store holds every value.
    bool capture_options_ok = true;
    if (!setting_specs.isEmpty()) {
        if (shot_path.isEmpty()) {
            qWarning("--setting needs --shot: a run without one reads the person's own settings, "
                     "which are not changed from the command line");
        } else {
            QSettings scratch(QSettings::defaultFormat(), QSettings::UserScope,
                              QCoreApplication::organizationName(), QCoreApplication::applicationName());
            for (const QString& spec : setting_specs) {
                const auto eq = spec.indexOf(QLatin1Char('='));
                if (eq <= 0) {
                    qWarning("--setting: %s is not group/key=value", qPrintable(spec));
                    capture_options_ok = false;
                    continue;
                }
                scratch.setValue(spec.left(eq), spec.mid(eq + 1));
            }
            scratch.sync();
        }
    }

    // `--app Name[,idle|silent|background]`: the window over a scripted machine. A machine that
    // is a list has no media sessions to read either, so the reading is off, and a lyrics file
    // with no time is left to wait for a session that cannot come.
    std::vector<iclforge::crucible::ui::ScriptedApp> scripted_apps;
    for (const QString& spec : app_specs) {
        if (auto parsed = iclforge::crucible::ui::parse_scripted_app(spec.toStdString())) {
            scripted_apps.push_back(std::move(*parsed));
        } else {
            qWarning("--app: %s is not Name or Name,idle|silent|background", qPrintable(spec));
            capture_options_ok = false;
        }
    }
    if (!scripted_apps.empty()) {
        qputenv("ICLFORGE_CRUCIBLE_MEDIA_SESSIONS", QByteArrayLiteral("off"));
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
    if (!scripted_apps.empty()) {
        // Before Main.qml starts the engine: the engine is built from these seams at start().
        // The room says what its list means under the list, so the platform's own sentence is
        // what the scripted monitor answers with.
        auto* controller = engine.singletonInstance<CrucibleController*>("Crucible", "CrucibleController");
        if (controller != nullptr) {
            auto machine = iclforge::crucible::ui::make_scripted_machine(
                scripted_apps, iclforge::crucible::platform_session_monitor()->listing_rule());
            controller->set_test_services(std::move(machine.sessions), std::move(machine.devices),
                                          std::move(machine.foreground), std::move(machine.default_device),
                                          std::move(machine.virtual_device));
            qInfo("--app: %d scripted application(s) on a scripted machine", static_cast<int>(scripted_apps.size()));
        } else {
            qWarning("--app: the controller is not there to be given a machine");
            capture_options_ok = false;
        }
    }
    // The window's size, position and maximised state, kept in the store between starts
    // (apps/shared/preferences/src/window_placement.hpp has the keys and the rules). Main.qml is
    // loaded hidden so that attach() places it before it is first shown. A capture is on the
    // scratch store above, which holds nothing unless --setting put something there, and its
    // --size is applied after, so it wins over what was remembered.
    iclforge::window_memory::WindowMemory remembered_window;
    engine.setInitialProperties({{QStringLiteral("visible"), false}});
    engine.loadFromModule("Crucible", "Main");
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }
    if (auto* root_window = qobject_cast<QQuickWindow*>(engine.rootObjects().first())) {
        mark_frames_for_tracy(root_window);
        remembered_window.attach(root_window);
    } else {
        engine.rootObjects().first()->setProperty("visible", true);
    }
    // A capture never shows the first-run dialog it did not ask for:
    // Main.qml reads this one event-loop turn later, after main() has had
    // its say. `--page firstrun` opens the dialog over the Room page.
    if (!shot_path.isEmpty()) {
        engine.rootObjects().first()->setProperty("suppressFirstRun", true);
    }
    if (window_width > 0) {
        engine.rootObjects().first()->setProperty("width", window_width);
        engine.rootObjects().first()->setProperty("height", window_height);
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
    auto capture_failed = std::make_shared<bool>(!capture_options_ok);
    for (const QString& spec : lyrics_specs) {
        if (auto request = iclforge::crucible::ui::parse_capture_lyrics(spec.toStdString())) {
            lyrics_jobs->push_back(LyricsJob{std::move(*request)});
        } else {
            qWarning("--lyrics: %s is not Name=<file> or Name=<file>@<time>", qPrintable(spec));
            *capture_failed = true;
        }
    }
    auto take_shot = [&engine, shot_path, capture_failed] {
        if (shot_path.isEmpty()) {
            return;
        }
        if (*capture_failed) {
            qWarning("--shot: no picture taken, what was asked for could not be shown");
            QCoreApplication::exit(kCaptureNotAsAskedExit);
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
                                                    lyrics_jobs, capture_failed] {
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
                *capture_failed = true;
            }
            // The strip starts now, so that the song is where the option said when the window is
            // grabbed, after the wait below.
            const qlonglong until_grab = shot_path.isEmpty() ? 0 : kSettleMs;
            for (const LyricsJob& job : *lyrics_jobs) {
                const QString name = QString::fromStdString(job.request.application);
                if (!job.imported || lyrics == nullptr) {
                    qWarning("--lyrics: no application named %s after %d polls", qPrintable(name), *tries);
                    *capture_failed = true;
                } else if (lyrics->status(job.app) == QLatin1String("error")) {
                    qWarning("--lyrics: %s: %s", qPrintable(name), qPrintable(lyrics->errorText(job.app)));
                    *capture_failed = true;
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
                        *capture_failed = true;
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
