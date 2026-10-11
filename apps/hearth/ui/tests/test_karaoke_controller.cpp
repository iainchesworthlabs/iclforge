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
#include <QVariantList>
#include <QVariantMap>

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
