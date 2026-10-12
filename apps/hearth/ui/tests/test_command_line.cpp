// Hearth's command line (apps/hearth/ui/src/command_line.hpp): what main() reads from argv.
// main() itself is the entry point of `hearth` alone and is linked into no test, so what the
// options mean is held here: --shot and --smoke-shot are the same capture and only the second
// is safe to run unattended, --open queues a file, --seek cues it, and nothing a launcher
// adds ever stops the window from opening.

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

TEST_CASE("an argument nobody here knows is skipped", "[hearth][command-line]") {
    const CommandLine cli = parse({"--platform", "offscreen", "--smoke-shot", "x.png", "--unknown"});
    CHECK(cli.shot_path == QStringLiteral("x.png"));
    CHECK(cli.smoke);
    CHECK(cli.warnings.isEmpty());
}
