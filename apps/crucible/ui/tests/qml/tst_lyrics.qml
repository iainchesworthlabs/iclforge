import QtQuick
import QtTest

import Crucible
import CrucibleTest

// The lyrics of an application (CrucibleLyrics, LyricStrip.qml; planning/karaoke-phase2.md, W13b).
//
// The reader and the cursor are tested where they live (libs/lyrics). What is held here is what this
// window makes of them: that an imported file is kept per application and says what it is, that
// Start and the nudge are the whole of the position (an application's own is not read), that the
// clock is an argument and so a test can set it, and that the strip reads the controller and says
// the truth about it.
TestCase {
    id: testCase
    name: "Lyrics"
    when: windowShown
    width: 900
    height: 700

    // Line 1 is word-timed (three words from 1.00 s), line 2 is line-timed, line 3 is word-timed.
    readonly property string song: [
        "[ti:Auld Lang Syne]",
        "[00:01.00]<00:01.00>Should <00:01.50>auld <00:02.00>acquaintance",
        "[00:05.00]And never brought to mind?",
        "[00:08.00]<00:08.00>For <00:08.50>auld <00:09.00>lang <00:09.50>syne",
        ""].join("\n")

    Component { id: stripComponent; LyricStrip { width: 320 } }
    Component { id: roomPage; RoomPage { width: 1480; height: 700 } }

    function cleanup() {
        for (const app of [7, 8, 9]) {
            CrucibleLyrics.clear(app);
        }
    }

    // --- the controller, on a clock of the test's own ------------------------------

    function test_nothingImportedIsNoneAndHasNoPosition() {
        compare(CrucibleLyrics.status(7), "none");
        compare(CrucibleLyrics.lines(7).length, 0);
        compare(CrucibleLyrics.running(7), false);
        compare(CrucibleLyrics.positionMs(7, 5000), -1);
        compare(CrucibleLyrics.cursor(7, 5000).line, -1);
        // Start has nothing to start.
        CrucibleLyrics.startAt(7, 1000);
        compare(CrucibleLyrics.running(7), false);
    }

    function test_aTimedFileIsFollowedFromStartAndTheNudgeMovesIt() {
        verify(CrucibleLyrics.importText(7, "song.lrc", song, 12000));
        compare(CrucibleLyrics.status(7), "timed");
        compare(CrucibleLyrics.fileName(7), "song.lrc");
        const lines = CrucibleLyrics.lines(7);
        compare(lines.length, 3);
        compare(lines[0].text, "Should auld acquaintance");
        compare(lines[0].wordTimed, true);
        compare(lines[1].wordTimed, false);
        // Imported is not started: the song's position is not known until a person says.
        compare(CrucibleLyrics.running(7), false);
        compare(CrucibleLyrics.cursor(7, 3000).line, -1);

        CrucibleLyrics.startAt(7, 10000);
        compare(CrucibleLyrics.running(7), true);
        compare(CrucibleLyrics.positionMs(7, 12250), 2250);
        // Before the first line: the song has begun and nothing is being sung.
        compare(CrucibleLyrics.cursor(7, 10500).line, -1);
        // 1.25 s in: half way through "Should" (1.00 to 1.50 s).
        const early = CrucibleLyrics.cursor(7, 11250);
        compare(early.line, 0);
        compare(early.word, 0);
        verify(Math.abs(early.fraction - 0.5) < 0.001, "fraction " + early.fraction);
        compare(early.fills.length, 3);
        // The line-timed line is current from 5.00 s, with no words to fill.
        const second = CrucibleLyrics.cursor(7, 15500);
        compare(second.line, 1);
        compare(second.word, -1);
        compare(second.fills.length, 0);

        // Words shown later by half a second: at 1.25 s the song is as at 0.75 s, before line one.
        CrucibleLyrics.setNudgeMs(7, 500);
        compare(CrucibleLyrics.nudgeMs(7), 500);
        compare(CrucibleLyrics.cursor(7, 11250).line, -1);
        // Earlier: at 0.60 s the line has started.
        CrucibleLyrics.setNudgeMs(7, -500);
        compare(CrucibleLyrics.cursor(7, 10600).line, 0);
        // A nudge adds to what is there and is held to the limit either way.
        CrucibleLyrics.setNudgeMs(7, 0);
        CrucibleLyrics.nudge(7, 100);
        CrucibleLyrics.nudge(7, 100);
        compare(CrucibleLyrics.nudgeMs(7), 200);
        CrucibleLyrics.setNudgeMs(7, 60000);
        compare(CrucibleLyrics.nudgeMs(7), 10000);
        CrucibleLyrics.setNudgeMs(7, -60000);
        compare(CrucibleLyrics.nudgeMs(7), -10000);
    }

    function test_startAgainBeginsTheSongAgainAndStopLeavesTheLyrics() {
        verify(CrucibleLyrics.importText(7, "song.lrc", song, 12000));
        CrucibleLyrics.startAt(7, 1000);
        compare(CrucibleLyrics.cursor(7, 3500).line, 0);
        // The player was restarted: pressing Start again puts the song at its beginning.
        CrucibleLyrics.startAt(7, 3000);
        compare(CrucibleLyrics.cursor(7, 3500).line, -1);
        compare(CrucibleLyrics.positionMs(7, 3500), 500);
        // A reading from before the press is the beginning, not a negative position.
        compare(CrucibleLyrics.positionMs(7, 2500), 0);
        CrucibleLyrics.stop(7);
        compare(CrucibleLyrics.running(7), false);
        compare(CrucibleLyrics.cursor(7, 9000).line, -1);
        compare(CrucibleLyrics.status(7), "timed");
        compare(CrucibleLyrics.lines(7).length, 3);
    }

    function test_aFileWithNoTimesIsShownWholeAndCannotBeStarted() {
        verify(CrucibleLyrics.importText(7, "plain.txt", "First line\nSecond line\n"));
        compare(CrucibleLyrics.status(7), "plain");
        compare(CrucibleLyrics.lines(7).length, 2);
        CrucibleLyrics.startAt(7, 1000);
        compare(CrucibleLyrics.running(7), false);
        compare(CrucibleLyrics.cursor(7, 2000).line, -1);
    }

    function test_whatIsNotLyricsSaysWhyAndKeepsNone() {
        compare(CrucibleLyrics.importText(7, "empty.lrc", "  \n\n"), false);
        compare(CrucibleLyrics.status(7), "error");
        verify(CrucibleLyrics.errorText(7).length > 0);
        compare(CrucibleLyrics.fileName(7), "empty.lrc");
        compare(CrucibleLyrics.lines(7).length, 0);
        // A file that cannot be read says so, and has no lyrics.
        compare(CrucibleLyrics.importFile(8, "C:/no/such/folder/missing.lrc"), false);
        compare(CrucibleLyrics.status(8), "error");
        compare(CrucibleLyrics.fileName(8), "missing.lrc");
        // Another import replaces the failure.
        verify(CrucibleLyrics.importText(7, "song.lrc", song));
        compare(CrucibleLyrics.status(7), "timed");
        compare(CrucibleLyrics.errorText(7), "");
    }

    function test_applicationsAreIndependentAndLeaveWithTheirApplication() {
        verify(CrucibleLyrics.importText(7, "a.lrc", song));
        verify(CrucibleLyrics.importText(8, "b.lrc", "[00:02.00]Another song\n"));
        CrucibleLyrics.startAt(7, 1000);
        compare(CrucibleLyrics.running(8), false);
        compare(CrucibleLyrics.fileName(7), "a.lrc");
        compare(CrucibleLyrics.fileName(8), "b.lrc");
        CrucibleLyrics.setNudgeMs(8, 300);
        compare(CrucibleLyrics.nudgeMs(7), 0);
        // Only 7 is still here: 8's lyrics, clock and nudge are gone, and 7's are not touched.
        CrucibleLyrics.prune([7]);
        compare(CrucibleLyrics.status(8), "none");
        compare(CrucibleLyrics.nudgeMs(8), 0);
        compare(CrucibleLyrics.status(7), "timed");
        compare(CrucibleLyrics.running(7), true);
        CrucibleLyrics.clear(7);
        compare(CrucibleLyrics.status(7), "none");
        compare(CrucibleLyrics.running(7), false);
    }

    function test_aFileOnDiskIsReadByItsUrlOrItsPath() {
        const path = TestServices.writeTextFile("song.lrc", song);
        verify(path.length > 0, "the file could not be written");
        verify(CrucibleLyrics.importFile(7, "file:///" + path.replace(/\\/g, "/")));
        compare(CrucibleLyrics.fileName(7), "song.lrc");
        compare(CrucibleLyrics.lines(7).length, 3);
        verify(CrucibleLyrics.importFile(8, path));
        compare(CrucibleLyrics.status(8), "timed");
    }

    // --- the other formats, told apart by what is in the file ------------------------

    function test_aSubRipFileIsImportedAsTimedLines() {
        const srt = ["1", "00:00:01,000 --> 00:00:03,000", "Should auld acquaintance", "",
                     "2", "00:00:05,000 --> 00:00:07,500", "And never brought to mind?", ""].join("\n");
        verify(CrucibleLyrics.importText(7, "song.srt", srt, 12000));
        compare(CrucibleLyrics.status(7), "timed");
        const lines = CrucibleLyrics.lines(7);
        compare(lines.length, 2);
        compare(lines[0].text, "Should auld acquaintance");
        compare(lines[0].startMs, 1000);
        compare(lines[0].wordTimed, false);
        compare(lines[1].startMs, 5000);
    }

    function test_anAssScriptIsImportedWithItsSyllableTimes() {
        // {\k50} is half a second (the tag counts hundredths) for the syllable after it.
        const ass = ["[Script Info]", "ScriptType: v4.00+", "", "[Events]",
                     "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text",
                     "Dialogue: 0,0:00:01.00,0:00:04.00,Default,,0,0,0,,{\\k50}Should {\\k50}auld {\\k100}acquaintance",
                     ""].join("\n");
        verify(CrucibleLyrics.importText(7, "song.ass", ass, 12000));
        compare(CrucibleLyrics.status(7), "timed");
        const lines = CrucibleLyrics.lines(7);
        compare(lines.length, 1);
        compare(lines[0].wordTimed, true);
        compare(lines[0].words.length, 3);
        compare(lines[0].words[0].startMs, 1000);
        compare(lines[0].words[1].startMs, 1500);
        compare(lines[0].words[2].startMs, 2000);
    }

    // A Standard MIDI File of one track at 480 ticks to the quarter note and 120 beats a minute (a
    // second is 960 ticks), with three Lyric events: "Hel" at 0, "lo " at half a second and, as a
    // new line, "/world" at one second.
    function midiKaraokeBytes() {
        const bytes = [];
        const push = function(list) { for (let i = 0; i < list.length; ++i) { bytes.push(list[i]); } };
        const text = function(s) { return Array.from(s).map(function(c) { return c.charCodeAt(0); }); };
        const track = [0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20];
        const lyric = function(delta, s) {
            for (const d of delta) { track.push(d); }
            track.push(0xFF, 0x05, s.length);
            for (const c of text(s)) { track.push(c); }
        };
        lyric([0x00], "Hel");
        lyric([0x83, 0x60], "lo ");
        lyric([0x83, 0x60], "/world");
        track.push(0x00, 0xFF, 0x2F, 0x00);
        push([0x4D, 0x54, 0x68, 0x64, 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xE0]);
        push([0x4D, 0x54, 0x72, 0x6B, 0, 0, (track.length >> 8) & 0xFF, track.length & 0xFF]);
        push(track);
        return bytes;
    }

    function test_aMidiKaraokeFileIsImportedFromItsBytes() {
        const path = TestServices.writeBinaryFile("song.kar", midiKaraokeBytes());
        verify(path.length > 0, "the MIDI file could not be written");
        const taken = CrucibleLyrics.importFile(7, path, 12000);
        verify(taken, CrucibleLyrics.errorText(7));
        compare(CrucibleLyrics.status(7), "timed");
        compare(CrucibleLyrics.fileName(7), "song.kar");
        const lines = CrucibleLyrics.lines(7);
        compare(lines.length, 2);
        compare(lines[0].text.trim(), "Hello");
        compare(lines[0].startMs, 0);
        compare(lines[0].words[1].startMs, 500);
        compare(lines[1].text.trim(), "world");
        compare(lines[1].startMs, 1000);
    }

    // --- the strip -----------------------------------------------------------------

    function newStrip(app) {
        const strip = createTemporaryObject(stripComponent, testCase.parent);
        verify(strip !== null);
        strip.app = app;
        return strip;
    }

    function test_theStripAsksForAnApplicationWhenThereIsNone() {
        const strip = newStrip(null);
        compare(strip.appId, -1);
        compare(strip.status, "none");
        compare(findChild(strip, "lyricsNoApp").visible, true);
        compare(findChild(strip, "lyricsImport").visible, false);
    }

    function test_theStripSaysWhatWasImportedAndFollowsOnceStarted() {
        const strip = newStrip({ app: 7 });
        compare(findChild(strip, "lyricsNoApp").visible, false);
        compare(strip.status, "none");
        // Nothing to start until there are lyrics with times.
        compare(findChild(strip, "lyricsStart").visible, false);

        verify(CrucibleLyrics.importText(7, "song.lrc", song, 12000));
        tryCompare(strip, "status", "timed", 5000);
        compare(findChild(strip, "lyricsFile").text, qsTr("%1 · %2 lines").arg("song.lrc").arg(3));
        compare(strip.following, false);
        compare(findChild(strip, "lyricStage").visible, false);

        // The song began 2.25 s ago: the strip is in the first line, at its third word.
        CrucibleLyrics.startAt(7, CrucibleLyrics.nowMs() - 2250);
        tryCompare(strip, "following", true, 5000);
        tryVerify(function() { return strip.currentRow === 0; }, 5000);
        compare(findChild(strip, "lyricStage").visible, true);
        compare(findChild(strip, "lyricsStart").text, qsTr("Start again"));
        // The line being sung is word-timed; the next rows are the line after it.
        compare(findChild(strip, "lyricCurrent").wordTimed, true);
        const next = findChild(strip, "lyricNext");
        verify(next !== null);
        compare(next.text, "And never brought to mind?");

        // The nudge buttons move the clock's offset by their step, and say so.
        findChild(strip, "lyricsLater").clicked();
        compare(CrucibleLyrics.nudgeMs(7), 100);
        compare(findChild(strip, "lyricsNudge").text, "+0.1 s");
        findChild(strip, "lyricsEarlier").clicked();
        findChild(strip, "lyricsEarlier").clicked();
        compare(CrucibleLyrics.nudgeMs(7), -100);
        compare(findChild(strip, "lyricsNudge").text, "-0.1 s");

        // Stop stops the following and keeps the lyrics; Clear drops them.
        findChild(strip, "lyricsStop").clicked();
        tryCompare(strip, "following", false, 5000);
        compare(strip.status, "timed");
        findChild(strip, "lyricsClear").clicked();
        tryCompare(strip, "status", "none", 5000);
    }

    function test_theStripSaysWhyAFileWasNotTakenAndWhenItHasNoTimes() {
        const strip = newStrip({ app: 8 });
        compare(CrucibleLyrics.importText(8, "empty.lrc", "\n"), false);
        tryCompare(strip, "status", "error", 5000);
        verify(findChild(strip, "lyricsFile").text.length > 0);
        compare(findChild(strip, "lyricsFile").text, CrucibleLyrics.errorText(8));
        compare(findChild(strip, "lyricsStart").visible, false);

        verify(CrucibleLyrics.importText(8, "plain.txt", "Just words\n"));
        tryCompare(strip, "status", "plain", 5000);
        verify(findChild(strip, "lyricsFile").text.indexOf("plain.txt") >= 0);
        compare(findChild(strip, "lyricsStart").visible, false);
    }

    function test_theRoomPageHasTheStripAndDropsTheLyricsOfAnApplicationThatLeft() {
        const page = createTemporaryObject(roomPage, testCase);
        verify(page);
        waitForRendering(page);
        verify(findChild(page, "lyricStrip") !== null, "the Room page has no lyrics block");
        // 7 is not in the list the engine published (there is none), so when the list changes its
        // lyrics are not kept for whichever session is given that id next.
        verify(CrucibleLyrics.importText(7, "song.lrc", song));
        compare(CrucibleLyrics.status(7), "timed");
        CrucibleController.appsChanged();
        compare(CrucibleLyrics.status(7), "none");
    }

    function test_theStripFollowsTheApplicationItIsGiven() {
        verify(CrucibleLyrics.importText(7, "a.lrc", song));
        verify(CrucibleLyrics.importText(8, "b.lrc", "[00:02.00]Another song\n"));
        const strip = newStrip({ app: 7 });
        tryCompare(strip, "fileName", "a.lrc", 5000);
        strip.app = { app: 8 };
        tryCompare(strip, "fileName", "b.lrc", 5000);
        compare(strip.lines.length, 1);
    }

    // --- the picture of a CD+G disc (planning/karaoke-phase2.md, W13f and D11) -----------------

    // One 24-byte CD+G packet appended to `bytes`: command 9, the instruction, two parity bytes,
    // sixteen data bytes, four parity bytes.
    function addCdgPacket(bytes, instruction, data) {
        const packet = new Array(24).fill(0);
        packet[0] = 9;
        packet[1] = instruction;
        for (let i = 0; i < data.length; ++i) {
            packet[4 + i] = data[i];
        }
        for (const value of packet) {
            bytes.push(value);
        }
    }

    // Colour 1 is red and colour 2 green; the picture is red from the first packets and green from
    // the packet at one second (300 packets in, applied by 1004 ms).
    function cdgDiscBytes() {
        const colours = new Array(16).fill(0);
        colours[2] = 0x3C;
        colours[4] = 0x03;
        colours[5] = 0x30;
        const bytes = [];
        addCdgPacket(bytes, 30, colours);
        addCdgPacket(bytes, 1, [1]);
        for (let i = 2; i < 300; ++i) {
            addCdgPacket(bytes, 0, []);
        }
        addCdgPacket(bytes, 1, [2]);
        return bytes;
    }

    function test_aCdgFileIsImportedAsAPictureWithNoLinesToFollow() {
        const path = TestServices.writeBinaryFile("disc.cdg", cdgDiscBytes());
        verify(path.length > 0, "the CD+G file could not be written");
        verify(CrucibleLyrics.importFile(7, path), CrucibleLyrics.errorText(7));
        compare(CrucibleLyrics.status(7), "picture");
        compare(CrucibleLyrics.fileName(7), "disc.cdg");
        compare(CrucibleLyrics.errorText(7), "");
        // Its words are pixels: no text is extracted, and the cursor has nothing to say.
        compare(CrucibleLyrics.lines(7).length, 0);
        compare(CrucibleLyrics.cursor(7, 5000).line, -1);
        // A disc is started and followed like a file with times.
        compare(CrucibleLyrics.running(7), false);
        CrucibleLyrics.startAt(7, 10000);
        compare(CrucibleLyrics.running(7), true);
        compare(CrucibleLyrics.positionMs(7, 15000), 5000);
        // Another application has no disc.
        compare(CrucibleLyrics.status(8), "none");
    }

    function test_aCdgFileInAnyCaseIsADiscAndOneThatIsNotCdgSaysWhy() {
        const shout = TestServices.writeBinaryFile("SHOUT.CDG", cdgDiscBytes());
        verify(CrucibleLyrics.importFile(7, shout), CrucibleLyrics.errorText(7));
        compare(CrucibleLyrics.status(7), "picture");

        // 25 bytes is not a whole number of 24-byte packets.
        const bad = TestServices.writeBinaryFile("bad.cdg", new Array(25).fill(120));
        compare(CrucibleLyrics.importFile(8, bad), false);
        compare(CrucibleLyrics.status(8), "error");
        compare(CrucibleLyrics.fileName(8), "bad.cdg");
        verify(CrucibleLyrics.errorText(8).length > 0);
        // And an error is not started.
        CrucibleLyrics.startAt(8, 1000);
        compare(CrucibleLyrics.running(8), false);
    }

    function test_theStripDrawsTheDiscsPictureAtThePositionAndTheNudgeMovesIt() {
        const path = TestServices.writeBinaryFile("disc.cdg", cdgDiscBytes());
        verify(path.length > 0);
        const strip = newStrip({ app: 7 });
        const picture = findChild(strip, "lyricPicture");
        verify(picture !== null);
        compare(picture.visible, false);

        verify(CrucibleLyrics.importFile(7, path), CrucibleLyrics.errorText(7));
        tryCompare(strip, "status", "picture", 5000);
        compare(findChild(strip, "lyricsFile").text,
                qsTr("%1 · a CD+G picture: its words are pixels, so none are read").arg("disc.cdg"));
        // The same Start, Stop and nudge a file with times has, and no words' stage.
        compare(findChild(strip, "lyricsStart").visible, true);
        compare(findChild(strip, "lyricsLater").visible, true);
        compare(findChild(strip, "lyricStage").visible, false);
        tryVerify(function() { return picture.visible && picture.ready; }, 5000);

        // Not started is the first picture, a black screen. The strip is 320 wide, so the 300 by
        // 216 picture is drawn once over, centred.
        waitForRendering(picture);
        // The layout has given it its size by the time a frame has been drawn.
        verify(picture.width > 0 && picture.height > 0, "the picture has no size");
        const mid = Math.round(picture.width / 2);
        const row = Math.round(picture.height / 2);
        let image = grabImage(picture);
        compare(image.red(mid, row), 0);
        compare(image.green(mid, row), 0);

        // A nudge of -0.5 s shows the picture that is 0.5 s further on, which is red; of -1.1 s,
        // past the packet at one second, green. The position is not running, so nothing but the
        // nudge decides which.
        CrucibleLyrics.setNudgeMs(7, -500);
        waitForRendering(picture);
        image = grabImage(picture);
        compare(image.red(mid, row), 255);
        compare(image.green(mid, row), 0);
        CrucibleLyrics.setNudgeMs(7, -1100);
        waitForRendering(picture);
        image = grabImage(picture);
        compare(image.red(mid, row), 0);
        compare(image.green(mid, row), 255);

        // Started long ago, with no nudge, it is past the end: the last picture, green; and going
        // back is back (a nudge of +59 s of a song begun a minute ago is 1 s in).
        CrucibleLyrics.setNudgeMs(7, 0);
        CrucibleLyrics.startAt(7, CrucibleLyrics.nowMs() - 60000);
        tryCompare(strip, "following", true, 5000);
        waitForRendering(picture);
        image = grabImage(picture);
        compare(image.green(mid, row), 255);

        // Stop keeps the disc; Clear drops it and the picture goes.
        findChild(strip, "lyricsStop").clicked();
        tryCompare(strip, "following", false, 5000);
        compare(strip.status, "picture");
        findChild(strip, "lyricsClear").clicked();
        tryCompare(strip, "status", "none", 5000);
        tryCompare(picture, "visible", false, 5000);
    }
}
