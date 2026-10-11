// KaraokeController (planning/karaoke-design.md, K5): the lyrics of the item
// playing as the lyric view binds to them, and the cursor the view asks every
// frame. The reader and the cursor's own rules are tested where they live
// (libs/lyrics/tests/test_lyrics.cpp); what is checked here is what this
// class makes of them for QML: the shapes, the statuses and their reasons, the
// delay and that it is kept.

#include <QDir>
#include <QFile>
#include <QImage>
#include <QSettings>
#include <QString>
#include <QTemporaryDir>
#include <QVariantList>
#include <QVariantMap>

#include <array>

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

using CdgData = std::array<unsigned char, 16>;

// One 24-byte CD+G packet: command 9, the instruction, two parity bytes, sixteen data bytes, four
// parity bytes. The format is held to its arithmetic in libs/lyrics/tests/test_cdg.cpp; here a disc
// only has to be a disc.
QByteArray cdg_packet(int instruction, const CdgData& data = {}) {
    QByteArray out(24, '\0');
    out[0] = static_cast<char>(9);
    out[1] = static_cast<char>(instruction);
    for (std::size_t i = 0; i < data.size(); ++i) {
        out[static_cast<qsizetype>(4 + i)] = static_cast<char>(data[i]);
    }
    return out;
}

CdgData first_byte(int value) {
    CdgData data{};
    data[0] = static_cast<unsigned char>(value);
    return data;
}

// A disc whose colour 1 is red and colour 2 green: the picture is `first` from the first packets
// and `second` from the packet at one second (300 packets in).
QByteArray cdg_disc(int first = 1, int second = 2) {
    CdgData colours{};
    // Colour 1 is 0xF00 and colour 2 is 0x0F0, as two six-bit bytes each.
    colours[2] = 0x3C;
    colours[3] = 0x00;
    colours[4] = 0x03;
    colours[5] = 0x30;
    QByteArray bytes = cdg_packet(30, colours);
    bytes += cdg_packet(1, first_byte(first));
    for (int i = 2; i < 300; ++i) {
        bytes += cdg_packet(0);
    }
    bytes += cdg_packet(1, first_byte(second));
    return bytes;
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

TEST_CASE("KaraokeController: a CD+G file with no lyrics file is a picture that follows the position",
          "[hearth][hearth-controller][karaoke][lyrics][cdg]") {
    Room room;
    room.write("disc.cdg", cdg_disc());
    KaraokeController controller;
    // Nothing loaded is no picture.
    CHECK(controller.pictureFrame(500).isNull());

    controller.load(room.path("disc.ac3"), 10000);
    REQUIRE(controller.status().toStdString() == "picture");
    CHECK(controller.fileName().toStdString() == "disc.cdg");
    CHECK(controller.errorText().isEmpty());
    // No text is extracted: there are no lines to show or to follow.
    CHECK(controller.lines().isEmpty());
    CHECK(controller.lineCount() == 0);
    CHECK(controller.cursor(1000).value("line").toInt() == -1);

    // Before the first packet the screen is black; by 500 ms the colours are loaded and it is red.
    const QImage start = controller.pictureFrame(0);
    REQUIRE_FALSE(start.isNull());
    CHECK(start.width() == 300);
    CHECK(start.height() == 216);
    CHECK(start.pixel(100, 100) == 0xFF000000U);
    const QImage red = controller.pictureFrame(500);
    CHECK(red.pixel(100, 100) == 0xFFFF0000U);
    // A position between two packets is the same picture, not a new one.
    const QImage same = controller.pictureFrame(502);
    CHECK(same.constBits() == red.constBits());
    // The packet at one second (sent by 1004 ms) turns it green, and going back is going back.
    const QImage green = controller.pictureFrame(1100);
    CHECK(green.pixel(100, 100) == 0xFF00FF00U);
    CHECK(green.constBits() != red.constBits());
    CHECK(controller.pictureFrame(500).pixel(100, 100) == 0xFFFF0000U);
    // Past the end is the last picture.
    CHECK(controller.pictureFrame(600000).pixel(100, 100) == 0xFF00FF00U);

    // The delay shows the picture later, as it does the words: 200 ms of delay at 1100 ms is the
    // picture of 900 ms, and it is green again by 1204 ms.
    controller.setDelayMs(200);
    CHECK(controller.pictureFrame(1100).pixel(100, 100) == 0xFFFF0000U);
    CHECK(controller.pictureFrame(1250).pixel(100, 100) == 0xFF00FF00U);

    // Another disc at a position with the same number of packets is its own picture, not the
    // last one: this one is green first.
    controller.setDelayMs(0);
    CHECK(controller.pictureFrame(500).pixel(100, 100) == 0xFFFF0000U);
    room.write("swap.cdg", cdg_disc(2, 1));
    controller.load(room.path("swap.ac3"), 10000);
    CHECK(controller.pictureFrame(500).pixel(100, 100) == 0xFF00FF00U);

    // Another item with nothing beside it takes the picture away.
    controller.load(room.path("other.ac3"), 10000);
    CHECK(controller.status().toStdString() == "none");
    CHECK(controller.pictureFrame(500).isNull());
}

TEST_CASE("KaraokeController: a lyrics file beside the item is shown in place of a picture",
          "[hearth][hearth-controller][karaoke][lyrics][cdg]") {
    Room room;
    room.write("song.cdg", cdg_disc());
    room.write("song.lrc", kEnhanced);
    KaraokeController controller;
    controller.load(room.path("song.ac3"), 10000);
    CHECK(controller.status().toStdString() == "timed");
    CHECK(controller.fileName().toStdString() == "song.lrc");
    CHECK(controller.pictureFrame(500).isNull());
}

TEST_CASE("KaraokeController: a CD+G file in any case, and one that is not CD+G says why",
          "[hearth][hearth-controller][karaoke][lyrics][cdg]") {
    Room room;
    room.write("shout.CDG", cdg_disc());
    room.write("bad.cdg", QByteArray(25, 'x'));
    KaraokeController controller;

    controller.load(room.path("shout.ac3"), 10000);
    CHECK(controller.status().toStdString() == "picture");
    CHECK(controller.fileName().toLower().toStdString() == "shout.cdg");

    controller.load(room.path("bad.ac3"), 10000);
    CHECK(controller.status().toStdString() == "error");
    CHECK(controller.fileName().toStdString() == "bad.cdg");
    CHECK(controller.errorText().toStdString() ==
          "The file is not CD+G: its length is not a whole number of packets.");
    CHECK(controller.pictureFrame(500).isNull());
}
