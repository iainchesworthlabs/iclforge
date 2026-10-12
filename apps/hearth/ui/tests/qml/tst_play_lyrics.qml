import QtQuick
import QtQuick.Window
import QtTest

import Hearth
import HearthTest

import "HearthTestHelpers.js" as H

// The lyrics on the Play page (PlayLyricsCard.qml, PlayPage.qml; planning/karaoke.md, "Found
// while building": a lyric view beside the queue). An item with a lyrics file gets a card: its
// own column beside the queue in a wide window, the first card of the monitor column in a
// narrower one. An item without one gets nothing, and the page is as it was.
//
// Played on the fake device room, in real time: a generated karaoke stream with a lyrics
// file beside it, and one without.
TestCase {
    id: testCase
    name: "PlayLyrics"
    when: windowShown
    width: 1100
    height: 900

    property string sung: ""        // an item with a lyrics file beside it
    property string silent: ""      // an item with none

    Component { id: mainComponent; Main { } }

    function initTestCase() {
        verify(TestServices.useFakeRoom(), "the fake device room could not be installed");
        HearthController.firstRunSeen = true;
        sung = TestServices.writeAc3Stream("sung_song.ac3", "karaoke-3/2");
        silent = TestServices.writeAc3Stream("silent_song.ac3", "karaoke-3/2");
        verify(sung.length > 0 && silent.length > 0, "the streams could not be written");
        // Three 0.5 s words, then a line, inside the stream's ten seconds.
        verify(TestServices.writeTextFile("sung_song.lrc",
               "[00:00.50]<00:00.50>La <00:01.00>la <00:01.50>la\n[00:03.00]A whole line\n").length > 0);
        HearthController.start();
        tryVerify(function() { return Object.keys(HearthController.decoderSettings).length > 0; }, 15000);
    }

    function cleanup() {
        HearthController.stop();
        for (let i = HearthController.queue.length - 1; i >= 0; --i) {
            HearthController.removeAt(i);
        }
        tryVerify(function() { return HearthController.queue.length === 0; }, 10000);
        KaraokeController.load("", 0);
    }

    function named(root, name) {
        const found = H.find(root.contentItem !== undefined ? root.contentItem : root,
                             function(item) { return item.objectName === name; });
        return found !== null ? found : findChild(root, name);
    }

    function openPlayPage(width) {
        const win = createTemporaryObject(mainComponent, testCase);
        verify(win !== null);
        tryCompare(win, "visible", true);
        win.width = width;
        win.height = 900;
        win.page = "play";
        waitForRendering(win.contentItem);
        HearthController.setLayoutText("5.1");
        tryVerify(function() { return HearthController.speakerLabels.length === 6; }, 10000);
        return win;
    }

    function play(path) {
        HearthController.addFiles([path]);
        tryVerify(function() { return HearthController.queue.length === 1; }, 10000);
        HearthController.playItem(0);
        tryCompare(HearthController, "state", "playing", 10000);
    }

    // --- placement ------------------------------------------------------------------

    function test_anItemWithLyricsGetsACardAtTheTopOfTheMonitorInANarrowWindow() {
        const win = openPlayPage(1100);
        const folded = named(win, "playLyricsFolded");
        const beside = named(win, "playLyricsBeside");
        verify(folded !== null && beside !== null, "the lyrics cards are not on the Play page");
        compare(folded.visible, false);
        play(sung);
        tryCompare(KaraokeController, "status", "timed", 10000);
        tryCompare(folded, "visible", true, 5000);
        compare(beside.visible, false);
        // It names the file and shows the same words as the Karaoke page.
        compare(folded.summary, "sung_song.lrc");
        const view = named(folded, "playLyricsView");
        verify(view !== null);
        tryVerify(function() { return view.currentLine >= 0; }, 15000, "the lyrics never started");
    }

    function test_aWideWindowGivesTheLyricsAColumnBesideTheQueue() {
        const win = openPlayPage(1700);
        const folded = named(win, "playLyricsFolded");
        const beside = named(win, "playLyricsBeside");
        play(sung);
        tryCompare(KaraokeController, "status", "timed", 10000);
        tryCompare(beside, "visible", true, 5000);
        compare(folded.visible, false);
        // Beside the queue: to the right of its list and level with it.
        const queue = named(win, "queueList");
        verify(queue !== null);
        const queueRight = queue.mapToItem(null, queue.width, 0).x;
        const cardLeft = beside.mapToItem(null, 0, 0).x;
        verify(cardLeft >= queueRight, "the lyrics are not beside the queue: " + cardLeft + " < " + queueRight);
    }

    function test_theCardMovesWithTheWindowsWidthAndKeepsOneCopyOnScreen() {
        const win = openPlayPage(1700);
        play(sung);
        tryCompare(KaraokeController, "status", "timed", 10000);
        const folded = named(win, "playLyricsFolded");
        const beside = named(win, "playLyricsBeside");
        tryCompare(beside, "visible", true, 5000);
        win.width = 1200;
        tryCompare(folded, "visible", true, 5000);
        compare(beside.visible, false);
    }

    // --- nothing to show ---------------------------------------------------------------

    function test_anItemWithoutLyricsHasNoCardAtAnyWidth() {
        for (const width of [1100, 1700]) {
            const win = openPlayPage(width);
            HearthController.addFiles([silent]);
            tryVerify(function() { return HearthController.queue.length === 1; }, 10000);
            HearthController.playItem(0);
            tryCompare(HearthController, "state", "playing", 10000);
            tryCompare(KaraokeController, "status", "none", 10000);
            wait(300);
            compare(named(win, "playLyricsFolded").visible, false);
            compare(named(win, "playLyricsBeside").visible, false);
            HearthController.stop();
            HearthController.removeAt(0);
            tryVerify(function() { return HearthController.queue.length === 0; }, 10000);
            win.destroy();
        }
    }

    // --- the button ---------------------------------------------------------------------

    function test_theCardsButtonOpensTheSingerView() {
        const win = openPlayPage(1100);
        play(sung);
        tryCompare(KaraokeController, "status", "timed", 10000);
        // The folded card's: a narrow window, so the one beside the queue is not placed.
        const button = named(named(win, "playLyricsFolded"), "playLyricsFullScreenButton");
        verify(button !== null);
        const screen = findChild(win, "lyricScreen");
        compare(screen.visible, false);
        mouseClick(button);
        tryCompare(screen, "visible", true, 5000);
        win.toggleLyricScreen();
        tryCompare(screen, "visible", false, 5000);
    }
}
