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
    readonly property url moreSourceUrl: Qt.resolvedUrl("../fixtures/speakers-none-3ch.wav")
    readonly property url outputUrl: Qt.resolvedUrl("_test_output/tst_truehd_encode.thd")
    readonly property url notAWavUrl: Qt.resolvedUrl("../fixtures/truehd_m51_s24.thd")

    function init() {
        TruehdEncodeController.clear();
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

    // Each container is written, the summary names it, and the file opens in the player as TrueHD
    // in that container with the duration and speakers that were coded. That the file holds the
    // stream that was coded, access unit for access unit, is held by test_truehd_wrap.cpp.
    function test_eachContainerIsWrittenAndNamedInTheSummary() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        const containers = [
            {file: "tst_truehd_encode.mkv", label: " · in Matroska", player: " · in Matroska"},
            {file: "tst_truehd_encode.mp4", label: " · in MP4", player: " · in MP4"},
            {file: "tst_truehd_encode.m2ts", label: " · in a Blu-ray transport stream", player: " · in MPEG-TS"}
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
            // The player reads the same file back.
            StreamPlayerController.openFile(url);
            tryCompare(StreamPlayerController, "busy", false, 15000);
            compare(StreamPlayerController.error, "", entry.file);
            verify(StreamPlayerController.summaryLine.indexOf("TrueHD") === 0,
                   StreamPlayerController.summaryLine);
            verify(StreamPlayerController.summaryLine.indexOf(entry.player) > 0,
                   StreamPlayerController.summaryLine);
            verify(StreamPlayerController.durationSeconds > 0);
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

    // The channel meaning: the options of the command line that name a field of a presentation go
    // into the command the dialog shows, a value the command line would refuse is refused with its
    // sentence and stops the encode, and an empty field is not on the line.
    function test_theChannelMeaningIsInTheCommandLineAndABadValueIsRefused() {
        TruehdEncodeController.chooseSource(speakers51Url);
        TruehdEncodeController.chooseOutput(outputUrl);
        TruehdEncodeController.setOption("dialnorm-6ch", " 27 ");
        TruehdEncodeController.setOption("control-6ch", "on");
        TruehdEncodeController.setOption("drc-start-up-gain", "-12");
        TruehdEncodeController.setOption("restart", "64");
        const line = TruehdEncodeController.cliLine;
        verify(line.indexOf(" truehd-restart=64 truehd-dialnorm-6ch=27 truehd-control-6ch=on truehd-drc-start-up-gain=-12") > 0, line);
        compare(TruehdEncodeController.options["dialnorm-6ch"], "27");
        compare(TruehdEncodeController.canEncode, true);
        verify(TruehdEncodeController.optionNames.indexOf("source-format-8ch") >= 0);
        compare(TruehdEncodeController.optionNames.length, 9 + 13);

        TruehdEncodeController.setOption("dialnorm-6ch", "32");
        compare(TruehdEncodeController.optionErrors["dialnorm-6ch"],
                "truehd-dialnorm-6ch is a whole number from 0 to 31 (got '32')");
        compare(TruehdEncodeController.canEncode, false);
        TruehdEncodeController.setOption("control-6ch", "yes");
        compare(TruehdEncodeController.optionErrors["control-6ch"],
                "truehd-control-6ch is on or off (got 'yes')");
        TruehdEncodeController.setOption("heavy-drc-start-up-gain", "-33");
        verify(TruehdEncodeController.optionErrors["heavy-drc-start-up-gain"].indexOf("from -32 to 31") > 0);
        TruehdEncodeController.encode();  // refused: nothing runs
        compare(TruehdEncodeController.busy, false);

        TruehdEncodeController.setOption("dialnorm-6ch", "");
        TruehdEncodeController.setOption("control-6ch", "");
        TruehdEncodeController.setOption("heavy-drc-start-up-gain", "");
        compare(Object.keys(TruehdEncodeController.optionErrors).length, 0);
        compare(TruehdEncodeController.canEncode, true);
        compare(TruehdEncodeController.cliLine.indexOf("dialnorm"), -1);
        TruehdEncodeController.resetOptions();
        compare(TruehdEncodeController.cliLine.indexOf("truehd-drc-start-up-gain"), -1);
    }

    // The channel meaning reaches the stream: QC reads back the dialogue norm each presentation
    // stores, and the summary says the meaning was written and that it is provisional.
    function test_theChannelMeaningIsWrittenIntoTheStream() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        encodeAndWait(speakers51Url);
        compare(TruehdEncodeController.summary.indexOf("channel meaning"), -1);

        TruehdEncodeController.setOption("dialnorm-2ch", "20");
        TruehdEncodeController.setOption("dialnorm-6ch", "27");
        encodeAndWait(speakers51Url);
        compare(TruehdEncodeController.error, "");
        verify(TruehdEncodeController.summary.indexOf("channel meaning written (provisional") > 0,
               TruehdEncodeController.summary);

        QcController.measureFile(outputUrl);
        tryCompare(QcController, "busy", false, 30000);
        compare(QcController.error, "");
        const programmes = QcController.programmes;
        compare(programmes.length, 2);
        compare(programmes[0].dialnorm, 20);
        compare(programmes[1].dialnorm, 27);
    }

    // The channel meaning panel: collapsed until asked for; the fields show the controller's values,
    // a refused value is refused under its field with the sentence of the command line, and the
    // control box sets on, off or back to the default.
    function test_theChannelMeaningPanelShowsAndSetsTheFields() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        const dialog = findChild(win.contentItem, "truehdEncodeDialog");
        verify(dialog !== null);
        dialog.open();
        tryCompare(dialog, "visible", true, 5000);
        const toggle = findChild(dialog.contentItem, "truehdEncodeMeaningToggle");
        const panel = findChild(dialog.contentItem, "truehdEncodeMeaningPanel");
        verify(toggle !== null && panel !== null);
        compare(panel.visible, false);
        toggle.clicked();
        compare(panel.visible, true);

        const field = findChild(dialog.contentItem, "truehdEncodeOption_dialnorm-8ch");
        verify(field !== null);
        field.insert(0, "40");
        field.textEdited();
        compare(TruehdEncodeController.options["dialnorm-8ch"], "40");
        const error = findChild(dialog.contentItem, "truehdEncodeOptionError_dialnorm-8ch");
        tryCompare(error, "visible", true, 2000);
        verify(error.text.indexOf("truehd-dialnorm-8ch is a whole number from 0 to 31") === 0, error.text);
        compare(TruehdEncodeController.canEncode, false);
        field.remove(0, field.length);
        field.textEdited();
        tryCompare(error, "visible", false, 2000);

        const control = findChild(dialog.contentItem, "truehdEncodeOption_control-2ch");
        verify(control !== null);
        control.currentIndex = 1;
        control.activated(1);
        compare(TruehdEncodeController.options["control-2ch"], "on");
        control.currentIndex = 2;
        control.activated(2);
        compare(TruehdEncodeController.options["control-2ch"], "off");
        control.currentIndex = 0;
        control.activated(0);
        compare(TruehdEncodeController.options["control-2ch"], "");

        // Source format is a field of the 6 and 8 channel presentations only.
        verify(findChild(dialog.contentItem, "truehdEncodeOption_source-format-6ch") !== null);
        verify(findChild(dialog.contentItem, "truehdEncodeOption_source-format-8ch") !== null);
        compare(findChild(dialog.contentItem, "truehdEncodeOption_source-format-2ch"), null);

        TruehdEncodeController.setOption("drc-start-up-gain", "5");
        findChild(dialog.contentItem, "truehdEncodeResetOptions").clicked();
        compare(TruehdEncodeController.options["drc-start-up-gain"], "");
        compare(findChild(dialog.contentItem, "truehdEncodeOption_drc-start-up-gain").text, "");
        toggle.clicked();
        compare(panel.visible, false);
        dialog.close();
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

    // Several sources: a second WAV needs a map to say where its channels go, a map the command line
    // would refuse and an offset it would refuse are refused with a sentence, and the command line
    // shown carries src=, map= and offset=.
    function test_severalSourcesNeedAMapAndTheCommandLineCarriesThem() {
        TruehdEncodeController.chooseSource(speakers51Url);
        TruehdEncodeController.chooseOutput(outputUrl);
        compare(TruehdEncodeController.canEncode, true);
        TruehdEncodeController.addSource(moreSourceUrl);
        compare(TruehdEncodeController.extraSources.length, 1);
        verify(TruehdEncodeController.mapError.indexOf("needs a map") > 0, TruehdEncodeController.mapError);
        compare(TruehdEncodeController.canEncode, false);

        TruehdEncodeController.setMap("0.0:Q");
        verify(TruehdEncodeController.mapError.length > 0);
        compare(TruehdEncodeController.canEncode, false);
        TruehdEncodeController.setMap(" 0.0:L,0.1:R,1.0:C ");
        compare(TruehdEncodeController.mapText, "0.0:L,0.1:R,1.0:C");
        compare(TruehdEncodeController.mapError, "");
        compare(TruehdEncodeController.canEncode, true);

        TruehdEncodeController.setOffset(1, "0.25");
        const line = TruehdEncodeController.cliLine;
        verify(line.indexOf(" src=\"") > 0, line);
        verify(line.indexOf("speakers-none-3ch.wav\" map=0.0:L,0.1:R,1.0:C offset=1:0.25") > 0, line);
        TruehdEncodeController.setOffset(1, "-1");
        verify(TruehdEncodeController.offsetErrors[1].indexOf("seconds, 0 or more") > 0);
        verify(TruehdEncodeController.cliLine.indexOf("offset=") < 0);
        compare(TruehdEncodeController.canEncode, false);
        TruehdEncodeController.setOffset(1, "soon");
        compare(TruehdEncodeController.canEncode, false);
        TruehdEncodeController.setOffset(1, "0.25");
        compare(TruehdEncodeController.canEncode, true);

        // Removing the source takes its offset with it and leaves the map as it was typed.
        TruehdEncodeController.removeSource(0);
        compare(TruehdEncodeController.extraSources.length, 0);
        compare(TruehdEncodeController.cliLine.indexOf("offset="), -1);
        compare(TruehdEncodeController.cliLine.indexOf("src="), -1);
        verify(TruehdEncodeController.cliLine.indexOf(" map=0.0:L,0.1:R,1.0:C") > 0);
        TruehdEncodeController.clear();
        compare(TruehdEncodeController.mapText, "");
        compare(TruehdEncodeController.offsetTexts.length, 0);
    }

    // Two WAVs are coded as one stream: the channels are put where the map says, the summary says
    // where each came from, and a start offset makes the stream longer.
    function test_twoWavsAreCodedAsOneStreamAndAnOffsetLengthensIt() {
        TruehdEncodeController.verify = true;
        TruehdEncodeController.chooseSource(speakers51Url);
        TruehdEncodeController.chooseOutput(outputUrl);
        TruehdEncodeController.addSource(moreSourceUrl);
        TruehdEncodeController.setMap("0.0:L,0.1:R,1.0:C");
        compare(TruehdEncodeController.canEncode, true);
        TruehdEncodeController.encode();
        compare(TruehdEncodeController.busy, true);
        tryCompare(TruehdEncodeController, "busy", false, 60000);
        compare(TruehdEncodeController.error, "");
        const summary = TruehdEncodeController.summary;
        verify(summary.indexOf("3 channel(s)") > 0, summary);
        verify(summary.indexOf("placed: L <- 0.0, R <- 0.1, C <- 1.0") > 0, summary);
        verify(summary.indexOf("verified") > 0, summary);
        const base = secondsOf(summary);
        verify(base > 0, summary);

        TruehdEncodeController.setOffset(1, "1");
        TruehdEncodeController.encode();
        tryCompare(TruehdEncodeController, "busy", false, 60000);
        compare(TruehdEncodeController.error, "");
        verify(secondsOf(TruehdEncodeController.summary) > base, TruehdEncodeController.summary);

        // A place that makes no channel assignment is refused when the files are read.
        TruehdEncodeController.setMap("0.0:L,1.0:Cs");
        TruehdEncodeController.encode();
        tryCompare(TruehdEncodeController, "busy", false, 60000);
        verify(TruehdEncodeController.error.length > 0);
        compare(TruehdEncodeController.summary, "");
    }

    function secondsOf(summary) {
        const found = /([0-9.]+) s,/.exec(summary);
        return found ? parseFloat(found[1]) : -1;
    }

    // The panel: its rows follow the sources, typing into the map and an offset sets them, and Remove
    // takes a source away.
    function test_theSourcesPanelShowsAndSetsTheSources() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        TruehdEncodeController.chooseSource(speakers51Url);
        const dialog = findChild(win.contentItem, "truehdEncodeDialog");
        verify(dialog !== null);
        dialog.open();
        tryCompare(dialog, "visible", true, 5000);
        const more = findChild(dialog.contentItem, "truehdEncodeMoreSources");
        const panel = findChild(dialog.contentItem, "truehdEncodeSourcesPanel");
        compare(panel.visible, false);
        more.clicked();
        compare(panel.visible, true);
        verify(findChild(dialog.contentItem, "truehdEncodeSourceName_0") !== null);
        compare(findChild(dialog.contentItem, "truehdEncodeSourceName_1"), null);

        TruehdEncodeController.addSource(moreSourceUrl);
        tryVerify(() => findChild(dialog.contentItem, "truehdEncodeSourceName_1") !== null, 2000);
        const name = findChild(dialog.contentItem, "truehdEncodeSourceName_1");
        verify(name.text.indexOf("speakers-none-3ch.wav") >= 0, name.text);

        const map = findChild(dialog.contentItem, "truehdEncodeMap");
        map.insert(0, "0");
        map.textEdited();
        compare(TruehdEncodeController.mapText, "0");
        const mapError = findChild(dialog.contentItem, "truehdEncodeMapError");
        tryCompare(mapError, "visible", true, 2000);

        const offset = findChild(dialog.contentItem, "truehdEncodeOffset_1");
        verify(offset !== null);
        offset.insert(0, "x");
        offset.textEdited();
        compare(TruehdEncodeController.offsetTexts[1], "x");
        const offsetError = findChild(dialog.contentItem, "truehdEncodeOffsetError_1");
        tryCompare(offsetError, "visible", true, 2000);

        findChild(dialog.contentItem, "truehdEncodeRemoveSource_1").clicked();
        compare(TruehdEncodeController.extraSources.length, 0);
        tryVerify(() => findChild(dialog.contentItem, "truehdEncodeSourceName_1") === null, 2000);
        dialog.close();
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
