import QtQuick
import QtTest

import ForgeGui

// Encode TrueHD: a WAV coded losslessly into a bare .thd stream (TruehdEncodeController, the
// dialog TruehdEncodeDialog.qml), the window's twin of `forge truehd-encode`. The proof is a round
// trip through the window's own other surfaces: the stream it writes opens in the player as
// TrueHD with the length of the WAV, and passes the integrity gate of QC; the encode's own verify
// option decodes every access unit against the input. The samples themselves are held to the
// input by libs/truehd's encoder tests and by `forge truehd-encode`'s decode-encode-decode case.
TestCase {
    id: testCase
    name: "TruehdEncode"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    readonly property url speakers51Url: Qt.resolvedUrl("../fixtures/speakers-3-0-lfe.wav")
    readonly property url outputUrl: Qt.resolvedUrl("_test_output/tst_truehd_encode.thd")
    readonly property url notAWavUrl: Qt.resolvedUrl("../fixtures/truehd_m51_s24.thd")

    function init() {
        TruehdEncodeController.verify = false;
        TruehdEncodeController.standardDownmix = false;
    }

    function encodeAndWait(source) {
        TruehdEncodeController.chooseSource(source);
        TruehdEncodeController.chooseOutput(outputUrl);
        compare(TruehdEncodeController.canEncode, true);
        TruehdEncodeController.encode();
        compare(TruehdEncodeController.busy, true);
        tryCompare(TruehdEncodeController, "busy", false, 60000);
    }

    // The command line a run is the twin of: the paths, then the options the boxes set.
    function test_theCommandLineNamesTheFilesAndTheOptionsChosen() {
        TruehdEncodeController.chooseSource(speakers51Url);
        TruehdEncodeController.chooseOutput(outputUrl);
        const plain = TruehdEncodeController.cliLine;
        verify(plain.indexOf("forge truehd-encode \"") === 0, plain);
        verify(plain.indexOf("speakers-3-0-lfe.wav\"") > 0);
        verify(plain.indexOf("tst_truehd_encode.thd\"") > 0);
        compare(plain.indexOf("verify"), -1);
        TruehdEncodeController.verify = true;
        TruehdEncodeController.standardDownmix = true;
        const full = TruehdEncodeController.cliLine;
        verify(full.indexOf(" truehd-downmix=standard") > 0, full);
        verify(full.lastIndexOf(" verify") === full.length - " verify".length, full);
    }

    function test_chooseSourceProposesAnOutputBesideItAndOutputsAreMadeThd() {
        TruehdEncodeController.chooseSource(speakers51Url);
        verify(TruehdEncodeController.outputPath.length > 0);
        TruehdEncodeController.chooseOutput(Qt.resolvedUrl("_test_output/named_oddly.wav"));
        verify(TruehdEncodeController.outputPath.slice(-4) === ".thd",
               TruehdEncodeController.outputPath);
    }

    // A WAV with a channel mask is coded, with verify on, and the stream it writes is TrueHD that
    // plays in the player and passes QC.
    function test_aWavIsCodedVerifiedAndTheStreamOpensInThePlayerAndPassesQc() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        TruehdEncodeController.verify = true;
        encodeAndWait(speakers51Url);
        compare(TruehdEncodeController.error, "");
        verify(TruehdEncodeController.summary.indexOf("Wrote ") === 0, TruehdEncodeController.summary);
        verify(TruehdEncodeController.summary.indexOf("verified") > 0);
        compare(TruehdEncodeController.progress, 1.0);

        StreamPlayerController.openFile(outputUrl);
        tryCompare(StreamPlayerController, "busy", false, 15000);
        compare(StreamPlayerController.error, "");
        verify(StreamPlayerController.summaryLine.indexOf("TrueHD") === 0,
               StreamPlayerController.summaryLine);
        verify(StreamPlayerController.durationSeconds > 0);

        QcController.measureFile(outputUrl);
        tryCompare(QcController, "busy", false, 30000);
        compare(QcController.error, "");
        compare(QcController.hasIntegrity, true);
        compare(QcController.integrityOk, true);
    }

    // A TrueHD session is saved as a project of codec truehd: the WAV, and the dialog's two options.
    // `forge project` runs the same file as truehd-encode (apps/forge/cli/tests/test_cli_project.cpp).
    function test_aTrueHdSessionIsSavedAsAProjectAndComesBack() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        TruehdEncodeController.clear();
        TruehdEncodeController.chooseSource(speakers51Url);
        TruehdEncodeController.verify = true;
        TruehdEncodeController.standardDownmix = true;
        const file = Qt.resolvedUrl("_test_output/tst_truehd_project.forgeproj");
        verify(win.saveProjectFile(file), "the project could not be saved");

        const project = win.captureProject();
        compare(project.encode.codec, "truehd");
        compare(project.sources.length, 1);
        compare(project.encode.truehd.verify, true);
        compare(project.encode.truehd.downmix, "standard");
        compare(Object.keys(project.encode.options).length, 0);

        TruehdEncodeController.clear();
        TruehdEncodeController.verify = false;
        TruehdEncodeController.standardDownmix = false;
        compare(TruehdEncodeController.sourcePath, "");
        verify(win.openProjectFile(file));
        verify(TruehdEncodeController.sourcePath.indexOf("speakers-3-0-lfe.wav") >= 0,
               TruehdEncodeController.sourcePath);
        compare(TruehdEncodeController.verify, true);
        compare(TruehdEncodeController.standardDownmix, true);
        compare(EncoderController.sourceModel.length, 0);
    }

    // What is not a WAV is refused with the reader's sentence, and nothing is written.
    function test_aFileThatIsNotAWavIsRefusedAndNothingIsWritten() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        encodeAndWait(notAWavUrl);
        compare(TruehdEncodeController.summary, "");
        verify(TruehdEncodeController.error.length > 0);
        verify(TruehdEncodeController.error.indexOf("truehd_m51_s24.thd") >= 0);
    }
}
