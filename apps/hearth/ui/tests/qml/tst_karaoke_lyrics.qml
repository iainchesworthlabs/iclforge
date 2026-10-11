import QtQuick
import QtQuick.Window
import QtTest

import Hearth
import HearthTest

import "HearthTestHelpers.js" as H

// The lyrics of the Karaoke page and the singer view (LyricClock.qml,
// LyricView.qml, LyricScreen.qml; planning/karaoke-design.md, K5).
//
// The reader and the cursor are tested where they live; what is held here is
// what a person sees from them: which word is in which of the three states and
// how far through the one being sung, what the view says when there is no file
// or no times, that the clock follows the poll and cannot run away from it, and
// that the page, the full-screen view and Settings reach the one controller.
TestCase {
    id: testCase
    name: "KaraokeLyrics"
    when: windowShown
    width: 1100
    height: 900

    property string words: ""       // the lyrics file's path
    property string item: ""        // the item it belongs to (same stem)
    property string stream: ""      // a real AC-3 karaoke stream with its own file

    Component { id: mainComponent; Main { } }
    Component { id: viewComponent; LyricView { width: 900; height: 400; follow: false } }
    Component { id: clockComponent; LyricClock { live: false; active: false; running: true } }

    // The first line is word-timed (five words, the last running to the next
    // line), the second only line-timed, the third word-timed again.
    function lyrics() {
        return ["[ti:Auld Lang Syne]",
                "[ar:Traditional]",
                "[00:01.00]<00:01.00>Should <00:01.50>auld <00:02.00>acquaintance <00:03.00>be <00:03.50>forgot,",
                "[00:05.00]And never brought to mind?",
                "[00:08.00]<00:08.00>For <00:08.50>auld <00:09.00>lang <00:09.50>syne",
                ""].join("\n");
    }

    function initTestCase() {
        verify(TestServices.useFakeRoom(), "the fake device room could not be installed");
        HearthController.firstRunSeen = true;
        words = TestServices.writeTextFile("words.lrc", lyrics());
        verify(words.length > 0, "the lyrics file could not be written");
        item = words.replace(/\.lrc$/, ".ac3");
        stream = TestServices.writeAc3Stream("song.ac3", "karaoke-3/2");
        verify(stream.length > 0, "the stream could not be written");
        // Three 0.5 s words, then a line, inside the stream's ten seconds.
        verify(TestServices.writeTextFile("song.lrc",
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
        KaraokeController.delayMs = 0;
        KaraokeController.load("", 0);
    }

    function named(root, name) {
        const start = root.contentItem !== undefined ? root.contentItem : root;
        const found = H.find(start, function(item) { return item.objectName === name; });
        return found !== null ? found : findChild(root, name);
    }

    function loaded(view, at) {
        KaraokeController.load(item, 10000);
        view.positionMs = at;
        return view;
    }

    function newView() {
        // Under the test case's parent: the test case itself is not visible, and
        // nothing under it would be.
        const view = createTemporaryObject(viewComponent, testCase.parent);
        verify(view !== null);
        return view;
    }

    function wordItem(view, index) {
        return H.find(view, function(item) { return item.objectName === "lyricWord-" + index; });
    }

    // --- the clock -------------------------------------------------------

    function test_clockIsTheReadingPlusTheTimeSinceItButNeverTwoPollsAhead() {
        const clock = createTemporaryObject(clockComponent, testCase);
        verify(clock !== null);
        clock.time = 1000;
        clock.poll(5000);
        compare(clock.positionMs, 5000);
        clock.time = 1050;
        compare(clock.positionMs, 5050);
        // The poll stalls: the words may not run away.
        clock.time = 1900;
        compare(clock.positionMs, 5000 + clock.maxAheadMs);
        // A poll snaps it to the reading, ahead of or behind where it ran to.
        clock.poll(5300);
        compare(clock.positionMs, 5300);
        clock.time = 1960;
        compare(clock.positionMs, 5360);
        clock.poll(5330);
        compare(clock.positionMs, 5330);
    }

    function test_clockStandsAtTheReadingWhilePausedAndSnapsOnASeek() {
        const clock = createTemporaryObject(clockComponent, testCase);
        clock.time = 2000;
        clock.poll(8000);
        clock.time = 2050;
        compare(clock.positionMs, 8050);
        // A pause: the reading, whatever time has passed.
        clock.running = false;
        clock.time = 3000;
        compare(clock.positionMs, 8000);
        // A seek while paused and one while playing are only another reading.
        clock.poll(1000);
        compare(clock.positionMs, 1000);
        clock.running = true;
        clock.time = 3040;
        compare(clock.positionMs, 1040);
        clock.poll(60000);
        compare(clock.positionMs, 60000);
    }

    // --- the three states of a word ---------------------------------------

    function test_wordsAreSungBeingSungAndNotYetSungByTime() {
        const view = loaded(newView(), 2500);
        compare(view.status, "timed");
        tryVerify(function() { return wordItem(view, 4) !== null; }, 5000);
        compare(view.currentLine, 0);
        compare(view.lineNumber, 1);
        compare(view.lineCount, 3);
        // "Should auld" are sung, half of "acquaintance" is, "be forgot," is not.
        compare(wordItem(view, 0).fill, 1);
        compare(wordItem(view, 1).fill, 1);
        fuzzyCompare(wordItem(view, 2).fill, 0.5, 0.001);
        compare(wordItem(view, 3).fill, 0);
        compare(wordItem(view, 4).fill, 0);
        // Only the one being sung is underlined, which is a cue that is not colour.
        const underlined = H.findAll(view, function(item) {
            return item.objectName === "lyricWordUnderline" && item.visible;
        }, []);
        compare(underlined.length, 1);
        compare(wordItem(view, 2).being, true);
        compare(wordItem(view, 1).being, false);
        // Its fill is cut at that fraction of the word's width.
        const fill = H.find(wordItem(view, 2), function(item) { return item.objectName === "lyricWordFill"; });
        verify(fill !== null);
        fuzzyCompare(fill.width, wordItem(view, 2).width * 0.5, wordItem(view, 2).width * 0.25);
        // The line after is shown, muted, and the one before the first is not.
        verify(H.find(view, function(item) { return item.objectName === "lyricNext"; }) !== null);
        compare(H.find(view, function(item) { return item.objectName === "lyricPast"; }), null);
    }

    function test_theFillAdvancesWithThePositionAndAWordIsSungOnceItEnds() {
        const view = loaded(newView(), 2250);
        tryVerify(function() { return wordItem(view, 2) !== null; }, 5000);
        fuzzyCompare(wordItem(view, 2).fill, 0.25, 0.001);
        view.positionMs = 2750;
        fuzzyCompare(wordItem(view, 2).fill, 0.75, 0.001);
        view.positionMs = 3000;
        compare(wordItem(view, 2).fill, 1);
        compare(wordItem(view, 3).being, true);
        // A seek back un-sings them: the view keeps nothing of where it was.
        view.positionMs = 1200;
        compare(wordItem(view, 0).being, true);
        compare(wordItem(view, 1).fill, 0);
        compare(wordItem(view, 2).fill, 0);
    }

    function test_aLineWithOnlyLineTimesIsEmphasisedWholeAndNoWordTimesAreInvented() {
        const view = loaded(newView(), 6000);
        compare(view.currentLine, 1);
        const whole = H.find(view, function(item) { return item.objectName === "lyricCurrentLine"; });
        verify(whole !== null);
        tryVerify(function() { return whole.visible; }, 5000);
        compare(whole.text, "And never brought to mind?");
        compare(whole.font.bold, true);
        compare(H.find(view, function(item) { return item.objectName === "lyricWords"; }).visible, false);
        // The first line is now behind it, in the sung colour.
        const past = H.find(view, function(item) { return item.objectName === "lyricPast"; });
        verify(past !== null);
        compare(past.text, "Should auld acquaintance be forgot,");
    }

    function test_nothingIsBeingSungBeforeTheFirstLineOrAfterTheLast() {
        const view = loaded(newView(), 200);
        compare(view.currentRow, -1);
        compare(view.lineNumber, 0);
        view.positionMs = 10500;
        compare(view.inLine, false);
        compare(view.currentRow, -1);
        compare(view.lineNumber, 3);
    }

    function test_theDelayShowsTheWordsLaterAndMovesTheFill() {
        const view = loaded(newView(), 2500);
        tryVerify(function() { return wordItem(view, 2) !== null; }, 5000);
        fuzzyCompare(wordItem(view, 2).fill, 0.5, 0.001);
        KaraokeController.delayMs = 500;
        // 2.5 s heard is 2.0 s of the words: the start of "acquaintance".
        compare(wordItem(view, 1).fill, 1);
        compare(wordItem(view, 2).fill, 0);
        compare(wordItem(view, 2).being, true);
        KaraokeController.delayMs = -500;
        fuzzyCompare(wordItem(view, 2).fill, 1.0, 0.001);
    }

    // --- no file, no times, no UTF-8 --------------------------------------

    function test_noFileSaysWhereItLooks() {
        const view = newView();
        const missing = words.replace("words.lrc", "no_such_song.ac3");
        KaraokeController.load(missing, 5000);
        compare(view.status, "none");
        const message = H.find(view, function(item) { return item.objectName === "lyricMessage"; });
        verify(message !== null);
        tryVerify(function() { return message.visible; }, 5000);
        verify(message.text.indexOf("no_such_song.lrc") >= 0, message.text);
        // Nothing playing is its own sentence.
        KaraokeController.load("", 0);
        compare(message.text, qsTr("Nothing is playing."));
    }

    function test_aFileWithNoTimesIsShownWholeAndNotFollowed() {
        const plain = TestServices.writeTextFile("plain.lrc", "Should auld acquaintance be forgot\nAnd never brought to mind\n");
        verify(plain.length > 0);
        const view = newView();
        KaraokeController.load(plain.replace(/\.lrc$/, ".ac3"), 5000);
        compare(view.status, "plain");
        view.positionMs = 3000;
        compare(view.currentRow, -1);
        const flick = H.find(view, function(item) { return item.objectName === "lyricPlain"; });
        verify(flick !== null);
        tryVerify(function() { return flick.visible; }, 5000);
        const text = H.find(flick, function(item) { return item.text !== undefined && item.text.length > 0; });
        verify(text !== null);
        verify(text.text.indexOf("And never brought to mind") >= 0);
    }

    function test_aFileThatIsNotUtf8IsNotShownAndPlaybackIsSaidToBeUnaffected() {
        const broken = TestServices.writeTextFile("broken.lrc", "[00:01.00]café au lait\n", true);
        verify(broken.length > 0);
        const view = newView();
        KaraokeController.load(broken.replace(/\.lrc$/, ".ac3"), 5000);
        compare(view.status, "error");
        const message = H.find(view, function(item) { return item.objectName === "lyricMessage"; });
        tryVerify(function() { return message.visible; }, 5000);
        verify(message.text.indexOf("not UTF-8") >= 0, message.text);
        verify(message.text.indexOf(qsTr("Playback is not affected.")) >= 0, message.text);
    }

    // --- on the page, from a real stream -----------------------------------

    function play() {
        HearthController.setLayoutText("5.1");
        tryVerify(function() { return HearthController.speakerLabels.length === 6; }, 10000);
        HearthController.addFiles([stream]);
        tryVerify(function() { return HearthController.queue.length === 1; }, 10000);
        HearthController.playItem(0);
        tryCompare(HearthController, "state", "playing", 10000);
    }

    function openPage() {
        const win = createTemporaryObject(mainComponent, testCase);
        verify(win !== null);
        tryCompare(win, "visible", true);
        win.page = "karaoke";
        waitForRendering(win.contentItem);
        return win;
    }

    function test_theLyricsCardFollowsTheItemPlayingAndNamesTheFile() {
        const win = openPage();
        const view = named(win, "lyricsView");
        verify(view !== null);
        play();
        tryCompare(view, "status", "timed", 10000);
        compare(KaraokeController.fileName, "song.lrc");
        tryVerify(function() {
            return named(win, "lyricsFile").text.indexOf("song.lrc") >= 0
                   && named(win, "lyricsFile").text.indexOf(qsTr(", word-timed")) >= 0;
        }, 5000);
        // The song plays on the fake device in real time: the first word is
        // sung half a second in, and the cursor reaches it from the poll.
        tryVerify(function() { return view.currentLine >= 0; }, 15000, "the lyrics never started");
        compare(view.lineCount, 2);
        verify(view.lineNumber >= 1);
    }

    function test_theLyricsCardSaysNoLyricsForAStreamWithoutAFile() {
        const bare = TestServices.writeAc3Stream("bare.ac3", "karaoke-3/2");
        verify(bare.length > 0);
        const win = openPage();
        HearthController.setLayoutText("5.1");
        tryVerify(function() { return HearthController.speakerLabels.length === 6; }, 10000);
        HearthController.addFiles([bare]);
        tryVerify(function() { return HearthController.queue.length === 1; }, 10000);
        HearthController.playItem(0);
        const view = named(win, "lyricsView");
        tryCompare(view, "status", "none", 10000);
        const message = named(win, "lyricMessage");
        tryVerify(function() { return message.visible && message.text.indexOf("bare.lrc") >= 0; }, 5000);
    }

    // --- the picture of a CD+G disc ----------------------------------------------

    // One 24-byte CD+G packet as a string of byte values: command 9, the instruction, two parity
    // bytes, sixteen data bytes, four parity bytes.
    function cdgPacket(instruction, data) {
        const bytes = new Array(24).fill(0);
        bytes[0] = 9;
        bytes[1] = instruction;
        for (let i = 0; i < data.length; ++i) {
            bytes[4 + i] = data[i];
        }
        return String.fromCharCode.apply(null, bytes);
    }

    // Colour 1 is red and colour 2 green; the picture is red from the first packets and green from
    // the packet at one second (300 packets in). Base64, for TestServices.writeBinaryFile().
    function cdgDisc() {
        const colours = new Array(16).fill(0);
        colours[2] = 0x3C;
        colours[4] = 0x03;
        colours[5] = 0x30;
        let disc = cdgPacket(30, colours) + cdgPacket(1, [1]);
        for (let i = 2; i < 300; ++i) {
            disc += cdgPacket(0, []);
        }
        disc += cdgPacket(1, [2]);
        return Qt.btoa(disc);
    }

    function test_aCdgFileBesideAnItemIsShownAsItsPictureAgainstThePosition() {
        const disc = TestServices.writeBinaryFile("disc.cdg", cdgDisc());
        verify(disc.length > 0, "the CD+G file could not be written");
        const view = newView();
        KaraokeController.load(disc.replace(/\.cdg$/, ".ac3"), 10000);
        compare(view.status, "picture");
        const picture = H.find(view, function(item) { return item.objectName === "lyricPicture"; });
        verify(picture !== null);
        tryVerify(function() { return picture.visible; }, 5000);
        verify(picture.ready);
        // No words are drawn, and there is nothing to say: the picture is the lyrics.
        const message = H.find(view, function(item) { return item.objectName === "lyricMessage"; });
        verify(!message.visible);
        const stage = H.find(view, function(item) { return item.objectName === "lyricStage"; });
        verify(!stage.visible);

        // The picture is drawn at the middle of the view in the colour of its packets: red before
        // the packet at one second, green after it, and back to red on a seek back. The view
        // is 900 by 400, so the 300 by 216 picture is drawn once over, centred, with black round it.
        view.positionMs = 500;
        waitForRendering(picture);
        let image = grabImage(picture);
        compare(image.red(450, 200), 255);
        compare(image.green(450, 200), 0);
        compare(image.red(20, 20), 0);
        // Drawn once over (the view fits it at 1x, not 1.85x), so the picture spans x 300 to 599:
        // red just inside it and black just outside.
        compare(image.red(310, 200), 255);
        compare(image.red(590, 200), 255);
        compare(image.red(250, 200), 0);
        compare(image.red(650, 200), 0);
        view.positionMs = 1100;
        waitForRendering(picture);
        image = grabImage(picture);
        compare(image.red(450, 200), 0);
        compare(image.green(450, 200), 255);

        // The delay shows it later, as it does the words: the position stands still at 1100 ms,
        // and 400 ms of delay makes it the picture of 700 ms, which is red.
        KaraokeController.delayMs = 400;
        waitForRendering(picture);
        image = grabImage(picture);
        compare(image.red(450, 200), 255);
        KaraokeController.delayMs = 0;
        waitForRendering(picture);
        image = grabImage(picture);
        compare(image.green(450, 200), 255);
        // And a seek back is a seek back.
        view.positionMs = 500;
        waitForRendering(picture);
        image = grabImage(picture);
        compare(image.red(450, 200), 255);
    }

    function test_theLyricsCardNamesTheCdgFileAndSaysThereIsNoTextToFollow() {
        const disc = TestServices.writeAc3Stream("disc_song.ac3", "karaoke-3/2");
        verify(disc.length > 0);
        verify(TestServices.writeBinaryFile("disc_song.cdg", cdgDisc()).length > 0);
        const win = openPage();
        HearthController.setLayoutText("5.1");
        tryVerify(function() { return HearthController.speakerLabels.length === 6; }, 10000);
        HearthController.addFiles([disc]);
        tryVerify(function() { return HearthController.queue.length === 1; }, 10000);
        HearthController.playItem(0);
        const view = named(win, "lyricsView");
        tryCompare(view, "status", "picture", 10000);
        compare(KaraokeController.fileName, "disc_song.cdg");
        tryVerify(function() { return named(win, "lyricsFile").text === "disc_song.cdg"; }, 5000);
        tryVerify(function() { return named(win, "lyricsNote").text.indexOf("CD+G disc") >= 0; }, 5000);
        const picture = H.find(view, function(item) { return item.objectName === "lyricPicture"; });
        verify(picture !== null);
        verify(picture.visible);
    }

    // --- Settings ------------------------------------------------------------

    function test_settingsHasALyricDelayThatIsKeptAndCanBeReset() {
        const win = openPage();
        win.page = "settings";
        waitForRendering(win.contentItem);
        const slider = named(win, "settingsLyricDelay");
        verify(slider !== null, "no lyric delay control in Settings");
        compare(KaraokeController.delayMs, 0);
        slider.value = 300;
        slider.moved();
        compare(KaraokeController.delayMs, 300);
        compare(named(win, "settingsLyricDelayValue").text, qsTr("%1 ms").arg("+300"));
        // The control cannot be taken past what the controller allows.
        compare(slider.to, KaraokeController.maxDelayMs);
        compare(slider.from, -KaraokeController.maxDelayMs);
        mouseClick(named(win, "settingsLyricDelayReset"));
        compare(KaraokeController.delayMs, 0);
        compare(slider.value, 0);
    }

    // --- the singer view -------------------------------------------------------

    function test_f11OpensTheSingerViewFullScreenAndEscapeLeavesIt() {
        const win = openPage();
        win.requestActivate();
        const screen = findChild(win, "lyricScreen");
        verify(screen !== null);
        compare(screen.visible, false);
        TestServices.keyClick(win, Qt.Key_F11);
        tryCompare(screen, "visible", true, 5000);
        tryCompare(win, "visibility", Window.FullScreen, 5000);
        // It shows the lyrics, large, from the same controller.
        const view = named(screen.contentItem, "lyricScreenView");
        verify(view !== null);
        compare(view.large, true);
        TestServices.keyClick(win, Qt.Key_Escape);
        tryCompare(screen, "visible", false, 5000);
        tryVerify(function() { return win.visibility !== Window.FullScreen; }, 5000);
    }

    function test_theLyricsCardButtonOpensTheSingerView() {
        const win = openPage();
        const screen = findChild(win, "lyricScreen");
        mouseClick(named(win, "lyricsFullScreenButton"));
        tryCompare(screen, "visible", true, 5000);
        win.toggleLyricScreen();
        tryCompare(screen, "visible", false, 5000);
    }

    function test_theSingerViewChoosesVocalsAndPausesFromItsOwnBar() {
        const win = openPage();
        play();
        tryVerify(function() { return HearthController.currentMedia.programmes !== undefined; }, 10000);
        const screen = findChild(win, "lyricScreen");
        win.openLyricScreen();
        tryCompare(screen, "visible", true, 5000);
        const v1 = named(screen.contentItem, "lyricScreenVocals-v1");
        verify(v1 !== null);
        tryVerify(function() { return v1.visible; }, 5000);
        mouseClick(v1);
        tryCompare(HearthController.decoderSettings, "karaoke", "capable", 10000);
        compare(HearthController.decoderSettings.karaokeVocals, "v1");
        tryCompare(v1, "selected", true, 5000);
        // Pause is on the bar; Play follows.
        const toggle = named(screen.contentItem, "lyricScreenPlayPause");
        mouseClick(toggle);
        tryCompare(HearthController, "state", "paused", 10000);
        mouseClick(toggle);
        tryCompare(HearthController, "state", "playing", 10000);
        win.toggleLyricScreen();
        tryCompare(screen, "visible", false, 5000);
        HearthController.setDecoderSettings({ karaoke: "aware", karaokeVocals: "both" });
    }
}
