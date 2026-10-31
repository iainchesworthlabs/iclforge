import QtQuick
import QtTest

import Hearth
import HearthTest

import "HearthTestHelpers.js" as H

// OutputPicker.qml driven inside the real window (FEATURE_COVERAGE.md rows
// 6, 7, 22, 33-38): Main.qml's header summary and the Play page's signal-path
// "Choose..." open it, it lists the endpoints HearthController.
// refreshOutputDevices() reads, and "Play here" moves what is playing to the
// row picked - asserted on the fake device room (TestServices.useFakeRoom(),
// test_room.hpp), which is what the engine actually reopened, not only on a
// property.
//
// The room is three endpoints: "Test speakers" (default, 6 ch), "Test
// headphones" (2 ch) and "Test receiver" (8 ch, says it takes AC-3/E-AC-3
// passthrough, so it is also the passthrough section's one row). The receiver
// has a link as well as a device (TestServices.link(), test_room.hpp): what
// the engine sends it as IEC 61937 bursts is read back there as a receiver
// would read it, so the passthrough tests hold the window to the bytes of the
// file and not to a property. Main.qml is its own top-level window;
// mouseClick() reaches any window's items, so nothing here needs keyboard
// delivery into it (tst_shell.qml does).
TestCase {
    id: testCase
    name: "OutputPicker"
    when: windowShown

    readonly property string repo: Qt.resolvedUrl("../../../../../").toString()
    property string longEac3: ""
    property string firstAc3: ""
    property string secondAc3: ""
    property string karaokeAc3: ""
    property string tonesAc4: ""

    Component { id: mainComponent; Main { } }

    function initTestCase() {
        verify(TestServices.useFakeRoom(), "the fake device room could not be installed");
        // Seen already, so the first-run dialog does not sit over the
        // window; tst_dialogs.qml is where it is exercised.
        HearthController.firstRunSeen = true;
        longEac3 = TestServices.stageFixture(repo + "testdata/external-baseline/eac3-music-stereo-96/dee.ec3",
                                             "Long stereo.ec3");
        verify(longEac3.length > 0);
        // Ten seconds each of AC-3 5.1, AC-3 karaoke and (twenty of) AC-4 5.1.
        firstAc3 = TestServices.writeAc3Stream("first.ac3", "main-3/2");
        secondAc3 = TestServices.writeAc3Stream("second.ac3", "main-3/2");
        karaokeAc3 = TestServices.writeAc3Stream("karaoke.ac3", "karaoke-3/2");
        tonesAc4 = TestServices.writeAc4Stream("tones.ac4", "tones");
        verify(firstAc3.length > 0 && secondAc3.length > 0 && karaokeAc3.length > 0 && tonesAc4.length > 0,
               "the streams could not be made");
    }

    function cleanup() {
        HearthController.stop();
        for (let i = HearthController.queue.length - 1; i >= 0; --i) {
            HearthController.removeAt(i);
        }
        tryVerify(function() { return HearthController.queue.length === 0; }, 10000);
        // Where a window starts: decoding, to the default output, at the
        // room's own pace, a karaoke stream reproduced as an aware decoder's.
        HearthController.selectOutputGroup("");
        TestServices.setClockSpeed(1.0);
        TestServices.setEndpointPassthrough("fake-receiver", true);
        if (HearthController.decoderSettings.karaoke !== undefined
                && HearthController.decoderSettings.karaoke !== "aware") {
            setKaraoke("aware");
        }
    }

    function setKaraoke(choice) {
        const next = Object.assign({}, HearthController.decoderSettings);
        next.karaoke = choice;
        HearthController.setDecoderSettings(next);
        tryVerify(function() { return HearthController.decoderSettings.karaoke === choice; }, 10000,
                  "the karaoke choice never reached the engine");
    }

    function openWindow() {
        const win = createTemporaryObject(mainComponent, testCase);
        verify(win !== null);
        tryCompare(win, "visible", true);
        waitForRendering(win.contentItem);
        return win;
    }

    // Queues the long item by dropping it on the Play page and presses the
    // transport bar's Play, then waits for the device to be open.
    function playSomething(win) {
        const queueList = findChild(win, "queueList");
        verify(queueList !== null);
        verify(TestServices.dropFiles(queueList, [longEac3]), "the Play page refused the drop");
        tryVerify(function() { return HearthController.queue.length === 1; }, 10000);
        mouseClick(findChild(win, "transportPlayPause"));
        tryCompare(HearthController, "state", "playing", 10000);
        tryVerify(function() { return TestServices.device().open; }, 10000, "the engine never opened the device");
    }

    function openPicker(win) {
        const picker = findChild(win, "outputPicker");
        verify(picker !== null);
        mouseClick(findChild(win, "outputSummaryButton"));
        tryVerify(function() { return picker.opened; }, 5000, "the header summary did not open the picker");
        // Rows are laid out once refreshOutputDevices() has filled the
        // Repeater on open; click only once they have settled.
        tryVerify(function() { return rowFor(picker, "fake-receiver") !== null; }, 5000);
        waitForRendering(picker.contentItem);
        return picker;
    }

    function rowFor(picker, id) {
        return H.find(picker.contentItem, function(item) {
            return item.objectName.indexOf("outputDeviceRow-") === 0 && item.modelData.id === id;
        });
    }

    // The same endpoint's row in the passthrough section.
    function passthroughRowFor(picker, id) {
        return H.find(picker.contentItem, function(item) {
            return item.objectName.indexOf("passthroughDeviceRow-") === 0 && item.modelData.id === id;
        });
    }

    // The receiver's passthrough row picked and "Play here" pressed.
    function playToTheReceiver(win) {
        const picker = openPicker(win);
        const row = passthroughRowFor(picker, "fake-receiver");
        verify(row !== null, "the passthrough section has no row for the receiver");
        mouseClick(row);
        compare(picker.selectedPassthroughId, "fake-receiver");
        compare(picker.selectedDeviceId, "");
        mouseClick(findChild(win, "outputPickerPlayHere"));
        tryVerify(function() { return !picker.visible; }, 5000, "Play here did not close the picker");
        compare(HearthController.passthroughDeviceId, "fake-receiver");
    }

    // The visible copy of one of the signal-path card's lines (the Play page
    // holds the card twice, one of them hidden at any width).
    function signalPath(win, name) {
        const line = H.find(win.contentItem, function(item) { return item.objectName === name && item.visible; });
        verify(line !== null, "no visible " + name);
        return line.text;
    }

    function queueAndPlay(win, paths) {
        const queueList = findChild(win, "queueList");
        verify(queueList !== null);
        verify(TestServices.dropFiles(queueList, paths), "the Play page refused the drop");
        tryVerify(function() { return HearthController.queue.length === paths.length; }, 10000);
        mouseClick(findChild(win, "transportPlayPause"));
        tryCompare(HearthController, "state", "playing", 10000);
    }

    function test_headerSummaryShowsTheOutputDecisionAndOpensThePicker() {
        const win = openWindow();
        playSomething(win);
        // The summary is the output decision's own sentence once something
        // has opened, not the placeholder.
        tryVerify(function() { return HearthController.outputReason.length > 0; }, 10000);
        verify(H.textItem(findChild(win, "outputSummaryButton"), HearthController.outputReason) !== null,
               "the header summary does not show the output reason");

        const picker = openPicker(win);
        // Every endpoint of the room is a row; the default one is labelled
        // so, and the one playing now carries the "playing here" pill.
        tryVerify(function() { return HearthController.outputDevices.length === 3; }, 5000);
        verify(rowFor(picker, "fake-speakers") !== null && rowFor(picker, "fake-headphones") !== null
               && rowFor(picker, "fake-receiver") !== null, "a room endpoint has no picker row");
        verify(H.textItem(picker.contentItem, "Test speakers · default") !== null, "the default row is not marked");
        verify(H.textItem(rowFor(picker, "fake-speakers"), "playing here") !== null,
               "the open endpoint's row has no \"playing here\" pill");
        verify(H.textItem(rowFor(picker, "fake-headphones"), "playing here") === null);
        // Its summary line: width, speakers, rates.
        verify(H.textContaining(rowFor(picker, "fake-headphones"), "2 ch") !== null);
        // The passthrough section lists the one endpoint that says it takes
        // a bitstream.
        verify(H.textContaining(picker.contentItem, "AC-3 ✓ · E-AC-3 ✓") !== null,
               "the passthrough section does not list the receiver");
        // It opens TrueHD's eight-channel link too, and says it has not been run rather than
        // showing a tick: no platform has played MAT to a receiver that named it TrueHD.
        verify(H.textContaining(picker.contentItem, "TrueHD (MAT) opens, not run") !== null,
               "the receiver's TrueHD mark does not say it is not run");
        verify(H.textContaining(picker.contentItem, "TrueHD (MAT) ✓") === null,
               "a TrueHD tick appeared for a platform that has not been run");
        // The current endpoint starts selected.
        compare(picker.selectedDeviceId, "fake-speakers");
        mouseClick(findChild(win, "outputPickerCancel"));
        tryVerify(function() { return !picker.visible; }, 5000);
    }

    function test_playHereMovesPlaybackToThePickedEndpoint() {
        const win = openWindow();
        playSomething(win);
        compare(TestServices.device().endpoint, "fake-speakers");
        const opensBefore = TestServices.device().opens;

        const picker = openPicker(win);
        mouseClick(rowFor(picker, "fake-headphones"));
        compare(picker.selectedDeviceId, "fake-headphones");
        mouseClick(findChild(win, "outputPickerPlayHere"));
        tryVerify(function() { return !picker.visible; }, 5000, "Play here did not close the picker");

        // The engine reopened on the headphones: the device says so, and so
        // does every property the window reads the output from.
        tryVerify(function() { return TestServices.device().endpoint === "fake-headphones"; }, 10000,
                  "playback never moved to the headphones (device on " + TestServices.device().endpoint + ")");
        verify(TestServices.device().opens > opensBefore);
        compare(TestServices.device().channels, 2);
        tryCompare(HearthController, "currentDeviceId", "fake-headphones", 10000);
        tryCompare(HearthController, "deviceName", "Test headphones", 10000);
        tryCompare(HearthController, "routingOutputs", 2, 10000);
        compare(HearthController.state, "playing");
        const heard = TestServices.device().framesHeard;
        tryVerify(function() { return TestServices.device().framesHeard > heard + 4800; }, 10000,
                  "the headphones are open but not playing");

        // Reopened, the picker marks the new row as the one playing.
        const again = openPicker(win);
        compare(again.selectedDeviceId, "fake-headphones");
        verify(H.textItem(rowFor(again, "fake-headphones"), "playing here") !== null);
        verify(H.textItem(rowFor(again, "fake-speakers"), "playing here") === null);
        mouseClick(findChild(win, "outputPickerCancel"));
        tryVerify(function() { return !again.visible; }, 5000);
    }

    // Picking a row and then Cancel changes nothing.
    function test_cancelLeavesTheOutputWhereItWas() {
        const win = openWindow();
        playSomething(win);
        const where = TestServices.device().endpoint;
        const opens = TestServices.device().opens;
        const picker = openPicker(win);
        mouseClick(rowFor(picker, "fake-receiver"));
        compare(picker.selectedDeviceId, "fake-receiver");
        mouseClick(findChild(win, "outputPickerCancel"));
        tryVerify(function() { return !picker.visible; }, 5000);
        // Cancel posts nothing, so there is nothing to wait for; a fresh
        // poll's worth of the engine still says the same endpoint.
        tryVerify(function() { return HearthController.currentDeviceId === where; }, 5000);
        compare(TestServices.device().endpoint, where);
        compare(TestServices.device().opens, opens);
    }

    // The passthrough section's rows are choices (FEATURE_COVERAGE.md row 35):
    // Play here on the receiver's row moves the E-AC-3 item playing off the
    // speakers and onto the receiver's link as IEC 61937 bursts, and the
    // header, the signal path, the picker and the pages whose controls a
    // receiver does not take all say so. A PCM row moves it back to a decode.
    function test_passthroughRowSendsTheItemToTheReceiverAsABitstream() {
        const win = openWindow();
        playSomething(win);
        compare(TestServices.device().endpoint, "fake-speakers");
        compare(TestServices.link().open, false);
        compare(HearthController.outputFormat.mode, "pcm");
        // Until a row there is chosen, nothing is sent as a bitstream, though
        // the receiver says it would take one.
        verify(HearthController.outputReason.indexOf("Decoding here and playing PCM to \"Test speakers\"") === 0,
               "the item did not start as a decode: " + HearthController.outputReason);
        const opensBefore = TestServices.link().opens;
        const burstsBefore = TestServices.link().bursts;

        let picker = openPicker(win);
        // The note under the section says what choosing a row does.
        const note = findChild(win, "passthroughNote");
        verify(note !== null);
        verify(note.text.indexOf("sent to it untouched") >= 0, "the note does not say what a row does: " + note.text);
        verify(note.text.indexOf("no effect") < 0);
        mouseClick(findChild(win, "outputPickerCancel"));
        tryVerify(function() { return !picker.visible; }, 5000);

        playToTheReceiver(win);
        tryVerify(function() { return TestServices.link().open; }, 10000,
                  "the engine never opened the receiver's link: " + HearthController.outputReason);
        compare(TestServices.link().opens, opensBefore + 1);
        compare(TestServices.link().endpoint, "fake-receiver");
        compare(TestServices.link().format, "E-AC-3");
        compare(TestServices.link().sampleRate, 48000);
        // One output at a time: the decode's device is closed.
        compare(TestServices.device().open, false);
        compare(HearthController.state, "playing");

        // Bursts reach the link, each one an IEC 61937 E-AC-3 burst, and the
        // link's clock plays them.
        tryVerify(function() { return TestServices.link().bursts > burstsBefore + 8; }, 10000,
                  "no bursts reached the link");
        compare(TestServices.link().badBursts, 0);
        compare(TestServices.link().dataType, "E-AC-3");
        const heard = TestServices.link().framesHeard;
        tryVerify(function() { return TestServices.link().framesHeard > heard + 4800; }, 10000,
                  "the link is open but not playing");

        // What the window says about it.
        tryCompare(HearthController, "currentDeviceId", "fake-receiver", 10000);
        compare(HearthController.deviceName, "Test receiver");
        tryVerify(function() { return HearthController.outputFormat.mode === "bitstream"; }, 10000);
        compare(HearthController.outputFormat.stream, "E-AC-3");
        verify(HearthController.outputReason.indexOf(
                   "Bitstreaming E-AC-3 to \"Test receiver\" over IEC 61937, untouched.") === 0,
               "the header's sentence is " + HearthController.outputReason);
        verify(H.textItem(findChild(win, "outputSummaryButton"), HearthController.outputReason) !== null,
               "the header summary does not show the output reason");
        tryVerify(function() { return signalPath(win, "signalPathOutput") === "Test receiver"; }, 5000);
        compare(signalPath(win, "signalPathOutputFormat"), "E-AC-3 bitstream over IEC 61937 at 48.0 kHz");
        compare(signalPath(win, "signalPathRender"), "Not rendered here");
        verify(signalPath(win, "signalPathDecode").indexOf("sent as it is") >= 0,
               "the decode stage reads " + signalPath(win, "signalPathDecode"));
        // The volume is the receiver's, and so are the decoder settings.
        compare(findChild(win, "transportVolume").enabled, false);
        verify(HearthController.decoderSettingsNote.indexOf("The receiver decodes the bitstream") === 0,
               "the decoder note is " + HearthController.decoderSettingsNote);
        win.page = "decoder";
        tryVerify(function() {
            const notice = findChild(win, "receiverDecodesNotice");
            return notice !== null && notice.visible && notice.text === HearthController.decoderSettingsNote;
        }, 5000, "the Decoder page does not say the receiver decodes");
        win.page = "speakers";
        tryVerify(function() {
            const line = findChild(win, "speakersSetupFor");
            return line !== null && line.visible && line.text.indexOf("Test receiver is sent a bitstream") === 0;
        }, 5000, "the Speakers page does not say the setup is not applied");
        win.page = "play";

        // Reopened, the picker starts on the row chosen, which is the one
        // playing; the same endpoint's PCM row is not.
        picker = openPicker(win);
        compare(picker.selectedPassthroughId, "fake-receiver");
        compare(picker.selectedDeviceId, "");
        verify(H.textItem(passthroughRowFor(picker, "fake-receiver"), "playing here") !== null,
               "the receiver's passthrough row has no \"playing here\" pill");
        verify(H.textItem(rowFor(picker, "fake-receiver"), "playing here") === null);
        verify(H.textItem(rowFor(picker, "fake-speakers"), "playing here") === null);

        // A PCM row moves playback back to a decode, and gives the choice up.
        mouseClick(rowFor(picker, "fake-speakers"));
        compare(picker.selectedPassthroughId, "");
        mouseClick(findChild(win, "outputPickerPlayHere"));
        tryVerify(function() { return !picker.visible; }, 5000);
        tryVerify(function() { return TestServices.device().endpoint === "fake-speakers"; }, 10000,
                  "playback never moved back to the speakers");
        compare(TestServices.link().open, false);
        compare(HearthController.passthroughDeviceId, "");
        tryVerify(function() { return HearthController.outputFormat.mode === "pcm"; }, 10000);
        tryCompare(findChild(win, "transportVolume"), "enabled", true, 5000);
        compare(HearthController.decoderSettingsNote, "");
        compare(signalPath(win, "signalPathOutput"), "Test speakers");
    }

    // A queue played to the receiver from its start: the link carries every
    // byte of both files, in order and unchanged, and the second item joins
    // the first on the one link - gapless, as on a decode.
    function test_receiverIsSentTheFilesOwnBytesAndTheNextItemJoins() {
        const win = openWindow();
        playToTheReceiver(win);
        const opensBefore = TestServices.link().opens;
        const deviceOpensBefore = TestServices.device().opens;
        // Twenty seconds of audio, on a clock that ticks once a sleep: fast
        // enough that a machine whose sleeps run long still gets through it.
        TestServices.setClockSpeed(16.0);
        queueAndPlay(win, [firstAc3, secondAc3]);
        tryVerify(function() { return TestServices.link().open; }, 10000, "the link never opened");
        compare(TestServices.link().format, "AC-3");
        tryVerify(function() { return HearthController.outputFormat.stream === "AC-3"; }, 5000,
                  "the window does not say the link carries AC-3");
        tryVerify(function() { return TestServices.linkCarried([firstAc3, secondAc3]); }, 10000,
                  "what the link carries is not the start of the first file");
        tryCompare(HearthController, "currentIndex", 1, 60000);
        tryCompare(HearthController, "state", "stopped", 60000);
        compare(TestServices.link().opens, opensBefore + 1, "the second item reopened the link");
        compare(TestServices.link().badBursts, 0);
        compare(TestServices.link().dataType, "AC-3");
        verify(TestServices.linkCarried([firstAc3, secondAc3], true),
               "the link did not carry both files whole (" + TestServices.link().payloadBytes + " bytes)");
        // Nothing was decoded to a device on the way.
        compare(TestServices.device().opens, deviceOpensBefore);
    }

    // What a receiver does not take is decoded, to the receiver's own
    // endpoint, and the choice stands: an AC-4 item (no receiver takes it),
    // then an AC-3 one after it, which goes back to the link.
    function test_receiverGetsADecodeOfWhatItDoesNotTake() {
        const win = openWindow();
        playToTheReceiver(win);
        const linkOpensBefore = TestServices.link().opens;
        queueAndPlay(win, [tonesAc4, firstAc3]);
        tryVerify(function() { return TestServices.device().open; }, 10000,
                  "the AC-4 item was not decoded: " + HearthController.outputReason);
        compare(TestServices.device().endpoint, "fake-receiver");
        compare(TestServices.device().channels, 8);
        compare(TestServices.link().open, false);
        compare(TestServices.link().opens, linkOpensBefore);
        tryVerify(function() { return HearthController.outputFormat.mode === "pcm"; }, 10000);
        verify(HearthController.outputReason.indexOf("Decoding here and playing PCM to \"Test receiver\"") === 0,
               "the header's sentence is " + HearthController.outputReason);
        verify(HearthController.outputReason.indexOf("No output takes AC-4") > 0,
               "the header's sentence does not say why: " + HearthController.outputReason);
        compare(HearthController.passthroughDeviceId, "fake-receiver");
        tryCompare(HearthController, "deviceName", "Test receiver", 10000);
        compare(signalPath(win, "signalPathOutputFormat"), "8 ch PCM at 48.0 kHz");

        // The picker still starts on the passthrough row, the person's
        // choice, while the pill is on the PCM row the decode is going to.
        const picker = openPicker(win);
        compare(picker.selectedPassthroughId, "fake-receiver");
        verify(H.textItem(rowFor(picker, "fake-receiver"), "playing here") !== null,
               "the receiver's PCM row has no \"playing here\" pill during a decode");
        verify(H.textItem(passthroughRowFor(picker, "fake-receiver"), "playing here") === null);
        mouseClick(findChild(win, "outputPickerCancel"));
        tryVerify(function() { return !picker.visible; }, 5000);

        // The AC-3 item after it is sent as a bitstream: the output reopens.
        HearthController.next();
        tryVerify(function() { return TestServices.link().open; }, 10000,
                  "the AC-3 item after it was not sent to the link: " + HearthController.outputReason);
        compare(TestServices.link().format, "AC-3");
        compare(TestServices.device().open, false);
    }

    // A karaoke reproduction is this decoder's to make, so a karaoke item
    // with one chosen is decoded to the receiver's endpoint; played as coded
    // it is the receiver's to decode, and goes to the link. Changing the
    // choice while it plays moves it.
    function test_receiverGetsADecodeOfAKaraokeReproduction() {
        const win = openWindow();
        playToTheReceiver(win);
        const linkOpensBefore = TestServices.link().opens;
        compare(HearthController.decoderSettings.karaoke, "aware");
        queueAndPlay(win, [karaokeAc3]);
        tryVerify(function() { return TestServices.device().open; }, 10000,
                  "the karaoke item was not decoded: " + HearthController.outputReason);
        compare(TestServices.device().endpoint, "fake-receiver");
        compare(TestServices.link().opens, linkOpensBefore);
        tryVerify(function() {
            return HearthController.outputReason.indexOf("The karaoke reproduction chosen is made here") > 0;
        }, 5000, "the header's sentence does not say why");

        setKaraoke("asCoded");
        tryVerify(function() { return TestServices.link().open; }, 10000,
                  "as coded, the item was not sent to the link: " + HearthController.outputReason);
        compare(TestServices.link().format, "AC-3");
        compare(TestServices.device().open, false);
        compare(HearthController.state, "playing");
        tryVerify(function() { return HearthController.outputFormat.mode === "bitstream"; }, 10000);

        setKaraoke("capable");
        tryVerify(function() { return TestServices.device().open; }, 10000,
                  "with a reproduction chosen again, the item was not decoded");
        compare(TestServices.link().open, false);
        compare(HearthController.state, "playing");
    }

    // The engine keeps its own reading of what each output takes, and opening
    // the picker has it read again, so a row is decided against the machine as
    // the picker shows it: a receiver that is off is decoded for even when it
    // is chosen, and one switched on since is listed and sent the stream.
    function test_receiverSwitchedOnIsSeenWhenThePickerOpens() {
        TestServices.setEndpointPassthrough("fake-receiver", false);
        const win = openWindow();
        playSomething(win);
        let picker = openPicker(win);
        verify(passthroughRowFor(picker, "fake-receiver") === null,
               "a receiver that takes no bitstream is listed for passthrough");
        verify(H.textItem(picker.contentItem, "No passthrough-capable outputs were found.") !== null);
        mouseClick(findChild(win, "outputPickerCancel"));
        tryVerify(function() { return !picker.visible; }, 5000);

        // Chosen as a receiver all the same, as a choice made while it was on
        // would leave it: the engine has read that it takes nothing, so the
        // item is decoded to it, and the header says why.
        HearthController.selectPassthroughDevice("fake-receiver");
        tryVerify(function() { return TestServices.device().endpoint === "fake-receiver"; }, 10000,
                  "the item was not decoded to the receiver: " + HearthController.outputReason);
        compare(TestServices.link().open, false);
        tryVerify(function() {
            return HearthController.outputReason.indexOf("No output takes E-AC-3 over IEC 61937.") > 0;
        }, 5000, "the header's sentence does not say why");

        // Back on the speakers, and the receiver is switched on. (An output
        // the engine is playing to is not read again until it is released -
        // output_selector.hpp - which is why the item is moved off it first.)
        HearthController.selectOutputDevice("fake-speakers");
        tryVerify(function() { return TestServices.device().endpoint === "fake-speakers"; }, 10000);
        TestServices.setEndpointPassthrough("fake-receiver", true);

        // The picker's rows are fresh, and so is what the engine decides the
        // row against: Play here sends the item to the link.
        const opensBefore = TestServices.link().opens;
        playToTheReceiver(win);
        tryVerify(function() { return TestServices.link().open; }, 10000,
                  "the receiver was chosen once it took a bitstream, and was not sent one: "
                  + HearthController.outputReason);
        compare(TestServices.link().opens, opensBefore + 1);
        compare(TestServices.link().endpoint, "fake-receiver");
        compare(TestServices.device().open, false);
    }

    // A group made on the Network page is a row in the picker's network
    // section (FEATURE_COVERAGE.md row 91): Play here pins playback to it,
    // the reopened picker starts on it, and a device row picked again moves
    // playback back to this computer. Main.qml starts the network itself.
    function groupRowFor(picker, id) {
        return H.find(picker.contentItem, function(item) {
            return item.objectName.indexOf("outputGroupRow-") === 0 && item.modelData.id === id;
        });
    }

    function test_groupRowPinsPlaybackToTheGroup() {
        const win = openWindow();
        const groupId = NetworkController.createGroup("Kitchen and lounge");
        verify(groupId.length > 0, "the network did not start, so no group could be made");
        const picker = openPicker(win);
        let row = null;
        tryVerify(function() { row = groupRowFor(picker, groupId); return row !== null; }, 5000,
                  "the picker lists no row for the group");
        verify(H.textContaining(row, "Kitchen and lounge") !== null);
        verify(H.textContaining(row, "No members") !== null, "an empty group's row does not say so");
        mouseClick(row);
        compare(picker.selectedGroupId, groupId);
        compare(picker.selectedDeviceId, "");
        mouseClick(findChild(win, "outputPickerPlayHere"));
        tryVerify(function() { return !picker.visible; }, 5000, "Play here did not close the picker");
        tryCompare(HearthController, "outputGroupName", groupId, 5000);

        const again = openPicker(win);
        compare(again.selectedGroupId, groupId);
        tryVerify(function() { return H.textItem(groupRowFor(again, groupId), "playing here") !== null; }, 5000,
                  "the pinned group's row has no \"playing here\" pill");
        mouseClick(rowFor(again, "fake-speakers"));
        compare(again.selectedGroupId, "");
        mouseClick(findChild(win, "outputPickerPlayHere"));
        tryVerify(function() { return !again.visible; }, 5000);
        tryCompare(HearthController, "outputGroupName", "", 5000);
        NetworkController.deleteGroup(groupId);
    }

    // The output a person chose is kept across starts (FEATURE_COVERAGE.md row
    // 91's neighbour). What a restart finds is the settings file, so the keys
    // are read the way tst_settings_page.qml reads its own; and a restart is
    // TestServices.startAnotherWindow(): a second controller over the same
    // settings, started as the window starts one, before anything plays.
    function kept() {
        return {kind: TestServices.storedSetting("output/kind") || "default",
                id: TestServices.storedSetting("output/id") || ""};
    }

    // The row `id` in the picker's PCM section picked and "Play here" pressed.
    function playToTheDevice(win, id) {
        const picker = openPicker(win);
        mouseClick(rowFor(picker, id));
        compare(picker.selectedDeviceId, id);
        mouseClick(findChild(win, "outputPickerPlayHere"));
        tryVerify(function() { return !picker.visible; }, 5000, "Play here did not close the picker");
    }

    function test_playHereKeepsTheKindOfRowAndItsId() {
        const win = openWindow();
        // Nothing chosen: a start decodes to the default output.
        compare(kept().kind, "default");

        playToTheReceiver(win);
        compare(kept().kind, "passthrough");
        compare(kept().id, "fake-receiver");

        // A PCM row replaces it, whichever kind it was.
        playToTheDevice(win, "fake-headphones");
        compare(kept().kind, "device");
        compare(kept().id, "fake-headphones");

        const groupId = NetworkController.createGroup("Kept group");
        verify(groupId.length > 0, "the network did not start, so no group could be made");
        const picker = openPicker(win);
        let row = null;
        tryVerify(function() { row = groupRowFor(picker, groupId); return row !== null; }, 5000,
                  "the picker lists no row for the group");
        mouseClick(row);
        mouseClick(findChild(win, "outputPickerPlayHere"));
        tryVerify(function() { return !picker.visible; }, 5000);
        tryCompare(HearthController, "outputGroupName", groupId, 5000);
        compare(kept().kind, "group");
        compare(kept().id, groupId);

        // Giving the group up is the default again, which is also what deleting
        // the group that plays does (Network page: "Delete group").
        HearthController.selectOutputGroup("");
        compare(kept().kind, "default");
        compare(kept().id, "");
        NetworkController.deleteGroup(groupId);
    }

    function test_theNextStartPlaysToTheReceiverChosenLastTime() {
        const win = openWindow();
        playToTheReceiver(win);

        const later = TestServices.startAnotherWindow();
        verify(later !== null, "a second window could not be started");
        // Put back in start(), before anything has played.
        compare(later.passthroughDeviceId, "fake-receiver");
        compare(later.outputGroupName, "");
        later.openAndPlay([firstAc3]);
        tryCompare(later, "state", "playing", 10000);
        tryVerify(function() { return later.outputFormat.mode === "bitstream"; }, 10000,
                  "the item was not sent as a bitstream: " + later.outputReason);
        compare(later.currentDeviceId, "fake-receiver");
        compare(later.outputFormat.stream, "AC-3");
        later.stop();
        TestServices.closeWindow(later);
    }

    function test_theNextStartDecodesToTheDeviceChosenLastTime() {
        const win = openWindow();
        playToTheDevice(win, "fake-headphones");

        const later = TestServices.startAnotherWindow();
        verify(later !== null);
        compare(later.passthroughDeviceId, "");
        later.openAndPlay([firstAc3]);
        tryCompare(later, "state", "playing", 10000);
        tryCompare(later, "currentDeviceId", "fake-headphones", 10000);
        compare(later.outputFormat.mode, "pcm");
        later.stop();
        TestServices.closeWindow(later);

        // A device that is no longer there is one the decision ignores: the
        // default output, still a decode.
        const gone = TestServices.startAnotherWindow(["fake-headphones"]);
        verify(gone !== null);
        gone.openAndPlay([firstAc3]);
        tryCompare(gone, "state", "playing", 10000);
        tryCompare(gone, "currentDeviceId", "fake-speakers", 10000);
        compare(gone.outputFormat.mode, "pcm");
        gone.stop();
        TestServices.closeWindow(gone);
        // And what is kept has not changed for it.
        compare(kept().id, "fake-headphones");
    }

    // The receiver chosen is not listed at the next start, and another output
    // is, which takes a bitstream. Posted as it was chosen, the decision would
    // ignore the endpoint it does not have and bitstream to that one: a
    // receiver a person never chose, held exclusively.
    function test_aReceiverThatIsNotThereIsNotReplacedByAnotherThatTakesABitstream() {
        const win = openWindow();
        playToTheReceiver(win);
        compare(kept().id, "fake-receiver");

        const later = TestServices.startAnotherWindow(["fake-receiver"], true);
        verify(later !== null);
        // The choice is not this window's now: it plays where a window with
        // nothing chosen does.
        compare(later.passthroughDeviceId, "");
        later.openAndPlay([firstAc3]);
        tryCompare(later, "state", "playing", 10000);
        tryCompare(later, "currentDeviceId", "fake-speakers", 10000);
        compare(later.outputFormat.mode, "pcm");
        verify(later.outputReason.indexOf("Decoding here and playing PCM to \"Test speakers\"") === 0,
               "the item was not decoded to the default output: " + later.outputReason);
        later.stop();
        TestServices.closeWindow(later);

        // Kept as it was: the receiver is the choice again once it is listed.
        compare(kept().kind, "passthrough");
        compare(kept().id, "fake-receiver");
        const again = TestServices.startAnotherWindow();
        verify(again !== null);
        compare(again.passthroughDeviceId, "fake-receiver");
        TestServices.closeWindow(again);
    }

    // A group is put back as it was chosen. The network is found after the
    // window opens, so it is not ready at first, and the engine does not
    // replace a group that was chosen with a local output: it refuses the item,
    // and says why.
    function test_theNextStartKeepsTheGroupAndRefusesItemsUntilItIsReady() {
        const win = openWindow();
        const groupId = NetworkController.createGroup("Kept lounge");
        verify(groupId.length > 0, "the network did not start, so no group could be made");
        const picker = openPicker(win);
        let row = null;
        tryVerify(function() { row = groupRowFor(picker, groupId); return row !== null; }, 5000,
                  "the picker lists no row for the group");
        mouseClick(row);
        mouseClick(findChild(win, "outputPickerPlayHere"));
        tryVerify(function() { return !picker.visible; }, 5000);
        tryCompare(HearthController, "outputGroupName", groupId, 5000);
        compare(kept().id, groupId);

        const later = TestServices.startAnotherWindow();
        verify(later !== null);
        compare(later.outputGroupName, groupId);
        compare(later.passthroughDeviceId, "");
        later.openAndPlay([firstAc3]);
        tryVerify(function() { return later.errorText.indexOf("is not ready") >= 0; }, 10000,
                  "the item was not refused for the group: state " + later.state + ", error \""
                  + later.errorText + "\", reason \"" + later.outputReason + "\"");
        verify(later.errorText.indexOf("Kept lounge") >= 0 || later.errorText.indexOf(groupId) >= 0,
               "the refusal does not name the group: " + later.errorText);
        compare(later.state, "stopped");
        compare(later.currentDeviceId, "");
        TestServices.closeWindow(later);
        NetworkController.deleteGroup(groupId);
    }

    function test_networkPageButtonSwitchesToTheNetworkPage() {
        const win = openWindow();
        const picker = openPicker(win);
        mouseClick(findChild(win, "outputPickerNetworkPage"));
        tryVerify(function() { return !picker.visible; }, 5000);
        compare(win.page, "network");
    }

    // The Play page's signal-path card has its own "Choose..." that opens
    // the same dialog (the sidebar's copy - the monitor column's is hidden
    // at this width).
    function test_signalPathChooseOpensThePicker() {
        const win = openWindow();
        const choose = H.find(win.contentItem, function(item) {
            return item.objectName === "playChooseOutput" && item.visible;
        });
        verify(choose !== null, "no visible Choose... on the Play page");
        mouseClick(choose);
        const picker = findChild(win, "outputPicker");
        tryVerify(function() { return picker.opened; }, 5000, "Choose... did not open the picker");
        mouseClick(findChild(win, "outputPickerCancel"));
        tryVerify(function() { return !picker.visible; }, 5000);
    }
}
