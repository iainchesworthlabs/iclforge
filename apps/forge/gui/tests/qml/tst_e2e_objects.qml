import QtQuick
import QtTest

import ForgeGui

// Object mode end to end from the Objects tab: the switch turns the loaded
// channels into objects, the timeline's own buttons author and remove keys,
// zoom, preview the motion and export the paths file, and Encode writes a
// Dolby Atmos (JOC) stream that the object inspector - reached from the
// finished run - decodes back into the same number of moving objects.
//
// Same seams as tst_e2e_encode.qml. Buttons on the timeline toolbar sit in a
// scrolling tab page, so they are pressed from the keyboard (focus + Space,
// what a keyboard user does) rather than at a coordinate that may be
// scrolled out of view.
TestCase {
    id: testCase
    name: "E2eObjects"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    readonly property url wavUrl:
        Qt.resolvedUrl("../../../../../libs/base/fuzz/seeds/fuzz_wav_read/roundtrip-stereo.wav")
    readonly property url pathsUrl: Qt.resolvedUrl("_test_output/tst_e2e_objects-paths.txt")
    readonly property url atmosOutUrl: Qt.resolvedUrl("_test_output/tst_e2e_objects.ec3")

    function cleanup() {
        EncoderController.stopMotionPreview();
        EncoderController.atmosEnabled = false;
        while (EncoderController.sourceModel.length > 0) {
            EncoderController.removeSource(0);
        }
        EncoderController.codecIndex = 0;
        EncoderController.bitrateKbps = 192;
    }

    // ---- helpers (same shape as tst_e2e_encode.qml's) --------------------

    function findByName(root, name) {
        let found = null;
        tryVerify(() => {
            found = findChild(root, name);
            return found !== null;
        }, 5000, "no object called " + name);
        return found;
    }

    // Key events from keyClick() go to the application's FOCUS window, not
    // to the window an item lives in - and the TestCase's own window starts
    // out as that. So the Main window is activated first, then the control
    // takes focus, exactly as clicking into the window would do.
    function focusIn(item) {
        const w = item.Window.window;
        w.requestActivate();
        tryVerify(() => w.active, 5000, "window never became active");
        item.forceActiveFocus();
        tryVerify(() => item.activeFocus, 5000, "control never took focus");
    }

    function settle(item) {
        waitForRendering(item);
        let last = "";
        tryVerify(() => {
            const p = item.mapToItem(null, 0, 0);
            const key = p.x + "," + p.y + "," + item.width + "," + item.height;
            const stable = item.width > 0 && key === last;
            last = key;
            return stable;
        }, 5000, "item never settled");
    }

    function click(item) {
        settle(item);
        mouseClick(item);
    }

    function press(item) {
        verify(item.enabled, item.objectName + " is disabled");
        focusIn(item);
        keyClick(Qt.Key_Space);
    }

    function pickFile(root, dialogName, url) {
        const dialog = findByName(root, dialogName);
        tryCompare(dialog, "visible", true, 5000);
        dialog.selectedFile = url;
        dialog.accepted();  // see pickFile
        dialog.close();
        tryCompare(dialog, "visible", false, 5000);
        return dialog;
    }

    function openObjectsTab(source = wavUrl) {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        click(findByName(win.contentItem, "firstRun-file"));
        pickFile(win, "openDialog", source);
        tryCompare(EncoderController, "sourceReady", true, 10000);
        click(findByName(win.contentItem, "seg-expert"));
        click(findByName(win.contentItem, "tab-objects"));
        compare(win.currentTab, "objects");
        return win;
    }

    // ---- cases ----------------------------------------------------------

    function test_objectSwitchAuthoringAndEncodeRoundTripThroughTheInspector() {
        const win = openObjectsTab();

        const atmosSwitch = findByName(win.contentItem, "atmosSwitch");
        compare(atmosSwitch.checked, false);
        press(atmosSwitch);
        compare(EncoderController.atmosEnabled, true);
        // Two loaded channels, two objects; the codec is no longer a choice.
        tryVerify(() => EncoderController.objectModel.length === 2);
        const presetButton = findByName(win.contentItem, "preset-5.1");
        compare(presetButton.enabled, false);
        tryCompare(findByName(win.contentItem, "encodeButton"), "text", "Encode to .ec3");

        // Select the first object, then author a key at the playhead from
        // the toolbar's own button.
        EncoderController.selectedObjectIndex = EncoderController.objectModel[0].index;
        const objIndex = EncoderController.selectedObjectIndex;
        const keysBefore = EncoderController.objectKeyframes(objIndex).length;
        const addKey = findByName(win.contentItem, "addKeyButton");
        tryCompare(addKey, "enabled", true);
        press(addKey);
        tryVerify(() => EncoderController.objectKeyframes(objIndex).length === keysBefore + 1);

        // Zoom in and back out from the toolbar: the readout follows.
        const readout = findByName(win.contentItem, "zoomReadout");
        compare(readout.text, "100%");
        press(findByName(win.contentItem, "zoomInButton"));
        compare(readout.text, "150%");
        const fit = findByName(win.contentItem, "zoomFitButton");
        tryCompare(fit, "visible", true);
        press(fit);
        compare(readout.text, "100%");

        // Preview plays the motion through a real output (MonitorSink): on a
        // machine without one the press is refused on the status line;
        // with one, the motion clock runs and the same button stops it.
        const preview = findByName(win.contentItem, "previewButton");
        compare(preview.text, "Preview");
        press(preview);
        tryVerify(() => EncoderController.motionPreviewActive
                        || EncoderController.status.indexOf("Could not open the preview output") === 0,
                  5000);
        if (EncoderController.motionPreviewActive) {
            compare(preview.text, "Stop");
            press(preview);
            tryCompare(EncoderController, "motionPreviewActive", false, 5000);
            tryCompare(EncoderController, "busy", false, 5000);
        } else {
            compare(preview.text, "Preview");
            compare(EncoderController.busy, false);
        }

        // Export paths: the picker suggests <source>-paths.txt, and once the
        // file is written the command bar quotes it.
        press(findByName(win.contentItem, "exportPathsButton"));
        const exportDialog = findByName(win, "exportPathsDialog");
        tryCompare(exportDialog, "visible", true);
        verify(exportDialog.selectedFile.toString().endsWith("roundtrip-stereo-paths.txt"),
               exportDialog.selectedFile);
        exportDialog.selectedFile = pathsUrl;
        exportDialog.accepted();  // see pickFile
        exportDialog.close();
        tryVerify(() => win.exportedPathsPath.toString().endsWith("tst_e2e_objects-paths.txt"));
        verify(win.cliLine.indexOf("tst_e2e_objects-paths.txt") > 0, win.cliLine);

        // Encode: an Atmos run, counted in Atmos access units.
        click(findByName(win.contentItem, "encodeButton"));
        pickFile(win, "saveDialog", atmosOutUrl);
        tryCompare(EncoderController, "busy", false, 60000);
        const run = EncoderController.runs[0];
        compare(run.status, "done", run.detail);
        compare(run.eac3, true);
        verify(EncoderController.status.indexOf("Atmos access units") > 0, EncoderController.status);

        // The finished run's inspector decodes the objects back out of the
        // file: as many as were authored, inside the room.
        win.openRunInInspector(run.path);
        tryVerify(() => win.objectInspectorDialogRef.opened);
        tryCompare(ObjectDecodeController, "busy", false, 15000);
        compare(ObjectDecodeController.error, "");
        verify(ObjectDecodeController.frameCount > 1);
        const decoded = ObjectDecodeController.frames[0].objects;
        verify(decoded.length >= 2, "decoded " + decoded.length + " objects");
        const rows = findByName(win.objectInspectorDialogRef.contentItem, "oiObjectRows");
        tryCompare(rows, "count", decoded.length);
        win.objectInspectorDialogRef.close();
    }

    function test_deleteKeyButtonRemovesTheSelectedKey() {
        const win = openObjectsTab();
        press(findByName(win.contentItem, "atmosSwitch"));
        tryVerify(() => EncoderController.objectModel.length === 2);
        EncoderController.selectedObjectIndex = EncoderController.objectModel[0].index;
        const objIndex = EncoderController.selectedObjectIndex;
        const before = EncoderController.objectKeyframes(objIndex).length;

        press(findByName(win.contentItem, "addKeyButton"));
        tryVerify(() => EncoderController.objectKeyframes(objIndex).length === before + 1);
        // Delete stays disabled until a key is selected on the timeline; the
        // toolbar reads the selection from objectsTab.selectedKeyTime.
        const del = findByName(win.contentItem, "deleteKeyButton");
        compare(del.enabled, false);
        const tab = findByName(win.contentItem, "objectsTab");
        const keys = EncoderController.objectKeyframes(objIndex);
        tab.selectedKeyTime = keys[keys.length - 1].time;
        tryCompare(del, "enabled", true);
        press(del);
        tryVerify(() => EncoderController.objectKeyframes(objIndex).length === before);
        compare(del.enabled, false);
    }

    // A preview nobody stops lasts the source's own length. The first case above takes the
    // short seed file and presses Preview a second time at once, which is the same either way;
    // here the source is two seconds, and the output's queue holds one of them: the preview has
    // read the whole source by about one second, and used to stop the output there with the
    // second still queued. Only the lower bound is asserted, since a loaded machine makes a
    // preview late, never shorter than its audio.
    function test_aPreviewNobodyStopsPlaysTheWholeSource() {
        const seconds = 2.0;
        const source = audioFixtures.writeTones("preview-length.wav", 48000, seconds, [440, 620]);
        verify(source.toString().length > 0);
        const win = openObjectsTab(source);
        press(findByName(win.contentItem, "atmosSwitch"));
        tryVerify(() => EncoderController.objectModel.length === 2);

        const preview = findByName(win.contentItem, "previewButton");
        verify(preview.enabled, "the preview button is disabled");
        focusIn(preview);
        const began = Date.now();
        keyClick(Qt.Key_Space);
        const refused = "Could not open the preview output";
        tryVerify(() => EncoderController.motionPreviewActive
                        || EncoderController.status.indexOf(refused) === 0, 5000);
        if (!EncoderController.motionPreviewActive) {
            // No output on this machine: refused on the status line, nothing started.
            compare(EncoderController.busy, false);
            return;
        }
        tryCompare(EncoderController, "motionPreviewActive", false, seconds * 1000 + 8000);
        const lasted = Date.now() - began;
        verify(lasted >= seconds * 1000 * 0.9,
               "a " + seconds + " s source was previewed for " + lasted + " ms");
        compare(EncoderController.busy, false);
    }

    // Stop Preview does not wait for the clip either. Three and a half seconds into five, the
    // source has been read and encoded to its end (the queue holds 1.8 s of it, and the encoder
    // is faster than the clock) and the output is only playing what it was given: where the wait
    // for it is. A press that a loaded machine delivers within the last 300 ms has no distance
    // to show, and the case is skipped.
    function test_stopPreviewInTheTailOfAPreviewEndsItAtOnce() {
        const seconds = 5.0;
        const source = audioFixtures.writeTones("preview-stop.wav", 48000, seconds, [440, 620]);
        verify(source.toString().length > 0);
        const win = openObjectsTab(source);
        press(findByName(win.contentItem, "atmosSwitch"));
        tryVerify(() => EncoderController.objectModel.length === 2);

        const preview = findByName(win.contentItem, "previewButton");
        verify(preview.enabled, "the preview button is disabled");
        focusIn(preview);
        const began = Date.now();
        keyClick(Qt.Key_Space);
        const refused = "Could not open the preview output";
        tryVerify(() => EncoderController.motionPreviewActive
                        || EncoderController.status.indexOf(refused) === 0, 5000);
        if (!EncoderController.motionPreviewActive) {
            compare(EncoderController.busy, false);
            return;
        }
        tryVerify(() => Date.now() - began >= 3500 || !EncoderController.motionPreviewActive, 8000);
        const pressed = Date.now() - began;
        if (!EncoderController.motionPreviewActive || pressed > seconds * 1000 - 300) {
            skip("the preview was over, or nearly, by the time it could be stopped");
        }
        focusIn(preview);
        keyClick(Qt.Key_Space);
        tryCompare(EncoderController, "motionPreviewActive", false, 5000);
        const lasted = Date.now() - began;
        verify(lasted < seconds * 1000 - 100,
               "stopped at " + pressed + " ms of a " + seconds + " s source, it ended after "
               + lasted + " ms");
        compare(EncoderController.busy, false);
    }
}
