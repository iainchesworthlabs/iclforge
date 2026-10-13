// Hearth's command line (apps/hearth/ui/src/command_line.hpp): what main() reads from argv.
// main() itself is the entry point of `hearth` alone and is linked into no test, so what the
// options mean is held here: --shot and --smoke-shot are the same capture and only the second
// is safe to run unattended, --open queues a file (and so does an argument on its own, which
// is all the shell's "%1" is), --seek cues it, --setting is a capture's alone, and nothing a
// launcher adds ever stops the window from opening.

#include <QString>
#include <QStringList>

#include <catch2/catch_test_macros.hpp>

#include "command_line.hpp"

using iclforge::hearth::ui::CommandLine;
using iclforge::hearth::ui::parse_command_line;

namespace {

// argv[0] first, as QCoreApplication::arguments() has it.
CommandLine parse(QStringList args) {
    args.prepend(QStringLiteral("hearth"));
    return parse_command_line(args);
}

}  // namespace

TEST_CASE("an ordinary launch asks for nothing", "[hearth][command-line]") {
    const CommandLine cli = parse({});
    CHECK(cli.shot_path.isEmpty());
    CHECK_FALSE(cli.smoke);
    CHECK(cli.page.isEmpty());
    CHECK(cli.decoder_format.isEmpty());
    CHECK_FALSE(cli.open_output_picker);
    CHECK(cli.open_files.isEmpty());
    CHECK_FALSE(cli.seek_ms.has_value());
    CHECK(cli.settings.isEmpty());
    CHECK_FALSE(cli.size.has_value());
    CHECK(cli.warnings.isEmpty());
}

TEST_CASE("the --shot option grabs the window and is not the unattended capture", "[hearth][command-line]") {
    const CommandLine cli = parse({"--shot", "play.png", "--page", "settings"});
    CHECK(cli.shot_path == QStringLiteral("play.png"));
    CHECK_FALSE(cli.smoke);
    CHECK(cli.page == QStringLiteral("settings"));
}

TEST_CASE("the --smoke-shot option is the same capture with network discovery off", "[hearth][command-line]") {
    const CommandLine cli = parse({"--smoke-shot", "karaoke.png", "--page", "karaoke"});
    CHECK(cli.shot_path == QStringLiteral("karaoke.png"));
    CHECK(cli.smoke);
    CHECK(cli.page == QStringLiteral("karaoke"));
}

TEST_CASE("naming both captures keeps the safe one", "[hearth][command-line]") {
    // A script that appends --smoke-shot to a command that already says --shot must not end up
    // browsing the network.
    CHECK(parse({"--shot", "a.png", "--smoke-shot", "b.png"}).smoke);
    CHECK(parse({"--smoke-shot", "b.png", "--shot", "a.png"}).smoke);
}

TEST_CASE("the --open option queues each file it is given, in order", "[hearth][command-line]") {
    const CommandLine cli = parse({"--open", "song.ac3", "--open", "next song.ac3"});
    REQUIRE(cli.open_files.size() == 2);
    CHECK(cli.open_files[0] == QStringLiteral("song.ac3"));
    CHECK(cli.open_files[1] == QStringLiteral("next song.ac3"));
    CHECK(cli.warnings.isEmpty());
}

TEST_CASE("a file on its own is opened, as the shell's \"%1\" hands one over", "[hearth][command-line]") {
    // What Explorer runs for a double-clicked .ac3 (cmake/Packaging.cmake registers
    // `"hearth.exe" "%1"`): one argument, spaces and all, and nothing else.
    const CommandLine cli = parse({"C:\\Music\\a song.ac3"});
    REQUIRE(cli.open_files.size() == 1);
    CHECK(cli.open_files[0] == QStringLiteral("C:\\Music\\a song.ac3"));
    CHECK(cli.warnings.isEmpty());
}

TEST_CASE("files on their own and --open files are one list, in the order given", "[hearth][command-line]") {
    const CommandLine cli = parse({"first.ac3", "--open", "second.ec3", "--page", "karaoke", "third.ac4"});
    REQUIRE(cli.open_files.size() == 3);
    CHECK(cli.open_files[0] == QStringLiteral("first.ac3"));
    CHECK(cli.open_files[1] == QStringLiteral("second.ec3"));
    CHECK(cli.open_files[2] == QStringLiteral("third.ac4"));
    // The page's own value is the page's, not a file.
    CHECK(cli.page == QStringLiteral("karaoke"));
    CHECK(cli.warnings.isEmpty());
}

TEST_CASE("everything after -- is a file, even what looks like an option", "[hearth][command-line]") {
    const CommandLine cli = parse({"--page", "karaoke", "--", "--shot", "-x.ac3", "--page"});
    REQUIRE(cli.open_files.size() == 3);
    CHECK(cli.open_files[0] == QStringLiteral("--shot"));
    CHECK(cli.open_files[1] == QStringLiteral("-x.ac3"));
    CHECK(cli.open_files[2] == QStringLiteral("--page"));
    CHECK(cli.page == QStringLiteral("karaoke"));
    CHECK(cli.shot_path.isEmpty());
}

TEST_CASE("an unknown option is skipped and takes no value, so the file after it is opened",
          "[hearth][command-line]") {
    const CommandLine cli = parse({"--unknown", "song.ac3", "-psn_0_4242", "-", "other.ac3"});
    REQUIRE(cli.open_files.size() == 2);
    CHECK(cli.open_files[0] == QStringLiteral("song.ac3"));
    CHECK(cli.open_files[1] == QStringLiteral("other.ac3"));
    CHECK(cli.warnings.isEmpty());
}

TEST_CASE("an empty argument is not a file", "[hearth][command-line]") {
    const CommandLine cli = parse({"", "song.ac3", ""});
    REQUIRE(cli.open_files.size() == 1);
    CHECK(cli.open_files[0] == QStringLiteral("song.ac3"));
}

TEST_CASE("the --seek option holds a file that was given on its own", "[hearth][command-line]") {
    const CommandLine cli = parse({"song.ac3", "--seek", "12500"});
    REQUIRE(cli.seek_ms.has_value());
    CHECK(*cli.seek_ms == 12500);
    CHECK(cli.warnings.isEmpty());
}

TEST_CASE("a launch plays its files; a capture and a --seek only cue them", "[hearth][command-line]") {
    // A file handed to the program is a request to play it, as in any player. A capture is for
    // a picture and stays quiet (it never opens a sound output it was not asked to), and a
    // --seek holds the item at its position instead.
    CHECK(parse({"song.ac3"}).plays_at_launch());
    CHECK(parse({"--open", "song.ac3"}).plays_at_launch());
    CHECK(parse({"--page", "karaoke", "song.ac3"}).plays_at_launch());
    CHECK_FALSE(parse({"--smoke-shot", "x.png", "song.ac3"}).plays_at_launch());
    CHECK_FALSE(parse({"--shot", "x.png", "--open", "song.ac3"}).plays_at_launch());
    CHECK_FALSE(parse({"song.ac3", "--seek", "100"}).plays_at_launch());
    // Nothing to play.
    CHECK_FALSE(parse({}).plays_at_launch());
    CHECK_FALSE(parse({"--page", "karaoke"}).plays_at_launch());
}

TEST_CASE("a value is never read as an option", "[hearth][command-line]") {
    // A file called --page is a file, and does not move the window to a page.
    const CommandLine cli = parse({"--open", "--page", "--smoke-shot", "x.png"});
    REQUIRE(cli.open_files.size() == 1);
    CHECK(cli.open_files[0] == QStringLiteral("--page"));
    CHECK(cli.page.isEmpty());
    CHECK(cli.smoke);
    CHECK(cli.shot_path == QStringLiteral("x.png"));
}

TEST_CASE("the --seek option is a position in milliseconds for the item that was opened", "[hearth][command-line]") {
    const CommandLine cli = parse({"--open", "song.ac3", "--seek", "12500"});
    REQUIRE(cli.seek_ms.has_value());
    CHECK(*cli.seek_ms == 12500);
    CHECK(cli.warnings.isEmpty());

    CHECK(*parse({"--seek", "0", "--open", "song.ac3"}).seek_ms == 0);
}

TEST_CASE("a position that is not one is said so and not used", "[hearth][command-line]") {
    for (const char* bad : {"soon", "-5", "1.5", ""}) {
        const CommandLine cli = parse({"--open", "song.ac3", "--seek", QString::fromLatin1(bad)});
        CHECK_FALSE(cli.seek_ms.has_value());
        REQUIRE(cli.warnings.size() == 1);
        CHECK(cli.warnings[0].contains(QStringLiteral("--seek")));
        // The file is still opened: a bad position does not cost the capture its song.
        CHECK(cli.open_files.size() == 1);
    }
}

TEST_CASE("the --seek option with nothing opened says so", "[hearth][command-line]") {
    const CommandLine cli = parse({"--seek", "3000"});
    CHECK_FALSE(cli.seek_ms.has_value());
    REQUIRE(cli.warnings.size() == 1);
    CHECK(cli.warnings[0].contains(QStringLiteral("--open")));
}

TEST_CASE("an option with no value at the end is ignored, and the new ones say so", "[hearth][command-line]") {
    // As it always was for --shot, --page and --decoder-format.
    const CommandLine shot = parse({"--shot"});
    CHECK(shot.shot_path.isEmpty());
    CHECK(shot.warnings.isEmpty());
    CHECK(parse({"--smoke-shot"}).shot_path.isEmpty());
    CHECK(parse({"--page"}).page.isEmpty());
    CHECK(parse({"--decoder-format"}).decoder_format.isEmpty());

    const CommandLine open = parse({"--open"});
    CHECK(open.open_files.isEmpty());
    REQUIRE(open.warnings.size() == 1);
    CHECK(open.warnings[0].contains(QStringLiteral("--open")));

    const CommandLine seek = parse({"--open", "song.ac3", "--seek"});
    CHECK_FALSE(seek.seek_ms.has_value());
    REQUIRE(seek.warnings.size() == 1);
}

TEST_CASE("the flags and values that were there before still read as they did", "[hearth][command-line]") {
    const CommandLine cli = parse({"--page", "decoder", "--decoder-format", "ac4", "--open-output-picker",
                                   "--shot", "decoder.png"});
    CHECK(cli.page == QStringLiteral("decoder"));
    CHECK(cli.decoder_format == QStringLiteral("ac4"));
    CHECK(cli.open_output_picker);
    CHECK(cli.shot_path == QStringLiteral("decoder.png"));
    CHECK_FALSE(cli.smoke);
}

TEST_CASE("the --setting option seeds a capture's settings, each key and its value, in order",
          "[hearth][command-line]") {
    const CommandLine cli = parse({"--smoke-shot", "x.png", "--setting", "microphone/takesFolder=D:/a folder/takes",
                                   "--setting", "appearance/textScale=150", "--setting", "karaoke/note="});
    REQUIRE(cli.settings.size() == 3);
    CHECK(cli.settings[0].first == QStringLiteral("microphone/takesFolder"));
    CHECK(cli.settings[0].second == QStringLiteral("D:/a folder/takes"));
    CHECK(cli.settings[1].first == QStringLiteral("appearance/textScale"));
    CHECK(cli.settings[1].second == QStringLiteral("150"));
    // An empty value is a value; only the first = divides, so a value may hold one.
    CHECK(cli.settings[2].second.isEmpty());
    CHECK(parse({"--shot", "x.png", "--setting", "a/b=c=d"}).settings[0].second == QStringLiteral("c=d"));
    CHECK(cli.warnings.isEmpty());
    // The value is the option's own, not a file to open.
    CHECK(cli.open_files.isEmpty());
}

TEST_CASE("the --setting option is dropped without a capture, and says so", "[hearth][command-line]") {
    // A capture runs against a scratch settings store. Any other launch has the person's own,
    // which a command line must never write to.
    for (const QStringList& args : {QStringList{"--setting", "microphone/headphones=true"},
                                    QStringList{"song.ac3", "--setting", "firstRun/seen=true", "--page", "karaoke"}}) {
        const CommandLine cli = parse(args);
        CHECK(cli.settings.isEmpty());
        REQUIRE(cli.warnings.size() == 1);
        CHECK(cli.warnings[0].contains(QStringLiteral("--setting")));
    }
    // The rest of the launch is as it was.
    CHECK(parse({"song.ac3", "--setting", "a/b=c"}).plays_at_launch());
}

TEST_CASE("a --setting that is not key=value is said so and not used", "[hearth][command-line]") {
    for (const char* bad : {"novalue", "=value", ""}) {
        const CommandLine cli = parse({"--smoke-shot", "x.png", "--setting", QString::fromLatin1(bad)});
        CHECK(cli.settings.isEmpty());
        REQUIRE(cli.warnings.size() == 1);
        CHECK(cli.warnings[0].contains(QStringLiteral("--setting")));
        CHECK(cli.smoke);
    }
    const CommandLine last = parse({"--smoke-shot", "x.png", "--setting"});
    CHECK(last.settings.isEmpty());
    REQUIRE(last.warnings.size() == 1);
}

TEST_CASE("the --size option is the window's width and height in pixels", "[hearth][command-line]") {
    const CommandLine cli = parse({"--smoke-shot", "x.png", "--size", "1280x1400", "--page", "karaoke"});
    REQUIRE(cli.size.has_value());
    CHECK(cli.size->first == 1280);
    CHECK(cli.size->second == 1400);
    CHECK(cli.page == QStringLiteral("karaoke"));
    CHECK(cli.warnings.isEmpty());
    // Not only a capture's: it is where a window opens, and writes nothing anywhere.
    CHECK(parse({"--size", "900x700"}).size.has_value());
}

TEST_CASE("a --size that is not one is said so and not used", "[hearth][command-line]") {
    for (const char* bad : {"1280", "1280x", "x800", "0x800", "1280x-5", "wide x tall", "1280x800x2", ""}) {
        const CommandLine cli = parse({"--size", QString::fromLatin1(bad), "song.ac3"});
        CHECK_FALSE(cli.size.has_value());
        REQUIRE(cli.warnings.size() == 1);
        CHECK(cli.warnings[0].contains(QStringLiteral("--size")));
        // The value was the option's own, and the file after it is still opened.
        REQUIRE(cli.open_files.size() == 1);
        CHECK(cli.open_files[0] == QStringLiteral("song.ac3"));
    }
    REQUIRE(parse({"--size"}).warnings.size() == 1);
}

TEST_CASE("the singer view and the timing window are named as pages are", "[hearth][command-line]") {
    // main() opens each over the Karaoke page; here they are a --page like any other.
    CHECK(parse({"--page", "singer", "song.ac3"}).page == QStringLiteral("singer"));
    CHECK(parse({"--smoke-shot", "x.png", "--page", "timing", "--open", "song.ac3"}).page == QStringLiteral("timing"));
}

TEST_CASE("an argument nobody here knows is skipped", "[hearth][command-line]") {
    // Not Qt's own options (-platform, -style, ...): QGuiApplication takes those, and their
    // values, out of the argument list before main() reads it.
    const CommandLine cli = parse({"--unknown", "--smoke-shot", "x.png", "--unknown-too"});
    CHECK(cli.shot_path == QStringLiteral("x.png"));
    CHECK(cli.smoke);
    CHECK(cli.open_files.isEmpty());
    CHECK(cli.warnings.isEmpty());
}
