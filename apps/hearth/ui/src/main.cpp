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
// `--page karaoke` opens the Karaoke page, and two more names open what a
// person reaches from it: `--page singer` the singer view (F11), which a
// capture draws in the window as it is and not on the whole screen, so that the
// picture is not the size of whatever monitor it was taken on, and
// `--page timing` the window that times the lyrics, once the item is loaded.
// `--setting <key>=<value>` puts a value in a capture's scratch settings store
// before the window reads it (`microphone/takesFolder=<folder>`,
// `appearance/textScale=150`): the store starts empty, and this is how a capture shows
// what a person set once. It is dropped, with a line in the log, when there is
// no capture, so it never writes to a person's own settings.
// `--size <width>x<height>` opens the window at that size, for a capture of a
// page whose cards run on below the size it opens at.
// `--sing <file.wav>` is a capture of a PERFORMANCE: the microphone is that
// file, sung along to the first `--open` item, which plays from its start to
// the `--seek` position with the Microphone card on, and the picture is taken
// as it reaches it - the card's pitch ribbon with the singer's line over the
// tune, the mark under each line sung. `--finish-singing` pauses there and
// ends the performance first, as a person's press on "Turn off" does, so that
// the card shows its summary ("Last performance"). Both need `--page karaoke`
// or `--page singer`, the pages that have the card, and are dropped with a
// line in the log where they cannot run (src/command_line.hpp says when).
// This opens NO microphone and no sound output, so MI2 (only a person's press
// opens a microphone; no test or capture opens a real one) is not bent: the file
// is a MicrophoneInput of its own (VoiceFileInput, sung along to the item as
// the item is heard, so that it is in time whatever the output adds), and the
// item plays to a sink that plays to nothing (silent_sink.hpp, handed to
// HearthController::set_test_outputs() as the Qt Quick suites hand it a fake
// room), at the rate of the clock. The window names that output "Speakers". The
// headphones answer is given as yes, since a file has no room for the backing
// to leak into. A reference that cannot be read (the item has no melody to
// score against) or a file that is not a WAV file ends the run with exit code
// 3, as an item that did not load does.
// src/command_line.hpp reads all of these, and test_command_line.cpp holds it
// to what is said here.
//
// Translations run through the family's own LanguageManager
// (apps/shared/preferences/src/language_manager.cpp, shared rather than copied), pointed at
// this app's own hearth_<code>.qm catalogues under :/i18n/. The six
// languages are the same set forge-gui and Crucible ship. The catalogues carry
// every source string, each translated or not: a string with no translation
// shows in English, and a language change always switches the layout
// direction and the typeface.

#include <QDir>
#include <QElapsedTimer>
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

#include <algorithm>
#include <memory>
#include <optional>

#include "iclforge/sendspin/firewall.hpp"
#include "command_line.hpp"
#include "hearth_controller.hpp"
#include "language_manager.hpp"
#include "microphone_controller.hpp"
#include "native_log_sink.hpp"
#include "network_controller.hpp"
#include "settings_migration.hpp"
#include "single_instance.hpp"
#include "window_memory.hpp"

// Qt's headers define `slots` as a macro, and iclforge::render::OutputLayout has a method of that
// name: undone before the engine's headers, which silent_outputs.hpp reaches through the sink.
#undef slots

#include "silent_outputs.hpp"

namespace {

// How long a capture waits for an --open file's media information before it
// grabs what there is and says so in its exit code.
constexpr int kOpenWaitMs = 15000;
constexpr int kOpenPollMs = 50;
// After the item is loaded (and cued at its --seek position): the lyric view
// reads its file one event-loop turn later and the page lays out once more.
constexpr int kOpenSettleMs = 500;
// How much longer a capture of the timing window settles: the window waits for the item's
// lyrics to have been read before it opens, and then comes up.
constexpr int kTimingOpensMs = 1500;
// A performance's picture is taken as the item reaches its position, which is already on the
// screen: there is nothing to wait for, and waiting would be the item playing on past the place.
constexpr int kSingSettleMs = 0;
constexpr int kExitItemNotLoaded = 3;
// What the window calls the output of a run that plays to nothing (--sing).
constexpr const char* kSilentOutputName = "Speakers";

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

// Puts the window where a `--page` says: on one of Main.qml's pages, or on the
// page that something opens over, with that open. The same for a launch and for
// what another launch hands over (single_instance.hpp), so that a name that is
// not a page never leaves the window on no page at all.
void show_page(QObject* root, const QString& page) {
    const auto open_over = [root](const char* under, const char* opener) {
        root->setProperty("page", QString::fromLatin1(under));
        QMetaObject::invokeMethod(root, opener);
    };
    if (page == QLatin1String("firstrun")) {
        QMetaObject::invokeMethod(root, "openFirstRun");
    } else if (page == QLatin1String("shortcuts")) {
        open_over("play", "openShortcuts");
    } else if (page == QLatin1String("about")) {
        open_over("play", "openAbout");
    } else if (page == QLatin1String("licences")) {
        open_over("play", "openLicences");
    } else if (page == QLatin1String("singer")) {
        open_over("karaoke", "openLyricScreen");
    } else if (page == QLatin1String("timing")) {
        open_over("karaoke", "openLyricTiming");
    } else if (!page.isEmpty()) {
        root->setProperty("page", page);
    }
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
        // The one thing a capture's store starts with: where the Karaoke page's microphone card
        // says takes go. Left to its default that is a folder under the person's own documents,
        // which is right for the person and puts an account's name in the picture.
        iclforge::hearth::ui::MicrophoneController::keep_capture_takes_folder();
    }

    // The paired sinks, the pairing keys and the server identity, the playback and network
    // settings: what a person saved under the old names (organisation ac3forge, application
    // Hearth) is copied to the store Hearth has now, once, before anything reads it. A capture
    // has just pointed the store at an empty scratch directory, so it finds nothing to copy.
    iclforge::settings_migration::migrate_program(iclforge::settings_migration::Program::kHearth);

    // --setting: what the capture's scratch store holds before anything reads it. The parser
    // keeps none without a capture, so this is never a person's own store. A value is kept as
    // text, which is how the INI store a capture uses holds every value.
    if (!command_line.settings.isEmpty()) {
        QSettings scratch(QSettings::defaultFormat(), QSettings::UserScope,
                          QCoreApplication::organizationName(),
                          QCoreApplication::applicationName());
        for (const auto& [key, value] : command_line.settings) {
            scratch.setValue(key, value);
        }
    }

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

    // --sing: the capture's microphone is a file, and its output plays to nothing. Both are seams
    // of controllers the window reads once, so they are in place before the QML loads and starts
    // them (Main.qml's HearthController.start()); the singletons are made here, by the engine
    // that owns them, and the QML finds them made. `microphone` stays null when there is no
    // performance to take, or when the file would not read (the log says why and the run ends
    // with exit code 3 after its picture).
    iclforge::hearth::ui::MicrophoneController* microphone = nullptr;
    bool sing_failed = false;
    if (!command_line.sing_file.isEmpty()) {
        auto* const player = engine.singletonInstance<iclforge::hearth::ui::HearthController*>(
            "Hearth", "HearthController");
        auto* const singer = engine.singletonInstance<iclforge::hearth::ui::MicrophoneController*>(
            "Hearth", "MicrophoneController");
        QString why;
        if (player == nullptr || singer == nullptr) {
            qWarning("hearth: --sing could not reach the controllers it sets");
            sing_failed = true;
        } else if (!singer->use_voice_file(command_line.sing_file, &why)) {
            qWarning("hearth: %s", qUtf8Printable(why));
            sing_failed = true;
        } else {
            player->set_test_outputs(
                iclforge::hearth::ui::make_silent_outputs(kSilentOutputName));
            microphone = singer;
        }
    }

    // The window's size, position and maximised state, kept in the store between starts
    // (apps/shared/preferences/src/window_placement.hpp has the keys and the rules). Main.qml is
    // loaded hidden so that attach() places it before it is first shown; a capture is on the
    // scratch store above, which holds nothing, so it opens at Main.qml's own size.
    iclforge::window_memory::WindowMemory remembered_window;
    engine.setInitialProperties({{QStringLiteral("visible"), false}});

    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("Hearth", "Main");
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
    // its say. `--page firstrun`/`shortcuts`/`about`/`licences` open their
    // dialog over the Play page, for a capture.
    if (!shot_path.isEmpty()) {
        engine.rootObjects().first()->setProperty("suppressFirstRun", true);
    }
    if (command_line.size.has_value()) {
        if (auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first())) {
            window->resize(command_line.size->first, command_line.size->second);
        }
    }
    show_page(engine.rootObjects().first(), page);
    // The singer view takes the whole screen. A capture puts the window back as it was, with
    // the view still open in it, so that the picture has the window's size on every machine.
    if (page == QLatin1String("singer") && !shot_path.isEmpty()) {
        if (auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().first())) {
            window->showNormal();
        }
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
                if (root != nullptr) {
                    show_page(root, requested_page);
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
    // The timing window opens a moment after the item is loaded (KaraokePage.qml's
    // openTiming()), so a capture of it settles for that much longer.
    //
    // --sing makes this a performance (MicrophoneController::use_voice_file()). The item is
    // started at its beginning, not at the position, and the microphone is turned on first, as a
    // person turns it on before the song starts; but not before the melody it is scored against
    // has been read (the card asks for it as the item is loaded, and a voice fed before it
    // arrives is a voice that was never scored). The item then plays in real time to the
    // position, which is why the time allowed is that much longer. The picture is taken as it
    // gets there, with the performance going on; --finish-singing holds the item there and ends
    // the performance first, which is when the card gives its summary.
    const bool performing = microphone != nullptr;
    const bool finishing = performing && command_line.finish_singing;
    const int settle_ms =
        (performing && !finishing)
            ? kSingSettleMs
            : kOpenSettleMs + (page == QLatin1String("timing") ? kTimingOpensMs : 0);
    const int wait_limit_ms =
        kOpenWaitMs +
        (performing ? static_cast<int>(std::min<qlonglong>(*command_line.seek_ms, 10 * 60 * 1000))
                    : 0);
    enum class Step { kLoading, kReference, kStarting, kHolding, kDone };
    struct Cue {
        Step step = Step::kLoading;
        int waited = 0;
        // --sing: from the item being started to its first heard position, which is what the
        // output and the engine add and what a voice started with play() would be early by.
        QElapsedTimer since_play;
        bool heard = false;
    };
    auto cue = std::make_shared<Cue>();
    auto* wait = new QTimer(&app);
    wait->setInterval(kOpenPollMs);
    QObject::connect(
        wait, &QTimer::timeout, &app,
        [&app, &engine, controller, microphone, performing, finishing, sing_failed, wait, cue,
         first_path, shot_path, settle_ms, wait_limit_ms, seek_ms = command_line.seek_ms] {
            cue->waited += kOpenPollMs;
            const bool timed_out = cue->waited >= wait_limit_ms;
            bool done = false;
            // A step that cannot be taken at all (not that it is late): the run ends as one
            // that did not get there, and says what it can in the picture and the log.
            bool gave_up = false;
            switch (cue->step) {
                case Step::kLoading: {
                    // Read, and a stream: a path with no codec is a file that was not one (or
                    // was not there), which the Media page explains and a capture is not for.
                    const QVariantMap media = controller->currentMedia();
                    if (media.value(QStringLiteral("path")).toString() == first_path &&
                        media.contains(QStringLiteral("codec"))) {
                        if (sing_failed) {
                            gave_up = true;  // --sing's file would not read: the log said so
                        } else if (performing) {
                            cue->step = Step::kReference;
                        } else if (seek_ms.has_value()) {
                            controller->seek(*seek_ms);
                            controller->playItem(controller->currentIndex());
                            cue->step = Step::kStarting;
                        } else {
                            done = true;
                        }
                    }
                    break;
                }
                case Step::kReference: {
                    const QString status = microphone->referenceStatus();
                    if (status == QLatin1String("ready")) {
                        // The score is shown only to a person who has said they wear
                        // headphones, because through speakers the microphone hears the
                        // backing too (SC5). A file has no room for it to leak into.
                        microphone->setHeadphones(true);
                        if (!microphone->start()) {
                            qWarning("hearth: --sing: %s", qUtf8Printable(microphone->errorText()));
                            gave_up = true;
                            break;
                        }
                        controller->playItem(controller->currentIndex());
                        cue->since_play.start();
                        cue->step = Step::kStarting;
                    } else if (status == QLatin1String("none") &&
                               microphone->itemPath() == first_path &&
                               !microphone->referenceText().isEmpty()) {
                        // Asked for, read, and there is no melody in it to score against: the
                        // card says why. (Not before the card has asked: until the item is known
                        // to be karaoke the card says "Nothing is playing.", which is not this.)
                        qWarning("hearth: --sing: %s", qUtf8Printable(microphone->referenceText()));
                        gave_up = true;
                    }
                    break;
                }
                case Step::kStarting:
                    if (performing && !cue->heard && controller->positionMs() > 0) {
                        cue->heard = true;
                        qInfo("hearth: --sing: the item was heard %lld ms after it was started",
                              static_cast<long long>(cue->since_play.elapsed()));
                    }
                    if (controller->playing() && controller->positionMs() >= *seek_ms) {
                        if (performing && !finishing) {
                            done = true;  // the picture is of the performance, going on
                        } else {
                            controller->pause();
                            cue->step = Step::kHolding;
                        }
                    }
                    break;
                case Step::kHolding:
                    done = controller->state() == QLatin1String("paused");
                    if (done && finishing) {
                        microphone->stop();  // the summary is made as it ends
                    }
                    break;
                case Step::kDone:
                    break;
            }
            if (!done && !gave_up && !timed_out) {
                return;
            }
            wait->stop();
            cue->step = Step::kDone;
            if (performing) {
                // What the run came to, for the person regenerating a picture to see: a lag of
                // more than a few tens of milliseconds, or a score well short of the file's own
                // singing, says the voice and the item are not as they were.
                const QVariantMap score =
                    finishing ? microphone->summary() : microphone->score();
                qInfo("hearth: --sing: %d%% of the tune, %d%% of it voiced, lag %s",
                      score.value(QStringLiteral("percent"), -1).toInt(),
                      score.value(QStringLiteral("coveragePercent"), 0).toInt(),
                      score.contains(QStringLiteral("lagMs"))
                          ? qUtf8Printable(QStringLiteral("%1 ms").arg(
                                score.value(QStringLiteral("lagMs")).toInt()))
                          : "not yet measured");
            }
            if (shot_path.isEmpty()) {
                return;
            }
            QTimer::singleShot(settle_ms, &app, [&engine, shot_path, done] {
                const bool saved = save_window(engine, shot_path);
                QCoreApplication::exit(!saved ? 2 : (done ? 0 : kExitItemNotLoaded));
            });
        });
    wait->start();

    return QGuiApplication::exec();
}
