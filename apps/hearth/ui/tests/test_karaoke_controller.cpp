// KaraokeController (planning/karaoke-design.md, K5): the lyrics of the item
// playing as the lyric view binds to them, and the cursor the view asks every
// frame. The reader and the cursor's own rules are tested where they live
// (libs/lyrics/tests/test_lyrics.cpp); what is checked here is what this
// class makes of them for QML: the shapes, the statuses and their reasons, the
// delay and that it is kept.

#include <QDir>
#include <QFile>
#include <QSettings>
#include <QString>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "karaoke_controller.hpp"

using iclforge::hearth::ui::KaraokeController;

namespace {

// A folder to put an item and its lyrics in, and a settings folder of its own
// so the delay of one case is not read by the next.
struct Room {
    QTemporaryDir folder;
    QTemporaryDir settings;

    Room() {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    }

    [[nodiscard]] QString path(const QString& name) const { return QDir(folder.path()).filePath(name); }

    void write(const QString& name, const QByteArray& bytes) const {
        QFile file(path(name));
        REQUIRE(file.open(QIODevice::WriteOnly));
        REQUIRE(file.write(bytes) == bytes.size());
    }
};

// The first line is word-timed, the second only line-timed, the third word-timed
// again; the words of the first run to the second line's start.
const QByteArray kEnhanced =
    "[ti:Auld Lang Syne]\n"
    "[ar:Traditional]\n"
    "[00:01.00]<00:01.00>Should <00:01.50>auld <00:02.00>acquaintance <00:03.00>be <00:03.50>forgot,\n"
    "[00:05.00]And never brought to mind?\n"
    "[00:08.00]<00:08.00>For <00:08.50>auld <00:09.00>lang <00:09.50>syne\n";

QVariantMap line_at(const KaraokeController& controller, int index) {
    return controller.lines().at(index).toMap();
}

}  // namespace

TEST_CASE("KaraokeController: a word-timed file loads as lines and words",
          "[hearth][hearth-controller][karaoke][lyrics]") {
    Room room;
    room.write("song.lrc", kEnhanced);
    KaraokeController controller;
    CHECK(controller.status().toStdString() == "none");

    int changes = 0;
    QObject::connect(&controller, &KaraokeController::lyricsChanged, [&changes] { ++changes; });
    controller.load(room.path("song.ac3"), 10000);

    REQUIRE(controller.status().toStdString() == "timed");
    CHECK(changes == 1);
    CHECK(controller.fileName().toStdString() == "song.lrc");
    CHECK(controller.title().toStdString() == "Auld Lang Syne");
    CHECK(controller.artist().toStdString() == "Traditional");
    CHECK(controller.wordTimed());
    REQUIRE(controller.lineCount() == 3);

    const QVariantMap first = line_at(controller, 0);
    CHECK(first.value("startMs").toLongLong() == 1000);
    CHECK(first.value("endMs").toLongLong() == 5000);
    CHECK(first.value("wordTimed").toBool());
    CHECK(first.value("text").toString().toStdString() == "Should auld acquaintance be forgot,");
    const QVariantList words = first.value("words").toList();
    REQUIRE(words.size() == 5);
    // A word's spacing is a flag, not part of its text, and the last has none.
    CHECK(words[2].toMap().value("text").toString().toStdString() == "acquaintance");
    CHECK(words[2].toMap().value("gap").toBool());
    CHECK(words[2].toMap().value("startMs").toLongLong() == 2000);
    CHECK(words[2].toMap().value("endMs").toLongLong() == 3000);
    CHECK(words[4].toMap().value("text").toString().toStdString() == "forgot,");
    CHECK_FALSE(words[4].toMap().value("gap").toBool());

    // The middle line has times and no words: nothing is invented for it.
    CHECK_FALSE(line_at(controller, 1).value("wordTimed").toBool());
    CHECK(line_at(controller, 1).value("words").toList().isEmpty());
    // The last line runs to the item's length.
    CHECK(line_at(controller, 2).value("endMs").toLongLong() == 10000);
}

TEST_CASE("KaraokeController: the cursor says line, word and how far through it",
          "[hearth][hearth-controller][karaoke][lyrics]") {
    Room room;
    room.write("song.lrc", kEnhanced);
    KaraokeController controller;
    controller.load(room.path("song.ac3"), 10000);
    REQUIRE(controller.status().toStdString() == "timed");

    // Before the first line: nothing sung, nothing filled.
    QVariantMap at = controller.cursor(500);
    CHECK(at.value("line").toInt() == -1);
    CHECK(at.value("word").toInt() == -1);
    CHECK(at.value("fills").toList().isEmpty());

    // Half way through "acquaintance": the two words before it are sung.
    at = controller.cursor(2500);
    CHECK(at.value("line").toInt() == 0);
    CHECK(at.value("inLine").toBool());
    CHECK(at.value("word").toInt() == 2);
    CHECK(at.value("fraction").toDouble() == Catch::Approx(0.5));
    QVariantList fills = at.value("fills").toList();
    REQUIRE(fills.size() == 5);
    CHECK(fills[0].toDouble() == Catch::Approx(1.0));
    CHECK(fills[1].toDouble() == Catch::Approx(1.0));
    CHECK(fills[2].toDouble() == Catch::Approx(0.5));
    CHECK(fills[3].toDouble() == Catch::Approx(0.0));
    CHECK(fills[4].toDouble() == Catch::Approx(0.0));

    // The last word runs to the end of its line.
    fills = controller.cursor(4250).value("fills").toList();
    REQUIRE(fills.size() == 5);
    CHECK(fills[3].toDouble() == Catch::Approx(1.0));
    CHECK(fills[4].toDouble() == Catch::Approx(0.5));

    // A line with no word times is followed as a line, with no fills.
    at = controller.cursor(6000);
    CHECK(at.value("line").toInt() == 1);
    CHECK(at.value("word").toInt() == -1);
    CHECK(at.value("fills").toList().isEmpty());

    // After the last line's end: no line is being sung.
    at = controller.cursor(10500);
    CHECK(at.value("line").toInt() == 2);
    CHECK_FALSE(at.value("inLine").toBool());
    CHECK(at.value("fills").toList().isEmpty());
}

TEST_CASE("KaraokeController: the delay shows the words later, is clamped and is kept",
          "[hearth][hearth-controller][karaoke][lyrics]") {
    Room room;
    room.write("song.lrc", kEnhanced);
    {
        KaraokeController controller;
        CHECK(controller.delayMs() == 0);
        controller.load(room.path("song.ac3"), 10000);

        int changes = 0;
        QObject::connect(&controller, &KaraokeController::delayChanged, [&changes] { ++changes; });
        controller.setDelayMs(500);
        CHECK(changes == 1);
        controller.setDelayMs(500);
        CHECK(changes == 1);

        // 2.5 s heard is 2.0 s of the words: the start of "acquaintance".
        const QVariantMap at = controller.cursor(2500);
        CHECK(at.value("positionMs").toLongLong() == 2000);
        CHECK(at.value("word").toInt() == 2);
        CHECK(at.value("fraction").toDouble() == Catch::Approx(0.0));

        controller.setDelayMs(5000);
        CHECK(controller.delayMs() == KaraokeController::kMaxDelayMs);
        controller.setDelayMs(-5000);
        CHECK(controller.delayMs() == -KaraokeController::kMaxDelayMs);
        controller.setDelayMs(-250);
        // Sooner: 2.5 s heard is 2.75 s of the words.
        CHECK(controller.cursor(2500).value("positionMs").toLongLong() == 2750);
    }
    // A new start reads what was kept.
    KaraokeController again;
    CHECK(again.delayMs() == -250);
}

TEST_CASE("KaraokeController: no file, a plain file and an unreadable one each say what they are",
          "[hearth][hearth-controller][karaoke][lyrics]") {
    Room room;
    KaraokeController controller;

    // Nothing playing.
    controller.load(QString{}, 0);
    CHECK(controller.status().toStdString() == "none");
    CHECK(controller.lines().isEmpty());

    // An item with no file beside it: the name it looks for, so the page can say.
    controller.load(room.path("auld_lang_syne.ac3"), 5000);
    CHECK(controller.status().toStdString() == "none");
    CHECK(controller.expectedName().toStdString() == "auld_lang_syne.lrc");
    CHECK(controller.fileName().isEmpty());

    // A file with no times is shown whole and not followed.
    room.write("plain.lrc", "Should auld acquaintance be forgot\nAnd never brought to mind\n");
    controller.load(room.path("plain.ac3"), 5000);
    CHECK(controller.status().toStdString() == "plain");
    CHECK(controller.lineCount() == 2);
    CHECK(controller.cursor(1000).value("line").toInt() == -1);

    // A file that is not UTF-8 is not shown, with the reason.
    room.write("broken.lrc", QByteArray("[00:01.00]caf\xe9 au lait\n"));
    controller.load(room.path("broken.ac3"), 5000);
    CHECK(controller.status().toStdString() == "error");
    CHECK(controller.fileName().toStdString() == "broken.lrc");
    CHECK(controller.errorText().toStdString() == "The lyrics file is not UTF-8 or UTF-16.");
    CHECK(controller.lines().isEmpty());

    // And the file's extension is matched in any case.
    room.write("loud.LRC", "[00:01.00]LOUD\n");
    controller.load(room.path("loud.ac3"), 5000);
    CHECK(controller.status().toStdString() == "timed");
}

TEST_CASE("KaraokeController: a duet's lines carry their singer, and the names come typed, then filed, then V1 and V2",
          "[hearth][hearth-controller][karaoke][lyrics][duet]") {
    Room room;
    room.write("duet.lrc",
               "[ti:Leave Her, Johnny]\n[v1:Shantyman]\n[v2:Crew]\n"
               "[00:01.00]V1: Oh the times are hard\n"
               "[00:04.00]V2: Leave her, Johnny\n"
               "[00:07.00]V1+V2: Leave her\n"
               "[00:09.00]For nobody in particular\n");
    KaraokeController controller;
    CHECK_FALSE(controller.hasSingers());
    controller.load(room.path("duet.ac3"), 12000);
    REQUIRE(controller.status().toStdString() == "timed");
    CHECK(controller.hasSingers());
    REQUIRE(controller.lineCount() == 4);
    CHECK(line_at(controller, 0).value("singer").toString().toStdString() == "v1");
    CHECK(line_at(controller, 0).value("text").toString().toStdString() == "Oh the times are hard");
    CHECK(line_at(controller, 1).value("singer").toString().toStdString() == "v2");
    CHECK(line_at(controller, 2).value("singer").toString().toStdString() == "both");
    CHECK(line_at(controller, 3).value("singer").toString().toStdString().empty());

    // The file's names, until a person types one: typed comes first, per singer.
    CHECK(controller.fileFirstSingerName().toStdString() == "Shantyman");
    CHECK(controller.firstSingerLabel().toStdString() == "Shantyman");
    CHECK(controller.secondSingerLabel().toStdString() == "Crew");
    int changes = 0;
    QObject::connect(&controller, &KaraokeController::singerNamesChanged, [&changes] { ++changes; });
    controller.setFirstSingerName(QStringLiteral("  Anna "));
    CHECK(controller.firstSingerName().toStdString() == "Anna");
    CHECK(controller.firstSingerLabel().toStdString() == "Anna");
    CHECK(controller.secondSingerLabel().toStdString() == "Crew");
    CHECK(changes == 1);
    // The same name again is not a change, and an empty one gives the file's back.
    controller.setFirstSingerName(QStringLiteral("Anna"));
    CHECK(changes == 1);
    controller.setFirstSingerName(QString());
    CHECK(controller.firstSingerLabel().toStdString() == "Shantyman");

    // A typed name is for the run, not for the song: the next item's lyrics keep it, and a file
    // without names falls to V1 and V2.
    controller.setSecondSingerName(QStringLiteral("Ben"));
    room.write("solo.lrc", "[00:01.00]Should auld acquaintance\n");
    controller.load(room.path("solo.ac3"), 12000);
    CHECK_FALSE(controller.hasSingers());
    CHECK(controller.secondSingerLabel().toStdString() == "Ben");
    CHECK(controller.firstSingerLabel().toStdString() == "V1");
    CHECK(line_at(controller, 0).value("singer").toString().isEmpty());
}

TEST_CASE("KaraokeController: the same item and length again is not read again",
          "[hearth][hearth-controller][karaoke][lyrics]") {
    Room room;
    room.write("song.lrc", kEnhanced);
    KaraokeController controller;
    int changes = 0;
    QObject::connect(&controller, &KaraokeController::lyricsChanged, [&changes] { ++changes; });

    controller.load(room.path("song.ac3"), 10000);
    controller.load(room.path("song.ac3"), 10000);
    CHECK(changes == 1);
    // A length that was not known and now is changes where the last line ends.
    controller.load(room.path("song.ac3"), 20000);
    CHECK(changes == 2);
    CHECK(line_at(controller, 2).value("endMs").toLongLong() == 20000);
}

// ---- timing the lyrics (planning/karaoke-phase2.md, W6) -------------------------------------
//
// The editing functions are tested where they live (libs/lyrics/tests/test_timing.cpp); what is
// held here is the session around them: what a press is taken for, what a line and a word look
// like to QML, the reaction time and that it is kept, and what a save does to the files.

namespace {

const QByteArray kPlain = "Should auld acquaintance\nBe forgot\nAnd never brought to mind\n";

QVariantMap timing_line(const KaraokeController& controller, int index) {
    return controller.timingLines().at(index).toMap();
}

QByteArray read_all(const QString& path) {
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    return file.readAll();
}

}  // namespace

TEST_CASE("KaraokeController timing: a plain file opens with no line stamped and the first next",
          "[hearth][hearth-controller][karaoke][timing]") {
    Room room;
    room.write("song.lrc", kPlain);
    KaraokeController controller;
    controller.load(room.path("song.ac3"), 20000);
    REQUIRE(controller.status().toStdString() == "plain");
    CHECK_FALSE(controller.timing());

    REQUIRE(controller.beginTiming());
    CHECK(controller.timing());
    CHECK(controller.nextLine() == 0);
    CHECK(controller.stampedLines() == 0);
    REQUIRE(controller.timingLines().size() == 3);
    CHECK_FALSE(timing_line(controller, 0).value("stamped").toBool());
    CHECK(timing_line(controller, 1).value("text").toString().toStdString() == "Be forgot");
    // The lyrics the view shows are untouched by a session.
    CHECK(controller.status().toStdString() == "plain");
}

TEST_CASE("KaraokeController timing: a press stamps the next line, less the reaction time",
          "[hearth][hearth-controller][karaoke][timing]") {
    Room room;
    room.write("song.lrc", kPlain);
    KaraokeController controller;
    controller.setReactionMs(100);
    controller.load(room.path("song.ac3"), 20000);
    REQUIRE(controller.beginTiming());

    CHECK(controller.tapLine(2100));
    CHECK(timing_line(controller, 0).value("startMs").toLongLong() == 2000);
    CHECK(timing_line(controller, 0).value("stamped").toBool());
    CHECK(controller.nextLine() == 1);
    CHECK(controller.stampedLines() == 1);
    CHECK(controller.timingDirty());

    CHECK(controller.tapLine(6100));
    CHECK(controller.tapLine(9100));
    CHECK(controller.nextLine() == -1);
    CHECK(controller.stampedLines() == 3);
    // Nothing left to stamp: a press is refused and nothing moves.
    CHECK_FALSE(controller.tapLine(12000));
    CHECK(timing_line(controller, 2).value("startMs").toLongLong() == 9000);
    // The last line runs to the item's length, the others to the next line.
    CHECK(timing_line(controller, 0).value("endMs").toLongLong() == 6000);
    CHECK(timing_line(controller, 2).value("endMs").toLongLong() == 20000);
}

TEST_CASE("KaraokeController timing: a line can be stamped again and nudged",
          "[hearth][hearth-controller][karaoke][timing]") {
    Room room;
    room.write("song.lrc", kPlain);
    KaraokeController controller;
    controller.setReactionMs(0);
    controller.load(room.path("song.ac3"), 20000);
    REQUIRE(controller.beginTiming());
    REQUIRE(controller.tapLine(1000));
    REQUIRE(controller.tapLine(5000));

    CHECK(controller.restampLine(1, 5500));
    CHECK(timing_line(controller, 1).value("startMs").toLongLong() == 5500);
    CHECK(controller.nudge(1, -1, -300));
    CHECK(timing_line(controller, 1).value("startMs").toLongLong() == 5200);
    // A nudge below the line before is held there.
    CHECK(controller.nudge(1, -1, -9000));
    CHECK(timing_line(controller, 1).value("startMs").toLongLong() == 1000);

    CHECK_FALSE(controller.restampLine(7, 1000));
    CHECK_FALSE(controller.nudge(7, -1, 10));
}

TEST_CASE("KaraokeController timing: the word pass starts from an estimate and a press replaces it",
          "[hearth][hearth-controller][karaoke][timing]") {
    Room room;
    room.write("song.lrc", kPlain);
    KaraokeController controller;
    controller.setReactionMs(0);
    controller.load(room.path("song.ac3"), 20000);
    REQUIRE(controller.beginTiming());
    // Nothing is stamped yet, so there is nothing to estimate.
    CHECK(controller.estimateWords() == 0);

    REQUIRE(controller.tapLine(1000));
    REQUIRE(controller.tapLine(5000));
    CHECK(controller.estimateWords() == 2);
    const QVariantList words = timing_line(controller, 0).value("words").toList();
    REQUIRE(words.size() == 3);
    // An estimate is not a tap, and is told apart as one.
    CHECK_FALSE(words[1].toMap().value("tapped").toBool());
    CHECK(words[0].toMap().value("text").toString().toStdString() == "Should");
    CHECK(words[0].toMap().value("gap").toBool());

    CHECK(controller.tapWord(0, 1, 2500));
    const QVariantMap tapped = timing_line(controller, 0).value("words").toList()[1].toMap();
    CHECK(tapped.value("startMs").toLongLong() == 2500);
    CHECK(tapped.value("tapped").toBool());
    // The estimate is not made again over what a person set.
    CHECK(controller.estimateWords() == 0);
    // A press for a word that is not there is refused.
    CHECK_FALSE(controller.tapWord(0, 9, 1));
    CHECK_FALSE(controller.tapWord(8, 0, 1));
}

TEST_CASE("KaraokeController timing: a file that is timed opens as given, and text can come from elsewhere",
          "[hearth][hearth-controller][karaoke][timing]") {
    Room room;
    room.write("song.lrc", kEnhanced);
    room.write("words.txt", "A line\nAnd another\n");
    KaraokeController controller;
    controller.load(room.path("song.ac3"), 10000);

    REQUIRE(controller.beginTiming());
    CHECK(controller.stampedLines() == 3);
    CHECK(controller.nextLine() == -1);
    CHECK(timing_line(controller, 0).value("words").toList()[0].toMap().value("tapped").toBool());
    controller.endTiming();
    CHECK_FALSE(controller.timing());
    CHECK(controller.timingLines().isEmpty());

    // The words of another file (a path, or a file URL) replace the file's own for the session.
    REQUIRE(controller.beginTiming(room.path("words.txt")));
    REQUIRE(controller.timingLines().size() == 2);
    CHECK(controller.stampedLines() == 0);
    CHECK(controller.nextLine() == 0);
    controller.endTiming();
    REQUIRE(controller.beginTiming(QUrl::fromLocalFile(room.path("words.txt")).toString()));
    CHECK(controller.timingLines().size() == 2);
}

TEST_CASE("KaraokeController timing: a session that cannot open says why",
          "[hearth][hearth-controller][karaoke][timing]") {
    Room room;
    KaraokeController controller;
    // Nothing is playing.
    CHECK_FALSE(controller.beginTiming());
    CHECK_FALSE(controller.timingError().isEmpty());

    // An item with no words beside it and no file given.
    controller.load(room.path("song.ac3"), 10000);
    CHECK_FALSE(controller.beginTiming());
    CHECK(controller.timingError().toStdString().find("no words") != std::string::npos);

    // A file that is not text the reader takes.
    room.write("bad.txt", QByteArray("caf\xE9\n"));
    CHECK_FALSE(controller.beginTiming(room.path("bad.txt")));
    CHECK(controller.timingError().toStdString().find("UTF-8") != std::string::npos);
    CHECK_FALSE(controller.timing());
}

TEST_CASE("KaraokeController timing: the reaction time is kept and held to its range",
          "[hearth][hearth-controller][karaoke][timing]") {
    Room room;
    {
        KaraokeController controller;
        CHECK(controller.reactionMs() == KaraokeController::kDefaultReactionMs);
        int changes = 0;
        QObject::connect(&controller, &KaraokeController::reactionChanged, [&changes] { ++changes; });
        controller.setReactionMs(180);
        CHECK(changes == 1);
        controller.setReactionMs(180);
        CHECK(changes == 1);
        controller.setReactionMs(9000);
        CHECK(controller.reactionMs() == KaraokeController::kMaxReactionMs);
        controller.setReactionMs(-5);
        CHECK(controller.reactionMs() == 0);
        controller.setReactionMs(180);
    }
    KaraokeController again;
    CHECK(again.reactionMs() == 180);
}

TEST_CASE("KaraokeController timing: a save writes the item's file, keeps the first one, and loads it",
          "[hearth][hearth-controller][karaoke][timing]") {
    Room room;
    room.write("song.lrc", kPlain);
    KaraokeController controller;
    controller.setReactionMs(0);
    controller.load(room.path("song.ac3"), 20000);
    REQUIRE(controller.beginTiming());
    REQUIRE(controller.tapLine(1000));
    REQUIRE(controller.tapLine(5000));
    REQUIRE(controller.tapLine(9000));
    REQUIRE(controller.estimateWords() == 3);
    REQUIRE(controller.tapWord(0, 1, 2400));

    int loads = 0;
    QObject::connect(&controller, &KaraokeController::lyricsChanged, [&loads] { ++loads; });
    CHECK(controller.saveTiming().isEmpty());
    CHECK(loads >= 1);
    CHECK_FALSE(controller.timingDirty());

    // The original is kept, once, and the file is what was timed.
    CHECK(read_all(room.path("song.lrc.bak")) == kPlain);
    const QByteArray written = read_all(room.path("song.lrc"));
    CHECK(written.startsWith("[00:01.00]<00:01.00>Should <00:02.40>auld <"));
    CHECK(controller.status().toStdString() == "timed");
    CHECK(controller.wordTimed());

    // A second save does not replace the first backup with the file the first save wrote.
    REQUIRE(controller.tapWord(1, 0, 5500));
    CHECK(controller.saveTiming().isEmpty());
    CHECK(read_all(room.path("song.lrc.bak")) == kPlain);

    // A fresh controller reads the file as lines and words.
    KaraokeController fresh;
    fresh.load(room.path("song.ac3"), 20000);
    REQUIRE(fresh.status().toStdString() == "timed");
    CHECK(fresh.wordTimed());
    CHECK(line_at(fresh, 0).value("words").toList().size() == 3);
}

TEST_CASE("KaraokeController timing: ending a session discards what was not saved",
          "[hearth][hearth-controller][karaoke][timing]") {
    Room room;
    room.write("song.lrc", kPlain);
    KaraokeController controller;
    controller.setReactionMs(0);
    controller.load(room.path("song.ac3"), 20000);
    REQUIRE(controller.beginTiming());
    REQUIRE(controller.tapLine(1000));
    controller.endTiming();
    CHECK(read_all(room.path("song.lrc")) == kPlain);
    CHECK(controller.status().toStdString() == "plain");
    // A save with no session is refused and writes nothing.
    CHECK_FALSE(controller.saveTiming().isEmpty());
    CHECK(read_all(room.path("song.lrc")) == kPlain);
}
