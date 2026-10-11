import QtQuick
import QtQuick.Window
import QtTest

import Hearth
import HearthTest

import "HearthTestHelpers.js" as H

// Timing the lyrics (LyricTimingDialog.qml; planning/karaoke-phase2.md, W6).
//
// The session, the estimate, the reaction time and the save are tested where they live
// (libs/lyrics/tests/test_timing.cpp, apps/hearth/ui/tests/test_karaoke_controller.cpp). What is
// held here is what a person has in front of them: that Space stamps the next line, that a press
// is taken to be late by the reaction time, that a guess is told apart from a press, that a save
// writes the item's file and keeps the first one, that a stray Escape does not throw edits away,
// and that an item with no words says how to give it some.
//
// The dialog's own clock is held still (`live: false`, not running) and set with poll(), so a
// press is at a position the test chose, not the one a poll happened to read.
TestCase {
    id: testCase
    name: "KaraokeTiming"
    when: windowShown
    width: 1100
    height: 900

    property string plain: ""       // a plain lyrics file's path, and the item beside it
    property string item: ""
    property string bare: ""        // an item with no lyrics file beside it
    property string words: ""       // a text file to time an item that has none

    Component { id: mainComponent; Main { } }

    function initTestCase() {
        verify(TestServices.useFakeRoom(), "the fake device room could not be installed");
        HearthController.firstRunSeen = true;
        plain = TestServices.writeTextFile("timing_song.lrc",
                "Should auld acquaintance\nBe forgot\nAnd never brought to mind\n");
        verify(plain.length > 0, "the lyrics file could not be written");
        item = plain.replace(/\.lrc$/, ".ac3");
        bare = plain.replace("timing_song.lrc", "timing_bare.ac3");
        words = TestServices.writeTextFile("timing_words.txt", "First line\nSecond line\n");
        verify(words.length > 0);
        HearthController.start();
        tryVerify(function() { return Object.keys(HearthController.decoderSettings).length > 0; }, 15000);
    }

    function init() {
        KaraokeController.reactionMs = 100;
    }

    function cleanup() {
        KaraokeController.endTiming();
        KaraokeController.load("", 0);
        KaraokeController.reactionMs = 120;
        // A saved file is what the next test's item would read: put the plain one back.
        verify(TestServices.writeTextFile("timing_song.lrc",
                "Should auld acquaintance\nBe forgot\nAnd never brought to mind\n").length > 0);
        TestServices.removeFile(plain + ".bak");
        HearthController.stop();
        for (let i = HearthController.queue.length - 1; i >= 0; --i) {
            HearthController.removeAt(i);
        }
        tryVerify(function() { return HearthController.queue.length === 0; }, 10000);
    }

    // The window's items by objectName: the visual tree first, then the QObject tree, then what is
    // inside the dialog (a popup's content is not under the window's content item).
    function named(win, name) {
        const match = function(it) { return it.objectName === name; };
        let found = H.find(win.contentItem, match);
        if (found === null) {
            found = findChild(win, name);
        }
        if (found === null) {
            const dialog = findChild(win, "lyricTimingDialog");
            found = dialog !== null ? H.find(dialog.contentItem, match) : null;
        }
        verify(found !== null, "no item called " + name);
        return found;
    }

    // The main window on the Karaoke page, with the item's words loaded as the page would have
    // them, and the timing dialog open with its clock held at `at` ms.
    function openDialog(path, at) {
        const win = createTemporaryObject(mainComponent, testCase);
        verify(win !== null);
        tryCompare(win, "visible", true);
        win.page = "karaoke";
        waitForRendering(win.contentItem);
        win.requestActivate();
        KaraokeController.load(path, 20000);
        const dialog = named(win, "lyricTimingDialog");
        dialog.open();
        tryCompare(dialog, "visible", true, 5000);
        hold(dialog, at);
        return { win: win, dialog: dialog };
    }

    // The clock stands where `ms` is, and does not advance.
    function hold(dialog, ms) {
        dialog.clock.live = false;
        dialog.clock.running = false;
        dialog.clock.poll(ms);
        compare(dialog.position, ms);
    }

    function line(index) {
        return KaraokeController.timingLines[index];
    }

    function space(win) {
        verify(TestServices.keyClick(win, Qt.Key_Space));
    }

    // --- the line pass ------------------------------------------------------------------------

    function test_aPlainFileOpensWithNoLineStampedAndTheFirstNamed() {
        const open = openDialog(item, 0);
        const win = open.win;
        tryCompare(KaraokeController, "timing", true, 5000);
        compare(KaraokeController.timingLines.length, 3);
        compare(KaraokeController.stampedLines, 0);
        compare(named(win, "timingNext").text, qsTr("Next line: %1").arg("Should auld acquaintance"));
        compare(named(win, "timingProgress").text, qsTr("%1 of %2 lines have a time").arg(0).arg(3));
        compare(named(win, "timingMode").currentValue, "lines");
        verify(named(win, "timingTap").enabled);
        // Nothing is stamped, so there is nothing to nudge or stamp again.
        compare(named(win, "timingEarlier").enabled, false);
        compare(named(win, "timingSave").enabled, false);
        // The dialog does not touch what the lyric view shows.
        compare(KaraokeController.status, "plain");
    }

    function test_spaceStampsTheNextLineAndTheReactionTimeIsTakenOff() {
        const open = openDialog(item, 2100);
        const win = open.win;
        tryCompare(KaraokeController, "timing", true, 5000);

        space(win);
        compare(KaraokeController.stampedLines, 1);
        compare(line(0).startMs, 2000);
        compare(named(win, "timingNext").text, qsTr("Next line: %1").arg("Be forgot"));

        hold(open.dialog, 6100);
        space(win);
        compare(line(1).startMs, 6000);
        compare(KaraokeController.nextLine, 2);
        compare(named(win, "timingProgress").text, qsTr("%1 of %2 lines have a time").arg(2).arg(3));
        // Edited, so Save is on.
        verify(named(win, "timingSave").enabled);
    }

    function test_theTapButtonDoesWhatSpaceDoes() {
        const open = openDialog(item, 3100);
        tryCompare(KaraokeController, "timing", true, 5000);
        mouseClick(named(open.win, "timingTap"));
        compare(line(0).startMs, 3000);
    }

    function test_aChangedReactionTimeChangesWhatAPressStores() {
        const open = openDialog(item, 5000);
        tryCompare(KaraokeController, "timing", true, 5000);
        const spin = named(open.win, "timingReaction");
        compare(spin.value, 100);
        compare(spin.to, KaraokeController.maxReactionMs);
        // Three steps of the arrow key, as a person would take them.
        spin.forceActiveFocus();
        for (let i = 0; i < 3; ++i) {
            verify(TestServices.keyClick(open.win, Qt.Key_Up));
        }
        compare(KaraokeController.reactionMs, 130);
        space(open.win);
        compare(line(0).startMs, 4870);
        // It is the controller's, so a change from elsewhere is shown.
        KaraokeController.reactionMs = 50;
        compare(spin.value, 50);
    }

    function test_theLastLineStampedGoesOnToTheWordsWithAnEstimateToPressOver() {
        const open = openDialog(item, 1100);
        const win = open.win;
        tryCompare(KaraokeController, "timing", true, 5000);
        space(win);
        hold(open.dialog, 5100);
        space(win);
        compare(open.dialog.mode, "lines");
        hold(open.dialog, 9100);
        space(win);

        // Every line has a time: the pass is the words', and each line already has an estimate.
        compare(KaraokeController.nextLine, -1);
        compare(open.dialog.mode, "words");
        compare(named(win, "timingMode").currentValue, "words");
        compare(line(0).words.length, 3);
        compare(line(0).words[1].tapped, false);
        compare(line(0).words[1].startMs > line(0).startMs, true);
        compare(line(0).words[1].startMs < line(1).startMs, true);
        // A guess is drawn slanted and a press solid.
        const guesses = H.findAll(open.dialog.contentItem, function(it) {
            return it.objectName === "timingWord" && it.visible && it.font.italic;
        }, []);
        verify(guesses.length >= 3, "the estimate was not drawn as a guess");
    }

    // --- the word pass ---------------------------------------------------------------------------

    function stampThreeLines(open) {
        space(open.win);
        hold(open.dialog, 5100);
        space(open.win);
        hold(open.dialog, 9100);
        space(open.win);
        compare(open.dialog.mode, "words");
    }

    function test_aPressInTheWordPassSetsTheFirstGuessInTheLineBeingSung() {
        const open = openDialog(item, 1100);
        const win = open.win;
        tryCompare(KaraokeController, "timing", true, 5000);
        stampThreeLines(open);

        // The position is in the first line (it starts at 1.0 s): the next word is its first.
        hold(open.dialog, 1700);
        compare(named(win, "timingNext").text, qsTr("Next word: %1").arg("Should"));
        space(win);
        compare(line(0).words[0].tapped, true);
        compare(line(0).words[0].startMs, 1600);
        // Pressed, so the next is the second word.
        compare(named(win, "timingNext").text, qsTr("Next word: %1").arg("auld"));
        hold(open.dialog, 2500);
        space(win);
        compare(line(0).words[1].startMs, 2400);
        compare(line(0).words[1].tapped, true);
        compare(line(0).words[2].tapped, false);

        // A press in the second line is for the second line's words.
        hold(open.dialog, 5600);
        compare(named(win, "timingNext").text, qsTr("Next word: %1").arg("Be"));
        space(win);
        compare(line(1).words[0].startMs, 5500);
        compare(line(0).words[2].tapped, false);
    }

    function test_beforeTheFirstLineThereIsNoWordToPress() {
        const open = openDialog(item, 1100);
        tryCompare(KaraokeController, "timing", true, 5000);
        stampThreeLines(open);
        hold(open.dialog, 200);
        compare(named(open.win, "timingTap").enabled, false);
        compare(named(open.win, "timingNext").text,
                qsTr("Play to the first line, then press Space as each word starts."));
        space(open.win);
        compare(line(0).words[0].tapped, false);
    }

    function test_goingBackToTheLinesAndOnAgainKeepsWhatWasPressed() {
        const open = openDialog(item, 1100);
        tryCompare(KaraokeController, "timing", true, 5000);
        stampThreeLines(open);
        hold(open.dialog, 1700);
        space(open.win);
        const pressed = line(0).words[0].startMs;
        compare(line(0).words[0].tapped, true);

        // Back to the lines (the segmented control's second cell is "words", the first "lines"),
        // a line stamped again, and on to the words: the estimate is made again for what is not
        // pressed and a press stays where it was put.
        open.dialog.setMode("lines");
        compare(named(open.win, "timingMode").currentValue, "lines");
        open.dialog.setMode("words");
        compare(line(0).words[0].startMs, pressed);
        compare(line(0).words[0].tapped, true);
        compare(line(0).words[1].tapped, false);
    }

    // --- correcting a line ---------------------------------------------------------------------

    function test_aLineCanBeSelectedNudgedAndStampedAgain() {
        const open = openDialog(item, 1100);
        const win = open.win;
        tryCompare(KaraokeController, "timing", true, 5000);
        space(win);
        hold(open.dialog, 5100);
        space(win);

        mouseClick(named(win, "timingLine-1"));
        compare(open.dialog.selectedLine, 1);
        mouseClick(named(win, "timingLater"));
        compare(line(1).startMs, 5010);
        mouseClick(named(win, "timingLater"));
        mouseClick(named(win, "timingEarlier"));
        compare(line(1).startMs, 5010);

        hold(open.dialog, 6600);
        mouseClick(named(win, "timingStampAgain"));
        compare(line(1).startMs, 6500);
        // An unstamped line has nothing to nudge.
        mouseClick(named(win, "timingLine-2"));
        compare(open.dialog.selectedLine, 2);
        compare(named(win, "timingLater").enabled, false);
    }

    // --- saving and leaving ------------------------------------------------------------------------

    function test_saveWritesTheItemsFileKeepsTheFirstAndTheViewReadsIt() {
        const open = openDialog(item, 1100);
        const win = open.win;
        tryCompare(KaraokeController, "timing", true, 5000);
        stampThreeLines(open);
        hold(open.dialog, 1700);
        space(win);

        mouseClick(named(win, "timingSave"));
        compare(KaraokeController.timingDirty, false);
        compare(named(win, "timingSave").enabled, false);
        verify(named(win, "timingNotice").text.indexOf("timing_song.lrc") >= 0, named(win, "timingNotice").text);

        const written = TestServices.readTextFile(plain);
        verify(written.indexOf("[00:01.00]") === 0, written);
        verify(written.indexOf("<00:01.60>Should") >= 0, written);
        compare(TestServices.readTextFile(plain + ".bak"),
                "Should auld acquaintance\nBe forgot\nAnd never brought to mind\n");
        // What was written is what the lyric view now reads.
        compare(KaraokeController.status, "timed");
        compare(KaraokeController.wordTimed, true);
    }

    function test_escapeDoesNotThrowAwayEditsAndTheCloseButtonSaysSo() {
        const open = openDialog(item, 1100);
        const win = open.win;
        tryCompare(KaraokeController, "timing", true, 5000);
        space(win);
        compare(named(win, "timingClose").text, qsTr("Discard and close"));

        verify(TestServices.keyClick(win, Qt.Key_Escape));
        wait(100);
        compare(open.dialog.visible, true);

        mouseClick(named(win, "timingClose"));
        tryCompare(open.dialog, "visible", false, 5000);
        compare(KaraokeController.timing, false);
        // Nothing was written.
        compare(TestServices.readTextFile(plain), "Should auld acquaintance\nBe forgot\nAnd never brought to mind\n");
        compare(TestServices.readTextFile(plain + ".bak"), "");
    }

    function test_withNothingEditedEscapeLeaves() {
        const open = openDialog(item, 0);
        tryCompare(KaraokeController, "timing", true, 5000);
        compare(named(open.win, "timingClose").text, qsTr("Close"));
        verify(TestServices.keyClick(open.win, Qt.Key_Escape));
        tryCompare(open.dialog, "visible", false, 5000);
    }

    // --- an item with no words ---------------------------------------------------------------------

    function test_anItemWithNoWordsSaysHowToGiveItSome() {
        const open = openDialog(bare, 0);
        const win = open.win;
        tryCompare(KaraokeController, "timing", false, 5000);
        const message = named(win, "timingNoWords");
        tryVerify(function() { return message.visible; }, 5000);
        verify(message.text.length > 0);
        verify(named(win, "timingChooseText").visible);
        // The lines and the tap button are not on offer.
        compare(named(win, "timingLines").visible, false);
        compare(named(win, "timingTap").visible, false);

        // A text file given for it (what the file chooser hands over) starts the session.
        verify(KaraokeController.beginTiming(words));
        tryVerify(function() { return named(win, "timingLines").visible; }, 5000);
        compare(KaraokeController.timingLines.length, 2);
        compare(named(win, "timingNext").text, qsTr("Next line: %1").arg("First line"));
        compare(named(win, "timingNoWords").visible, false);
    }

    function test_aTextFileThatIsNotTextTheReaderTakesIsSaidSo() {
        const latin = TestServices.writeTextFile("timing_latin.txt", "café\n", true);
        verify(latin.length > 0);
        const open = openDialog(bare, 0);
        verify(!KaraokeController.beginTiming(latin));
        const message = named(open.win, "timingNoWords");
        tryVerify(function() { return message.visible && message.text.indexOf("UTF-8") >= 0; }, 5000);
    }

    // --- the page ------------------------------------------------------------------------------

    function test_theLyricsCardOffersItOnlyOnceAnItemIsLoaded() {
        const win = createTemporaryObject(mainComponent, testCase);
        verify(win !== null);
        tryCompare(win, "visible", true);
        win.page = "karaoke";
        waitForRendering(win.contentItem);
        const button = named(win, "timeLyricsButton");
        compare(button.enabled, false);

        const stream = TestServices.writeAc3Stream("timing_item.ac3", "karaoke-3/2");
        verify(stream.length > 0);
        HearthController.setLayoutText("5.1");
        tryVerify(function() { return HearthController.speakerLabels.length === 6; }, 10000);
        HearthController.addFiles([stream]);
        tryVerify(function() { return HearthController.queue.length === 1; }, 10000);
        HearthController.playItem(0);
        tryCompare(HearthController, "state", "playing", 10000);
        tryCompare(button, "enabled", true, 10000);

        const dialog = named(win, "lyricTimingDialog");
        compare(dialog.visible, false);
        mouseClick(button);
        tryCompare(dialog, "visible", true, 5000);
        // No lyrics file sits beside it: the dialog says so rather than showing an empty list.
        tryCompare(KaraokeController, "timing", false, 5000);
        tryVerify(function() { return named(win, "timingNoWords").visible; }, 5000);
        dialog.close();
        tryCompare(dialog, "visible", false, 5000);
    }
}
