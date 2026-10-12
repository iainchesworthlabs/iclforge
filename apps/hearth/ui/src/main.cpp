// hearth: the desktop reference player's window
// (planning/hearth-reference-player.md, A5). Everything that is not the
// window lives in ../../engine/src and hearth_controller.hpp; this file stands the
// QML up and offers Crucible's own debugging aid: `--shot <path.png>` grabs
// the window after it has settled and quits, so a headless check (or the
// screenshot script, later) can see it; `--page <name>` picks the page it
// opens on first - play, media, speakers, decoder, network or settings, or
// opens the "Before you play anything" dialog (firstrun), the
// keyboard-shortcuts reference (shortcuts, issue #830), the About dialog
// (about) or its Licences view (licences) over the Play page, the same
// special values apps/crucible/ui/src/main.cpp's own `--page` accepts. A
// `--shot` run never shows the first-run dialog unless `--page firstrun`
// asked for it (Crucible's own main.cpp carries the identical shape for the
// identical reason), and now also runs HearthController against a scratch
// settings store instead of the real per-user one (registry key
// HKCU\Software\iclforge\Hearth on Windows) - apps/forge/gui/src/main.cpp's `--smoke`
// uses the identical recipe - so repeated captures on a shared machine
// neither inherit nor pollute anyone's real queue/device/pairing state
// (issue #884). `--open-output-picker` opens the output picker dialog
// (OutputPicker.qml) before the grab, since nothing else drives its mouse
// click headlessly. `--decoder-format ac4` switches the Decoder page's own
// AC-3/E-AC-3 vs AC-4 sub-tab (DecoderPage.qml's `format` property) before
// the grab - otherwise nothing reaches that sub-tab headlessly either
// (issue #901).
//
// `--smoke-shot <png>` is `--shot` for a run nobody is watching: the same
// capture with network discovery OFF. `--shot` browses mDNS for sinks and on
// Windows that asks for a Windows Firewall rule, a UAC prompt, so it is not
// run unattended; `--smoke-shot` opens no socket (NetworkController::
// set_network_discovery(false), the setting the Qt Quick suites use), and the
// name is the Forge GUI's own for its capture without side effects.
// `--open <file>` adds a file to the queue, once or several times, and with
// nothing in the queue the first becomes the item that is loaded and cued, not
// playing: with a capture the grab waits for the item's media information to
// be read first, so a page that shows a song (Karaoke, Play, Media) shows it.
// A file named on its own - `hearth song.ac3`, or what the shell runs for a
// double-clicked .ac3 (`hearth.exe "%1"`, cmake/Packaging.cmake) - is
// `--open <file>` written without the option, and a launch plays it: the file
// goes behind the queue the window brings back, becomes the item playing and
// starts (Engine::add_and_play()). It only waits, cued as above, on a first
// run (the "Before you play anything" dialog says where the sound goes and
// comes first), with a capture, and with `--seek`. `--` ends the options, so
// `hearth -- --page` opens a file called `--page`.
// One Hearth plays: a launch that would play files first hands them (and its
// `--page`) to the Hearth already running under the same account, which queues
// them, plays the first, comes forward and switches page, and the second
// process ends before it has a window (src/single_instance.hpp). A launch with
// no files, a `--seek` or a capture takes no part in it, apart from listening
// (a capture does not even listen).
// `--seek <ms>` starts that item at a position and holds it paused there (a
// stopped item has no position to show, and a seek on it is kept for its next
// start), so a lyric view shows a word part way through; that is a moment of
// sound at the output the item goes to. A file that does not load, or reach
// its position, within fifteen seconds leaves the capture's exit code at 3,
// with the picture of whatever the window said, so a script that regenerates
// the documentation fails rather than keeping a wrong picture.
// `--page karaoke` opens the Karaoke page. src/command_line.hpp reads all of
// these, and test_command_line.cpp holds it to what is said here.
//
// Translations run through the family's own LanguageManager
// (apps/shared/preferences/src/language_manager.cpp, shared rather than copied), pointed at
// this app's own hearth_<code>.qm catalogues under :/i18n/. The six
// languages are the same set forge-gui and Crucible ship. The catalogues carry
// every source string, each translated or not: a string with no translation
// shows in English, and a language change always switches the layout
// direction and the typeface.

#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

#include "iclforge/base/detail/profiling.hpp"

#include <memory>
#include <optional>

#include "iclforge/sendspin/firewall.hpp"
#include "command_line.hpp"
#include "hearth_controller.hpp"
#include "language_manager.hpp"
#include "native_log_sink.hpp"
#include "network_controller.hpp"
#include "settings_migration.hpp"
#include "single_instance.hpp"

namespace {

// How long a capture waits for an --open file's media information before it
// grabs what there is and says so in its exit code.
constexpr int kOpenWaitMs = 15000;
constexpr int kOpenPollMs = 50;
// After the item is loaded (and cued at its --seek position): the lyric view
// reads its file one event-loop turn later and the page lays out once more.
constexpr int kOpenSettleMs = 500;
constexpr int kExitItemNotLoaded = 3;

bool save_window(QQmlApplicationEngine& engine, const QString& path) {
    if (engine.rootObjects().isEmpty()) {
        return false;
    }
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    if (window == nullptr) {
        return false;
    }
    const QImage shot = window->grabWindow();
    return !shot.isNull() && shot.save(path);
}

// Ties Tracy's frame view to real Qt Quick presentation instead of leaving it
// empty - see apps/crucible/ui/src/main.cpp's identical helper for why this is a
// NAMED ("UI") frame mark rather than the bare ICLFORGE_FRAME_MARK(), and why the
// connection is direct rather than queued.
void mark_frames_for_tracy(QQuickWindow* window) {
    QObject::connect(window, &QQuickWindow::frameSwapped, window,
                     [] { ICLFORGE_FRAME_MARK_NAMED("UI"); }, Qt::DirectConnection);
}

}  // namespace

int main(int argc, char** argv) {
    // Elevated relaunch for a Windows Firewall rule NetworkController's mDNS browsing is about
    // to need (ac3/sendspin/firewall.hpp): std::exit()s before touching Qt when argv says this
    // is that relaunch, so an ordinary launch is the only one that reaches the window below.
    iclforge::sendspin::firewall::maybe_run_as_firewall_helper_and_exit(argc, argv);

    // Once, for the process's whole life - never from HearthController's
    // constructor, so iclforge-hearth-tests and the Qt Quick test binary (each their own
    // main(), never this one) don't register it (native_log_sink.hpp's own
    // comment says why that matters).
    iclforge::hearth::install_native_log_sink();

    // Render on the GUI thread, as Crucible's window does and for the same
    // reason: the threaded loop's render thread paints a frame behind a
    // window drag on Windows.
    if (qEnvironmentVariableIsEmpty("QSG_RENDER_LOOP")) {
        qputenv("QSG_RENDER_LOOP", QByteArrayLiteral("basic"));
    }
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("Hearth"));
    QGuiApplication::setOrganizationName(QStringLiteral("iclforge"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Hearth"));

    QIcon app_icon;
    app_icon.addFile(QStringLiteral(":/icons/iclforge-32.png"));
    app_icon.addFile(QStringLiteral(":/icons/iclforge-256.png"));
    QGuiApplication::setWindowIcon(app_icon);

    // The family's own faces (apps/shared/theme/assets/fonts), registered before the engine
    // loads so the Theme's font probe finds them.
    for (const auto* face : {":/fonts/Archivo-Regular.ttf", ":/fonts/Archivo-Medium.ttf",
                             ":/fonts/Archivo-SemiBold.ttf", ":/fonts/Archivo-ExtraBold.ttf",
                             ":/fonts/MaterialSymbolsSharp-Regular.ttf",
                             ":/fonts/NotoSansArabic.ttf",
                             ":/fonts/NotoSansHebrew.ttf"}) {
        if (QFontDatabase::addApplicationFont(QLatin1String(face)) < 0) {
            qWarning("could not register bundled font %s", face);
        }
    }
    QFont default_font = QGuiApplication::font();
    default_font.setFamily(QStringLiteral("Archivo"));
    QGuiApplication::setFont(default_font);
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    // What the command line asks for (src/command_line.hpp, which the header
    // comment above is the usage of).
    const iclforge::hearth::ui::CommandLine command_line =
        iclforge::hearth::ui::parse_command_line(QCoreApplication::arguments());
    for (const QString& warning : command_line.warnings) {
        qWarning("hearth: %s", qUtf8Printable(warning));
    }
    const QString& shot_path = command_line.shot_path;
    const QString& page = command_line.page;
    const QString& decoder_format = command_line.decoder_format;
    const bool open_output_picker = command_line.open_output_picker;

    // --smoke-shot: no mDNS socket, so no firewall exception is asked for and
    // no sink on whatever network this is gets listed in the picture. Before
    // the engine below, whose Main.qml starts NetworkController.
    if (command_line.smoke) {
        iclforge::hearth::ui::NetworkController::set_network_discovery(false);
    }

    // One Hearth to play (src/single_instance.hpp). A launch that would play files first
    // offers them to the Hearth that is already running under this account, and is done when
    // that one takes them - before any window or engine of its own exists. Every other launch
    // listens for the next one, except a capture, which runs on a scratch store of its own and
    // is neither handed anything nor the one a later double-click is handed to. A listen that
    // fails (a Hearth is running and was not asked, or the system said no) runs on as it
    // always did. Absolute paths: the sender's working directory is not the receiver's, and the
    // queue keeps them.
    QStringList launch_files;
    launch_files.reserve(command_line.open_files.size());
    for (const QString& file : command_line.open_files) {
        launch_files.push_back(QDir::cleanPath(QFileInfo(file).absoluteFilePath()));
    }
    iclforge::hearth::ui::SingleInstance single_instance(
        iclforge::hearth::ui::SingleInstance::default_name());
    if (command_line.plays_at_launch() &&
        iclforge::hearth::ui::SingleInstance::hand_over(
            iclforge::hearth::ui::SingleInstance::default_name(), launch_files, page)) {
        return 0;
    }
    if (shot_path.isEmpty() && !single_instance.listen()) {
        qInfo("hearth: another Hearth already takes what is handed over; this one runs on its own");
    }

    // A --shot capture must not touch the real, persistent per-user settings
    // store (issue #884): HearthController builds settings_ from
    // QSettings::defaultFormat() rather than a fixed native format
    // (hearth_controller.hpp's own comment explains why), so overriding the
    // process-wide default here - before the engine, and so
    // HearthController's QML singleton, exists - reaches it with no change
    // to HearthController itself, the same recipe apps/forge/gui/src/main.cpp's
    // --smoke already uses. shot_settings_scratch has to outlive the run, so
    // it is kept in scope here rather than left as a temporary whose
    // directory would vanish immediately.
    std::optional<QTemporaryDir> shot_settings_scratch;
    if (!shot_path.isEmpty()) {
        shot_settings_scratch.emplace();
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, shot_settings_scratch->path());
    }

    // The paired sinks, the pairing keys and the server identity, the playback and network
    // settings: what a person saved under the old names (organisation ac3forge, application
    // Hearth) is copied to the store Hearth has now, once, before anything reads it. A capture
    // has just pointed the store at an empty scratch directory, so it finds nothing to copy.
    iclforge::settings_migration::migrate_program(iclforge::settings_migration::Program::kHearth);

    QQmlApplicationEngine engine;
    // The family's own language manager, pointed at this app's catalogues:
    // the system locale by default, a saved override once the person has
    // chosen one (docs/forge/gui/localisation.md). Constructed and applied
    // BEFORE the QML loads, so the first frame is already translated and
    // already mirrored where the language is written right to left.
    LanguageManager language_manager(app, engine, QStringLiteral("hearth"));
    language_manager.applyInitialLanguage();
    // A singleton instance rather than a context property, and under its own
    // URI rather than this module's: registering a type into Hearth
    // by hand marks that module registered, and its own types
    // (HearthController, NetworkController) then never register at load.
    // apps/crucible/ui/src/main.cpp carries the identical comment for the
    // identical reason.
    qmlRegisterSingletonInstance("HearthLanguage", 1, 0, "LanguageManager",
                                 &language_manager);

    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("Hearth", "Main");
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }
    if (auto* root_window = qobject_cast<QQuickWindow*>(engine.rootObjects().first())) {
        mark_frames_for_tracy(root_window);
    }
    // A capture never shows the first-run dialog it did not ask for:
    // Main.qml reads this one event-loop turn later, after main() has had
    // its say. `--page firstrun`/`shortcuts`/`about`/`licences` open their
    // dialog over the Play page, for a capture.
    if (!shot_path.isEmpty()) {
        engine.rootObjects().first()->setProperty("suppressFirstRun", true);
    }
    if (page == QLatin1String("firstrun")) {
        QMetaObject::invokeMethod(engine.rootObjects().first(), "openFirstRun");
    } else if (page == QLatin1String("shortcuts")) {
        engine.rootObjects().first()->setProperty("page", QStringLiteral("play"));
        QMetaObject::invokeMethod(engine.rootObjects().first(), "openShortcuts");
    } else if (page == QLatin1String("about")) {
        engine.rootObjects().first()->setProperty("page", QStringLiteral("play"));
        QMetaObject::invokeMethod(engine.rootObjects().first(), "openAbout");
    } else if (page == QLatin1String("licences")) {
        engine.rootObjects().first()->setProperty("page", QStringLiteral("play"));
        QMetaObject::invokeMethod(engine.rootObjects().first(), "openLicences");
    } else if (!page.isEmpty()) {
        engine.rootObjects().first()->setProperty("page", page);
    }
    if (!decoder_format.isEmpty()) {
        engine.rootObjects().first()->setProperty("decoderFormat", decoder_format);
    }
    if (open_output_picker) {
        QMetaObject::invokeMethod(engine.rootObjects().first(), "openOutputPicker");
    }

    // What another launch hands over (single_instance above) is what a launch with
    // those files is - behind the queue, the first becomes the item playing and
    // starts, unless this is the first run - and the window comes forward, on the
    // page the sender asked for if it asked for one. (Windows may only flash the
    // taskbar button when another program's window is in front.)
    if (shot_path.isEmpty()) {
        auto* const receiver =
            engine.singletonInstance<iclforge::hearth::ui::HearthController*>("Hearth",
                                                                              "HearthController");
        QObject::connect(
            &single_instance, &iclforge::hearth::ui::SingleInstance::received, &app,
            [&engine, receiver](const QStringList& files, const QString& requested_page) {
                if (receiver != nullptr) {
                    if (receiver->firstRunSeen()) {
                        receiver->openAndPlay(files);
                    } else {
                        receiver->addFiles(files);
                    }
                }
                QObject* const root =
                    engine.rootObjects().isEmpty() ? nullptr : engine.rootObjects().first();
                if (root != nullptr && !requested_page.isEmpty()) {
                    root->setProperty("page", requested_page);
                }
                if (auto* window = qobject_cast<QQuickWindow*>(root)) {
                    if ((window->windowStates() & Qt::WindowMinimized) != 0) {
                        window->showNormal();
                    }
                    window->raise();
                    window->requestActivate();
                }
            });
    }

    // --open and a file on its own: the files join the queue behind whatever is
    // in it, and with an empty queue (a capture's scratch store always is) the
    // first becomes the current item, loaded and cued, which is what a page
    // that shows a song reads. A launch plays the first instead
    // (CommandLine::plays_at_launch(), which a capture and --seek are not),
    // unless this is the first run: the dialog that says where the sound goes
    // is open over the window, and the file waits cued behind it. Absolute
    // paths: the queue keeps them, and the lyrics file is found beside the
    // item.
    iclforge::hearth::ui::HearthController* const controller =
        command_line.open_files.isEmpty()
            ? nullptr
            : engine.singletonInstance<iclforge::hearth::ui::HearthController*>(
                  "Hearth", "HearthController");
    QString first_path;
    bool plays = false;
    if (controller != nullptr) {
        const QStringList& paths = launch_files;
        first_path = paths.first();
        plays = command_line.plays_at_launch() && controller->firstRunSeen();
        if (plays) {
            controller->openAndPlay(paths);
        } else {
            controller->addFiles(paths);
        }
    }

    // Nothing more to wait for: no file, or a file that is already playing
    // (which is never a capture, so there is no picture to take either).
    if (controller == nullptr || plays) {
        if (!shot_path.isEmpty()) {
            // Let the page switch and the layout settle before the grab.
            QTimer::singleShot(500, &app, [&engine, shot_path] {
                const int code = save_window(engine, shot_path) ? 0 : 2;
                QCoreApplication::exit(code);
            });
        }
        return QGuiApplication::exec();
    }

    // The item's media information is read off the GUI thread and arrives
    // with a poll (HearthController::currentMedia), so everything that depends
    // on the item being there waits for it. A stopped item reports no
    // position, and a seek on it is kept for its next start, so --seek starts
    // the item at that position and holds it paused there (a moment of sound
    // at the output it plays to). A step that does not come within the time
    // allowed ends the sequence as not done: a capture is taken all the same,
    // for what the window says, and the exit code says it did not load.
    enum class Step { kLoading, kStarting, kHolding, kDone };
    struct Cue {
        Step step = Step::kLoading;
        int waited = 0;
    };
    auto cue = std::make_shared<Cue>();
    auto* wait = new QTimer(&app);
    wait->setInterval(kOpenPollMs);
    QObject::connect(
        wait, &QTimer::timeout, &app,
        [&app, &engine, controller, wait, cue, first_path, shot_path, seek_ms = command_line.seek_ms] {
            cue->waited += kOpenPollMs;
            const bool timed_out = cue->waited >= kOpenWaitMs;
            bool done = false;
            switch (cue->step) {
                case Step::kLoading: {
                    // Read, and a stream: a path with no codec is a file that was not one (or
                    // was not there), which the Media page explains and a capture is not for.
                    const QVariantMap media = controller->currentMedia();
                    if (media.value(QStringLiteral("path")).toString() == first_path &&
                        media.contains(QStringLiteral("codec"))) {
                        if (seek_ms.has_value()) {
                            controller->seek(*seek_ms);
                            controller->playItem(controller->currentIndex());
                            cue->step = Step::kStarting;
                        } else {
                            done = true;
                        }
                    }
                    break;
                }
                case Step::kStarting:
                    if (controller->playing() && controller->positionMs() >= *seek_ms) {
                        controller->pause();
                        cue->step = Step::kHolding;
                    }
                    break;
                case Step::kHolding:
                    done = controller->state() == QLatin1String("paused");
                    break;
                case Step::kDone:
                    break;
            }
            if (!done && !timed_out) {
                return;
            }
            wait->stop();
            cue->step = Step::kDone;
            if (shot_path.isEmpty()) {
                return;
            }
            QTimer::singleShot(kOpenSettleMs, &app, [&engine, shot_path, done] {
                const bool saved = save_window(engine, shot_path);
                QCoreApplication::exit(!saved ? 2 : (done ? 0 : kExitItemNotLoaded));
            });
        });
    wait->start();

    return QGuiApplication::exec();
}
