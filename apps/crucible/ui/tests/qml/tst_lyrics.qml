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
}
