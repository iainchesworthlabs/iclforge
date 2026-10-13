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
    Component { id: shell; Main {} }
    // The window's smallest size is 960x620 (Main.qml). Its Room page has that less the header, 64 px
    // at the least, and the status strip, 32 px at the least, so no page in a real window is taller
    // than this one.
    Component { id: smallRoomPage; RoomPage { width: 960; height: 620 - 64 - 32 } }

    function cleanup() {
        for (const app of [7, 8, 9, 900, 901]) {
            CrucibleLyrics.clear(app);
        }
        // And the scripted machine of a case that ran the window, back to the real one.
        CrucibleController.stop();
        TestServices.clear();
        // No session of an earlier case is left for the next to be matched with.
        CrucibleLyrics.applySessionList([], 0);
        // And the events out are off again, with the destination they were given.
        CrucibleLyrics.oscEnabled = false;
        CrucibleLyrics.oscHost = "127.0.0.1";
        CrucibleLyrics.oscPort = 9000;
        CrucibleLyrics.oscLines = true;
        CrucibleLyrics.oscWords = true;
        CrucibleLyrics.oscPosition = false;
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

    function test_aFileInACodePageSaysWhereItIsMadeReadable() {
        // "[00:01.00]Caf" and an e acute as the one byte E9: Latin-1, which is not UTF-8.
        const path = TestServices.writeBinaryFile(
            "latin1.lrc", [0x5b, 0x30, 0x30, 0x3a, 0x30, 0x31, 0x2e, 0x30, 0x30, 0x5d, 0x43, 0x61, 0x66, 0xe9, 0x0a]);
        verify(path.length > 0);
        compare(CrucibleLyrics.importFile(8, path), false);
        compare(CrucibleLyrics.status(8), "error");
        // The reader's reason, and then the sentence that names the command and its key.
        verify(CrucibleLyrics.errorText(8).indexOf("not UTF-8") >= 0, CrucibleLyrics.errorText(8));
        verify(CrucibleLyrics.errorText(8).indexOf("forge lyrics-to-lrc") >= 0, CrucibleLyrics.errorText(8));
        verify(CrucibleLyrics.errorText(8).indexOf("lyrics-encoding=") >= 0, CrucibleLyrics.errorText(8));
        // Another reason is not followed by it.
        compare(CrucibleLyrics.importText(7, "empty.lrc", "  \n"), false);
        compare(CrucibleLyrics.errorText(7).indexOf("forge lyrics-to-lrc"), -1);
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

    // The Room puts the strip in a rail 280 to 320 pixels wide. A row of buttons that cannot be
    // narrower than all of them side by side made the strip's column, and so the rail's, wider than
    // the window, with every card in the rail cut off at its right edge (found taking the picture of
    // the strip). Whatever the font draws, the column the strip lays out in keeps the strip's own
    // width and the buttons wrap.
    function test_theStripsControlsWrapAndNeverMakeItsColumnWiderThanIt() {
        const strip = newStrip({ app: 7 });
        strip.width = 280;
        verify(CrucibleLyrics.importText(7, "song.lrc", song, 12000));
        // By hand, which is the state with the most buttons: Start again, Stop, Follow the media
        // session, and the three of the nudge.
        CrucibleLyrics.startAt(7, CrucibleLyrics.nowMs() - 2250);
        tryCompare(strip, "following", true, 5000);
        tryVerify(function() { return findChild(strip, "lyricsUseSession").visible; }, 5000);
        const controls = findChild(strip, "lyricsControls");
        verify(controls.visible);
        waitForItemPolished(strip);
        verify(controls.parent.width <= strip.width, "the column is " + controls.parent.width + " in a strip of " + strip.width);
        verify(controls.width <= strip.width, "the controls are " + controls.width + " in a strip of " + strip.width);
        // The nudge is still all there, in the order a person tabs through it.
        verify(findChild(strip, "lyricsEarlier").visible);
        verify(findChild(strip, "lyricsNudge").visible);
        verify(findChild(strip, "lyricsLater").visible);
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

    // The right rail ends with the Lyrics block, so it is the first thing a short window cuts off.
    // What the rail owes a person at the window's smallest size, whichever way it is reached: that it
    // scrolls, that it says so, and that the strip's controls can be brought into view by the wheel,
    // by Tab and by Page Down. `rail` is the rail's scrolling area and `strip` the lyric strip in it,
    // with timed lyrics imported against its application.
    function checkTheRailReachesTheStrip(rail, strip) {
        const start = findChild(strip, "lyricsStart");
        verify(start !== null);
        tryVerify(function() { return start.visible; }, 5000);
        tryVerify(function() { return rail.contentHeight > rail.height; }, 5000,
                  "the rail's content is taller than the rail: " + rail.contentHeight + " in " + rail.height);

        // Where the control is in the rail's own window onto its content: it is seen when it is
        // between 0 and the window's height.
        function top(item) { return item.mapToItem(rail, 0, 0).y; }
        function bottom(item) { return top(item) + item.height; }
        function seen(item) { return top(item) >= 0 && bottom(item) <= rail.height; }

        // The rail is at its top, and Start is below its lower edge: what the window cut off.
        compare(rail.contentY, 0);
        verify(!seen(start), "Start is below the fold before the rail is scrolled: " + top(start) + " in " + rail.height);

        // The rail says there is more of it: a bar that is on, not one that waits to be hovered.
        const bar = findChild(rail.parent, "railScrollBar");
        verify(bar !== null, "the rail has no scroll bar");
        verify(bar.visible, "the scroll bar is not shown");
        verify(bar.size > 0 && bar.size < 1, "the bar's handle is a part of its track: " + bar.size);

        // The wheel moves it, a notch at a time, and not past its content. Under the harness's
        // synthetic events the first notch can be taken without moving the Flickable (it reports
        // moving, and contentY stays put), so the notches go on until it has moved rather than one
        // being waited for.
        function most() { return rail.contentHeight - rail.height; }
        for (let notch = 0; notch < 60 && rail.contentY < most(); ++notch) {
            mouseWheel(rail, rail.width / 2, rail.height / 2, 0, -120);
            wait(30);
        }
        verify(rail.contentY > 0, "the wheel did not move the rail");
        // It comes to rest at the end of its content and not beyond it.
        tryVerify(function() { return !rail.moving && Math.abs(rail.contentY - most()) < 0.5; }, 5000,
                  "the wheel did not take it to the end and stop there: " + rail.contentY + " of " + most());
        tryVerify(function() { return seen(start); }, 5000,
                  "Start is not on screen at the end of the rail: " + top(start) + " in " + rail.height);

        // Tab walks the rail in the order it always did - Import, Clear, Start - and the control it
        // arrives at is on screen, whichever end of the rail the walk began from.
        rail.contentY = 0;
        const choose = findChild(strip, "lyricsImport");
        const clear = findChild(strip, "lyricsClear");
        verify(choose !== null && clear !== null);
        choose.forceActiveFocus();
        verify(choose.activeFocus);
        tryVerify(function() { return seen(choose); }, 5000, "Import is not on screen with the keyboard on it");
        keyClick(Qt.Key_Tab);
        verify(clear.activeFocus, "Tab from Import reaches Clear");
        keyClick(Qt.Key_Tab);
        verify(start.activeFocus, "Tab from Clear reaches Start");
        tryVerify(function() { return seen(start); }, 5000, "Start is not on screen with the keyboard on it");
        // And back up, from the other end of the rail.
        rail.contentY = most();
        keyClick(Qt.Key_Backtab);
        verify(clear.activeFocus, "Shift+Tab from Start reaches Clear");
        tryVerify(function() { return seen(clear); }, 5000, "Clear is not on screen with the keyboard on it");
        // The rail's first control, which is off its top when the rail is at its end: the keyboard
        // going to it brings the top of the rail back. The tab stops are found in the order they are
        // in the tree, which is the order Tab walks them.
        function stops(item, found) {
            for (let i = 0; i < item.children.length; ++i) {
                const child = item.children[i];
                if (child.activeFocusOnTab === true && child.visible && child.enabled) {
                    found.push(child);
                }
                stops(child, found);
            }
            return found;
        }
        const all = stops(rail.contentItem, []);
        verify(all.length >= 3, "the rail has its tab stops: " + all.length);
        rail.contentY = most();
        all[0].forceActiveFocus();
        tryVerify(function() { return seen(all[0]); }, 5000, "the rail's first control is not on screen with the keyboard on it");

        // Page Down and Page Up with the keyboard in the rail move it by most of a page.
        rail.contentY = 0;
        start.forceActiveFocus();
        rail.contentY = 0;
        keyClick(Qt.Key_PageDown);
        tryVerify(function() { return rail.contentY > 0; }, 5000, "Page Down did not move the rail");
        verify(rail.contentY <= rail.height, "Page Down moved by more than a page: " + rail.contentY);
        keyClick(Qt.Key_PageUp);
        tryCompare(rail, "contentY", 0, 5000);
    }

    // The Room page at the size the smallest window gives it, with no engine under it.
    function test_theRightRailScrollsSoTheStripsStartIsReachableInTheSmallestWindow() {
        const page = createTemporaryObject(smallRoomPage, testCase.parent);
        verify(page);
        waitForRendering(page);
        const rail = findChild(page, "railScroll");
        verify(rail !== null, "the right rail has no scrolling area");
        const strip = findChild(page, "lyricStrip");
        verify(strip !== null);
        // Timed lyrics against an application, so that the strip has its Start button.
        verify(CrucibleLyrics.importText(7, "song.lrc", song, 12000));
        strip.app = { app: 7, name: "Spotify" };
        tryCompare(strip, "status", "timed", 5000);
        checkTheRailReachesTheStrip(rail, strip);
    }

    // The window itself at its minimum, over a scripted machine, with the application chosen the way a
    // person chooses it.
    function test_theWindowAtItsMinimumSizeCanBringTheLyricStripIntoView() {
        if (!TestServices.scriptSessions([{ app: 900, name: "Hearth", active: true },
                                          { app: 901, name: "Microsoft Edge", active: true }])) {
            skip("the scripted machine is not available in this harness");
        }
        CrucibleController.firstRunAcknowledged = true;
        const window = createTemporaryObject(shell, testCase, { suppressFirstRun: true });
        verify(window);
        tryCompare(window, "visible", true);
        if (!CrucibleController.running) {
            skip("the engine did not run over the scripted machine: " + CrucibleController.lastError);
        }
        tryVerify(function() { return CrucibleController.apps.length === 2; }, 5000,
                  "the scripted applications reach the room");
        window.width = window.minimumWidth;
        window.height = window.minimumHeight;
        tryCompare(window, "height", 620, 5000);
        compare(window.width, 960);
        const rail = findChild(window, "railScroll");
        verify(rail !== null, "the right rail has no scrolling area");
        const strip = findChild(window, "lyricStrip");
        verify(strip !== null);
        window.selectApplication(901);
        compare(strip.appId, 901);
        verify(CrucibleLyrics.importText(901, "song.lrc", song, 12000));
        tryCompare(strip, "status", "timed", 5000);
        checkTheRailReachesTheStrip(rail, strip);
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

    // `crucible --shot --lyrics Name=<file>@<time>` (main.cpp) finds the application by its name in
    // the list the engine published and selects it through the window's selectApplication(), which
    // the strip follows, and then imports the file and starts the strip by hand. The option's own
    // parsing is plain Catch2 (test_capture_lyrics.cpp); this holds the window's half of it.
    function test_theCaptureHookSelectsTheApplicationTheStripIsFor() {
        if (!TestServices.scriptSessions([{ app: 900, name: "Hearth", active: true },
                                          { app: 901, name: "Microsoft Edge", active: true }])) {
            skip("the scripted machine is not available in this harness");
        }
        CrucibleController.firstRunAcknowledged = true;
        // The window starts the engine itself, as it does for a capture.
        const window = createTemporaryObject(shell, testCase, { suppressFirstRun: true });
        verify(window);
        tryCompare(window, "visible", true);
        if (!CrucibleController.running) {
            skip("the engine did not run over the scripted machine: " + CrucibleController.lastError);
        }
        tryVerify(function() { return CrucibleController.apps.length === 2; }, 5000,
                  "the scripted applications reach the room");
        const strip = findChild(window, "lyricStrip");
        verify(strip, "the Room page has the strip");
        // Nothing is selected on a fresh window, and the strip asks for an application.
        compare(strip.appId, -1);
        compare(findChild(strip, "lyricsNoApp").visible, true);

        // The option's steps, in its order, over one of the two applications.
        window.selectApplication(901);
        compare(strip.appId, 901);
        compare(strip.app.name, "Microsoft Edge");
        verify(CrucibleLyrics.importFile(901, TestServices.writeTextFile("capture.lrc", song)));
        // 1.25 s into the song, which is in the first line (1.00 to 5.00 s). The word it is at
        // moves on in a quarter of a second, too short a moment for a case to wait for.
        CrucibleLyrics.startAt(901, CrucibleLyrics.nowMs() - 1250);
        tryCompare(strip, "status", "timed", 5000);
        tryCompare(strip, "following", true, 5000);
        tryVerify(function() { return strip.currentRow === 0; }, 5000, "the first line is being sung");
        verify(findChild(strip, "lyricStage").visible, "the line being sung is drawn");
        compare(strip.sessionState, "hand");

        // Another application is another strip, with no lyrics of this one's.
        window.selectApplication(900);
        compare(strip.appId, 900);
        compare(strip.status, "none");
        compare(CrucibleLyrics.status(901), "timed");
    }

    // --- the position from a media session (planning/karaoke-phase2.md, D9) ------------------------
    // The reader is switched off in this suite (qml_test_main.cpp), so a machine's own music does
    // not move a case; what the system would have read is given to the controller as a script, at
    // the time of the case's own choosing.

    // One session as the script says it: the position is as of the `now` it is applied at.
    function session(id, name, title, positionMs, playback) {
        return {
            id: id, name: name, executable: id.toLowerCase().replace(/\.exe$/, ""),
            title: title, artist: "Anon", durationMs: 180000, hasPosition: true,
            positionMs: positionMs, rate: 1.0, playback: playback || "playing"
        };
    }

    function importForSpotify(app) {
        verify(CrucibleLyrics.importText(app, "song.lrc", song, 12000));
        CrucibleLyrics.setApplication(app, "Spotify", "C:\\Users\\a\\AppData\\Roaming\\Spotify\\Spotify.exe", "");
    }

    function test_aMediaSessionIsThePositionWhenTheApplicationPublishesOne() {
        importForSpotify(7);
        // Read, and nothing playing: waiting for one, and not started.
        CrucibleLyrics.applySessionList([], 100000);
        compare(CrucibleLyrics.positionSource(7), "session");
        compare(CrucibleLyrics.sessionState(7), "waiting");
        compare(CrucibleLyrics.running(7), false);
        compare(CrucibleLyrics.positionMs(7, 100000), -1);

        // It publishes: followed with no press of Start, from the player's own position, carried
        // forward at its rate until the next read.
        CrucibleLyrics.applySessionList([session("Spotify.exe", "Spotify", "Auld Lang Syne", 2250)], 100000);
        compare(CrucibleLyrics.sessionState(7), "following");
        compare(CrucibleLyrics.sessionLabel(7), "Spotify");
        compare(CrucibleLyrics.sessionTrack(7), "Auld Lang Syne - Anon");
        compare(CrucibleLyrics.running(7), true);
        compare(CrucibleLyrics.positionMs(7, 100000), 2250);
        compare(CrucibleLyrics.positionMs(7, 101000), 3250);
        // 2.25 s in is the third word of the first line.
        const at = CrucibleLyrics.cursor(7, 100000);
        compare(at.line, 0);
        compare(at.word, 2);
        // The nudge is still on top of whichever origin.
        CrucibleLyrics.setNudgeMs(7, 1000);
        compare(CrucibleLyrics.cursor(7, 100000).positionMs, 1250);
        CrucibleLyrics.setNudgeMs(7, 0);

        // A pause in the player holds the words where they were: what a clock cannot see.
        CrucibleLyrics.applySessionList([session("Spotify.exe", "Spotify", "Auld Lang Syne", 4000, "paused")], 102000);
        compare(CrucibleLyrics.positionMs(7, 107000), 4000);
        compare(CrucibleLyrics.running(7), true);
        // And a seek in it is followed: 8.5 s is in the last line.
        CrucibleLyrics.applySessionList([session("Spotify.exe", "Spotify", "Auld Lang Syne", 8500)], 108000);
        compare(CrucibleLyrics.positionMs(7, 108000), 8500);
        compare(CrucibleLyrics.cursor(7, 108000).line, 2);
    }

    function test_theSessionOfAnotherApplicationIsOfferedAndNotFollowedUnlessChosen() {
        importForSpotify(7);
        CrucibleLyrics.applySessionList([session("chrome.exe", "Chrome", "Another Song", 1000)], 50000);
        compare(CrucibleLyrics.sessionState(7), "waiting");
        compare(CrucibleLyrics.running(7), false);
        const choices = CrucibleLyrics.sessions(7);
        compare(choices.length, 1);
        compare(choices[0].id, "chrome.exe");
        compare(choices[0].matched, false);

        // Chosen by a person, it is the one followed, and held while it is there.
        CrucibleLyrics.useSession(7, "chrome.exe");
        compare(CrucibleLyrics.sessionState(7), "following");
        compare(CrucibleLyrics.sessionLabel(7), "Chrome");
        compare(CrucibleLyrics.sessions(7)[0].matched, true);
    }

    function test_aSessionThatGoesAwayIsCarriedOnByHandFromWhereItWas() {
        importForSpotify(7);
        CrucibleLyrics.applySessionList([session("Spotify.exe", "Spotify", "Auld Lang Syne", 2000)], 10000);
        compare(CrucibleLyrics.sessionState(7), "following");
        // The player quits 3 s later: the words do not jump or stop, they go on by the clock.
        CrucibleLyrics.applySessionList([], 13000);
        compare(CrucibleLyrics.sessionState(7), "lost");
        compare(CrucibleLyrics.positionSource(7), "hand");
        compare(CrucibleLyrics.running(7), true);
        compare(CrucibleLyrics.positionMs(7, 13000), 5000);
        compare(CrucibleLyrics.positionMs(7, 14000), 6000);

        // A session that was paused when it went has no time to carry on with: held, stopped.
        importForSpotify(8);
        CrucibleLyrics.applySessionList([session("Spotify.exe", "Spotify", "Auld Lang Syne", 2000, "paused")], 20000);
        compare(CrucibleLyrics.sessionState(8), "following");
        CrucibleLyrics.applySessionList([], 25000);
        compare(CrucibleLyrics.sessionState(8), "lost");
        compare(CrucibleLyrics.running(8), false);
    }

    function test_aNewTrackIsNotFollowedAgainstTheLyricsOfTheLast() {
        importForSpotify(7);
        CrucibleLyrics.applySessionList([session("Spotify.exe", "Spotify", "Auld Lang Syne", 2000)], 10000);
        compare(CrucibleLyrics.sessionState(7), "following");
        // The same track a read later is the same track.
        CrucibleLyrics.applySessionList([session("Spotify.exe", "Spotify", "Auld Lang Syne", 3000)], 11000);
        compare(CrucibleLyrics.sessionState(7), "following");
        // Another title: the lyrics are for the last, and are not followed against this.
        CrucibleLyrics.applySessionList([session("Spotify.exe", "Spotify", "Another Song", 500)], 12000);
        compare(CrucibleLyrics.sessionState(7), "changed");
        compare(CrucibleLyrics.running(7), false);
        compare(CrucibleLyrics.positionMs(7, 12000), -1);
        compare(CrucibleLyrics.sessionTrack(7), "Another Song - Anon");
        // Following it anyway binds the lyrics to this track.
        CrucibleLyrics.useSession(7);
        compare(CrucibleLyrics.sessionState(7), "following");
        compare(CrucibleLyrics.positionMs(7, 12000), 500);
    }

    function test_startIsByHandAndLetsGoOfTheSessionUntilItIsFollowedAgain() {
        importForSpotify(7);
        CrucibleLyrics.applySessionList([session("Spotify.exe", "Spotify", "Auld Lang Syne", 2000)], 10000);
        CrucibleLyrics.startAt(7, 30000);
        compare(CrucibleLyrics.positionSource(7), "hand");
        compare(CrucibleLyrics.sessionState(7), "hand");
        compare(CrucibleLyrics.positionMs(7, 31500), 1500);
        // A later read does not take it back.
        CrucibleLyrics.applySessionList([session("Spotify.exe", "Spotify", "Auld Lang Syne", 9000)], 31000);
        compare(CrucibleLyrics.positionMs(7, 31500), 1500);
        // Stop stops it, and it stays stopped through the next read.
        CrucibleLyrics.stop(7);
        compare(CrucibleLyrics.running(7), false);
        CrucibleLyrics.applySessionList([session("Spotify.exe", "Spotify", "Auld Lang Syne", 9500)], 32000);
        compare(CrucibleLyrics.running(7), false);
        // The session again.
        CrucibleLyrics.useSession(7);
        compare(CrucibleLyrics.sessionState(7), "following");
        compare(CrucibleLyrics.positionMs(7, 32000), 9500);
    }

    function test_aSessionThatPublishesNoPositionCannotBeFollowed() {
        importForSpotify(7);
        const stream = session("Spotify.exe", "Spotify", "Radio", 0);
        stream.hasPosition = false;
        CrucibleLyrics.applySessionList([stream], 10000);
        compare(CrucibleLyrics.sessionState(7), "no-timeline");
        compare(CrucibleLyrics.running(7), false);
        compare(CrucibleLyrics.positionMs(7, 10000), -1);
    }

    function test_theStripSaysWhereThePositionComesFromAndOffersWhatIsPlaying() {
        const strip = newStrip({ app: 7, name: "Spotify", imagePath: "C:\\x\\Spotify.exe", appId: "" });
        verify(CrucibleLyrics.importText(7, "song.lrc", song, 12000));
        CrucibleLyrics.applySessionList([session("chrome.exe", "Chrome", "Another Song", 1000)], 10000);
        tryCompare(strip, "sessionState", "waiting", 5000);
        // Nothing matched; one is playing: it is offered as a button.
        tryVerify(function() { return findChild(strip, "lyricsSessions").visible; }, 5000);
        const offered = findChild(strip, "lyricsSession");
        verify(offered !== null);
        compare(offered.text, "Chrome \u00b7 Another Song");

        // Spotify starts: matched by its executable, followed with no press, and the strip says so.
        CrucibleLyrics.applySessionList([session("Spotify.exe", "Spotify", "Auld Lang Syne", 2250)], CrucibleLyrics.nowMs());
        tryCompare(strip, "following", true, 5000);
        compare(strip.sessionState, "following");
        compare(findChild(strip, "lyricsHint").text,
                qsTr("Following %1: %2").arg("Spotify").arg("Auld Lang Syne - Anon"));
        compare(findChild(strip, "lyricsSessions").visible, false);
        compare(findChild(strip, "lyricsUseSession").visible, false);

        // Start is by hand, and the strip offers the session back.
        findChild(strip, "lyricsStart").clicked();
        tryCompare(strip, "positionSource", "hand", 5000);
        compare(findChild(strip, "lyricsUseSession").visible, true);
        findChild(strip, "lyricsUseSession").clicked();
        tryCompare(strip, "positionSource", "session", 5000);
    }

    // --- events out over OSC (planning/karaoke-phase2.md, D9's alternative) ----------------------

    function test_theEventsOutAreOffUntilTurnedOnAndAnAddressThatIsNotIpv4IsSaid() {
        compare(CrucibleLyrics.oscEnabled, false);
        compare(CrucibleLyrics.oscHost, "127.0.0.1");
        compare(CrucibleLyrics.oscPort, 9000);
        compare(CrucibleLyrics.oscLines, true);
        compare(CrucibleLyrics.oscWords, true);
        compare(CrucibleLyrics.oscPosition, false);
        compare(CrucibleLyrics.oscProblem, "");

        // A name is not resolved (nothing here may block on DNS), and says so; "localhost" is this machine.
        CrucibleLyrics.oscHost = "example.com";
        verify(CrucibleLyrics.oscProblem.length > 0);
        CrucibleLyrics.oscHost = "300.1.1.1";
        verify(CrucibleLyrics.oscProblem.length > 0);
        CrucibleLyrics.oscHost = "localhost";
        compare(CrucibleLyrics.oscProblem, "");
        CrucibleLyrics.oscHost = "192.168.1.20";
        compare(CrucibleLyrics.oscProblem, "");

        // A port is held to 1 to 65535.
        CrucibleLyrics.oscPort = 70000;
        compare(CrucibleLyrics.oscPort, 65535);
        CrucibleLyrics.oscPort = 0;
        compare(CrucibleLyrics.oscPort, 1);
    }

    function test_whatTheEventsWouldSayIsReadableWithoutASocket() {
        importForSpotify(7);
        CrucibleLyrics.startAt(7, 10000);
        // 1.25 s in: the first line, at its first word.
        const first = CrucibleLyrics.oscMessagesAt(11250);
        compare(first[0], "/crucible/lyrics/state Spotify following");
        verify(first[1].indexOf("/crucible/lyrics/line Spotify 0 Should auld acquaintance 1000 ") === 0, first[1]);
        verify(first[2].indexOf("/crucible/lyrics/word Spotify 0 0 Should 1000 1500") === 0, first[2]);
        // The same moment says nothing again; 1.6 s in is the second word.
        compare(CrucibleLyrics.oscMessagesAt(11260).length, 0);
        const next = CrucibleLyrics.oscMessagesAt(11600);
        compare(next.length, 1);
        verify(next[0].indexOf("/crucible/lyrics/word Spotify 0 1 auld 1500 2000") === 0, next[0]);
    }

    function test_theLyricEventsArriveOverUdpAtTheDestinationChosen() {
        const port = TestServices.oscListen();
        verify(port > 0, "no loopback port could be had");
        CrucibleLyrics.oscHost = "127.0.0.1";
        CrucibleLyrics.oscPort = port;
        CrucibleLyrics.oscEnabled = true;
        importForSpotify(7);
        // The song began 1.25 s ago.
        CrucibleLyrics.startAt(7, CrucibleLyrics.nowMs() - 1250);
        tryVerify(function() { return TestServices.oscReceived().length >= 4; }, 5000);
        const got = TestServices.oscReceived();
        compare(got[0], "/crucible/lyrics/state Spotify following");
        verify(got[1].indexOf("/crucible/lyrics/line Spotify 0 Should auld acquaintance 1000 ") === 0, got[1]);
        verify(got[2].indexOf("/crucible/lyrics/word Spotify 0 0 Should 1000 1500") === 0, got[2]);
        // The next word follows on the song's own time, a quarter of a second later.
        verify(got[3].indexOf("/crucible/lyrics/word Spotify 0 1 auld 1500 2000") === 0, got[3]);

        // Stop says stopped; clearing the lyrics says cleared.
        CrucibleLyrics.stop(7);
        tryVerify(function() {
            return TestServices.oscReceived().indexOf("/crucible/lyrics/state Spotify stopped") >= 0;
        }, 5000);
        CrucibleLyrics.clear(7);
        tryVerify(function() {
            return TestServices.oscReceived().indexOf("/crucible/lyrics/state Spotify cleared") >= 0;
        }, 5000);
    }

    function test_nothingIsSentWhileTheEventsAreOff() {
        const port = TestServices.oscListen();
        verify(port > 0);
        CrucibleLyrics.oscHost = "127.0.0.1";
        CrucibleLyrics.oscPort = port;
        importForSpotify(7);
        CrucibleLyrics.startAt(7, CrucibleLyrics.nowMs() - 1250);
        // A moment for what would have been sent to have been; the socket stays empty.
        wait(300);
        compare(TestServices.oscReceived().length, 0);
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
