#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>
#include <utility>

// What Hearth's command line asks for, read once by main() (main.cpp's own header comment
// is the usage text, and docs/hearth/index.md's "Command line" section the reference).
//
// Header-only and Qt Core only, so that hearth_controller_tests can hold the parsing to what
// the docs say without a window: main() is the entry point of `hearth` alone and is linked
// into no test.
//
// The options are `--name value` pairs, as they were before this file existed, and `--name`
// alone for the flag. A pair whose value is missing (the option is the last argument) is
// ignored, as it always was; the new options say so in `warnings` as well, which main() puts
// in the log. Nothing here ever stops the program: an unknown option is skipped (it takes no
// value of its own), so a launcher that adds one of its own does not stop a window from
// opening.
//
// An argument that does not start with a dash is a file, and so is every argument after a
// bare `--`: that is the whole command line of a double-clicked .ac3 (the shell runs
// `hearth.exe "%1"`, cmake/Packaging.cmake), of `hearth song.ac3` and of an "open with".
// Qt's own options (-platform, -style, ...) never reach here: QGuiApplication takes them and
// their values out of QCoreApplication::arguments() before main() reads it.
namespace iclforge::hearth::ui {

struct CommandLine {
    // --shot <png> and --smoke-shot <png>: grab the window once it has settled and quit.
    QString shot_path;
    // --smoke-shot: the same capture with network discovery OFF. --shot browses mDNS for
    // sinks, which on Windows asks for a firewall rule (a UAC prompt), so only this one is
    // safe to run unattended, and a capture that is not about the Network page has no
    // use for what discovery finds on whatever network the machine is on.
    bool smoke = false;
    // --page <name>: the page the window opens on (or a dialog, main.cpp lists them).
    QString page;
    // --decoder-format <eac3|ac4>: the Decoder page's sub-tab.
    QString decoder_format;
    // --open-output-picker: open the output picker before the grab.
    bool open_output_picker = false;
    // Every file the command line names, in the order it names them: `--open <file>` (as many
    // times as it is given), a bare argument, and each argument after `--`. Each is added to
    // the queue and the first becomes the item that is loaded; whether it then starts is
    // plays_at_launch().
    QStringList open_files;
    // --seek <ms>: where in the first item to start it, in milliseconds from its start. It is
    // held paused there: a stopped item reports no position, so a page that shows one (a lyric
    // view part way through a word) needs the item to have been started.
    std::optional<qlonglong> seek_ms;
    // --setting <key>=<value>, as many times as it is given: what a capture's settings store
    // holds before the window reads it (`microphone/takesFolder=D:/takes`). A capture runs
    // against a scratch store that starts empty, so that it shows nobody's own devices or
    // folders, and this is how it shows a state that a person reaches by choosing something
    // once. Only a capture has that store: without --shot or --smoke-shot the option is
    // dropped and said so, so that it can never write to a person's own settings.
    QList<std::pair<QString, QString>> settings;
    // --size <width>x<height>: the window's size in pixels, for a capture of a page that is
    // longer than the window opens at (the Karaoke page's cards run on below it). The system
    // still has its say: a window is not made taller than the screen it is on.
    std::optional<std::pair<int, int>> size;
    // --sing <file>: a WAV file as the singer, for a capture of a performance. The microphone is
    // the file, sung along to the first item as it is heard (VoiceFileInput), from its start to
    // --seek, and the sound goes to no output at all, so a picture of the Microphone card's
    // ribbon, a line's mark or the summary needs no singer, no microphone and no speaker.
    // Nothing a person can reach opens a microphone but their press on the card (MI2); this opens
    // none, so it never bends that, and it is dropped, and said so, wherever it would: without a
    // capture, a file to sing along to, a --seek to sing to, or a page that has the card
    // (karaoke or singer).
    QString sing_file;
    // --finish-singing: end the performance before the picture, as a person's press on "Turn
    // off" does, so that the card shows the summary ("Last performance"). Needs --sing.
    bool finish_singing = false;
    // What was not understood, for the log.
    QStringList warnings;

    // Whether the first file starts playing as the window comes up. A file handed to the
    // program is a request to play it, as in any player. A capture (--shot, --smoke-shot) is
    // for a picture and never opens a sound output it was not asked to, and --seek holds the
    // item paused at its position, so those two only cue it.
    [[nodiscard]] bool plays_at_launch() const {
        return !open_files.isEmpty() && shot_path.isEmpty() && !seek_ms.has_value();
    }
};

[[nodiscard]] inline CommandLine parse_command_line(const QStringList& args) {
    CommandLine out;
    // A bare flag, not a "--name value" pair: checked separately so it can be the last
    // argument with nothing following it.
    out.open_output_picker = args.contains(QLatin1String("--open-output-picker"));
    out.finish_singing = args.contains(QLatin1String("--finish-singing"));
    // After a bare "--" every argument is a file, whatever it looks like (a file called
    // `--page`, or `-x.ac3`).
    bool files_only = false;
    for (qsizetype i = 1; i < args.size(); ++i) {
        const QString& arg = args[i];
        if (files_only || !arg.startsWith(QLatin1Char('-'))) {
            // A file. An empty argument is not one, and a lone "-" is an option nobody here
            // knows. Looked at before any option's value is, so a value is never a file.
            if (!arg.isEmpty()) {
                out.open_files.push_back(arg);
            }
            continue;
        }
        if (arg == QLatin1String("--")) {
            files_only = true;
            continue;
        }
        const bool has_value = i + 1 < args.size();
        if (arg == QLatin1String("--shot") || arg == QLatin1String("--smoke-shot")) {
            if (has_value) {
                out.shot_path = args[i + 1];
                out.smoke = out.smoke || arg == QLatin1String("--smoke-shot");
            }
        } else if (arg == QLatin1String("--page")) {
            // play, media, speakers, decoder, network, settings, karaoke, firstrun,
            // shortcuts, about, licences, singer or timing
            if (has_value) {
                out.page = args[i + 1];
            }
        } else if (arg == QLatin1String("--decoder-format")) {
            // "eac3" or "ac4" - DecoderPage.qml's own `format` values.
            if (has_value) {
                out.decoder_format = args[i + 1];
            }
        } else if (arg == QLatin1String("--open")) {
            if (!has_value) {
                out.warnings.push_back(QStringLiteral("--open needs a file"));
            } else {
                out.open_files.push_back(args[i + 1]);
            }
        } else if (arg == QLatin1String("--seek")) {
            if (!has_value) {
                out.warnings.push_back(QStringLiteral("--seek needs a position in milliseconds"));
            } else {
                bool ok = false;
                const qlonglong ms = args[i + 1].toLongLong(&ok);
                if (ok && ms >= 0) {
                    out.seek_ms = ms;
                } else {
                    out.warnings.push_back(
                        QStringLiteral("--seek takes a position in milliseconds, not \"%1\"")
                            .arg(args[i + 1]));
                }
            }
        } else if (arg == QLatin1String("--size")) {
            const QStringList parts = has_value ? args[i + 1].split(QLatin1Char('x')) : QStringList{};
            bool width_ok = false;
            bool height_ok = false;
            const int width = parts.size() == 2 ? parts[0].toInt(&width_ok) : 0;
            const int height = parts.size() == 2 ? parts[1].toInt(&height_ok) : 0;
            if (width_ok && height_ok && width > 0 && height > 0) {
                out.size = std::pair{width, height};
            } else {
                out.warnings.push_back(
                    QStringLiteral("--size takes <width>x<height> in pixels, as 1280x1400"));
            }
        } else if (arg == QLatin1String("--sing")) {
            if (!has_value) {
                out.warnings.push_back(QStringLiteral("--sing needs a WAV file"));
            } else {
                out.sing_file = args[i + 1];
            }
        } else if (arg == QLatin1String("--setting")) {
            const qsizetype eq = has_value ? args[i + 1].indexOf(QLatin1Char('=')) : -1;
            if (eq <= 0) {
                out.warnings.push_back(QStringLiteral("--setting takes <key>=<value>"));
            } else {
                out.settings.push_back({args[i + 1].left(eq), args[i + 1].mid(eq + 1)});
            }
        } else {
            continue;
        }
        // The value was this option's own: it is not looked at as an option itself, so a file
        // that is called `--page` is a file.
        if (has_value) {
            ++i;
        }
    }
    if (out.seek_ms && out.open_files.isEmpty()) {
        out.warnings.push_back(
            QStringLiteral("--seek has no item to cue without a file (on its own, or --open <file>)"));
        out.seek_ms.reset();
    }
    if (!out.settings.isEmpty() && out.shot_path.isEmpty()) {
        out.warnings.push_back(QStringLiteral(
            "--setting is for a capture's scratch settings (--shot or --smoke-shot), and is not used"));
        out.settings.clear();
    }
    if (!out.sing_file.isEmpty()) {
        // Each reason alone is enough to drop it, and the first that holds is the one said.
        QString why;
        if (out.shot_path.isEmpty()) {
            why = QStringLiteral("--sing is for a capture (--shot or --smoke-shot)");
        } else if (out.open_files.isEmpty()) {
            why = QStringLiteral("--sing has nothing to sing along to without a file");
        } else if (!out.seek_ms) {
            why = QStringLiteral(
                "--sing needs --seek <ms>, how far into the song the voice sings before the picture");
        } else if (out.page != QLatin1String("karaoke") && out.page != QLatin1String("singer")) {
            why = QStringLiteral(
                "--sing is for --page karaoke or --page singer, which have the Microphone card");
        }
        if (!why.isEmpty()) {
            out.warnings.push_back(why + QStringLiteral(", and is not used"));
            out.sing_file.clear();
        }
    }
    if (out.finish_singing && out.sing_file.isEmpty()) {
        out.warnings.push_back(QStringLiteral("--finish-singing ends what --sing begins, and there is no --sing in use"));
        out.finish_singing = false;
    }
    return out;
}

}  // namespace iclforge::hearth::ui
