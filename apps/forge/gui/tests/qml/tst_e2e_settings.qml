import QtQuick
import QtTest

import ForgeGui

// The settings a person reaches from the header and the Expert tabs, each
// followed through to what it actually changes: the container combo to the
// file that gets written, the DRC combo and Heavy compression box to the compr word
// QC finds in that file, the service combo to the metadata token, and the header's About and
// Preferences dialogs to what they show and write.
//
// Same seams as tst_e2e_encode.qml: pickers are found by objectName and
// their accepted() signal emitted; plain Controls with no objectName are found by the accessible
// name a screen reader would announce and driven from the keyboard (a combo
// steps with Down, exactly as it does for a person without a mouse).
TestCase {
    id: testCase
    name: "E2eSettings"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    readonly property url wavUrl:
        Qt.resolvedUrl("../../../../../libs/base/fuzz/seeds/fuzz_wav_read/roundtrip-stereo.wav")
    readonly property url mkvOutUrl: Qt.resolvedUrl("_test_output/tst_e2e_settings.mkv")
    readonly property url drcOutUrl: Qt.resolvedUrl("_test_output/tst_e2e_settings_drc.ac3")
    readonly property url diagnosticsUrl:
        Qt.resolvedUrl("_test_output/tst_e2e_settings_diagnostics.txt")

    function cleanup() {
        while (EncoderController.sourceModel.length > 0) {
            EncoderController.removeSource(0);
        }
        EncoderController.codecIndex = 0;
        EncoderController.containerIndex = 0;
        EncoderController.drcIndex = 0;
        EncoderController.heavy = false;
        EncoderController.bsmodIndex = 0;
        EncoderController.loudnessTouched = false;
        EncoderController.formatDefaultsTouched = false;
        if (languageManager) {
            languageManager.setLanguage("en");
        }
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

    function findWhere(item, pred) {
        if (pred(item)) {
            return item;
        }
        const kids = item.children;
        for (let i = 0; i < kids.length; ++i) {
            const hit = findWhere(kids[i], pred);
            if (hit) {
                return hit;
            }
        }
        return null;
    }

    function findAccessible(item, name) {
        return findWhere(item, (n) => n.Accessible && n.Accessible.name === name && n.visible);
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

    function pickFile(root, dialogName, url) {
        const dialog = findByName(root, dialogName);
        tryCompare(dialog, "visible", true, 5000);
        dialog.selectedFile = url;
        dialog.accepted();  // see pickFile
        dialog.close();
        tryCompare(dialog, "visible", false, 5000);
        return dialog;
    }

    function loadWav(win) {
        click(findByName(win.contentItem, "firstRun-file"));
        pickFile(win, "openDialog", wavUrl);
        tryCompare(EncoderController, "sourceReady", true, 10000);
    }

    function expertTab(win, key) {
        click(findByName(win.contentItem, "seg-expert"));
        compare(win.tier, "expert");
        click(findByName(win.contentItem, "tab-" + key));
        compare(win.currentTab, key);
    }

    function stepCombo(combo, steps) {
        focusIn(combo);
        for (let i = 0; i < steps; ++i) {
            keyClick(Qt.Key_Down);
        }
    }

    function encodeTo(win, url) {
        const button = findByName(win.contentItem, "encodeButton");
        tryCompare(button, "enabled", true);
        click(button);
        pickFile(win, "saveDialog", url);
        tryCompare(EncoderController, "busy", false, 30000);
        const run = EncoderController.runs[0];
        compare(run.status, "done", run.detail);
        return run;
    }

    function qcRun(win, run) {
        win.openRunInQc(run.path);
        tryVerify(() => win.qcDialogRef.opened);
        tryCompare(QcController, "busy", false, 15000);
        compare(QcController.error, "");
    }

    // ---- cases ----------------------------------------------------------

    function test_containerComboWritesAMatroskaFileQcCanReadBack() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        loadWav(win);
        expertTab(win, "format");

        const container = findAccessible(win.contentItem, "Container");
        verify(container !== null, "no Container combo");
        compare(container.currentIndex, 0);
        stepCombo(container, 1);
        compare(EncoderController.containerIndex, 1);
        compare(EncoderController.formatDefaultsTouched, true);
        compare(EncoderController.outputSuffix(), "mkv");
        tryCompare(findByName(win.contentItem, "encodeButton"), "text", "Encode to .mkv");
        // The command bar owns up to the two-step CLI equivalent.
        verify(win.cliLine.indexOf("&& forge mkv") > 0, win.cliLine);

        const run = encodeTo(win, mkvOutUrl);
        verify(run.path.endsWith(".mkv"), run.path);
        qcRun(win, run);
        // QC sniffs the container and measures the AC-3 inside it: same
        // codec, rate and 32-frame length the elementary encode has.
        verify(QcController.summaryLine.indexOf("AC-3") === 0, QcController.summaryLine);
        verify(QcController.summaryLine.indexOf("48000 Hz") > 0, QcController.summaryLine);
        verify(QcController.summaryLine.indexOf("32 ") > 0, QcController.summaryLine);
        win.qcDialogRef.close();
    }

    function test_drcAndHeavyCompressionFromTheMetadataTabPutComprIntoTheStream() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        loadWav(win);
        expertTab(win, "meta");

        const drc = findAccessible(win.contentItem, "DRC profile");
        verify(drc !== null, "no DRC combo");
        compare(drc.currentIndex, 0);
        stepCombo(drc, 1);  // "none" -> the first real profile (film standard)
        compare(EncoderController.drcIndex, 1);
        compare(EncoderController.loudnessTouched, true);
        verify(EncoderController.metaTokens.indexOf("drc=") >= 0, EncoderController.metaTokens);
        // The Metadata tab badge now counts it.
        compare(win.visibleTabs.filter((t) => t.key === "meta")[0].badge, "1");

        // dynrng (the DRC profile) has no readout in QC; compr does. It is
        // the Heavy compression card's word, so tick that too.
        const heavy = findWhere(win.contentItem,
                                (n) => n.text === "Heavy compression" && n.checkable === true);
        verify(heavy !== null, "no Heavy compression checkbox");
        focusIn(heavy);
        keyClick(Qt.Key_Space);
        compare(EncoderController.heavy, true);
        verify(EncoderController.metaTokens.indexOf("heavy") >= 0, EncoderController.metaTokens);
        compare(win.visibleTabs.filter((t) => t.key === "meta")[0].badge, "2");

        const run = encodeTo(win, drcOutUrl);
        qcRun(win, run);
        compare(QcController.programmes[0].hasCompr, true);
        const block = findByName(win.qcDialogRef.contentItem, "qcDialnormBlock");
        const texts = [];
        (function walk(n) {
            if (typeof n.text === "string") texts.push(n.text);
            for (let i = 0; i < n.children.length; ++i) walk(n.children[i]);
        })(block);
        verify(texts.some((t) => t.indexOf("compr present") === 0), texts.join(" | "));
        win.qcDialogRef.close();
    }

    function test_serviceComboSetsBsmodAndTheMetadataBadge() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        loadWav(win);
        expertTab(win, "meta");

        const bsmod = findByName(win.contentItem, "bsmodCombo");
        compare(bsmod.currentIndex, 0);
        stepCombo(bsmod, 4);  // "dialogue"
        compare(EncoderController.bsmodIndex, 4);
        compare(bsmod.displayText, EncoderController.bsmodNames[4]);
        verify(EncoderController.metaTokens.indexOf("bsmod=dialogue") >= 0, EncoderController.metaTokens);
        compare(win.visibleTabs.filter((t) => t.key === "meta")[0].badge, "1");
    }

    function test_aboutDialogShowsTheBuildsVersionAndCloses() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        click(findByName(win.contentItem, "aboutOpenButton"));
        const version = findByName(win.contentItem, "aboutVersionText");
        tryCompare(version, "visible", true);
        verify(version.text.indexOf("iclforge") === 0, version.text);
        click(findByName(win.contentItem, "aboutCloseButton"));
        tryCompare(version, "visible", false);
    }

    // The notices the Licences dialog shows are the NOTICES.txt this build
    // installs, embedded at build time (forge_gui_notices in CMake), so what
    // they say has to be the whole file and not the sentence
    // licenceNotices() falls back to when a binary skipped the resource.
    function test_licenceNoticesAreThisBuildsEmbeddedFile() {
        const notices = EncoderController.licenceNotices;
        verify(notices.length > 2000, "the embedded notices are the whole file; got "
                                      + notices.length + " characters: " + notices.substring(0, 120));
        verify(notices.indexOf("no embedded notices file") < 0, notices.substring(0, 120));
        verify(notices.indexOf("ICL Forge") === 0, notices.substring(0, 80));
        // The program compiles {fmt} in and embeds the OFL faces.
        verify(notices.indexOf("{fmt}") >= 0, "the {fmt} section is missing");
        verify(notices.indexOf("SIL OPEN FONT LICENSE") >= 0, "the OFL text is missing");
    }

    // About says LICENCES (the source language is British English, as
    // Crucible's and Hearth's About do), names the repository as a link, and
    // opens the notices over itself from a Licences… button.
    function test_aboutHeadsTheLicencesNamesTheRepositoryAndOpensTheNotices() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        click(findByName(win.contentItem, "aboutOpenButton"));
        const version = findByName(win.contentItem, "aboutVersionText");
        tryCompare(version, "visible", true);

        // The dialog's own content, which is the version text's parent: a
        // popup is not a visual child of the window, so findWhere cannot walk
        // down to it from win.contentItem.
        const content = version.parent;
        verify(findWhere(content, (n) => n.text === "LICENCES" && n.visible) !== null,
               "About has no LICENCES heading");
        verify(findWhere(content, (n) => n.text === "LICENSE") === null,
               "About still says LICENSE");

        const link = findByName(win.contentItem, "aboutRepositoryLink");
        verify(link.text.indexOf("href=\"https://github.com/iainchesworthlabs/iclforge\"") >= 0, link.text);

        const licences = findByName(win.contentItem, "licencesDialog");
        compare(licences.visible, false);
        click(findByName(win.contentItem, "aboutLicencesButton"));
        tryCompare(licences, "visible", true);
        const text = findByName(licences.contentItem, "licenceNoticesText");
        compare(text.text, EncoderController.licenceNotices);
        verify(text.text.length > 2000);

        // The two stack: closing Licences leaves About open beneath it.
        click(findByName(licences.contentItem, "licencesCloseButton"));
        tryCompare(licences, "visible", false);
        compare(version.visible, true);
        click(findByName(win.contentItem, "aboutCloseButton"));
        tryCompare(version, "visible", false);
    }

    // The dialog is called Settings, as Crucible's and Hearth's page is, and
    // About is reached from it. About opens over the dialog and closing it
    // gives the dialog back with its working copy as it was left: an act,
    // not a setting, so neither Save nor Cancel changes meaning.
    function test_settingsDialogIsCalledSettingsAndReachesAbout() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        const opener = findByName(win.contentItem, "prefsOpenButton");
        compare(opener.text, "Settings");
        click(opener);
        tryVerify(() => win.prefsDialog.opened);
        verify(findWhere(win.prefsDialog.contentItem,
                         (n) => n.text === "Settings" && n.visible) !== null,
               "the dialog's heading does not say Settings");
        verify(findWhere(win.prefsDialog.contentItem, (n) => n.text === "Preferences") === null,
               "the dialog still says Preferences");

        // Something in the working copy that is not what is saved.
        compare(win.settings.showCli, true);
        win.prefsDialog.cliVisible = false;

        click(findByName(win.prefsDialog.contentItem, "prefsAboutButton"));
        const version = findByName(win.contentItem, "aboutVersionText");
        tryCompare(version, "visible", true);
        verify(win.prefsDialog.visible, "Settings closed when About opened");
        click(findByName(win.contentItem, "aboutCloseButton"));
        tryCompare(version, "visible", false);

        // Back at Settings with the working copy as it was; Cancel still
        // cancels it.
        verify(win.prefsDialog.visible);
        compare(win.prefsDialog.cliVisible, false);
        click(findByName(win.prefsDialog.contentItem, "prefsCancelButton"));
        tryVerify(() => !win.prefsDialog.visible);
        compare(win.settings.showCli, true);
    }

    function test_preferencesSaveDiagnosticsWritesTheSupportFile() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        click(findByName(win.contentItem, "prefsOpenButton"));
        tryVerify(() => win.prefsDialog.opened);

        const saveDiag = findByName(win.prefsDialog.contentItem, "prefsDiagnosticsButton");
        // The Diagnostics section sits at the foot of a scrolling page, so
        // press it from the keyboard rather than at a coordinate that may be
        // scrolled out of view.
        focusIn(saveDiag);
        keyClick(Qt.Key_Space);
        const dialog = findByName(win.prefsDialog, "diagnosticsDialog");
        tryCompare(dialog, "visible", true);
        // The suggested name is filled in before the picker shows.
        verify(dialog.selectedFile.toString().endsWith(".txt"), dialog.selectedFile);
        dialog.selectedFile = diagnosticsUrl;
        dialog.accepted();  // see pickFile
        dialog.close();
        tryVerify(() => EncoderController.diagnosticsMessage.length > 0);
        verify(EncoderController.diagnosticsMessage.indexOf("tst_e2e_settings_diagnostics.txt") >= 0,
               EncoderController.diagnosticsMessage);
        tryCompare(findByName(win.prefsDialog.contentItem, "prefsDiagnosticsMessage"),
                   "visible", true);
        click(findByName(win.prefsDialog.contentItem, "prefsCancelButton"));
        tryVerify(() => !win.prefsDialog.visible);
    }

    function test_preferencesLanguageComboSwitchesTheWindowImmediately() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        verify(languageManager !== null);
        compare(languageManager.currentLanguage, "en");
        const englishTier = findByName(win.contentItem, "seg-guided").Accessible.name;

        win.prefsDialog.open();
        tryVerify(() => win.prefsDialog.opened);
        const combo = findByName(win.prefsDialog.contentItem, "prefsLanguage");
        compare(combo.currentValue, "en");
        stepCombo(combo, 1);
        const picked = combo.currentValue;
        verify(picked !== "en");
        // Applied on the pick, not behind Save.
        compare(languageManager.currentLanguage, picked);
        tryVerify(() => findChild(win.contentItem, "seg-guided").Accessible.name !== englishTier,
                  5000, "header did not retranslate");
        click(findByName(win.prefsDialog.contentItem, "prefsCancelButton"));
        tryVerify(() => !win.prefsDialog.visible);
        languageManager.setLanguage("en");
        tryVerify(() => findChild(win.contentItem, "seg-guided").Accessible.name === englishTier);
    }
}
