import QtQuick
import QtQuick.Window
import QtTest

import Hearth
import HearthTest

import "HearthTestHelpers.js" as H

// What the singer view (LyricScreen.qml, F11) lets a room do without leaving it, beyond the
// vocals and the transport K5 gave it (tst_karaoke_lyrics.qml): move the item by pressing the
// bar or with the arrow keys, see what plays next, and open the queue and pick from it
// (planning/karaoke.md, "Found while building": seeking and a queue in the singer view).
//
// Played on the fake device room, in real time: two generated karaoke streams of ten seconds.
TestCase {
    id: testCase
    name: "SingerView"
    when: windowShown
    width: 1100
    height: 900

    property string first: ""
    property string second: ""

    Component { id: mainComponent; Main { } }

    function initTestCase() {
        verify(TestServices.useFakeRoom(), "the fake device room could not be installed");
        HearthController.firstRunSeen = true;
        first = TestServices.writeAc3Stream("first_song.ac3", "karaoke-3/2");
        second = TestServices.writeAc3Stream("second_song.ac3", "karaoke-3/2");
        verify(first.length > 0 && second.length > 0, "the streams could not be written");
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
        const start = root.contentItem !== undefined ? root.contentItem : root;
        const found = H.find(start, function(item) { return item.objectName === name; });
        return found !== null ? found : findChild(root, name);
    }

    // Both streams in the queue, the first playing, the singer view open.
    function open(paused) {
        const win = createTemporaryObject(mainComponent, testCase);
        verify(win !== null);
        tryCompare(win, "visible", true);
        HearthController.setLayoutText("5.1");
        tryVerify(function() { return HearthController.speakerLabels.length === 6; }, 10000);
        HearthController.addFiles([first, second]);
        tryVerify(function() { return HearthController.queue.length === 2; }, 10000);
        HearthController.playItem(0);
        tryCompare(HearthController, "state", "playing", 10000);
        tryVerify(function() { return HearthController.durationMs > 5000; }, 10000, "no duration");
        if (paused) {
            HearthController.pause();
            tryCompare(HearthController, "state", "paused", 10000);
        }
        const screen = findChild(win, "lyricScreen");
        verify(screen !== null);
        win.openLyricScreen();
        tryCompare(screen, "visible", true, 5000);
        return { win: win, screen: screen };
    }

    function near(actual, wanted, within) {
        return Math.abs(actual - wanted) <= within;
    }

    // --- seeking ------------------------------------------------------------------

    function test_pressingTheBarMovesTheItemToThatPlace() {
        const view = open(false);
        const area = named(view.screen.contentItem, "lyricScreenSeekArea");
        verify(area !== null, "no seek bar in the singer view");
        tryVerify(function() { return area.enabled; }, 5000);
        const target = HearthController.durationMs * 0.75;
        mouseClick(area, area.width * 0.75, area.height / 2);
        tryVerify(function() { return near(HearthController.positionMs, target, 2500); }, 10000,
                  "the position " + HearthController.positionMs + " never reached ~" + target);
        // A seek leaves the transport as it was: this one was playing.
        compare(HearthController.state, "playing");
        compare(view.screen.scrubbing, false);
    }

    function test_aPausedItemMovedByTheBarStaysPausedAndShowsThePlace() {
        // The view shows the place it was moved to at once, without waiting for a poll, and the
        // item stays paused: a seek does not start or stop anything.
        const view = open(true);
        const area = named(view.screen.contentItem, "lyricScreenSeekArea");
        tryVerify(function() { return area.enabled; }, 5000);
        const target = HearthController.durationMs * 0.75;
        mouseClick(area, area.width * 0.75, area.height / 2);
        const title = named(view.screen.contentItem, "lyricScreenTitle");
        tryVerify(function() {
            return title.text.indexOf(view.screen.clock(target) + " /") >= 0;
        }, 5000, "the title does not show the place moved to: " + title.text);
        compare(HearthController.state, "paused");
        HearthController.play();
        tryCompare(HearthController, "state", "playing", 10000);
        tryVerify(function() { return near(HearthController.positionMs, target, 2500); }, 10000,
                  "the item did not start where it was moved to: " + HearthController.positionMs);
    }

    function test_theBarShowsWhereItIsHeldAndMovesTheItemOnlyWhenLetGo() {
        const view = open(true);
        const area = named(view.screen.contentItem, "lyricScreenSeekArea");
        tryVerify(function() { return area.enabled; }, 5000);
        const before = HearthController.positionMs;
        const target = HearthController.durationMs * 0.5;
        mousePress(area, area.width * 0.25, area.height / 2);
        compare(view.screen.scrubbing, true);
        mouseMove(area, area.width * 0.5, area.height / 2);
        tryVerify(function() { return near(view.screen.scrubMs, target, 200); }, 5000);
        // The title says the place held; the item has not moved.
        const title = named(view.screen.contentItem, "lyricScreenTitle");
        verify(title.text.indexOf(view.screen.clock(view.screen.scrubMs) + " /") >= 0,
               "the title does not show the place held: " + title.text);
        wait(300);
        compare(HearthController.positionMs, before);
        mouseRelease(area, area.width * 0.5, area.height / 2);
        compare(view.screen.scrubbing, false);
        tryVerify(function() { return near(HearthController.positionMs, target, 2500); }, 10000,
                  "the position " + HearthController.positionMs + " never reached ~" + target);
    }

    function test_theArrowKeysMoveTheItemFiveSecondsEachWay() {
        const view = open(true);
        view.win.requestActivate();
        const start = Math.round(HearthController.durationMs * 0.5);
        HearthController.seek(start);
        tryVerify(function() { return near(HearthController.positionMs, start, 1500); }, 10000);
        TestServices.keyClick(view.win, Qt.Key_Right);
        tryVerify(function() { return near(HearthController.positionMs, start + 5000, 1500); }, 10000,
                  "Right did not move the item forward: " + HearthController.positionMs);
        const there = HearthController.positionMs;
        TestServices.keyClick(view.win, Qt.Key_Left);
        tryVerify(function() { return near(HearthController.positionMs, there - 5000, 1500); }, 10000,
                  "Left did not move the item back: " + HearthController.positionMs);
    }

    function test_theArrowKeysStopAtTheEndsOfTheItem() {
        const view = open(true);
        view.win.requestActivate();
        HearthController.seek(1000);
        tryVerify(function() { return near(HearthController.positionMs, 1000, 1500); }, 10000);
        TestServices.keyClick(view.win, Qt.Key_Left);
        // Five seconds back from one second in is the start, not "nowhere" (the controller
        // refuses a negative position, which would leave the item at one second).
        tryVerify(function() { return HearthController.positionMs < 600; }, 10000,
                  "Left from one second in did not stop at the start: " + HearthController.positionMs);
        verify(HearthController.positionMs >= 0);
    }

    // --- the queue ----------------------------------------------------------------

    function test_theQueueIsClosedUntilAskedForAndNamesWhatIsNext() {
        const view = open(false);
        const panel = named(view.screen.contentItem, "lyricScreenQueue");
        verify(panel !== null);
        compare(panel.visible, false);
        const next = named(view.screen.contentItem, "lyricScreenNext");
        verify(next !== null);
        tryVerify(function() { return next.visible; }, 5000);
        verify(next.text.indexOf("second_song.ac3") >= 0, "the next item is not named: " + next.text);
    }

    function test_theQueueButtonOpensAListOfTheQueueAndAPressPlaysThatItem() {
        const view = open(false);
        const panel = named(view.screen.contentItem, "lyricScreenQueue");
        mouseClick(named(view.screen.contentItem, "lyricScreenQueueButton"));
        tryCompare(panel, "visible", true, 5000);
        compare(view.screen.queueOpen, true);
        const list = named(view.screen.contentItem, "lyricScreenQueueList");
        verify(list !== null);
        tryCompare(list, "count", 2, 5000);

        // The second row, found by its name; the list rebuilds its delegates when the queue
        // snapshot changes, so a press that lands on one being replaced is made again.
        let played = false;
        tryVerify(function() {
            played = played || HearthController.currentIndex === 1;
            if (!played) {
                const row = H.find(list, function(item) {
                    return item.objectName === "lyricScreenQueueRow-1";
                });
                if (row !== null) {
                    mouseClick(row);
                }
            }
            return played;
        }, 10000, "pressing the second row did not play it");
        tryCompare(HearthController, "state", "playing", 10000);
        // Nothing follows the last item, so there is no Next.
        const next = named(view.screen.contentItem, "lyricScreenNext");
        tryVerify(function() { return !next.visible; }, 5000);
    }

    function test_aLongQueueIsShownAtTheItemThatIsPlaying() {
        const view = open(false);
        const more = [];
        for (let i = 0; i < 12; ++i) {
            more.push(i % 2 === 0 ? first : second);
        }
        HearthController.addFiles(more);
        tryVerify(function() { return HearthController.queue.length === 14; }, 10000);
        HearthController.playItem(12);
        tryCompare(HearthController, "currentIndex", 12, 10000);
        mouseClick(named(view.screen.contentItem, "lyricScreenQueueButton"));
        const list = named(view.screen.contentItem, "lyricScreenQueueList");
        tryVerify(function() { return list.visible && list.count === 14; }, 5000);
        // Fourteen rows of 52 px or more do not fit the list: it is scrolled to the playing one,
        // whose row is wholly inside the part of the list that is on screen.
        function inView() {
            const row = list.itemAtIndex(12);
            if (row === null) {
                return false;
            }
            const top = row.y - list.contentY;
            return top >= 0 && top + row.height <= list.height + 1;
        }
        tryVerify(inView, 5000, "the playing row is not in view: contentY " + list.contentY);
    }

    function test_qOpensAndClosesTheQueue() {
        const view = open(false);
        view.win.requestActivate();
        const panel = named(view.screen.contentItem, "lyricScreenQueue");
        compare(panel.visible, false);
        TestServices.keyClick(view.win, Qt.Key_Q);
        tryCompare(panel, "visible", true, 5000);
        TestServices.keyClick(view.win, Qt.Key_Q);
        tryCompare(panel, "visible", false, 5000);
    }
}
