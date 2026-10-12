#pragma once

#include <QString>
#include <QStringList>

#include <optional>

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
            // shortcuts, about or licences
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
    return out;
}

}  // namespace iclforge::hearth::ui
