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
        EncoderController.bsmodIndex = 0;
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
        const read = ProjectFile.read(file);
        verify(read.ok, JSON.stringify(read.error));
        const project = read.project;
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
        compare(project.encode.bitrateKbps, 256);
        compare(project.encode.bsmod, 7);
        compare(project.encode.cmixlev, 2);
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
        const read = ProjectFile.read(file);
        verify(read.ok);
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
        const text = JSON.stringify({
            format: "iclforge-project",
            version: 1,
            title: "From a later Forge",
            flow: "expert",
            sources: [{ path: "kp-backing.wav", note: "kept with its source", loop: true }],
            encode: { bitrateKbps: 256, tomorrow: 7 },
            comments: ["a", "b"],
            future: { x: 1 }
        }, null, 2);
        const file = audioFixtures.writeText("kp-future.forgeproj", text);
        verify(file.toString().length > 0);

        const win = openWindow();
        verify(win.openProjectFile(file));
        compare(EncoderController.bitrateKbps, 256);
        compare(EncoderController.sourceModel.length, 1);

        const again = inTheFolder("kp-future-saved.forgeproj");
        verify(win.saveProjectFile(again));
        const back = ProjectFile.read(again).project;
        compare(back.title, "From a later Forge");
        compare(back.future.x, 1);
        compare(back.comments.length, 2);
        compare(back.encode.tomorrow, 7);
        compare(back.encode.bitrateKbps, 256);
        compare(back.sources[0].note, "kept with its source");
        compare(back.sources[0].loop, true);
        compare(back.sources[0].path, "kp-backing.wav");
    }

    function test_theEncodeSettingsAreKeptByIdAndNotByPosition() {
        EncoderController.codecIndex = 1;
        EncoderController.containerIndex = 0;
        EncoderController.bitrateKbps = 256;
        EncoderController.bsmodIndex = 3;
        EncoderController.cmixIndex = 2;
        EncoderController.surmixIndex = 1;
        const kept = ProjectFile.captureEncode(EncoderController);
        compare(kept.codec, "eac3");
        compare(kept.container, "elementary");
        compare(kept.bitrateKbps, 256);
        compare(kept.bsmod, 3);
        compare(kept.cmixlev, 2);
        compare(kept.surmixlev, 1);

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

        cleanup();
        compare(EncoderController.codecIndex, 0);
        compare(ProjectFile.applyEncode(EncoderController, kept, 0).length, 0);
        compare(ProjectFile.applyEncode(EncoderController, kept, 1).length, 0);
        compare(EncoderController.codecIndex, 1);
        compare(EncoderController.bitrateKbps, 256);
        compare(EncoderController.bsmodIndex, 3);
        compare(EncoderController.cmixIndex, 2);
        compare(EncoderController.surmixIndex, 1);
    }

    function test_aValueTheTableCannotUseIsReportedAndLeftAlone() {
        const before = EncoderController.codecIndex;
        let notes = ProjectFile.applyEncode(EncoderController, { codec: "ac9" }, 0);
        compare(notes.length, 1);
        compare(notes[0].code, "setting-unknown-choice");
        compare(notes[0].args[0], "codec");
        compare(notes[0].args[1], "ac9");
        compare(EncoderController.codecIndex, before);

        notes = ProjectFile.applyEncode(EncoderController, { bsmod: 12 }, 1);
        compare(notes[0].code, "setting-out-of-range");
        notes = ProjectFile.applyEncode(EncoderController, { bitrateKbps: 100 }, 1);
        compare(notes[0].code, "bitrate-not-offered");
        // A key of some other version is not this one's to report.
        compare(ProjectFile.applyEncode(EncoderController, { tomorrow: 1 }, 1).length, 0);
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
