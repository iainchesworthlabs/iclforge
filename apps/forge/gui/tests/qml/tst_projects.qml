import QtQuick
import QtTest

import ForgeGui

// Project files (ProjectFile and Main.qml's captureProject / applyProject;
// docs/forge/gui/projects.md): what the window saves, that it opens the same
// way again from another place, that a key it does not know goes through a
// round trip, and that a file that is not a project, or is a newer one, is
// refused with a reason rather than half read.
//
// The last case is the example under examples/karaoke/auld-lang-syne/: its
// project opens in the Karaoke flow, and the window's own encode of it is the
// stream the example ships, byte for byte.
TestCase {
    id: testCase
    name: "Projects"
    when: windowShown

    Component { id: mainWindowComponent; Main {} }

    property url backing
    property url melody
    property url lead
    property url effects
    readonly property url exampleProject:
        Qt.resolvedUrl("../../../../../examples/karaoke/auld-lang-syne/auld-lang-syne.forgeproj")
    readonly property url exampleStream:
        Qt.resolvedUrl("../../../../../examples/karaoke/auld-lang-syne/auld-lang-syne.ac3")

    property int defaultCmix: 0
    property int defaultSurmix: 0

    // 1.024 s at 48 kHz is exactly 32 AC-3 frames, as in tst_karaoke_flow.qml.
    function initTestCase() {
        backing = audioFixtures.writeTones("kp-backing.wav", 48000, 1.024, [440, 620]);
        melody = audioFixtures.writeTones("kp-melody.wav", 48000, 1.024, [800]);
        lead = audioFixtures.writeTones("kp-lead.wav", 48000, 1.024, [1030]);
        effects = audioFixtures.writeTones("kp-effects.wav", 48000, 1.024, [60]);
        verify(backing.toString().length > 0 && lead.toString().length > 0, "the parts could not be written");
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
        EncoderController.resetEncodeSettings();
        EncoderController.cmixIndex = defaultCmix;
        EncoderController.surmixIndex = defaultSurmix;
        EncoderController.applyChannelPreset("stereo");
    }

    // ---- helpers --------------------------------------------------------------
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

    function openWindow() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        tryCompare(win, "visible", true);
        return win;
    }

    function inTheFolder(name) {
        return audioFixtures.folder() + "/" + name;
    }

    // A project file, as the window's own reader hands it over: the whole document, with where
    // each source is made absolute.
    function readProject(file) {
        const read = ProjectFile.read(file);
        verify(read.ok, JSON.stringify(read.error));
        return JSON.parse(read.text);
    }

    function writeProject(name, document) {
        const file = audioFixtures.writeText(name, JSON.stringify(document, null, 2));
        verify(file.toString().length > 0);
        return file;
    }

    function bedId() {
        return EncoderController.bedChoices[EncoderController.bedIndex].id;
    }

    // The window set up as a karaoke stream of three parts, saved, and the
    // window put back to nothing: what the next case opens.
    function saveThreeParts(name) {
        const win = openWindow();
        win.tier = "karaoke";
        const flow = findByName(win, "karaokeFlow");
        flow.setUrl("backing", backing.toString());
        flow.setUrl("melody", melody.toString());
        flow.setUrl("vocal1", lead.toString());
        tryCompare(flow.plan, "mode", "3/1", 5000);
        flow.chooseListening("capable");
        flow.chooseVocals("v1");
        EncoderController.bitrateKbps = 256;
        EncoderController.cmixIndex = 2;
        const file = inTheFolder(name);
        verify(win.saveProjectFile(file), "the project could not be saved");
        return file;
    }

    // ---- saving ---------------------------------------------------------------
    function test_aSavedProjectNamesItsPartsBesideItAndHowTheyAreHeard() {
        const file = saveThreeParts("kp-saved.forgeproj");
        const project = readProject(file);
        compare(project.format, "iclforge-project");
        compare(project.version, 1);
        compare(project.flow, "karaoke");
        compare(project.sources.length, 3);
        // Relative to the project's folder, so the folder can be moved.
        compare(project.sources[0].path, "kp-backing.wav");
        compare(project.sources[0].part, "backing");
        compare(project.sources[1].path, "kp-melody.wav");
        compare(project.sources[1].part, "melody");
        compare(project.sources[2].part, "vocal1");
        compare(project.encode.codec, "ac3");
        compare(project.encode.container, "elementary");
        compare(project.encode.bitrateKbps, 256);
        // The command line's own words for the service and the mix level.
        compare(project.encode.options.bsmod, "voiceover");
        compare(project.encode.options.cmixlev, -6);
        compare(project.karaoke.listening, "capable");
        compare(project.karaoke.vocals, "v1");
        compare(project.bed, "3/1");
        // Read adds where each source is, made absolute: the window opens that.
        verify(project.sources[0].url.indexOf("kp-backing.wav") > 0);
        compare(project.sources[0].exists, true);
    }

    // ---- opening --------------------------------------------------------------
    function test_aProjectOpensInTheFlowItWasMadeIn() {
        const file = saveThreeParts("kp-open.forgeproj");
        cleanup();

        const win = openWindow();
        verify(win.openProjectFile(file));
        compare(win.tier, "karaoke");
        const flow = findByName(win, "karaokeFlow");
        compare(EncoderController.sourceModel.length, 3);
        compare(flow.plan.mode, "3/1");
        compare(flow.failedParts.length, 0);
        verify(flow.backingUrl.endsWith("kp-backing.wav"), flow.backingUrl);
        verify(flow.vocal1Url.endsWith("kp-lead.wav"), flow.vocal1Url);
        compare(flow.vocal2Url, "");
        compare(bedId(), "3/1");
        compare(EncoderController.bsmodIndex, 7);
        compare(EncoderController.bitrateKbps, 256);
        compare(EncoderController.cmixIndex, 2);
        compare(flow.listening, "capable");
        compare(flow.vocalsChoice, "v1");
        compare(win.projectUrl, file.toString());
    }

    // An LFE part is a part like the others: named in the project, and the same stream on opening.
    function test_anLfePartIsKeptInTheProjectAndComesBack() {
        const win = openWindow();
        win.tier = "karaoke";
        const flow = findByName(win, "karaokeFlow");
        flow.setUrl("backing", backing.toString());
        flow.setUrl("melody", melody.toString());
        flow.setUrl("vocal1", lead.toString());
        flow.setUrl("lfe", effects.toString());
        tryCompare(flow.plan, "lfe", true, 5000);
        const file = inTheFolder("kp-lfe.forgeproj");
        verify(win.saveProjectFile(file), "the project could not be saved");
        const project = readProject(file);
        compare(project.sources.length, 4);
        compare(project.sources[3].part, "lfe");
        compare(project.sources[3].path, "kp-effects.wav");
        compare(project.bed, "3/1");
        cleanup();

        const again = openWindow();
        verify(again.openProjectFile(file));
        const reopened = findByName(again, "karaokeFlow");
        compare(EncoderController.sourceModel.length, 4);
        verify(reopened.lfeUrl.endsWith("kp-effects.wav"), reopened.lfeUrl);
        compare(reopened.plan.lfe, true);
        compare(reopened.plan.mode, "3/1");
        compare(EncoderController.bedLfe, true);
        compare(reopened.failedParts.length, 0);
    }

    // The flow keeps an offset and a trim set on a part across the parts changing; a project
    // is another state altogether, and must not inherit what the session had set.
    function test_aProjectOpensWithoutTheSessionsOffsetsAndTrims() {
        const win = openWindow();
        win.tier = "karaoke";
        const flow = findByName(win, "karaokeFlow");
        flow.setUrl("backing", backing.toString());
        flow.setUrl("melody", melody.toString());
        flow.setUrl("vocal1", lead.toString());
        tryCompare(flow.plan, "mode", "3/1", 5000);
        const file = inTheFolder("kp-clean.forgeproj");
        verify(win.saveProjectFile(file), "the project could not be saved");

        // The same session, with its parts nudged and trimmed.
        EncoderController.setSourceOffset(0, 0.5);
        EncoderController.setAssignmentTrim(0, 0, -6.0);
        compare(EncoderController.sourceModel[0].offsetSeconds, 0.5);

        verify(win.openProjectFile(file));
        compare(EncoderController.sourceModel.length, 3);
        compare(EncoderController.sourceModel[0].offsetSeconds, 0);
        compare(EncoderController.assignmentRows[0].trimDb, 0);
    }

    function test_theHeaderOffersOpenAndSaveAndThePickersRunTheSameCode() {
        const file = saveThreeParts("kp-picker.forgeproj");
        cleanup();

        const win = openWindow();
        verify(findByName(win, "projectMenuButton") !== null);
        const dialog = findByName(win, "openProjectDialog");
        dialog.open();
        tryCompare(dialog, "visible", true, 5000);
        dialog.selectedFile = file;
        dialog.accepted();  // what a real pick runs
        dialog.close();
        tryCompare(dialog, "visible", false, 5000);
        compare(win.tier, "karaoke");
        compare(EncoderController.sourceModel.length, 3);
    }

    function test_openingAProjectReplacesTheSourcesRatherThanAddingToThem() {
        const file = saveThreeParts("kp-replace.forgeproj");
        const win = openWindow();
        EncoderController.addSourceFile(backing);
        compare(EncoderController.sourceModel.length >= 1, true);
        verify(win.openProjectFile(file));
        compare(EncoderController.sourceModel.length, 3);
    }

    function tokens() {
        const out = [];
        for (const row of EncoderController.assignmentRows) {
            out.push(row.source + ":" + row.channel + "=" + row.destToken);
        }
        return out.join(" ");
    }

    function setBed(id) {
        const beds = EncoderController.bedChoices;
        for (let i = 0; i < beds.length; ++i) {
            if (beds[i].id === id) {
                EncoderController.bedIndex = i;
            }
        }
    }

    function test_aDraftOutsideTheFlowsKeepsItsPlanAndItsRouting() {
        const win = openWindow();
        win.tier = "expert";
        EncoderController.loadSourceFile(backing);
        EncoderController.addSourceFile(melody);
        setBed("3/0");
        EncoderController.setAssignment(0, 0, "L");
        EncoderController.setAssignment(0, 1, "R");
        EncoderController.setAssignment(1, 0, "C");
        compare(tokens(), "0:0=L 0:1=R 1:0=C");
        EncoderController.setSourceOffset(1, 0.25);
        const file = inTheFolder("kp-expert.forgeproj");
        verify(win.saveProjectFile(file));
        const read = { project: readProject(file) };
        compare(read.project.flow, "expert");
        compare(read.project.bed, "3/0");
        compare(read.project.lfe, false);
        compare(read.project.sources.length, 2);
        compare(read.project.sources[0].part, undefined);
        compare(read.project.sources[1].offsetSeconds, 0.25);
        compare(read.project.assignments.length, 3);
        compare(read.project.assignments[2].to, "C");

        cleanup();
        const second = openWindow();
        verify(second.openProjectFile(file));
        compare(second.tier, "expert");
        compare(EncoderController.sourceModel.length, 2);
        compare(EncoderController.sourceModel[1].offsetSeconds, 0.25);
        compare(bedId(), "3/0");
        compare(tokens(), "0:0=L 0:1=R 1:0=C");
    }

    // ---- extending ------------------------------------------------------------
    function test_aKeyThisVersionDoesNotKnowSurvivesAnOpenAndASave() {
        const file = writeProject("kp-future.forgeproj", {
            format: "iclforge-project",
            version: 1,
            title: "From a later Forge",
            flow: "expert",
            sources: [{ path: "kp-backing.wav", note: "kept with its source", loop: true }],
            encode: { bitrateKbps: 256, tomorrow: 7,
                      options: { dialnorm: 27, "some-future-option": "x" } },
            comments: ["a", "b"],
            future: { x: 1 }
        });

        const win = openWindow();
        verify(win.openProjectFile(file));
        compare(EncoderController.bitrateKbps, 256);
        compare(EncoderController.sourceModel.length, 1);

        compare(EncoderController.dialnorm, 27);
        const again = inTheFolder("kp-future-saved.forgeproj");
        verify(win.saveProjectFile(again));
        const back = readProject(again);
        compare(back.title, "From a later Forge");
        compare(back.future.x, 1);
        compare(back.comments.length, 2);
        compare(back.encode.tomorrow, 7);
        compare(back.encode.bitrateKbps, 256);
        // What the window holds is the window's; what it does not is passed through.
        compare(back.encode.options.dialnorm, 27);
        compare(back.encode.options["some-future-option"], "x");
        compare(back.sources[0].note, "kept with its source");
        compare(back.sources[0].loop, true);
        compare(back.sources[0].path, "kp-backing.wav");
    }

    function test_theCodecAndTheContainerAreKeptByIdAndNotByPosition() {
        // The positions the ids stand for, which the file must not depend on.
        compare(EncoderController.codecNames[0], "AC-3");
        compare(EncoderController.codecNames[1], "E-AC-3");
        compare(EncoderController.codecNames[2], "AC-4");
        compare(EncoderController.containerNames[0], "Elementary stream");
        compare(EncoderController.containerNames[1].indexOf("Matroska"), 0);
        compare(EncoderController.containerNames[2].indexOf("S/PDIF"), 0);
        compare(EncoderController.containerNames[3].indexOf("MP4"), 0);
        compare(EncoderController.containerNames[4].indexOf("Fragmented"), 0);
        compare(EncoderController.containerNames[5].indexOf("MPEG-TS"), 0);

        EncoderController.codecIndex = 1;
        EncoderController.containerIndex = 3;
        EncoderController.bitrateKbps = 256;
        const kept = JSON.parse(ProjectFile.captureEncode(EncoderController));
        compare(kept.codec, "eac3");
        compare(kept.container, "mp4");
        compare(kept.bitrateKbps, 256);

        cleanup();
        compare(ProjectFile.applyEncode(EncoderController, kept, 0).length, 0);
        compare(EncoderController.codecIndex, 1);
        compare(EncoderController.containerIndex, 3);
        compare(ProjectFile.applyEncode(EncoderController, kept, 1).length, 0);
        compare(EncoderController.bitrateKbps, 256);
    }

    function test_aFreshDraftHasNoOptionsAndAProjectOfOneHasNone() {
        EncoderController.resetEncodeSettings();
        const kept = JSON.parse(ProjectFile.captureEncode(EncoderController));
        compare(Object.keys(kept.options).length, 0, JSON.stringify(kept.options));
        compare(kept.tools, undefined);
        compare(kept.vbr, undefined);
    }

    function test_theMetadataAndTheLoudnessAreKeptInTheCommandLinesOwnWords() {
        EncoderController.resetEncodeSettings();
        // The LFE mix and the preferred downmix ride E-AC-3's mixing metadata, and are kept with it.
        EncoderController.codecIndex = 1;
        EncoderController.mixmeta = true;
        EncoderController.drcIndex = 2;
        EncoderController.heavy = true;
        EncoderController.ceilingDb = -2.0;
        EncoderController.dialogueDb = -24.0;
        EncoderController.measureDialnorm = true;
        EncoderController.cmixIndex = 0;
        EncoderController.surmixIndex = 2;
        EncoderController.lfeMix = -1;
        EncoderController.dmixIndex = 1;
        EncoderController.bsmodIndex = 5;
        EncoderController.mixLevelDbSpl = 105;
        EncoderController.roomTypeIndex = 1;
        EncoderController.copyrightBit = true;
        EncoderController.originalBitstream = false;
        const names = EncoderController.drcNames;
        const kept = JSON.parse(ProjectFile.captureEncode(EncoderController)).options;
        compare(kept.drc, names[2]);
        compare(kept.heavy, true);
        compare(kept.ceiling, -2);
        compare(kept.dialogue, -24);
        compare(kept.dialnorm, "auto");
        compare(kept.cmixlev, -3);
        compare(kept.surmixlev, "off");
        compare(kept.mixmeta, true);
        compare(kept.lfemix, "off");
        compare(kept.dmixmod, "ltrt");
        compare(kept.bsmod, "commentary");
        compare(kept.mixlevel, 105);
        compare(kept.roomtyp, "large");
        compare(kept.copyright, true);
        compare(kept.origbs, "off");
        // The same words the window's own command line says them in.
        const line = EncoderController.metaTokens;
        for (const word of ["drc=" + names[2], "heavy", "ceiling=-2", "dialogue=-24",
                            "dialnorm=auto", "cmixlev=-3", "surmixlev=off", "mixmeta", "lfemix=off",
                            "dmixmod=ltrt", "bsmod=commentary", "mixlevel=105", "roomtyp=large",
                            "copyright", "origbs=off"]) {
            verify(line.split(" ").indexOf(word) >= 0, word + " is not in: " + line);
        }

        // And put back, over a window that held something else.
        const text = ProjectFile.captureEncode(EncoderController);
        EncoderController.resetEncodeSettings();
        EncoderController.drcIndex = 1;
        EncoderController.dialnorm = 20;
        EncoderController.surmixIndex = 0;
        const encode = JSON.parse(text);
        compare(ProjectFile.applyEncode(EncoderController, encode, 0).length, 0);
        compare(ProjectFile.applyEncode(EncoderController, encode, 1).length, 0);
        compare(ProjectFile.captureEncode(EncoderController), text);
        compare(EncoderController.metaTokens, line);
    }

    function test_aValueHeldUnderAGroupThatIsOffIsNeitherInTheLineNorInTheProject() {
        // The LFE mix and the preferred downmix are written only inside E-AC-3's mixing metadata
        // (and the downmix in AC-3's Annex D). `forge` reads either word as asking for its group,
        // so held under one that is off - which the encode leaves out - they would make the line a
        // stream the window does not write. They are left out of the line and the project alike.
        EncoderController.resetEncodeSettings();
        EncoderController.codecIndex = 1;
        EncoderController.lfeMix = -1;
        EncoderController.dmixIndex = 1;
        compare(EncoderController.mixmeta, false);
        compare(EncoderController.metaTokens, "");
        compare(Object.keys(JSON.parse(ProjectFile.captureEncode(EncoderController)).options).length, 0);

        EncoderController.mixmeta = true;
        compare(EncoderController.metaTokens.split(" ").sort().join(" "), "dmixmod=ltrt lfemix=off mixmeta");

        // AC-3: the downmix needs Annex D, and the LFE mix has no place in AC-3 at all.
        EncoderController.codecIndex = 0;
        EncoderController.mixmeta = false;
        compare(EncoderController.metaTokens, "");
        EncoderController.annexD = true;
        compare(EncoderController.metaTokens, "dmixmod=ltrt");

        // And a project that names either word has asked for its group, as the command line does.
        EncoderController.resetEncodeSettings();
        compare(ProjectFile.applyEncode(EncoderController, { codec: "eac3" }, 0).length, 0);
        compare(ProjectFile.applyEncode(EncoderController, { options: { lfemix: 3 } }, 1).length, 0);
        compare(EncoderController.mixmeta, true);
        compare(EncoderController.lfeMix, 3);
        compare(EncoderController.metaTokens, "mixmeta lfemix=3");
        compare(ProjectFile.applyEncode(EncoderController, { codec: "ac3" }, 0).length, 0);
        compare(ProjectFile.applyEncode(EncoderController, { options: { dmixmod: "loro" } }, 1).length, 0);
        compare(EncoderController.annexD, true);
        compare(EncoderController.dmixIndex, 2);
    }

    function test_theAc4TabsChoicesAreKeptInAc4EncodesOwnWords() {
        EncoderController.resetEncodeSettings();
        EncoderController.codecIndex = 2;
        // A fresh AC-4 draft has nothing to say.
        compare(Object.keys(JSON.parse(ProjectFile.captureEncode(EncoderController)).options).length, 0);
        compare(ProjectFile.ac4OptionKeys().indexOf("frame-rate") >= 0, true);

        EncoderController.ac4FrameRateIndex = 2;
        EncoderController.ac4RateModeIndex = 1;
        EncoderController.ac4CodecModeIndex = 1;
        EncoderController.ac4Dialnorm = 27.5;
        EncoderController.ac4LoudnessIndex = 1;
        EncoderController.ac4DrcIndex = 5;
        EncoderController.ac4DialogueLeft = true;
        EncoderController.ac4DialogueCentre = true;
        EncoderController.ac4IframeInterval = 12;
        EncoderController.ac4Crc = false;
        const line = EncoderController.ac4Tokens;
        verify(line.length > 0);
        const text = ProjectFile.captureEncode(EncoderController);
        const kept = JSON.parse(text).options;
        compare(kept["frame-rate"], 25);
        compare(kept["rate-mode"], "average");
        compare(kept["codec-mode"], "simple");
        compare(kept.dialnorm, 27.5);
        compare(kept.drc, "speech");
        compare(kept["dialogue-channels"], "l,c");
        compare(kept["iframe-interval"], 12);
        compare(kept.crc, "off");
        // The same words the tab's own command line says them in.
        compare(Object.keys(kept).length, line.split(" ").length);

        // And put back, over a tab that held something else.
        cleanup();
        EncoderController.codecIndex = 2;
        EncoderController.ac4Dialnorm = 20;
        EncoderController.ac4DialogueRight = true;
        const encode = JSON.parse(text);
        compare(ProjectFile.applyEncode(EncoderController, encode, 0).length, 0);
        compare(ProjectFile.applyEncode(EncoderController, encode, 1).length, 0);
        compare(EncoderController.ac4Tokens, line);
        compare(ProjectFile.captureEncode(EncoderController), text);
    }

    function test_anAc4OptionTheTabCannotTakeOrHasNoControlForIsSaid() {
        EncoderController.resetEncodeSettings();
        EncoderController.codecIndex = 2;
        let notes = ProjectFile.applyEncode(
            EncoderController,
            { options: { "frame-rate": 26, lfemix: -4.5, "codec-mode": "scpl", dialnorm: 27 } }, 1);
        compare(notes.length, 3);
        const codes = notes.map((n) => n.code + ":" + n.args[0]).sort();
        compare(codes.join(" "),
                "setting-not-held:codec-mode setting-not-held:lfemix setting-out-of-range:frame-rate");
        // What it could take, it took.
        compare(EncoderController.ac4Dialnorm, 27);
        compare(EncoderController.ac4FrameRateIndex, 13);
    }

    function test_theCodingToolsAndTheVbrAreKeptAsTheTokensTheCommandLineTakes() {
        EncoderController.resetEncodeSettings();
        EncoderController.codecIndex = 1;
        EncoderController.coupling = true;
        EncoderController.spx = true;
        EncoderController.vbrEnabled = true;
        EncoderController.vbrQuality = 60;
        EncoderController.vbrMaxEnabled = true;
        EncoderController.vbrMaxKbps = 448;
        const kept = JSON.parse(ProjectFile.captureEncode(EncoderController));
        compare(kept.tools, EncoderController.toolsToken);
        verify(kept.tools.indexOf("cpl") >= 0 && kept.tools.indexOf("spx") >= 0, kept.tools);
        compare(kept.vbr, EncoderController.vbrToken);

        EncoderController.resetEncodeSettings();
        compare(EncoderController.coupling, false);
        compare(EncoderController.vbrEnabled, false);
        compare(ProjectFile.applyEncode(EncoderController, kept, 1).length, 0);
        compare(EncoderController.coupling, true);
        compare(EncoderController.spx, true);
        compare(EncoderController.vbrEnabled, true);
        compare(EncoderController.vbrQuality, 60);
        compare(EncoderController.vbrMaxEnabled, true);
        compare(EncoderController.vbrMaxKbps, 448);
        compare(EncoderController.toolsToken, kept.tools);
    }

    function test_aProjectIsAWholePlanAndNotAPatchOnWhatTheWindowHeld() {
        // Settings the project does not mention come back to the command line's defaults, so
        // that what it opens is what `forge` would write from it, whatever was set before.
        EncoderController.dialnorm = 20;
        EncoderController.heavy = true;
        EncoderController.bsmodIndex = 3;
        EncoderController.coupling = true;
        compare(ProjectFile.applyEncode(EncoderController, { codec: "ac3" }, 0).length, 0);
        compare(EncoderController.dialnorm, 31);
        compare(EncoderController.heavy, false);
        compare(EncoderController.bsmodIndex, 0);
        compare(EncoderController.coupling, false);
        compare(EncoderController.metaTokens, "");
    }

    function test_aValueTheTableCannotUseIsReportedAndLeftAlone() {
        EncoderController.resetEncodeSettings();
        const before = EncoderController.codecIndex;
        let notes = ProjectFile.applyEncode(EncoderController, { codec: "ac9" }, 0);
        compare(notes.length, 1);
        compare(notes[0].code, "setting-unknown-choice");
        compare(notes[0].args[0], "codec");
        compare(notes[0].args[1], "ac9");
        compare(EncoderController.codecIndex, before);

        notes = ProjectFile.applyEncode(EncoderController, { bitrateKbps: 100 }, 1);
        compare(notes[0].code, "bitrate-not-offered");
        notes = ProjectFile.applyEncode(EncoderController,
                                        { options: { bsmod: "cartoon", dialnorm: 99 } }, 1);
        compare(notes.length, 2);
        compare(notes[0].code, "setting-out-of-range");
        compare(EncoderController.bsmodIndex, 0);
        compare(EncoderController.dialnorm, 31);
        notes = ProjectFile.applyEncode(EncoderController, { tools: "nonsense" }, 1);
        compare(notes[0].code, "setting-unknown-choice");
        // An average-rate target is the command line's: the window has no controls for it.
        notes = ProjectFile.applyEncode(EncoderController, { vbr: "avg:256" }, 1);
        compare(notes[0].code, "setting-unknown-choice");
        compare(EncoderController.vbrEnabled, false);
    }

    function test_anOptionTheWindowHasNoControlForIsSaidAndIsCarriedThrough() {
        EncoderController.resetEncodeSettings();
        const notes = ProjectFile.applyEncode(
            EncoderController, { options: { lorocmixlev: -3, "from-a-newer-forge": 1 } }, 1);
        compare(notes.length, 2);
        compare(notes[0].code, "setting-not-held");
        const keys = ProjectFile.optionKeys();
        verify(keys.indexOf("dialnorm") >= 0);
        verify(keys.indexOf("lorocmixlev") < 0);
    }

    function test_theObjectsPlacementsAndMotionAreKept() {
        const win = openWindow();
        EncoderController.loadSourceFile(backing);
        EncoderController.atmosEnabled = true;
        const count = EncoderController.objectCount;
        verify(count >= 2, "objects: " + count);
        EncoderController.setObjectPathKeyframes(0, [
            { time: 0.0, x: 0.1, y: 0.2, z: 0.0, gain: 0.5, lfeSend: 0.0 },
            { time: 0.5, x: 0.9, y: 0.8, z: 0.5, gain: 0.5, lfeSend: 0.1 }
        ], "");
        EncoderController.setObjectPosition(1, 0.25, 0.75, -0.5);
        const text = ProjectFile.captureObjects(EncoderController);
        verify(text.length > 0);
        const kept = JSON.parse(text);
        compare(kept.count, count);
        verify(kept.scene !== undefined);

        EncoderController.clearObjectPath(0);
        compare(EncoderController.objectKeyframes(0).length, 0);
        const notes = ProjectFile.applyObjects(EncoderController, text);
        compare(notes.length, 0, JSON.stringify(notes));
        const keys = EncoderController.objectKeyframes(0);
        compare(keys.length, 2);
        compare(keys[1].time, 0.5);
        compare(keys[1].x, 0.9);
        compare(keys[1].y, 0.8);
        // An object that never moved holds its place as a single keyframe at the start.
        const still = EncoderController.objectKeyframes(1);
        compare(still.length, 1);
        compare(still[0].x, 0.25);
        compare(still[0].z, -0.5);

        // Outside object mode there is nothing to keep.
        EncoderController.atmosEnabled = false;
        compare(ProjectFile.captureObjects(EncoderController), "");
    }

    function test_theObjectsOfAnObjectSessionRideInTheProject() {
        const win = openWindow();
        win.tier = "advanced";
        EncoderController.loadSourceFile(backing);
        EncoderController.atmosEnabled = true;
        EncoderController.setObjectPathKeyframes(0, [
            { time: 0.0, x: 0.0, y: 0.0, z: 0.0, gain: 0.6, lfeSend: 0.0 },
            { time: 0.4, x: 1.0, y: 1.0, z: 0.0, gain: 0.6, lfeSend: 0.0 }
        ], "");
        const file = inTheFolder("kp-objects.forgeproj");
        verify(win.saveProjectFile(file));
        const saved = readProject(file);
        compare(saved.atmos, true);
        verify(saved.objects.scene !== undefined);

        cleanup();
        const second = openWindow();
        verify(second.openProjectFile(file));
        compare(EncoderController.atmosEnabled, true);
        const keys = EncoderController.objectKeyframes(0);
        compare(keys.length, 2);
        compare(keys[1].x, 1.0);
    }

    // ---- refusing -------------------------------------------------------------
    function test_aFileThatIsNotAProjectIsRefusedWithAReason() {
        const win = openWindow();
        const cases = [
            { name: "kp-prose.forgeproj", text: "not json at all", says: "not a JSON document" },
            { name: "kp-other.forgeproj", text: "{ \"hello\": 1 }", says: "does not say it is one" },
            { name: "kp-nover.forgeproj", text: "{ \"format\": \"iclforge-project\" }",
              says: "which version" },
            { name: "kp-newer.forgeproj",
              text: "{ \"format\": \"iclforge-project\", \"version\": 99 }",
              says: "newer Forge" },
        ];
        for (const c of cases) {
            const file = audioFixtures.writeText(c.name, c.text);
            compare(win.openProjectFile(file), false, c.name);
            const dialog = findByName(win, "projectNoticeDialog");
            tryCompare(dialog, "visible", true, 5000);
            // The notes of the case before may still be on their way out.
            tryVerify(() => {
                const note = visualFind(dialog.contentItem, "projectNote");
                return note !== null && note.text.indexOf(c.says) >= 0;
            }, 5000, c.name + ": the dialog does not say \"" + c.says + "\"");
            mouseClick(findByName(win, "projectNoticeClose"));
            tryCompare(dialog, "visible", false, 5000);
        }
        compare(EncoderController.sourceModel.length, 0);
    }

    function test_aSourceThatIsNotThereIsNamedAndTheRestStillOpens() {
        const text = JSON.stringify({
            format: "iclforge-project",
            version: 1,
            flow: "karaoke",
            sources: [
                { path: "kp-backing.wav", part: "backing" },
                { path: "kp-gone.wav", part: "melody" },
                { path: "kp-lead.wav", part: "vocal1" }
            ]
        });
        const file = audioFixtures.writeText("kp-missing.forgeproj", text);
        const win = openWindow();
        verify(win.openProjectFile(file));
        compare(EncoderController.sourceModel.length, 2);
        const dialog = findByName(win, "projectNoticeDialog");
        tryCompare(dialog, "visible", true, 5000);
        tryVerify(() => {
            const note = visualFind(dialog.contentItem, "projectNote");
            return note !== null && note.text.indexOf("kp-gone.wav") >= 0;
        }, 5000, "the dialog does not name the missing part");
        compare(findByName(win, "karaokeFlow").plan.mode, "2/1");
    }

    // ---- the window and forge -------------------------------------------------
    // The window's own encode of a draft, and what `forge project` writes from the project the
    // window saved of it: the same bytes, which is what makes a project the line its command bar
    // shows and not a second way of saying it.
    function windowAndForgeWriteTheSameBytes(name, suffix, setup) {
        if (!cliRunner.available()) {
            skip("this build has no forge to run");
        }
        const win = openWindow();
        win.tier = "expert";
        EncoderController.loadSourceFile(backing);
        EncoderController.applyChannelPreset("stereo");
        setup();
        const project = inTheFolder(name + ".forgeproj");
        verify(win.saveProjectFile(project), "the project could not be saved");

        const fromWindow = Qt.resolvedUrl("_test_output/tst_projects_" + name + suffix);
        EncoderController.encodeTo(fromWindow);
        tryCompare(EncoderController, "busy", false, 120000);
        verify(cliRunner.size(fromWindow) > 0, "the window wrote nothing");

        const folder = audioFixtures.folder();
        const result = "from-forge-" + name + suffix;
        const line = "forge project " + ProjectFile.localPath(project) + " "
                     + ProjectFile.localPath(inTheFolder(result));
        compare(cliRunner.run(line, folder), 0, line);
        verify(cliRunner.sameBytes(fromWindow, inTheFolder(result)),
               "forge project wrote different bytes than the window did: " + line);
    }

    function test_forgeProjectWritesWhatTheWindowWritesForAnAc3WithItsMetadata() {
        windowAndForgeWriteTheSameBytes("eq-ac3", ".ac3", () => {
            EncoderController.bitrateKbps = 256;
            EncoderController.coupling = true;
            EncoderController.dialnorm = 27;
            EncoderController.drcIndex = 2;
            EncoderController.heavy = true;
            EncoderController.cmixIndex = 0;
            EncoderController.surmixIndex = 2;
            EncoderController.bsmodIndex = 5;
            EncoderController.copyrightBit = true;
        });
    }

    function test_forgeProjectWritesWhatTheWindowWritesForAnEac3WithToolsAndVbr() {
        windowAndForgeWriteTheSameBytes("eq-eac3", ".ec3", () => {
            EncoderController.codecIndex = 1;
            EncoderController.bitrateKbps = 192;
            EncoderController.coupling = true;
            EncoderController.spx = true;
            EncoderController.vbrEnabled = true;
            EncoderController.vbrQuality = 55;
            EncoderController.vbrMaxEnabled = true;
            EncoderController.vbrMaxKbps = 256;
            EncoderController.dialnorm = 24;
        });
    }

    function test_forgeProjectWritesWhatTheWindowWritesForAnObjectSession() {
        windowAndForgeWriteTheSameBytes("eq-objects", ".ec3", () => {
            EncoderController.atmosEnabled = true;
            EncoderController.bitrateKbps = 448;
            EncoderController.setObjectPathKeyframes(0, [
                { time: 0.0, x: 0.1, y: 0.1, z: 0.0, gain: 0.5, lfeSend: 0.0 },
                { time: 0.5, x: 0.9, y: 0.9, z: 0.2, gain: 0.5, lfeSend: 0.0 }
            ], "");
            EncoderController.setObjectPosition(1, 0.3, 0.6, 0.0);
            EncoderController.dialnorm = 26;
        });
    }

    function test_forgeProjectWritesWhatTheWindowWritesForAnEac3WithItsMixingMetadata() {
        windowAndForgeWriteTheSameBytes("eq-eac3-mix", ".ec3", () => {
            EncoderController.codecIndex = 1;
            EncoderController.bitrateKbps = 192;
            EncoderController.mixmeta = true;
            EncoderController.lfeMix = -1;
            EncoderController.dmixIndex = 1;
        });
    }

    function test_forgeProjectWritesWhatTheWindowWritesForAnEac3WithItsMixingMetadataOff() {
        // The LFE mix and the downmix are held, and the group they ride in is not on: the stream has
        // neither, and the project, which `forge` reads as asking for the group, must not either.
        windowAndForgeWriteTheSameBytes("eq-eac3-held", ".ec3", () => {
            EncoderController.codecIndex = 1;
            EncoderController.bitrateKbps = 192;
            EncoderController.lfeMix = -1;
            EncoderController.dmixIndex = 1;
        });
    }

    function test_forgeProjectWritesWhatTheWindowWritesForAnAc3WithItsDownmix() {
        windowAndForgeWriteTheSameBytes("eq-ac3-annexd", ".ac3", () => {
            EncoderController.bitrateKbps = 256;
            EncoderController.annexD = true;
            EncoderController.dmixIndex = 1;
        });
    }

    function test_forgeProjectWritesWhatTheWindowWritesForAnAc4() {
        windowAndForgeWriteTheSameBytes("eq-ac4", ".ac4", () => {
            EncoderController.codecIndex = 2;
            EncoderController.bitrateKbps = 128;
            EncoderController.ac4CodecModeIndex = 1;
            EncoderController.ac4Dialnorm = 27.5;
            EncoderController.ac4IframeInterval = 12;
            EncoderController.ac4DrcIndex = 5;
            EncoderController.ac4Crc = false;
        });
    }

    function test_forgeProjectWritesWhatTheWindowWritesForAnAc4InAnMp4File() {
        windowAndForgeWriteTheSameBytes("eq-ac4-mp4", ".mp4", () => {
            EncoderController.codecIndex = 2;
            EncoderController.containerIndex = 3;
            EncoderController.bitrateKbps = 128;
            EncoderController.ac4Dialnorm = 24;
        });
    }

    function test_forgeProjectWritesWhatTheWindowWritesForAnAc4ObjectSession() {
        windowAndForgeWriteTheSameBytes("eq-ac4-objects", ".ac4", () => {
            EncoderController.atmosEnabled = true;
            EncoderController.codecIndex = 2;
            EncoderController.bitrateKbps = 384;
            EncoderController.setObjectPathKeyframes(0, [
                { time: 0.0, x: 0.1, y: 0.1, z: 0.0, gain: 0.5, lfeSend: 0.0 },
                { time: 0.5, x: 0.9, y: 0.9, z: 0.2, gain: 0.5, lfeSend: 0.0 }
            ], "");
            EncoderController.setObjectPosition(1, 0.3, 0.6, 0.0);
            EncoderController.ac4Dialnorm = 26;
            EncoderController.ac4ObjectCodingIndex = 1;
        });
    }

    function test_forgeProjectWritesWhatTheWindowWritesForAnAc4ObjectSessionOfSeveralFiles() {
        windowAndForgeWriteTheSameBytes("eq-ac4-objects-routed", ".ac4", () => {
            EncoderController.atmosEnabled = true;
            EncoderController.codecIndex = 2;
            EncoderController.bitrateKbps = 384;
            EncoderController.addSourceFile(melody);
            EncoderController.setAssignment(0, 0, "obj");
            EncoderController.setAssignment(0, 1, "obj@-3");
            EncoderController.setAssignment(1, 0, "objm");
        });
    }

    function test_forgeProjectWritesWhatTheWindowWritesForAnEac3ObjectSessionOfSeveralFiles() {
        windowAndForgeWriteTheSameBytes("eq-eac3-objects-routed", ".ec3", () => {
            EncoderController.atmosEnabled = true;
            EncoderController.bitrateKbps = 448;
            EncoderController.addSourceFile(melody);
            EncoderController.setAssignment(0, 0, "obj");
            EncoderController.setAssignment(0, 1, "obj");
            EncoderController.setAssignment(1, 0, "objm");
        });
    }

    function test_aProjectOfNamedPartsIsOneStreamInTheWindowAndInForge() {
        if (!cliRunner.available()) {
            skip("this build has no forge to run");
        }
        // Parts and nothing of how they are laid out: no bed, no assignments, no service. The
        // Karaoke flow lays them out where Annex C puts them and `forge project` does the same.
        const file = writeProject("kp-parts-only.forgeproj", {
            format: "iclforge-project",
            version: 1,
            flow: "karaoke",
            sources: [
                { path: ProjectFile.localPath(backing), part: "backing" },
                { path: ProjectFile.localPath(melody), part: "melody" },
                { path: ProjectFile.localPath(lead), part: "vocal1" }
            ],
            encode: { codec: "ac3", bitrateKbps: 192 }
        });
        const win = openWindow();
        verify(win.openProjectFile(file));
        compare(findByName(win, "karaokeFlow").plan.mode, "3/1");

        const fromWindow = Qt.resolvedUrl("_test_output/tst_projects_parts_only.ac3");
        EncoderController.encodeTo(fromWindow);
        tryCompare(EncoderController, "busy", false, 120000);
        verify(cliRunner.size(fromWindow) > 0, "the window wrote nothing");

        const line = "forge project " + ProjectFile.localPath(file) + " "
                     + ProjectFile.localPath(inTheFolder("from-forge-parts-only.ac3"));
        compare(cliRunner.run(line, audioFixtures.folder()), 0, line);
        verify(cliRunner.sameBytes(fromWindow, inTheFolder("from-forge-parts-only.ac3")),
               "forge project wrote a different stream than the window: " + line);
    }

    // ---- the example ----------------------------------------------------------
    function test_theExampleProjectOpensAndTheWindowEncodesItsStream() {
        const win = openWindow();
        verify(win.openProjectFile(exampleProject));
        compare(win.tier, "karaoke");
        const flow = findByName(win, "karaokeFlow");
        compare(EncoderController.sourceModel.length, 4);
        compare(flow.failedParts.length, 0);
        compare(flow.plan.mode, "3/2");
        compare(flow.plan.hasProblem, false);
        compare(bedId(), "3/2");
        compare(EncoderController.bsmodIndex, 7);
        compare(EncoderController.bitrateKbps, 384);
        compare(flow.listening, "aware");
        compare(flow.vocalsChoice, "both");

        // The Save step's own button, then the picker it opens.
        flow.currentStepKey = "save";
        const next = findByName(win, "karaokeNextButton");
        tryCompare(next, "enabled", true, 5000);
        mouseClick(next);
        const dialog = findByName(win, "saveDialog");
        tryCompare(dialog, "visible", true, 5000);
        const out = Qt.resolvedUrl("_test_output/tst_projects_example.ac3");
        dialog.selectedFile = out;
        dialog.accepted();
        dialog.close();
        tryCompare(EncoderController, "busy", false, 120000);

        verify(cliRunner.sameBytes(out, exampleStream),
               "the window wrote a different stream than the example ships");
    }
}
