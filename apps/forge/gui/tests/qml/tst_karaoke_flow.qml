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
    property url effects
    property url silentEffects
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
        effects = audioFixtures.writeTones("kb-effects.wav", 48000, 1.024, [60]);
        // Silence: the window band-limits a channel assigned to the LFE (120 Hz, source-
        // assignment.md) where `forge encode` writes it as given, so a byte comparison of the
        // two is of a part that band-limiting leaves as it is.
        silentEffects = audioFixtures.writeTones("kb-effects-silent.wav", 48000, 1.024, [0]);
        verify(backing.toString().length > 0 && harmony.toString().length > 0 && effects.toString().length > 0,
               "the parts could not be written");
        defaultCmix = EncoderController.cmixIndex;
        defaultSurmix = EncoderController.surmixIndex;
    }

    function cleanup() {
        StreamPlayerController.pause();
        StreamPlayerController.karaokeReproduction = "asCoded";
        StreamPlayerController.karaokeVocals = "both";
        StreamPlayerController.resetKaraokeLevels();
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

        // A file's own LFE is the stream's: the bed has one, and the LFE channel is assigned.
        choose(ctx, "file", speakers30Lfe);
        tryCompare(ctx.flow.plan, "buildable", true, 5000);
        compare(ctx.flow.plan.lfe, true);
        compare(bedId(), "3/0");
        compare(EncoderController.bedLfe, true);
        verify(tokens().indexOf("=LFE") >= 0, tokens());
        verify(checkCodes(ctx.flow).indexOf("lfe-part") >= 0);
    }

    // ---- the LFE part ---------------------------------------------------------------
    function test_anLfePartIsTheStreamsLfeAndTheDraftHasIt() {
        const ctx = openFlow();
        useParts(ctx, { backing: backing, melody: melody, vocal1: lead, vocal2: harmony, lfe: effects });
        tryCompare(ctx.flow.plan, "mode", "3/2", 5000);
        compare(ctx.flow.plan.buildable, true);
        compare(ctx.flow.plan.lfe, true);
        // The 3/2 bed with its LFE, and the part assigned to it, after the full-bandwidth five.
        compare(bedId(), "3/2");
        compare(EncoderController.bedLfe, true);
        compare(tokens(), "0:0=L 0:1=R 1:0=C 2:0=Ls 3:0=Rs 4:0=LFE");
        compare(EncoderController.unassignedWarnings.length, 0);
        verify(checkCodes(ctx.flow).indexOf("lfe-part") >= 0);
        verify(checkCodes(ctx.flow).indexOf("parts-aligned") >= 0);
        // The row says what it is.
        compare(findByName(ctx.win, "karaokePartFile-lfe").text, "kb-effects.wav");
    }

    function test_removingTheLfePartTakesTheLfeOutOfTheDraft() {
        const ctx = openFlow();
        useParts(ctx, { backing: backing, melody: melody, vocal1: lead, lfe: effects });
        tryCompare(ctx.flow.plan, "lfe", true, 5000);
        compare(EncoderController.bedLfe, true);
        press(ctx, "karaokeRemove-lfe");
        tryCompare(ctx.flow.plan, "lfe", false, 5000);
        compare(ctx.flow.plan.mode, "3/1");
        compare(EncoderController.bedLfe, false);
        compare(tokens(), "0:0=L 0:1=R 1:0=C 2:0=Cs");
        verify(checkCodes(ctx.flow).indexOf("lfe-part") < 0);
    }

    function test_anLfePartAndABackingAreStillNothingToSingAlong() {
        const ctx = openFlow();
        useParts(ctx, { backing: backing, lfe: effects });
        tryVerify(() => checkCodes(ctx.flow).indexOf("nothing-to-sing") >= 0, 5000);
        compare(ctx.flow.plan.buildable, false);
        compare(ctx.flow.plan.mode, "");
    }

    function test_theStreamWithAnLfeIsHeardWithItsLfeInEveryListening() {
        const ctx = openFlow();
        useParts(ctx, { backing: backing, melody: melody, vocal1: lead, vocal2: harmony, lfe: effects });
        tryCompare(ctx.flow.plan, "mode", "3/2", 5000);
        press(ctx, "karaokeNextButton");   // Levels
        press(ctx, "karaokeNextButton");   // Hear: the stream is built
        tryCompare(ctx.flow, "currentStepKey", "hear", 5000);
        tryCompare(ctx.flow, "previewState", "ready", 60000);
        compare(StreamPlayerController.isKaraoke, true);
        verify(StreamPlayerController.codedLayoutLabel.indexOf("3/2") >= 0, StreamPlayerController.codedLayoutLabel);

        // Aware is L, C, R and the LFE: four channels where five and the LFE are coded.
        tryCompare(StreamPlayerController, "karaokeReproduction", "aware", 5000);
        tryVerify(() => StreamPlayerController.channelMeta.length === 4, 30000);
        compare(StreamPlayerController.karaokeReproduced, true);
        compare(findByName(ctx.win, "karaokePlays-LFE").text, "LFE");

        // As coded is the six the stream codes, the vocals in the rear speakers still.
        mouseClick(findByName(findByName(ctx.win, "karaokeListening"), "seg-asCoded"));
        tryCompare(StreamPlayerController, "karaokeReproduction", "asCoded", 5000);
        tryVerify(() => StreamPlayerController.channelMeta.length === 6, 30000);
        compare(findByName(ctx.win, "karaokePlays-LFE").text, "LFE");
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

    // The decoded stream the player holds, as a WAV, and the peak of one of its channels: what the
    // reproduction actually put there, not only that the page says so.
    function exportedPeak(name, channel) {
        const out = audioFixtures.folder().toString() + "/" + name;
        StreamPlayerController.exportDecodedWav(out);
        tryVerify(() => !StreamPlayerController.exporting, 30000);
        return audioFixtures.channelPeak(out, channel);
    }

    function test_thePartLevelsAreTheCapablePlayersAndTakeAPartOutOfTheSums() {
        const ctx = openFlow();
        useParts(ctx, { backing: backing, melody: melody, vocal1: lead, vocal2: harmony });
        tryCompare(ctx.flow.plan, "mode", "3/2", 5000);
        press(ctx, "karaokeNextButton");   // Levels
        press(ctx, "karaokeNextButton");   // Hear: the stream is built
        tryCompare(ctx.flow, "currentStepKey", "hear", 5000);
        tryCompare(ctx.flow, "previewState", "ready", 60000);

        // The levels are the capable player's: closed under the aware one, open under capable.
        const levels = findByName(ctx.win, "karaokePartLevels");
        tryCompare(StreamPlayerController, "karaokeReproduction", "aware", 5000);
        compare(levels.enabled, false);
        mouseClick(findByName(findByName(ctx.win, "karaokeListening"), "seg-capable"));
        tryCompare(StreamPlayerController, "karaokeReproduction", "capable", 5000);
        compare(levels.enabled, true);
        mouseClick(findByName(findByName(ctx.win, "karaokeVocals"), "seg-none"));
        tryCompare(StreamPlayerController, "karaokeVocals", "none", 5000);
        tryVerify(() => !StreamPlayerController.busy && StreamPlayerController.channelMeta.length === 3, 30000);

        // At the table's own levels the centre is the melody, and it is a real signal.
        compare(findByName(ctx.win, "karaokePlays-C").text, "M");
        compare(findByName(ctx.win, "karaokeLevelText-M").text, "0 dB");
        const at_table = exportedPeak("kl-table.wav", 2);
        verify(at_table > 0.01, "the melody is heard: " + at_table);

        // The melody 6 dB down: the centre is that fraction of it, exactly - no vocals are
        // chosen, so nothing else is scaled - and the page still says what plays.
        StreamPlayerController.karaokeMelodyDb = -6;
        tryVerify(() => !StreamPlayerController.busy && StreamPlayerController.hasResult, 30000);
        compare(findByName(ctx.win, "karaokeLevelText-M").text, "-6 dB");
        const six_down = exportedPeak("kl-six.wav", 2);
        fuzzyCompare(six_down, at_table * Math.pow(10, -6 / 20), 1e-4);
        compare(findByName(ctx.win, "karaokePlays-C").text, "M");

        // All the way down is off: nothing in the centre, and the page takes it out of what plays.
        StreamPlayerController.karaokeMelodyDb = -24;
        tryVerify(() => !StreamPlayerController.busy && StreamPlayerController.hasResult, 30000);
        compare(findByName(ctx.win, "karaokeLevelText-M").text, "off");
        compare(findByName(ctx.win, "karaokePlays-C").text, "silent");
        compare(exportedPeak("kl-off.wav", 2), 0.0);
        verify(exportedPeak("kl-off-left.wav", 0) > 0.01, "the backing is still heard");

        // A level is held to the library's span, and Reset puts all three back.
        StreamPlayerController.karaokeVocal1Db = 40;
        compare(StreamPlayerController.karaokeVocal1Db, 12);
        compare(findByName(ctx.win, "karaokeLevelText-V1").text, "+12 dB");
        StreamPlayerController.karaokeVocal2Db = -100;
        compare(StreamPlayerController.karaokeVocal2Db, -24);
        verify(ctx.flow.levelsMoved);
        mouseClick(findByName(ctx.win, "karaokeResetLevels"));
        compare(StreamPlayerController.karaokeMelodyDb, 0);
        compare(StreamPlayerController.karaokeVocal1Db, 0);
        compare(StreamPlayerController.karaokeVocal2Db, 0);
        tryVerify(() => !StreamPlayerController.busy && StreamPlayerController.hasResult, 30000);
        compare(findByName(ctx.win, "karaokePlays-C").text, "M");
        verify(exportedPeak("kl-reset.wav", 2) > 0.01);
    }

    function test_aStreamWithNoVocal2HasNoLevelForIt() {
        const ctx = openFlow();
        useParts(ctx, { backing: backing, melody: melody, vocal1: lead });
        tryCompare(ctx.flow.plan, "mode", "3/1", 5000);
        press(ctx, "karaokeNextButton");
        press(ctx, "karaokeNextButton");
        tryCompare(ctx.flow, "currentStepKey", "hear", 5000);
        tryCompare(ctx.flow, "previewState", "ready", 60000);
        // The levels are the capable player's; a part the stream does not have has none.
        mouseClick(findByName(findByName(ctx.win, "karaokeListening"), "seg-capable"));
        tryCompare(StreamPlayerController, "karaokeReproduction", "capable", 5000);
        tryCompare(findByName(ctx.win, "karaokeLevel-M"), "enabled", true, 5000);
        compare(findByName(ctx.win, "karaokeLevel-V1").enabled, true);
        compare(findByName(ctx.win, "karaokeLevel-V2").enabled, false);
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

    function test_saveWithAnLfeWritesTheStreamTheCommandLineWritesAndItHasAnLfe() {
        const ctx = openFlow();
        useParts(ctx, { backing: backing, melody: melody, vocal1: lead, lfe: silentEffects });
        tryCompare(ctx.flow.plan, "lfe", true, 5000);
        ctx.flow.currentStepKey = "save";
        const line = findByName(ctx.win, "karaokeCommandLine").text;
        verify(line.indexOf("forge encode") === 0, line);
        verify(line.indexOf("lfe") >= 0 || line.indexOf("LFE") >= 0, line);

        const out = Qt.resolvedUrl("_test_output/tst_karaoke_flow_lfe.ac3");
        press(ctx, "karaokeNextButton");
        const dialog = findByName(ctx.win, "saveDialog");
        tryCompare(dialog, "visible", true, 5000);
        dialog.selectedFile = out;
        dialog.accepted();
        dialog.close();
        tryCompare(EncoderController, "busy", false, 60000);

        StreamPlayerController.karaokeReproduction = "asCoded";
        StreamPlayerController.openFile(out);
        tryCompare(StreamPlayerController, "hasResult", true, 30000);
        compare(StreamPlayerController.isKaraoke, true);
        verify(StreamPlayerController.codedLayoutLabel.indexOf("3/1") >= 0, StreamPlayerController.codedLayoutLabel);
        // Four coded channels and the LFE, which a plain surround player decodes as five.
        tryVerify(() => StreamPlayerController.channelMeta.length === 5, 30000);

        if (cliRunner.available()) {
            const folder = Qt.resolvedUrl("_test_output/tst_karaoke_flow_lfe_cli");
            verify(cliRunner.prepare(folder, backing));
            verify(cliRunner.copyInto(folder, melody));
            verify(cliRunner.copyInto(folder, lead));
            verify(cliRunner.copyInto(folder, silentEffects));
            compare(cliRunner.run(line, folder), 0, line);
            verify(cliRunner.sameBytes(out, Qt.resolvedUrl("_test_output/tst_karaoke_flow_lfe_cli/out.ac3")),
                   "the command line wrote different bytes than the window did");
        }
    }

    // ---- The format (planning/karaoke-eac3.md, E6, D7) -----------------------------------
    // AC-3 is what Annex C defines and the default; E-AC-3 carries the same service type, and a
    // note says what is this project's to reproduce and what a player may not.
    function test_theFormatIsAc3ByDefaultAndEac3IsAChoiceThatSaysWhatIsOurs() {
        const ctx = openFlow();
        useParts(ctx, { backing: backing, melody: melody, vocal1: lead });
        tryCompare(ctx.flow.plan, "mode", "3/1", 5000);
        ctx.flow.currentStepKey = "save";
        const format = findByName(ctx.win, "karaokeFormat");
        compare(format.currentIndex, 0);
        compare(EncoderController.codecIndex, 0);
        compare(findByName(ctx.win, "karaokeFormatNote").visible, false);
        verify(findByName(ctx.win, "karaokeCommandLine").text.indexOf("forge encode") === 0);

        ctx.flow.setFormat("eac3");
        tryCompare(EncoderController, "codecIndex", 1, 5000);
        // The parts are loaded again under the codec, and the plan with them.
        tryCompare(ctx.flow.plan, "mode", "3/1", 5000);
        compare(format.currentIndex, 1);
        compare(findByName(ctx.win, "karaokeFormatNote").visible, true);
        const line = findByName(ctx.win, "karaokeCommandLine").text;
        verify(line.indexOf("forge eac3-encode") === 0, line);
        verify(line.indexOf("bsmod=") >= 0, line);
        verify(line.indexOf(".ec3") >= 0, line);

        ctx.flow.setFormat("ac3");
        tryCompare(EncoderController, "codecIndex", 0, 5000);
        compare(findByName(ctx.win, "karaokeFormatNote").visible, false);
    }

    function test_saveAsEac3WritesAKaraokeStreamThePlayerReproducesAndTheCommandLineWritesTheSame() {
        const ctx = openFlow();
        useParts(ctx, { backing: backing, melody: melody, vocal1: lead });
        tryCompare(ctx.flow.plan, "mode", "3/1", 5000);
        ctx.flow.setFormat("eac3");
        tryCompare(EncoderController, "codecIndex", 1, 5000);
        tryCompare(ctx.flow.plan, "mode", "3/1", 5000);
        ctx.flow.currentStepKey = "save";
        const line = findByName(ctx.win, "karaokeCommandLine").text;
        verify(line.indexOf("forge eac3-encode") === 0, line);

        const out = Qt.resolvedUrl("_test_output/tst_karaoke_flow_eac3.ec3");
        press(ctx, "karaokeNextButton");
        const dialog = findByName(ctx.win, "saveDialog");
        tryCompare(dialog, "visible", true, 5000);
        dialog.selectedFile = out;
        dialog.accepted();
        dialog.close();
        tryCompare(EncoderController, "busy", false, 60000);

        // A karaoke stream the player names, as coded and reproduced: bsmod 7 at 3/1 is L, C, R
        // and V1, which the aware reproduction folds to L, C and R.
        StreamPlayerController.karaokeReproduction = "asCoded";
        StreamPlayerController.openFile(out);
        tryCompare(StreamPlayerController, "hasResult", true, 30000);
        compare(StreamPlayerController.isKaraoke, true);
        verify(StreamPlayerController.codedLayoutLabel.indexOf("3/1") >= 0,
               StreamPlayerController.codedLayoutLabel);
        tryVerify(() => StreamPlayerController.channelMeta.length === 4, 30000);

        StreamPlayerController.karaokeReproduction = "aware";
        tryCompare(StreamPlayerController, "karaokeReproduced", true, 30000);
        tryVerify(() => StreamPlayerController.channelMeta.length === 3, 30000);

        if (cliRunner.available()) {
            const folder = Qt.resolvedUrl("_test_output/tst_karaoke_flow_eac3_cli");
            verify(cliRunner.prepare(folder, backing));
            verify(cliRunner.copyInto(folder, melody));
            verify(cliRunner.copyInto(folder, lead));
            compare(cliRunner.run(line, folder), 0, line);
            verify(cliRunner.sameBytes(out, Qt.resolvedUrl("_test_output/tst_karaoke_flow_eac3_cli/out.ec3")),
                   "the command line wrote different bytes than the window did");
        }
    }

    function test_theHearStepPlaysTheEac3StreamUnderTheSameChoicesAndLevels() {
        const ctx = openFlow();
        useParts(ctx, { backing: backing, melody: melody, vocal1: lead, vocal2: harmony });
        tryCompare(ctx.flow.plan, "mode", "3/2", 5000);
        ctx.flow.setFormat("eac3");
        tryCompare(EncoderController, "codecIndex", 1, 5000);
        tryCompare(ctx.flow.plan, "mode", "3/2", 5000);
        press(ctx, "karaokeNextButton");   // Levels
        press(ctx, "karaokeNextButton");   // Hear: the stream is built
        tryCompare(ctx.flow, "currentStepKey", "hear", 5000);
        tryCompare(ctx.flow, "previewState", "ready", 60000);
        compare(StreamPlayerController.isKaraoke, true);
        verify(StreamPlayerController.codedLayoutLabel.indexOf("3/2") >= 0,
               StreamPlayerController.codedLayoutLabel);

        // Aware: L, C and R, the vocals mixed, from an E-AC-3 stream as from an AC-3 one.
        tryCompare(StreamPlayerController, "karaokeReproduction", "aware", 5000);
        tryVerify(() => StreamPlayerController.channelMeta.length === 3, 30000);
        compare(StreamPlayerController.karaokeReproduced, true);
        compare(findByName(ctx.win, "karaokePlays-L").text, "L + V1");
        compare(findByName(ctx.win, "karaokePlays-C").text, "M");
        compare(findByName(ctx.win, "karaokePlays-R").text, "R + V2");

        // As coded: the five a plain surround player decodes, the vocals in the rear.
        mouseClick(findByName(findByName(ctx.win, "karaokeListening"), "seg-asCoded"));
        tryCompare(StreamPlayerController, "karaokeReproduction", "asCoded", 5000);
        tryVerify(() => StreamPlayerController.channelMeta.length === 5, 30000);
        compare(StreamPlayerController.karaokeReproduced, false);
        compare(findByName(ctx.win, "karaokePlays-Ls").text, "V1");
        compare(findByName(ctx.win, "karaokePlays-Rs").text, "V2");

        // Capable takes the singer's vocals, as it does for AC-3.
        mouseClick(findByName(findByName(ctx.win, "karaokeListening"), "seg-capable"));
        tryCompare(StreamPlayerController, "karaokeReproduction", "capable", 5000);
        mouseClick(findByName(findByName(ctx.win, "karaokeVocals"), "seg-v1"));
        tryCompare(StreamPlayerController, "karaokeVocals", "v1", 5000);
        tryVerify(() => StreamPlayerController.channelMeta.length === 3, 30000);
        compare(findByName(ctx.win, "karaokePlays-C").text, "M + V1");
        compare(findByName(ctx.win, "karaokePlays-L").text, "L");
    }

    // ---- Naming, the rate, edits, the preview and the lyrics ---------------------------
    function test_aKaraokeDraftIsNamedForWhatItIs() {
        const ctx = openFlow();
        useParts(ctx, { backing: backing, melody: melody, vocal1: lead, vocal2: harmony });
        tryCompare(ctx.flow.plan, "mode", "3/2", 5000);
        // Five channels, which a room would call 5.0: a karaoke stream's are a backing and parts.
        compare(EncoderController.planLabel, "Karaoke 3/2");
        verify(ctx.win.planLine.indexOf("Karaoke 3/2") >= 0, ctx.win.planLine);
        compare(EncoderController.channelShapeName, "5.0");
        // The same five channels with another service are a 5.0 again.
        EncoderController.bsmodIndex = 0;
        compare(EncoderController.planLabel, "5.0");
    }

    function test_theRateStartsAtTheModesAndTheAuthorsChoiceIsKept() {
        const ctx = openFlow();
        useParts(ctx, { backing: backing, melody: melody });
        tryCompare(ctx.flow.plan, "mode", "3/0", 5000);
        compare(EncoderController.bitrateKbps, 256);
        useParts(ctx, { vocal1: lead, vocal2: harmony });
        tryCompare(ctx.flow.plan, "mode", "3/2", 5000);
        compare(EncoderController.bitrateKbps, 384);

        // Picked on the Save step, the rate is the author's: a part leaving does not move it.
        ctx.flow.currentStepKey = "save";
        const box = findByName(ctx.win, "karaokeBitrate");
        const at = EncoderController.bitrates.indexOf(448);
        verify(at >= 0);
        box.currentIndex = at;
        box.activated(at);
        compare(EncoderController.bitrateKbps, 448);
        ctx.flow.currentStepKey = "parts";
        press(ctx, "karaokeRemove-vocal2");
        tryCompare(ctx.flow.plan, "mode", "3/1", 5000);
        compare(EncoderController.bitrateKbps, 448);
    }

    function test_whatTheAuthorSetOnThePartsSurvivesAPartChanging() {
        const ctx = openFlow();
        useParts(ctx, { backing: backing, melody: melody, vocal1: lead });
        tryCompare(ctx.flow.plan, "mode", "3/1", 5000);
        // As the rail and Expert set them: a start offset on the lead, a trim on the backing's
        // left channel and one on the lead.
        EncoderController.setSourceOffset(2, 0.25);
        EncoderController.setAssignmentTrim(0, 0, -3.0);
        EncoderController.setAssignmentTrim(2, 0, 2.5);

        // A second vocal arrives, which reloads every source.
        useParts(ctx, { vocal2: harmony });
        tryCompare(ctx.flow.plan, "mode", "3/2", 5000);
        const rows = EncoderController.sourceModel;
        compare(rows.length, 4);
        compare(rows[2].label, "kb-lead.wav");
        compare(rows[2].offsetSeconds, 0.25);
        compare(rows[0].offsetSeconds, 0);
        const trims = {};
        for (const row of EncoderController.assignmentRows) {
            trims[row.source + ":" + row.channel] = row.trimDb;
        }
        compare(trims["0:0"], -3.0);
        compare(trims["2:0"], 2.5);
        compare(trims["1:0"], 0, "nobody trimmed the melody");
    }

    function saveTo(ctx, url) {
        press(ctx, "karaokeNextButton");
        const dialog = findByName(ctx.win, "saveDialog");
        tryCompare(dialog, "visible", true, 5000);
        dialog.selectedFile = url;
        dialog.accepted();
        dialog.close();
        tryCompare(EncoderController, "busy", false, 60000);
    }

    function test_theHearPreviewIsNotARunAndASavedStreamIs() {
        const ctx = openFlow();
        useParts(ctx, { backing: backing, melody: melody, vocal1: lead });
        tryCompare(ctx.flow.plan, "mode", "3/1", 5000);
        const before = EncoderController.runs.length;
        ctx.flow.currentStepKey = "hear";
        tryCompare(ctx.flow, "previewState", "ready", 60000);
        compare(EncoderController.runs.length, before, "the preview was listed as a run");

        ctx.flow.currentStepKey = "save";
        const out = Qt.resolvedUrl("_test_output/tst_karaoke_polish_run.ac3");
        verify(audioFixtures.remove(out));
        saveTo(ctx, out);
        compare(EncoderController.runs.length, before + 1);
        compare(EncoderController.runs[0].filename, "tst_karaoke_polish_run.ac3");
        compare(EncoderController.runs[0].status, "done");
    }

    function test_aLyricsFileBesideTheBackingGoesBesideTheStream() {
        const song = audioFixtures.writeTones("kb-song.wav", 48000, 1.024, [440, 620]);
        const words = audioFixtures.writeText("kb-song.lrc", "[ti:Test]\n[00:00.10]<00:00.10>La <00:00.50>la\n");
        verify(song.toString().length > 0 && words.toString().length > 0);
        const ctx = openFlow();
        useParts(ctx, { backing: song, melody: melody });
        tryCompare(ctx.flow.plan, "mode", "3/0", 5000);
        // Offered because a .lrc of the backing's name is beside it.
        compare(ctx.flow.lyricsUrl, words.toString());
        ctx.flow.currentStepKey = "save";
        compare(findByName(ctx.win, "karaokeLyricsFile").text, "kb-song.lrc");

        const out = Qt.resolvedUrl("_test_output/tst_karaoke_polish_lyrics.ac3");
        const beside = Qt.resolvedUrl("_test_output/tst_karaoke_polish_lyrics.lrc");
        verify(audioFixtures.remove(out) && audioFixtures.remove(beside));
        saveTo(ctx, out);
        // Under the stream's name, with the file's own bytes, where Hearth looks.
        verify(cliRunner.sameBytes(words, beside), "the lyrics were not put beside the stream");
        verify(ctx.flow.lyricsNote.indexOf("beside the stream") >= 0, ctx.flow.lyricsNote);

        // Removed, none goes.
        press(ctx, "karaokeRemoveLyrics");
        compare(ctx.flow.lyricsUrl, "");
        const out2 = Qt.resolvedUrl("_test_output/tst_karaoke_polish_nolyrics.ac3");
        const beside2 = Qt.resolvedUrl("_test_output/tst_karaoke_polish_nolyrics.lrc");
        verify(audioFixtures.remove(out2) && audioFixtures.remove(beside2));
        saveTo(ctx, out2);
        compare(cliRunner.size(beside2), 0, "a lyrics file the author removed was put beside the stream");
    }

    function test_aSubRipFileBesideTheBackingIsWrittenBesideTheStreamAsUtf8Lrc() {
        const song = audioFixtures.writeTones("kc-song.wav", 48000, 1.024, [440, 620]);
        const words = audioFixtures.writeText(
            "kc-song.srt", "1\n00:00:01,000 --> 00:00:03,000\nLa la\n\n2\n00:00:05,000 --> 00:00:06,000\nLa\n");
        // What the stream's own lyrics file should hold: LRC, with the silence between the cues
        // marked as a time with no text, which a player reads as the line being over.
        const expected = audioFixtures.writeText("kc-song-expected.lrc",
                                                 "[00:01.00]La la\n[00:03.00]\n[00:05.00]La\n");
        verify(song.toString().length > 0 && words.toString().length > 0 && expected.toString().length > 0);
        const ctx = openFlow();
        useParts(ctx, { backing: song, melody: melody });
        tryCompare(ctx.flow.plan, "mode", "3/0", 5000);
        // Offered because an .srt of the backing's name is beside it and there is no .lrc.
        compare(ctx.flow.lyricsUrl, words.toString());
        compare(ctx.flow.lyricsProblem, "");
        ctx.flow.currentStepKey = "save";
        compare(findByName(ctx.win, "karaokeLyricsFile").text, "kc-song.srt");
        verify(!findByName(ctx.win, "karaokeLyricsProblem").visible);

        const out = Qt.resolvedUrl("_test_output/tst_karaoke_srt_lyrics.ac3");
        const beside = Qt.resolvedUrl("_test_output/tst_karaoke_srt_lyrics.lrc");
        verify(audioFixtures.remove(out) && audioFixtures.remove(beside));
        saveTo(ctx, out);
        verify(cliRunner.sameBytes(expected, beside), "the SubRip file was not written as LRC beside the stream");
        verify(ctx.flow.lyricsNote.indexOf("beside the stream") >= 0, ctx.flow.lyricsNote);
    }

    function test_aLyricsFileInACodePageIsSaidSoWhenChosenAndNothingIsWritten() {
        const song = audioFixtures.writeTones("kd-song.wav", 48000, 1.024, [440, 620]);
        // "[00:01.00]Caf" and an e acute as the one byte E9: Latin-1, which is not UTF-8.
        const words = audioFixtures.writeBytes("kd-song.lrc", [0x5b, 0x30, 0x30, 0x3a, 0x30, 0x31, 0x2e, 0x30, 0x30, 0x5d, 0x43, 0x61, 0x66, 0xe9, 0x0a]);
        verify(song.toString().length > 0 && words.toString().length > 0);
        const ctx = openFlow();
        useParts(ctx, { backing: song, melody: melody });
        tryCompare(ctx.flow.plan, "mode", "3/0", 5000);
        compare(ctx.flow.lyricsUrl, words.toString());
        // Said when it is offered, with the reader's own reason, and not found out at save.
        verify(ctx.flow.lyricsProblem.indexOf("UTF-8") >= 0, ctx.flow.lyricsProblem);
        ctx.flow.currentStepKey = "save";
        const problem = findByName(ctx.win, "karaokeLyricsProblem");
        verify(problem.visible);
        verify(problem.text.indexOf("cannot be used") >= 0, problem.text);

        const out = Qt.resolvedUrl("_test_output/tst_karaoke_codepage_lyrics.ac3");
        const beside = Qt.resolvedUrl("_test_output/tst_karaoke_codepage_lyrics.lrc");
        verify(audioFixtures.remove(out) && audioFixtures.remove(beside));
        saveTo(ctx, out);
        // The stream is written; the words are not, and the note says why.
        verify(cliRunner.size(out) > 0, "the stream was not written");
        compare(cliRunner.size(beside), 0, "a file in a code page was put beside the stream");
        verify(ctx.flow.lyricsNote.indexOf("could not be read") >= 0, ctx.flow.lyricsNote);
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
