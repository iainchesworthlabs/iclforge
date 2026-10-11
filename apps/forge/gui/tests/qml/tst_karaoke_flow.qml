import QtQuick
import QtTest

import ForgeGui

// The Karaoke flow (KaraokeFlow.qml; planning/karaoke-design.md, K6; planning/
// karaoke.md A1 to A8) driven the way a person drives it: the part rows' own
// buttons open the WAV picker, Next walks Parts, Levels, Hear and Save, and the
// Save step's button goes through the same encode flow Guided's does. Every
// step goes through the real EncoderController / StreamPlayerController
// singletons.
//
// The parts are generated: one sine tone a channel (audioFixtures,
// qml_test_main.cpp), so that what a stream carries is named by its
// frequencies - backing L 440 and R 620, melody 800, vocal 1 1030, vocal 2
// 1270 Hz. Annex C's tables themselves are checked against the library's
// decoder by iclforge-forge-gui-tests (test_karaoke_outputs.cpp); these hold
// that the flow reaches them: the mode, the draft and the command line it
// writes, what the stream player decodes under each choice, and that a file
// that cannot be a karaoke stream says why.
TestCase {
    id: testCase
    name: "KaraokeFlow"
    when: windowShown

    Component { id: mainWindowComponent; Main {} }

    property url backing
    property url monoBacking
    property url melody
    property url lead
    property url harmony
    property url stereoLead
    readonly property url speakers21: Qt.resolvedUrl("../fixtures/speakers-2-1.wav")
    readonly property url speakers30Lfe: Qt.resolvedUrl("../fixtures/speakers-3-0-lfe.wav")
    readonly property url speakersNone: Qt.resolvedUrl("../fixtures/speakers-none-3ch.wav")

    // What the draft held before this suite, put back after every case: the
    // controller is one singleton shared by every suite in this binary.
    property int defaultCmix: 0
    property int defaultSurmix: 0

    // 1.024 s at 48 kHz is exactly 32 AC-3 frames, and 2.0 s is 62.5: the second
    // ends in a partial frame, which both the window and forge pad by holding each
    // channel's last sample (source_frame.hpp), and the byte comparisons below hold
    // them to it for both.
    property url longBacking
    property url longMelody
    property url longLead

    function initTestCase() {
        longBacking = audioFixtures.writeTones("kb-long-backing.wav", 48000, 2.0, [440, 620]);
        longMelody = audioFixtures.writeTones("kb-long-melody.wav", 48000, 2.0, [800]);
        longLead = audioFixtures.writeTones("kb-long-lead.wav", 48000, 2.0, [1030]);
        verify(longBacking.toString().length > 0 && longLead.toString().length > 0,
               "the long parts could not be written");
        backing = audioFixtures.writeTones("kb-backing.wav", 48000, 1.024, [440, 620]);
        monoBacking = audioFixtures.writeTones("kb-mono.wav", 48000, 1.024, [440]);
        melody = audioFixtures.writeTones("kb-melody.wav", 48000, 1.024, [800]);
        lead = audioFixtures.writeTones("kb-lead.wav", 48000, 1.024, [1030]);
        harmony = audioFixtures.writeTones("kb-harmony.wav", 48000, 1.024, [1270]);
        stereoLead = audioFixtures.writeTones("kb-duet.wav", 48000, 1.024, [1030, 1270]);
        verify(backing.toString().length > 0 && harmony.toString().length > 0, "the parts could not be written");
        defaultCmix = EncoderController.cmixIndex;
        defaultSurmix = EncoderController.surmixIndex;
    }

    function cleanup() {
        StreamPlayerController.pause();
        StreamPlayerController.karaokeReproduction = "asCoded";
        StreamPlayerController.karaokeVocals = "both";
        while (EncoderController.sourceModel.length > 0) {
            EncoderController.removeSource(0);
        }
        EncoderController.atmosEnabled = false;
        EncoderController.codecIndex = 0;
        EncoderController.containerIndex = 0;
        EncoderController.bitrateKbps = 192;
        EncoderController.bsmodIndex = 0;
        EncoderController.cmixIndex = defaultCmix;
        EncoderController.surmixIndex = defaultSurmix;
        EncoderController.applyChannelPreset("stereo");
    }

    // ---- helpers --------------------------------------------------------------
    // By objectName: the QObject tree first (dialogs are not items), then the
    // visual one, which reaches what a Repeater made - findChild does not.
    function visualFind(item, name) {
        if (item === null || item === undefined) {
            return null;
        }
        if (item.objectName === name) {
            return item;
        }
        const kids = item.children;
        if (kids !== undefined) {
            for (let i = 0; i < kids.length; ++i) {
                const found = visualFind(kids[i], name);
                if (found !== null) {
                    return found;
                }
            }
        }
        return null;
    }

    function findByName(root, name) {
        let found = null;
        tryVerify(() => {
            found = findChild(root, name);
            if (found === null) {
                found = visualFind(root.contentItem !== undefined ? root.contentItem : root, name);
            }
            return found !== null;
        }, 5000, "no object called " + name);
        return found;
    }

    function pickFile(root, dialogName, url) {
        const dialog = findByName(root, dialogName);
        tryCompare(dialog, "visible", true, 5000);
        dialog.selectedFile = url;
        dialog.accepted();  // runs the handler a real pick runs
        dialog.close();
        tryCompare(dialog, "visible", false, 5000);
    }

    function openFlow() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        tryCompare(win, "visible", true);
        win.tier = "karaoke";
        const flow = findByName(win, "karaokeFlow");
        tryCompare(flow, "visible", true, 5000);
        return { win: win, flow: flow };
    }

    // The part row's own button, then the picker it opened.
    function choose(ctx, part, url) {
        const button = findByName(ctx.win, "karaokeChoose-" + part);
        // The rows are in the step's own scroll: bring this one into view, as a
        // person would, before pressing it.
        const flick = ctx.flow.contentFlickable;
        const at = button.mapToItem(flick.contentItem, 0, 0);
        flick.contentY = Math.max(0, at.y - 60);
        mouseClick(button);
        pickFile(ctx.win, "karaokePartDialog", url);
    }

    function useParts(ctx, spec) {
        for (const part in spec) {
            choose(ctx, part, spec[part]);
        }
    }

    function tokens() {
        const out = [];
        for (const row of EncoderController.assignmentRows) {
            out.push(row.source + ":" + row.channel + "=" + row.destToken);
        }
        return out.join(" ");
    }

    function bedId() {
        return EncoderController.bedChoices[EncoderController.bedIndex].id;
    }

    function checkCodes(flow) {
        return flow.plan.checks.map(c => c.code);
    }

    function press(ctx, name) {
        const button = findByName(ctx.win, name);
        tryCompare(button, "enabled", true, 5000);
        mouseClick(button);
    }

    // ---- Parts --------------------------------------------------------------------
    function test_theFourPartsMakeA32StreamAndTheDraftFollows() {
        const ctx = openFlow();
        useParts(ctx, { backing: backing, melody: melody, vocal1: lead, vocal2: harmony });
        tryCompare(ctx.flow.plan, "mode", "3/2", 5000);
        compare(findByName(ctx.win, "karaokeModeLabel").text, qsTr("%1 · %2").arg("3/2").arg("L M R V1 V2"));
        compare(ctx.flow.plan.buildable, true);
        // The draft is what Expert would show: the 3/2 bed without an LFE, bsmod
        // 7, and each coded channel an assignment of its own.
        compare(bedId(), "3/2");
        compare(EncoderController.bedLfe, false);
        compare(EncoderController.codecIndex, 0);
        compare(EncoderController.bsmodIndex, 7);
        compare(tokens(), "0:0=L 0:1=R 1:0=C 2:0=Ls 3:0=Rs");
        compare(EncoderController.unassignedWarnings.length, 0);
        // The checks the sheet shows.
        verify(checkCodes(ctx.flow).indexOf("parts-aligned") >= 0);
        verify(checkCodes(ctx.flow).indexOf("each-part-one-place") >= 0);
        compare(findByName(ctx.win, "karaokeNextButton").enabled, true);
    }

    function test_eachModeFollowsWhatWasSupplied() {
        const ctx = openFlow();
        const cases = [
            { parts: { backing: backing, melody: melody }, mode: "3/0", map: "0:0=L 0:1=R 1:0=C" },
            { parts: { backing: backing, melody: melody, vocal1: lead }, mode: "3/1",
              map: "0:0=L 0:1=R 1:0=C 2:0=Cs" },
            { parts: { backing: backing, vocal1: lead, vocal2: harmony }, mode: "2/2",
              map: "0:0=L 0:1=R 1:0=Ls 2:0=Rs" },
            { parts: { backing: backing, vocal1: lead }, mode: "2/1", map: "0:0=L 0:1=R 1:0=Cs" },
        ];
        for (const c of cases) {
            // Parts not in this case are taken away, so each case stands alone.
            for (const part of ["melody", "vocal1", "vocal2"]) {
                if (!(part in c.parts)) {
                    ctx.flow.setUrl(part, "");
                }
            }
            useParts(ctx, c.parts);
            tryCompare(ctx.flow.plan, "mode", c.mode, 5000);
            compare(bedId(), c.mode, c.mode);
            compare(EncoderController.bsmodIndex, 7, c.mode);
            compare(tokens(), c.map, c.mode);
            compare(ctx.flow.plan.buildable, true, c.mode);
        }
    }

    function test_whatCannotBeUsedIsNamedAndNextStaysOff() {
        const ctx = openFlow();
        // Nothing chosen is not a list of problems.
        compare(ctx.flow.plan.checks.length, 0);
        compare(findByName(ctx.win, "karaokeNextButton").enabled, false);

        // Parts without a backing.
        useParts(ctx, { melody: melody, vocal1: lead });
        tryVerify(() => checkCodes(ctx.flow).indexOf("no-backing") >= 0, 5000);
        compare(ctx.flow.plan.buildable, false);
        compare(findByName(ctx.win, "karaokeNextButton").enabled, false);
        verify(findByName(ctx.win, "karaokeCheck-no-backing").text.indexOf("backing") >= 0);

        // A backing that is one channel is not L and R.
        choose(ctx, "backing", monoBacking);
        tryVerify(() => checkCodes(ctx.flow).indexOf("backing-not-stereo") >= 0, 5000);
        compare(ctx.flow.plan.buildable, false);

        // A vocal that is a pair of channels asks which one.
        choose(ctx, "backing", backing);
        choose(ctx, "vocal1", stereoLead);
        tryVerify(() => checkCodes(ctx.flow).indexOf("part-not-mono") >= 0, 5000);
        compare(ctx.flow.plan.buildable, false);

        // A second vocal with no first has no mode.
        ctx.flow.setUrl("vocal1", "");
        ctx.flow.setUrl("melody", "");
        choose(ctx, "vocal2", harmony);
        tryVerify(() => checkCodes(ctx.flow).indexOf("vocal2-needs-vocal1") >= 0, 5000);
        compare(ctx.flow.plan.buildable, false);
    }

    function test_aFileThatDoesNotSayWhichSpeakersItsChannelsAreIsNamed() {
        const ctx = openFlow();
        ctx.flow.setInputKind("file");
        tryCompare(ctx.flow, "inputKind", "file");
        choose(ctx, "file", speakersNone);
        tryVerify(() => checkCodes(ctx.flow).indexOf("speakers-unnamed") >= 0, 5000);
        compare(ctx.flow.plan.buildable, false);

        // An LFE is named, not dropped.
        choose(ctx, "file", speakers30Lfe);
        tryVerify(() => checkCodes(ctx.flow).indexOf("lfe-present") >= 0, 5000);
        compare(ctx.flow.plan.buildable, false);
    }

    function test_oneFileWhoseSpeakersAreNamedIsTheModeItSays() {
        const ctx = openFlow();
        ctx.flow.setInputKind("file");
        choose(ctx, "file", speakers21);
        tryCompare(ctx.flow.plan, "mode", "2/1", 5000);
        compare(ctx.flow.plan.buildable, true);
        compare(bedId(), "2/1");
        compare(EncoderController.bsmodIndex, 7);
        // L, R and the one surround, which a 2/1 bed calls Cs.
        compare(tokens(), "0:0=L 0:1=R 0:2=Cs");
    }

    // ---- Levels --------------------------------------------------------------------
    function test_theLevelsAreTheEncodersAndOnlyWhereTheModeHasThem() {
        const ctx = openFlow();
        useParts(ctx, { backing: backing, vocal1: lead });
        tryCompare(ctx.flow.plan, "mode", "2/1", 5000);
        press(ctx, "karaokeNextButton");
        tryCompare(ctx.flow, "currentStepKey", "levels", 5000);
        // 2/1 has a surround and no centre: the surround level is a control and
        // the centre level is greyed, with the reason beside it.
        const centre = findByName(ctx.win, "karaokeCentreLevel");
        const surround = findByName(ctx.win, "karaokeSurroundLevel");
        compare(centre.parent.enabled, false);
        compare(surround.parent.enabled, true);
        mouseClick(findByName(surround, "seg-1"));
        compare(EncoderController.surmixIndex, 1);
    }

    // ---- Hear -----------------------------------------------------------------------
    function test_theStreamBuiltIsDecodedUnderEachChoiceASingerHas() {
        const ctx = openFlow();
        useParts(ctx, { backing: backing, melody: melody, vocal1: lead, vocal2: harmony });
        tryCompare(ctx.flow.plan, "mode", "3/2", 5000);
        press(ctx, "karaokeNextButton");   // Levels
        press(ctx, "karaokeNextButton");   // Hear: the stream is built
        tryCompare(ctx.flow, "currentStepKey", "hear", 5000);
        tryCompare(ctx.flow, "previewState", "ready", 60000);
        compare(StreamPlayerController.hasResult, true);
        compare(StreamPlayerController.isKaraoke, true);
        verify(StreamPlayerController.codedLayoutLabel.indexOf("3/2") >= 0, StreamPlayerController.codedLayoutLabel);

        // Aware is the player's default here: L, C and R, the vocals mixed.
        tryCompare(StreamPlayerController, "karaokeReproduction", "aware", 5000);
        tryVerify(() => StreamPlayerController.channelMeta.length === 3, 30000);
        compare(StreamPlayerController.karaokeReproduced, true);
        compare(findByName(ctx.win, "karaokePlays-L").text, "L + V1");
        compare(findByName(ctx.win, "karaokePlays-C").text, "M");
        compare(findByName(ctx.win, "karaokePlays-R").text, "R + V2");

        // As coded is the stream as a plain surround player decodes it: five
        // channels, the vocals in the rear speakers.
        mouseClick(findByName(findByName(ctx.win, "karaokeListening"), "seg-asCoded"));
        tryCompare(StreamPlayerController, "karaokeReproduction", "asCoded", 5000);
        tryVerify(() => StreamPlayerController.channelMeta.length === 5, 30000);
        compare(StreamPlayerController.karaokeReproduced, false);
        compare(findByName(ctx.win, "karaokePlays-Ls").text, "V1");
        compare(findByName(ctx.win, "karaokePlays-Rs").text, "V2");

        // Capable takes the singer's choice of vocals.
        mouseClick(findByName(findByName(ctx.win, "karaokeListening"), "seg-capable"));
        tryCompare(StreamPlayerController, "karaokeReproduction", "capable", 5000);
        mouseClick(findByName(findByName(ctx.win, "karaokeVocals"), "seg-v1"));
        tryCompare(StreamPlayerController, "karaokeVocals", "v1", 5000);
        tryVerify(() => StreamPlayerController.channelMeta.length === 3, 30000);
        compare(findByName(ctx.win, "karaokePlays-C").text, "M + V1");
        compare(findByName(ctx.win, "karaokePlays-L").text, "L");
    }

    // ---- Save -----------------------------------------------------------------------
    function test_saveWritesAKaraokeStreamTheCommandLineReproduces() {
        const ctx = openFlow();
        useParts(ctx, { backing: backing, melody: melody, vocal1: lead });
        tryCompare(ctx.flow.plan, "mode", "3/1", 5000);
        ctx.flow.currentStepKey = "save";
        const line = findByName(ctx.win, "karaokeCommandLine").text;
        verify(line.indexOf("forge encode") === 0, line);
        verify(line.indexOf("bsmod=") >= 0, line);
        verify(line.indexOf("map=") >= 0, line);

        // The same encode flow Guided's "Encode now" runs: the save picker with a
        // planned name, then the encoder.
        const out = Qt.resolvedUrl("_test_output/tst_karaoke_flow.ac3");
        press(ctx, "karaokeNextButton");
        const dialog = findByName(ctx.win, "saveDialog");
        tryCompare(dialog, "visible", true, 5000);
        dialog.selectedFile = out;
        dialog.accepted();
        dialog.close();
        tryCompare(EncoderController, "busy", false, 60000);

        // What it wrote is a karaoke stream: bsmod 7 at 3/1, which the stream
        // player says by its own layout line.
        StreamPlayerController.karaokeReproduction = "asCoded";
        StreamPlayerController.openFile(out);
        tryCompare(StreamPlayerController, "hasResult", true, 30000);
        compare(StreamPlayerController.isKaraoke, true);
        verify(StreamPlayerController.codedLayoutLabel.indexOf("3/1") >= 0, StreamPlayerController.codedLayoutLabel);
        verify(StreamPlayerController.summaryLine.indexOf("karaoke") >= 0, StreamPlayerController.summaryLine);

        // And the line the page shows writes the same bytes through forge.
        if (cliRunner.available()) {
            const folder = Qt.resolvedUrl("_test_output/tst_karaoke_flow_cli");
            verify(cliRunner.prepare(folder, backing));
            verify(cliRunner.copyInto(folder, melody));
            verify(cliRunner.copyInto(folder, lead));
            compare(cliRunner.run(line, folder), 0, line);
                verify(cliRunner.sameBytes(out, Qt.resolvedUrl("_test_output/tst_karaoke_flow_cli/out.ac3")),
                   "the command line wrote different bytes than the window did");
        }
    }

    // Parts of 2.0 s end in half a frame. The window once wrote silence into the
    // rest of that frame where forge held the last sample, so the two files
    // differed in their last frame (610 of 48,384 bytes) for the same plan.
    function test_aPartialLastFrameIsWrittenAsTheCommandLineWritesIt() {
        const ctx = openFlow();
        useParts(ctx, { backing: longBacking, melody: longMelody, vocal1: longLead });
        tryCompare(ctx.flow.plan, "mode", "3/1", 5000);
        ctx.flow.currentStepKey = "save";
        const line = findByName(ctx.win, "karaokeCommandLine").text;
        verify(line.indexOf("forge encode") === 0, line);

        const out = Qt.resolvedUrl("_test_output/tst_karaoke_flow_long.ac3");
        press(ctx, "karaokeNextButton");
        const dialog = findByName(ctx.win, "saveDialog");
        tryCompare(dialog, "visible", true, 5000);
        dialog.selectedFile = out;
        dialog.accepted();
        dialog.close();
        tryCompare(EncoderController, "busy", false, 60000);
        // 63 frames: 62.5 rounded up, which is the case the test is about.
        compare(cliRunner.size(out) % 63, 0, "the stream is not a whole number of 63 frames");

        if (cliRunner.available()) {
            const folder = Qt.resolvedUrl("_test_output/tst_karaoke_flow_long_cli");
            verify(cliRunner.prepare(folder, longBacking));
            verify(cliRunner.copyInto(folder, longMelody));
            verify(cliRunner.copyInto(folder, longLead));
            compare(cliRunner.run(line, folder), 0, line);
            verify(cliRunner.sameBytes(out, Qt.resolvedUrl("_test_output/tst_karaoke_flow_long_cli/out.ac3")),
                   "the command line wrote different bytes than the window did for a partial last frame");
        }
    }
}
