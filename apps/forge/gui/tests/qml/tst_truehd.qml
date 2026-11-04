import QtQuick
import QtTest

import ForgeGui

// TrueHD and MLP in the window: Open stream (the player), the drop and launch routing, and QC a
// stream. The files are the FFmpeg fixtures of testdata/truehd, copied to ../fixtures: a 5.1 stream
// with a 2 channel and a 6 channel presentation, and two streams joined end to end, whose lossless
// check fails at the join (libs/truehd/ERRATA.md, question 20). The player reads the file a window
// at a time (src/truehd_pcm_source.hpp), and the Catch2 cases of iclforge-forge-gui-tests hold the
// samples it delivers to FFmpeg's decode; what is held here is that the controllers and the dialogs
// use it. No audio output exists under the offscreen harness, so Play asserts whichever honest
// outcome the machine gives, as tst_stream_player.qml does.
TestCase {
    id: testCase
    name: "TrueHd"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    readonly property url streamUrl: Qt.resolvedUrl("../fixtures/truehd_m51_s24.thd")
    readonly property url mlpUrl: Qt.resolvedUrl("../fixtures/mlp_st48_s24.mlp")
    readonly property url joinedUrl: Qt.resolvedUrl("../fixtures/truehd_cat_st48_s24.thd")
    readonly property url exportUrl: Qt.resolvedUrl("_test_output/tst_truehd.wav")

    function init() {
        StreamPlayerController.pause();
        QcController.presetIndex = 0;
    }

    function open(url) {
        StreamPlayerController.openFile(url);
        compare(StreamPlayerController.busy, true);
        tryCompare(StreamPlayerController, "busy", false, 15000);
    }

    // Open stream: the format, the speakers and the length come from the index of the file, and no
    // decoded copy of it is held.
    function test_openingATrueHdStreamReportsItsFormatAndSpeakers() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        open(streamUrl);
        compare(StreamPlayerController.error, "");
        compare(StreamPlayerController.hasResult, true);
        verify(StreamPlayerController.summaryLine.indexOf("TrueHD") === 0);
        verify(StreamPlayerController.summaryLine.indexOf("L R C LFE Ls Rs") > 0);
        verify(StreamPlayerController.summaryLine.indexOf("48000 Hz") > 0);
        compare(StreamPlayerController.hasObjects, false);
        fuzzyCompare(StreamPlayerController.durationSeconds, 0.25, 0.001);
        const meta = StreamPlayerController.channelMeta;
        compare(meta.length, 6);
        compare(meta[0].name, "L");
        compare(meta[3].name, "LFE");
        compare(meta[5].name, "Rs");
    }

    function test_anMlpStreamSaysSo() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        open(mlpUrl);
        compare(StreamPlayerController.error, "");
        verify(StreamPlayerController.summaryLine.indexOf("MLP") === 0);
        compare(StreamPlayerController.channelMeta.length, 2);
    }

    function test_seekWhilePausedMovesPositionOnATrueHdStream() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        open(streamUrl);
        compare(StreamPlayerController.playing, false);
        const target = StreamPlayerController.durationSeconds / 2;
        StreamPlayerController.seek(target);
        compare(StreamPlayerController.positionSeconds, target);
        StreamPlayerController.seek(-5);
        compare(StreamPlayerController.positionSeconds, 0);
    }

    function test_playingATrueHdStreamStartsOrNamesTheOutputItCouldNotOpen() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        open(streamUrl);
        StreamPlayerController.seek(0.1);
        StreamPlayerController.play();
        if (StreamPlayerController.error.length > 0) {
            verify(StreamPlayerController.error.indexOf("playback output") >= 0,
                   StreamPlayerController.error);
            return;
        }
        // The clip is short: it plays to its end and stops, with the position back at the start.
        tryCompare(StreamPlayerController, "playing", false, 15000);
        StreamPlayerController.pause();
    }

    // The WAV is the one `forge decode` writes; the Catch2 case holds its bytes.
    function test_exportingAnTrueHdStreamWritesAWavWithoutError() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        open(streamUrl);
        StreamPlayerController.exportDecodedWav(exportUrl);
        tryCompare(StreamPlayerController, "exporting", false, 15000);
        compare(StreamPlayerController.exportError, "");
    }

    // Drag and drop, and `forge-gui <file>`: a .thd or .mlp opens the player, and does not fall to
    // the WAV source loader that refused it.
    function test_aDroppedTrueHdFileOpensThePlayer() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        win.openDroppedFile(streamUrl);
        tryCompare(StreamPlayerController, "busy", false, 15000);
        verify(StreamPlayerController.filePath.indexOf("truehd_m51_s24.thd") >= 0);
        verify(StreamPlayerController.summaryLine.indexOf("TrueHD") === 0);
        compare(StreamPlayerController.error, "");
    }

    function test_aFileThatIsNotAStreamIsNotCalledTrueHd() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        open(Qt.resolvedUrl("../fixtures/speakers-2-1.wav"));
        verify(StreamPlayerController.summaryLine.indexOf("TrueHD") !== 0);
    }

    // QC: one programme for each presentation, the dialogue norm the stream stores beside the
    // measurement, and the integrity gate.
    function test_qcOfATrueHdStreamMeasuresEachPresentationAndPassesTheGate() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        QcController.measureFile(streamUrl);
        compare(QcController.busy, true);
        tryCompare(QcController, "busy", false, 30000);
        compare(QcController.error, "");
        compare(QcController.hasResult, true);
        verify(QcController.summaryLine.indexOf("TrueHD") === 0);
        const programmes = QcController.programmes;
        compare(programmes.length, 2);
        verify(programmes[0].label.indexOf("2ch (L R)") === 0);
        verify(programmes[1].label.indexOf("6ch (L R C LFE Ls Rs)") === 0);
        compare(QcController.hasIntegrity, true);
        compare(QcController.integrityOk, true);
        verify(QcController.integrityText.indexOf("lossless check:") === 0);
        verify(QcController.integrityText.indexOf("0 failed") > 0);
    }

    function test_qcOfAStreamJoinedEndToEndFailsTheIntegrityGate() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        QcController.measureFile(joinedUrl);
        tryCompare(QcController, "busy", false, 30000);
        compare(QcController.error, "");
        compare(QcController.hasIntegrity, true);
        compare(QcController.integrityOk, false);
        verify(QcController.integrityText.indexOf("1 failed") > 0);
    }

    function test_qcOfAnAc3StreamHasNoIntegrityGate() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        QcController.measureFile(Qt.resolvedUrl(
            "../../../../../libs/base/fuzz/seeds/fuzz_wav_read/roundtrip-stereo.ac3"));
        tryCompare(QcController, "busy", false, 15000);
        compare(QcController.hasIntegrity, false);
        compare(QcController.integrityText, "");
    }
}
