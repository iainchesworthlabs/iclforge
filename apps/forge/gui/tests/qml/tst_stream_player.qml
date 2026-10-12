import QtQuick
import QtTest

import ForgeGui

// StreamPlayerController (GUI stream player) - the GUI twin of `forge monitor`/
// `forge decode`, opened from the header's "Open stream…" button and from a
// finished run chip's own "More…" menu (see tst_run_history.qml's sibling
// coverage of the encode side that menu's items reach into). Like
// tst_object_inspector.qml's own note: the playback path itself (play()/
// pause(), which opens a real MonitorSink) is not exercised here - it opens
// a real platform audio device this offscreen suite does not assume is
// present. seek() IS exercised: while nothing is playing it is a pure
// in-memory position update, no device involved.
TestCase {
    id: testCase
    name: "StreamPlayer"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    // The same real, already-encoded Dolby Atmos E-AC-3 fixture
    // tst_object_inspector.qml uses - see that file's own comment on why
    // this one and not the similarly-named fuzz corpus seed.
    readonly property url atmosStreamUrl:
        Qt.resolvedUrl("../fixtures/atmos-objects.ec3")
    readonly property url wavFixtureUrl:
        Qt.resolvedUrl("../../../../../libs/base/fuzz/seeds/fuzz_wav_read/roundtrip-stereo.wav")
    readonly property url outputUrl:
        Qt.resolvedUrl("_test_output/tst_stream_player.ec3")
    // A 5.1 main and a mono description as two independent substreams of one
    // E-AC-3 stream (§E2.3.1.2's I0 and I1), eight access units of each:
    //   forge eac3-encode main51.wav two-programmes.ec3 448 none 51 off \
    //       programme2=ad.wav programme2-layout=mono programme2-bitrate=96
    // from 12288 frames (0.256 s) of a tone per channel.
    readonly property url twoProgrammeStreamUrl:
        Qt.resolvedUrl("../fixtures/two-programmes.ec3")

    function init() {
        StreamPlayerController.pause();
    }

    function test_openingRealAtmosStreamDecodesAsyncAndReportsObjects() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        StreamPlayerController.openFile(atmosStreamUrl);
        compare(StreamPlayerController.busy, true);
        tryCompare(StreamPlayerController, "busy", false, 15000);

        compare(StreamPlayerController.error, "");
        compare(StreamPlayerController.hasResult, true);
        verify(StreamPlayerController.summaryLine.indexOf("E-AC-3") === 0);
        verify(StreamPlayerController.summaryLine.indexOf("Hz") > 0);
        compare(StreamPlayerController.hasObjects, true);
        verify(StreamPlayerController.objectCount > 0);
        verify(StreamPlayerController.durationSeconds > 0);
    }

    function test_dialogRendersOneMeterRowPerChannelAndOffersObjectExport() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        StreamPlayerController.openFile(atmosStreamUrl);
        tryCompare(StreamPlayerController, "busy", false, 15000);
        compare(StreamPlayerController.hasResult, true);

        win.streamPlayerDialogRef.open();
        tryVerify(() => win.streamPlayerDialogRef.opened);

        const meterRows = findChild(win.contentItem, "spMeterRows");
        verify(meterRows !== null);
        compare(meterRows.count, StreamPlayerController.channelMeta.length);

        const exportObjectsButton = findChild(win.contentItem, "spExportObjectsButton");
        verify(exportObjectsButton !== null);
        compare(exportObjectsButton.visible, true);

        const summary = findChild(win.contentItem, "spSummaryText");
        verify(summary !== null);
        compare(summary.text, StreamPlayerController.summaryLine);

        win.streamPlayerDialogRef.close();
    }

    // seek() while nothing is playing is a pure in-memory position update -
    // see StreamPlayerController::seek's own comment - so this is safe to
    // exercise without an audio device.
    function test_seekWhilePausedMovesPositionImmediately() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        StreamPlayerController.openFile(atmosStreamUrl);
        tryCompare(StreamPlayerController, "busy", false, 15000);
        compare(StreamPlayerController.playing, false);

        const target = StreamPlayerController.durationSeconds / 2;
        StreamPlayerController.seek(target);
        compare(StreamPlayerController.positionSeconds, target);

        StreamPlayerController.seek(-5);
        compare(StreamPlayerController.positionSeconds, 0);

        StreamPlayerController.seek(StreamPlayerController.durationSeconds + 5);
        compare(StreamPlayerController.positionSeconds, StreamPlayerController.durationSeconds);
    }

    // A stream with a second independent substream comes back from an
    // unfiltered split_access_units with its programmes interleaved, one frame
    // period of each in turn: the 5.1 main, then the mono description, then
    // the main again. The player decoded every unit with its channel order
    // sized from the first (5.1), so the mono unit read channels that were not
    // there - the access violation `forge monitor` died with - and, where it
    // survived, played the programmes one after the other. It plays ONE, the
    // first the stream carries as forge's own commands do, and says which.
    function test_aStreamWithTwoProgrammesPlaysTheFirstAndSaysSo() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        StreamPlayerController.openFile(twoProgrammeStreamUrl);
        tryCompare(StreamPlayerController, "busy", false, 15000);

        compare(StreamPlayerController.error, "");
        compare(StreamPlayerController.hasResult, true);
        // The main's own 5.1 bed: not the description's one channel, and not
        // a splice of the two.
        compare(StreamPlayerController.channelMeta.length, 6);
        // Eight access units of the main (8 x 1536 / 48000 s), not sixteen
        // units of both.
        fuzzyCompare(StreamPlayerController.durationSeconds, 0.256, 0.001);
        verify(StreamPlayerController.summaryLine.indexOf("8 frame(s)") > 0);
        verify(StreamPlayerController.summaryLine.indexOf("programme 0 of 2 (0, 1)") > 0);
    }

    // ---- the picture of a CD+G disc beside the stream ------------------------------
    // A packet as its 24 bytes: command 9, the instruction, two parity bytes, sixteen data
    // bytes, four parity bytes. libs/lyrics/tests/test_cdg.cpp holds the format to its
    // arithmetic; here a disc only has to be a disc.
    function cdgPacket(instruction, data) {
        const out = new Array(24).fill(0);
        out[0] = 9;
        out[1] = instruction;
        for (let i = 0; i < data.length; ++i) {
            out[4 + i] = data[i];
        }
        return out;
    }

    // Colour 1 is red and colour 2 green; 60 packets, a fifth of a second, 300 a second. In the
    // order they are sent (a packet is applied once the position has passed its place):
    //   packet 1   (by   7 ms)  the screen is red
    //   packet 30  (by 104 ms)  the screen is green
    //   packet 31  (by 107 ms)  a red tile, every pixel set, at x 12 to 17 and y 24 to 35
    //   packet 40  (by 137 ms)  a scroll that moves no tile and sets the fine offset to 3 across
    //                           and 5 down: the tile is seen at x 9 to 14 and y 19 to 30
    //   packet 50  (by 170 ms)  green is the transparent colour
    function cdgDisc() {
        const colours = new Array(16).fill(0);
        colours[2] = 0x3C;  // colour 1, 0xF00: two six-bit bytes
        colours[3] = 0x00;
        colours[4] = 0x03;  // colour 2, 0x0F0
        colours[5] = 0x30;
        const tile = [2, 1, 2, 2];  // clear pixels green, set pixels red, row 2, column 2
        for (let line = 0; line < 12; ++line) {
            tile.push(0x3F);
        }
        const atPacket = {
            0: cdgPacket(30, colours),
            1: cdgPacket(1, [1]),
            30: cdgPacket(1, [2]),
            31: cdgPacket(6, tile),
            40: cdgPacket(20, [0, 3, 5]),
            50: cdgPacket(28, [2]),
        };
        let bytes = [];
        for (let i = 0; i < 60; ++i) {
            bytes = bytes.concat(i in atPacket ? atPacket[i] : cdgPacket(0, []));
        }
        return bytes;
    }

    // The stream copied into a folder of its own (nothing else of the suite's is beside it)
    // and `disc` written beside it under `discName`; the stream's URL there.
    function streamWithDisc(folderName, discName, disc) {
        const folder = audioFixtures.folder() + "/" + folderName;
        verify(cliRunner.prepare(folder, twoProgrammeStreamUrl));
        if (disc !== null) {
            verify(audioFixtures.writeBytes(folderName + "/" + discName, disc).toString().length > 0);
        }
        return Qt.resolvedUrl(folder + "/two-programmes.ec3");
    }

    // The colour of one pixel of the disc's 300 by 216, as the window paints it: "red", "green",
    // "black" or "other". The window is grabbed whole and read where the item sits in it:
    // grabImage(item) crops by the item's own x and y, which is wrong for an item inside a
    // dialog's layout. The item draws the disc in whole steps, so at its 600 by 432 each pixel of
    // the disc is two pixels of the window (the test says so before it reads one).
    function pictureColourAt(win, picture, px, py) {
        const shot = grabImage(win.contentItem);
        const scale = picture.width / 300;
        const at = picture.mapToItem(win.contentItem, (px + 0.5) * scale, (py + 0.5) * scale);
        const x = Math.round(at.x);
        const y = Math.round(at.y);
        const r = shot.red(x, y);
        const g = shot.green(x, y);
        const b = shot.blue(x, y);
        if (r > 200 && g < 50 && b < 50) {
            return "red";
        }
        if (g > 200 && r < 50 && b < 50) {
            return "green";
        }
        return (r < 50 && g < 50 && b < 50) ? "black" : "other";
    }

    // A `name.cdg` beside the stream (in any case) is the picture of a karaoke disc: the dialog
    // draws it against the transport's position, a seek back replays it, and a pause is exact.
    function test_aCdgFileBesideTheStreamIsDrawnAgainstThePosition() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        StreamPlayerController.openFile(streamWithDisc("cdg-good", "two-programmes.CDG", cdgDisc()));
        tryCompare(StreamPlayerController, "busy", false, 15000);
        compare(StreamPlayerController.error, "");
        compare(StreamPlayerController.hasPicture, true);
        // The file is `.CDG`; a case-insensitive file system answers the usual `.cdg` spelling
        // and a case-sensitive one the file's own, so the name is compared in lower case.
        compare(StreamPlayerController.pictureFileName.toLowerCase(), "two-programmes.cdg");
        compare(StreamPlayerController.pictureError, "");

        win.streamPlayerDialogRef.open();
        tryVerify(() => win.streamPlayerDialogRef.opened);
        const picture = findChild(win.contentItem, "spCdgPicture");
        verify(picture !== null);
        compare(picture.visible, true);
        verify(findChild(win.contentItem, "spPictureCaption").visible);
        compare(findChild(win.contentItem, "spPictureError").visible, false);

        // The item is drawn at the disc's own proportions in whole steps: at 600 by 432 it is 2.
        tryCompare(picture, "width", 600);
        tryCompare(picture, "height", 432);
        const centre = [150, 108];

        // At 50 ms the preset to red has been sent and the one to green has not.
        StreamPlayerController.seek(0.05);
        compare(StreamPlayerController.pictureSeconds, StreamPlayerController.positionSeconds);
        tryVerify(() => picture.ready);
        tryVerify(() => pictureColourAt(win, picture, centre[0], centre[1]) === "red");
        // At 120 ms it is green, and the tile is where its memory has it (x 12 to 17, y 24 to 35):
        // the pixel at (9, 19) is not in it.
        StreamPlayerController.seek(0.12);
        tryVerify(() => pictureColourAt(win, picture, centre[0], centre[1]) === "green");
        compare(pictureColourAt(win, picture, 12, 24), "red");
        compare(pictureColourAt(win, picture, 9, 19), "green");
        // At 150 ms the scroll's fine offset shows the same tile 3 pixels left and 5 up: (9, 19) is
        // its corner now, and (8, 19) beside it is not.
        StreamPlayerController.seek(0.15);
        tryVerify(() => pictureColourAt(win, picture, 9, 19) === "red");
        compare(pictureColourAt(win, picture, 8, 19), "green");
        compare(pictureColourAt(win, picture, 14, 30), "red");
        compare(pictureColourAt(win, picture, 15, 30), "green");
        // At 190 ms green is the transparent colour: what was green is the item's black backdrop,
        // and the red tile is as it was.
        StreamPlayerController.seek(0.19);
        tryVerify(() => pictureColourAt(win, picture, centre[0], centre[1]) === "black");
        compare(pictureColourAt(win, picture, 9, 19), "red");
        // Going back is going back: the picture is the one the packets up to 50 ms draw, with
        // neither the offset nor the transparent colour.
        StreamPlayerController.seek(0.05);
        tryVerify(() => pictureColourAt(win, picture, centre[0], centre[1]) === "red");
        StreamPlayerController.seek(0.15);
        tryVerify(() => pictureColourAt(win, picture, centre[0], centre[1]) === "green");
        win.streamPlayerDialogRef.close();
    }

    // No file beside the stream is no picture, and the dialog shows none.
    function test_aStreamWithNoCdgFileBesideItHasNoPicture() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        StreamPlayerController.openFile(streamWithDisc("cdg-none", "", null));
        tryCompare(StreamPlayerController, "busy", false, 15000);
        compare(StreamPlayerController.hasResult, true);
        compare(StreamPlayerController.hasPicture, false);
        compare(StreamPlayerController.pictureFileName, "");
        compare(StreamPlayerController.pictureError, "");

        win.streamPlayerDialogRef.open();
        tryVerify(() => win.streamPlayerDialogRef.opened);
        compare(findChild(win.contentItem, "spCdgPicture").visible, false);
        compare(findChild(win.contentItem, "spPictureCaption").visible, false);
        compare(findChild(win.contentItem, "spPictureError").visible, false);
        win.streamPlayerDialogRef.close();
    }

    // A file of that name that is not CD+G says why, beside the stream that plays on, and the
    // next stream with no disc beside it takes the picture and the sentence away.
    function test_aCdgFileThatIsNotCdgSaysWhyAndTheNextStreamClearsIt() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        StreamPlayerController.openFile(streamWithDisc("cdg-bad", "two-programmes.cdg", [1, 2, 3]));
        tryCompare(StreamPlayerController, "busy", false, 15000);
        compare(StreamPlayerController.hasResult, true);
        compare(StreamPlayerController.hasPicture, false);
        verify(StreamPlayerController.pictureError.length > 0);
        compare(StreamPlayerController.pictureFileName, "two-programmes.cdg");

        win.streamPlayerDialogRef.open();
        tryVerify(() => win.streamPlayerDialogRef.opened);
        const sentence = findChild(win.contentItem, "spPictureError");
        compare(sentence.visible, true);
        verify(sentence.text.indexOf("two-programmes.cdg") >= 0);
        compare(findChild(win.contentItem, "spCdgPicture").visible, false);

        StreamPlayerController.openFile(atmosStreamUrl);
        tryCompare(StreamPlayerController, "busy", false, 15000);
        compare(StreamPlayerController.pictureError, "");
        compare(StreamPlayerController.pictureFileName, "");
        compare(sentence.visible, false);
        win.streamPlayerDialogRef.close();
    }

    // The other half: a stream with one programme is not annotated, so the
    // line the dialog has always shown is unchanged for it.
    function test_aStreamWithOneProgrammeCarriesNoProgrammeNote() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        StreamPlayerController.openFile(atmosStreamUrl);
        tryCompare(StreamPlayerController, "busy", false, 15000);

        compare(StreamPlayerController.hasResult, true);
        verify(StreamPlayerController.summaryLine.indexOf("programme") < 0);
    }

    // A finished run's own "More…" menu (Main.qml's runMoreMenu) is the run-
    // chip shortcut docs/forge/gui/qc.md and docs/forge/gui/inspect-objects.md used to
    // both say did not exist yet. The menu itself is a Popup, so its
    // MenuItems are not Item-derived and findChild - which only walks
    // Item.children, the same reason every OTHER findChild in this suite
    // targets a Button/Text/Repeater/etc - can never locate one; Main.qml's
    // window.openRunInQc/openRunInInspector exist specifically so this test
    // (and any other caller) can invoke exactly what a MenuItem's own
    // onTriggered does without reaching into the live popup. The button that
    // opens the menu IS an ordinary Item, so its own presence/gating is
    // checked directly.
    function test_runChipMoreMenuOpensQcAndInspectForThisRunsFile() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        EncoderController.loadSourceFile(wavFixtureUrl);
        tryCompare(EncoderController, "sourceReady", true);
        EncoderController.codecIndex = 1;  // E-AC-3, so runMoreInspect- is visible
        EncoderController.encodeTo(outputUrl);
        tryCompare(EncoderController, "busy", false, 10000);
        EncoderController.codecIndex = 0;

        const run = EncoderController.runs[0];
        compare(run.status, "done");
        compare(run.eac3, true);

        const moreButton = findChild(win.contentItem, "runMore-" + run.id);
        verify(moreButton !== null);
        compare(moreButton.visible, true);

        win.openRunInQc(run.path);
        tryVerify(() => win.qcDialogRef.opened);
        tryCompare(QcController, "busy", false, 15000);
        compare(QcController.filePath, run.path);
        win.qcDialogRef.close();

        win.openRunInInspector(run.path);
        tryVerify(() => win.objectInspectorDialogRef.opened);
        tryCompare(ObjectDecodeController, "busy", false, 15000);
        compare(ObjectDecodeController.filePath, run.path);
        win.objectInspectorDialogRef.close();
    }
}
