import QtQuick
import QtTest

import ForgeGui

// Encode TrueHD: a WAV coded losslessly into a .thd stream or a container (TruehdEncodeController, the
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
        TruehdEncodeController.resetOptions();
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

    // The way in: a button of the Open stream dialog, which closes it and opens the encode dialog.
    function test_theOpenStreamDialogOffersTheEncodeDialog() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        win.streamPlayerDialogRef.open();
        const button = findChild(win.contentItem, "spEncodeTruehdButton");
        verify(button !== null);
        const encodeDialog = findChild(win.contentItem, "truehdEncodeDialog");
        verify(encodeDialog !== null);
        compare(encodeDialog.visible, false);
        button.clicked();
        tryCompare(encodeDialog, "visible", true, 5000);
        compare(win.streamPlayerDialogRef.visible, false);
        encodeDialog.close();
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

    // The name of the output decides the container: .mkv, .mp4 and .m2ts / .ts are kept as chosen,
    // in any case, and a name that is none of those (or no name at all) is the bare stream.
    function test_aContainerNameIsKeptAndAnyOtherIsMadeThd() {
        TruehdEncodeController.chooseSource(speakers51Url);
        const kept = ["a.mkv", "a.MP4", "a.m2ts", "a.ts", "a.mka", "a.thd"];
        for (const name of kept) {
            TruehdEncodeController.chooseOutput(Qt.resolvedUrl("_test_output/" + name));
            verify(TruehdEncodeController.outputPath.slice(-name.length) === name,
                   TruehdEncodeController.outputPath);
        }
        TruehdEncodeController.chooseOutput(Qt.resolvedUrl("_test_output/a.mlp"));
        verify(TruehdEncodeController.outputPath.slice(-9) === "a.mlp.thd",
               TruehdEncodeController.outputPath);
    }

    // Each container is written, and the summary names it. That the file holds the stream that
    // was coded, access unit for access unit, is held by test_truehd_wrap.cpp (the player does not
    // open a TrueHD track in a container yet, so it cannot be the proof here).
    function test_eachContainerIsWrittenAndNamedInTheSummary() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        const containers = [
            {file: "tst_truehd_encode.mkv", label: " · in Matroska"},
            {file: "tst_truehd_encode.mp4", label: " · in MP4"},
            {file: "tst_truehd_encode.m2ts", label: " · in a Blu-ray transport stream"}
        ];
        for (const entry of containers) {
            const url = Qt.resolvedUrl("_test_output/" + entry.file);
            TruehdEncodeController.chooseSource(speakers51Url);
            TruehdEncodeController.chooseOutput(url);
            verify(TruehdEncodeController.cliLine.indexOf(entry.file + "\"") > 0,
                   TruehdEncodeController.cliLine);
            TruehdEncodeController.encode();
            tryCompare(TruehdEncodeController, "busy", false, 60000);
            compare(TruehdEncodeController.error, "", entry.file);
            verify(TruehdEncodeController.summary.indexOf("Wrote ") === 0,
                   TruehdEncodeController.summary);
            verify(TruehdEncodeController.summary.indexOf(entry.label) > 0,
                   TruehdEncodeController.summary);
        }
        // A bare stream names no container.
        TruehdEncodeController.chooseOutput(outputUrl);
        TruehdEncodeController.encode();
        tryCompare(TruehdEncodeController, "busy", false, 60000);
        compare(TruehdEncodeController.error, "");
        compare(TruehdEncodeController.summary.indexOf(" · in "), -1);
    }

    // The options of the "More options" panel are the truehd- options of the command line: each
    // value goes into the command the dialog shows, in the order `forge` is written, and a value the
    // command line would refuse is refused with its sentence and stops the encode.
    function test_theOptionsAreInTheCommandLineAndABadOneIsRefused() {
        TruehdEncodeController.chooseSource(speakers51Url);
        TruehdEncodeController.chooseOutput(outputUrl);
        compare(TruehdEncodeController.canEncode, true);
        TruehdEncodeController.setOption("restart", " 64 ");
        TruehdEncodeController.setOption("stereo-mode", "ltrt");
        TruehdEncodeController.setOption("peak-rate", "12000");
        const line = TruehdEncodeController.cliLine;
        verify(line.indexOf(" truehd-restart=64 truehd-peak-rate=12000 truehd-stereo-mode=ltrt") > 0, line);
        compare(TruehdEncodeController.options["restart"], "64");
        compare(TruehdEncodeController.canEncode, true);

        TruehdEncodeController.setOption("restart", "0");
        verify(TruehdEncodeController.optionErrors["restart"].indexOf("truehd-restart is") === 0,
               TruehdEncodeController.optionErrors["restart"]);
        verify(TruehdEncodeController.optionErrors["restart"].indexOf("(got '0')") > 0);
        compare(TruehdEncodeController.canEncode, false);
        TruehdEncodeController.setOption("downmix-gains", "-3,-3,off");
        verify(TruehdEncodeController.optionErrors["downmix-gains"].indexOf("-3,-3,off,-3") > 0);

        TruehdEncodeController.setOption("restart", "");
        TruehdEncodeController.setOption("downmix-gains", "");
        compare(Object.keys(TruehdEncodeController.optionErrors).length, 0);
        compare(TruehdEncodeController.canEncode, true);
        TruehdEncodeController.setOption("not-an-option", "1");
        compare(TruehdEncodeController.cliLine.indexOf("not-an-option"), -1);
        TruehdEncodeController.resetOptions();
        compare(TruehdEncodeController.cliLine.indexOf("truehd-restart"), -1);
        compare(TruehdEncodeController.options["peak-rate"], "");
    }

    // The options reach the encoder: a shorter restart interval makes more restart intervals in the
    // same audio, and the stream still verifies.
    function test_anOptionChangesWhatTheEncoderWrites() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        TruehdEncodeController.verify = true;
        encodeAndWait(speakers51Url);
        compare(TruehdEncodeController.error, "");
        const base = restartIntervalsOf(TruehdEncodeController.summary);
        verify(base > 0, TruehdEncodeController.summary);

        TruehdEncodeController.setOption("restart", "8");
        encodeAndWait(speakers51Url);
        compare(TruehdEncodeController.error, "");
        verify(TruehdEncodeController.summary.indexOf("verified") > 0);
        verify(restartIntervalsOf(TruehdEncodeController.summary) > base,
               TruehdEncodeController.summary);
    }

    function restartIntervalsOf(summary) {
        const found = /([0-9]+) restart intervals/.exec(summary);
        return found ? parseInt(found[1]) : -1;
    }

    // The panel: its fields show the controller's values, and typing into one sets the option.
    function test_thePanelShowsAndSetsTheOptions() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        const dialog = findChild(win.contentItem, "truehdEncodeDialog");
        verify(dialog !== null);
        dialog.open();
        tryCompare(dialog, "visible", true, 5000);
        const more = findChild(dialog.contentItem, "truehdEncodeMoreOptions");
        const panel = findChild(dialog.contentItem, "truehdEncodeOptionsPanel");
        compare(panel.visible, false);
        more.clicked();
        compare(panel.visible, true);

        const field = findChild(dialog.contentItem, "truehdEncodeOption_fir-order");
        verify(field !== null);
        field.insert(0, "9");  // an edit like typing, which keeps the field bound to the controller
        field.textEdited();
        compare(TruehdEncodeController.options["fir-order"], "9");
        const error = findChild(dialog.contentItem, "truehdEncodeOptionError_fir-order");
        tryCompare(error, "visible", true, 2000);
        verify(error.text.indexOf("truehd-fir-order is 0 to 8") === 0, error.text);

        const choice = findChild(dialog.contentItem, "truehdEncodeOption_stereo-mode");
        verify(choice !== null);
        choice.currentIndex = 2;
        choice.activated(2);
        compare(TruehdEncodeController.options["stereo-mode"], "ltrt");

        findChild(dialog.contentItem, "truehdEncodeResetOptions").clicked();
        compare(TruehdEncodeController.options["stereo-mode"], "");
        compare(field.text, "");
        dialog.close();
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
