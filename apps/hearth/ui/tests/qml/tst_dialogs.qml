import QtQuick
import QtTest

import Hearth
import HearthLanguage
import HearthTest

import "HearthTestHelpers.js" as H

// The window's dialogs (FEATURE_COVERAGE.md rows 87-90): the first-run
// dialog on a first start and its two ways out, and the "? -> Keyboard
// shortcuts -> About -> Licences" chain Main.qml wires together, each hop
// through the button a person would press.
//
// First-run state lives in HearthController.firstRunSeen, persisted through
// the suite's own throwaway QSettings (qml_test_main.cpp), so each case sets
// it before it opens the window rather than relying on what an earlier
// case (alphabetical order) left behind.
//
// The dialogs are text, so at the largest text size, or in a language whose
// face stands taller, they are taller than a small window: the cases that
// follow the chain hold that each is capped at the window, scrolls above its
// buttons and keeps them inside it.
TestCase {
    id: testCase
    name: "Dialogs"
    when: windowShown

    Component { id: mainComponent; Main { } }

    property string startLanguage: ""

    function initTestCase() {
        verify(TestServices.useFakeRoom(), "the fake device room could not be installed");
    }

    function init() {
        startLanguage = LanguageManager.currentLanguage;
    }

    function cleanup() {
        HearthController.textScale = "100";
        if (startLanguage.length > 0 && LanguageManager.currentLanguage !== startLanguage) {
            LanguageManager.setLanguage(startLanguage);
        }
    }

    function key(win, k) {
        verify(TestServices.keyClick(win, k, Qt.NoModifier), "the key could not be delivered to the window");
    }

    // The smallest window the design allows (Main.qml: 960 x 620).
    function openSmallWindow() {
        const win = openWindow();
        win.width = 960;
        win.height = 620;
        tryCompare(win, "height", 620, 5000);
        waitForRendering(win.contentItem);
        return win;
    }

    function inDialog(dialog, name) {
        const found = H.find(dialog.contentItem, function(item) { return item.objectName === name; });
        verify(found !== null, "no " + name + " in the dialog");
        return found;
    }

    // The dialog is no taller than the window; what its text leaves no room for scrolls, to the end,
    // above its buttons, which are inside the window; and the keys scroll it from the focused button.
    function checkFitsTheWindow(win, dialog, scrollName, buttonNames, lastFragment) {
        tryVerify(function() { return dialog.opened; }, 5000);
        const root = win.contentItem;
        verify(dialog.height <= root.height, "the dialog is " + dialog.height + " tall in a window "
               + root.height + " tall");
        const scroll = inDialog(dialog, scrollName);
        const flick = scroll.contentItem;
        verify(scroll.overflowing, "the body is not scrolling, so the text did not need it here");
        verify(flick.contentHeight > flick.height, "the body is no taller than its view");
        for (let i = 0; i < buttonNames.length; ++i) {
            const button = inDialog(dialog, buttonNames[i]);
            const top = button.mapToItem(root, 0, 0).y;
            const bottom = button.mapToItem(root, 0, button.height).y;
            verify(top >= 0 && bottom <= root.height, buttonNames[i] + " is outside the window: " + top
                   + " to " + bottom + " of " + root.height);
        }
        // A key on the focused button scrolls the text, and Home and End go to its ends.
        flick.contentY = 0;
        key(win, Qt.Key_PageDown);
        tryVerify(function() { return flick.contentY > 0; }, 2000, "Page Down did not scroll");
        key(win, Qt.Key_End);
        tryCompare(flick, "contentY", flick.contentHeight - flick.height, 2000);
        const last = H.textContaining(flick.contentItem, lastFragment);
        verify(last !== null, "no text with " + lastFragment);
        const lastBottom = last.mapToItem(scroll, 0, last.height).y;
        verify(lastBottom <= scroll.height + 1, "the last of the text is below the view at the end of it: "
               + lastBottom + " of " + scroll.height);
        key(win, Qt.Key_Home);
        tryCompare(flick, "contentY", 0, 2000);
    }

    function test_theDialogsOfTheChainAreCappedAtTheWindowAndScrollAtTheLargestTextSize_data() {
        return [
            { tag: "English", language: "en" },
            { tag: "Hebrew", language: "he" }
        ];
    }

    // The report: at 175% Keyboard shortcuts and About were taller than the window in Arabic, Hebrew
    // and Yiddish, and lost Close, About... and Licences....
    function test_theDialogsOfTheChainAreCappedAtTheWindowAndScrollAtTheLargestTextSize(data) {
        HearthController.firstRunSeen = true;
        HearthController.textScale = "175";
        if (data.language !== LanguageManager.currentLanguage) {
            LanguageManager.setLanguage(data.language);
        }
        const win = openSmallWindow();
        compare(Theme.fontScale, 1.75);
        const shortcuts = findChild(win, "shortcutsDialog");
        const about = findChild(win, "aboutDialog");
        const licences = findChild(win, "licencesDialog");
        verify(shortcuts !== null && about !== null && licences !== null);

        mouseClick(findChild(win, "helpButton"));
        tryVerify(function() { return shortcuts.opened; }, 5000, "? did not open Keyboard shortcuts");
        checkFitsTheWindow(win, shortcuts, "shortcutsScroll", ["shortcutsAboutButton", "shortcutsCloseButton"],
                           "Show or hide the queue");

        // About... is reachable by the pointer, which is the point of it staying in the window.
        mouseClick(inDialog(shortcuts, "shortcutsAboutButton"));
        tryVerify(function() { return about.opened; }, 5000, "About... did not open About");
        checkFitsTheWindow(win, about, "aboutScroll", ["aboutLicencesButton", "aboutCloseButton"],
                           "github.com");

        mouseClick(inDialog(about, "aboutLicencesButton"));
        tryVerify(function() { return licences.opened; }, 5000, "Licences... did not open Licences");
        mouseClick(findChild(win, "licencesCloseButton"));
        tryVerify(function() { return !licences.visible; }, 5000);
        mouseClick(inDialog(about, "aboutCloseButton"));
        tryVerify(function() { return !about.visible; }, 5000);
        mouseClick(inDialog(shortcuts, "shortcutsCloseButton"));
        tryVerify(function() { return !shortcuts.visible; }, 5000);
    }

    function test_firstRunIsCappedAtTheWindowAndScrollsAtTheLargestTextSize_data() {
        return [
            { tag: "English", language: "en" },
            { tag: "Arabic", language: "ar" }
        ];
    }

    function test_firstRunIsCappedAtTheWindowAndScrollsAtTheLargestTextSize(data) {
        HearthController.firstRunSeen = false;
        HearthController.textScale = "175";
        if (data.language !== LanguageManager.currentLanguage) {
            LanguageManager.setLanguage(data.language);
        }
        const win = openSmallWindow();
        const dialog = findChild(win, "firstRunDialog");
        verify(dialog !== null);
        tryVerify(function() { return dialog.opened; }, 5000, "the first-run dialog did not open");
        checkFitsTheWindow(win, dialog, "firstRunScroll", ["firstRunNotNow", "firstRunOpenSpeakers"],
                           "a single sink");
        mouseClick(inDialog(dialog, "firstRunOpenSpeakers"));
        tryVerify(function() { return !dialog.visible; }, 5000);
        compare(win.page, "speakers");
    }

    // A dialog that fits is the height of its text, with no bar and nothing to scroll: what the window
    // looked like before is what it looks like.
    function test_aDialogThatFitsDoesNotScroll() {
        HearthController.firstRunSeen = true;
        const win = openWindow();
        const shortcuts = findChild(win, "shortcutsDialog");
        mouseClick(findChild(win, "helpButton"));
        tryVerify(function() { return shortcuts.opened; }, 5000);
        const scroll = inDialog(shortcuts, "shortcutsScroll");
        compare(scroll.overflowing, false);
        verify(scroll.contentItem.contentHeight <= scroll.contentItem.height + 1,
               "the body scrolls although the dialog fits");
        fuzzyCompare(shortcuts.height, shortcuts.implicitHeight, 1);
        mouseClick(findChild(win, "shortcutsCloseButton"));
        tryVerify(function() { return !shortcuts.visible; }, 5000);
    }

    function openWindow() {
        const win = createTemporaryObject(mainComponent, testCase);
        verify(win !== null);
        tryCompare(win, "visible", true);
        waitForRendering(win.contentItem);
        return win;
    }

    function test_firstRunShowsOnceAndNotNowRemembersIt() {
        HearthController.firstRunSeen = false;
        const win = openWindow();
        const notNow = findChild(win, "firstRunNotNow");
        verify(notNow !== null);
        // Opened one event-loop turn after the window comes up.
        tryVerify(function() { return notNow.visible; }, 5000, "the first-run dialog did not open on a first start");
        // Its first step names the output it will play to, and says where
        // the output name is without naming a side of the window: the header
        // it sits in is mirrored in a language written right to left.
        const outputRow = findChild(win, "firstRunOutputRow");
        verify(outputRow !== null);
        verify(outputRow.body.indexOf("at the top of the window") >= 0,
               "the first-run dialog does not say where the output name is: " + outputRow.body);
        verify(!/\b(left|right)\b/i.test(outputRow.body),
               "the first-run dialog names a side of the window: " + outputRow.body);
        // Its third step says a GROUP, not just pairing, is what a network
        // sink needs to play - the gap behind the report that a paired
        // sink's first Play fell back to the local output with nothing said
        // (hearth-followups-group-ux-and-live-diagnostics-2026-09-26).
        const dialog = findChild(win, "firstRunDialog");
        verify(dialog !== null);
        verify(H.textContaining(dialog.contentItem, "a group to play") !== null,
               "the first-run dialog's network step does not mention groups");
        mouseClick(notNow);
        tryVerify(function() { return !notNow.visible; }, 5000);
        tryCompare(HearthController, "firstRunSeen", true, 2000);
        compare(win.page, "play");

        // A second window on the same settings does not show it again.
        const again = openWindow();
        const againNotNow = findChild(again, "firstRunNotNow");
        // Give the window its Qt.callLater() turn before reading.
        waitForRendering(again.contentItem);
        verify(!againNotNow.visible, "the first-run dialog showed again after Not now");
    }

    function test_firstRunOpenSpeakersGoesToTheSpeakersPage() {
        HearthController.firstRunSeen = false;
        const win = openWindow();
        const open = findChild(win, "firstRunOpenSpeakers");
        verify(open !== null);
        tryVerify(function() { return open.visible; }, 5000, "the first-run dialog did not open on a first start");
        mouseClick(open);
        tryVerify(function() { return !open.visible; }, 5000);
        compare(win.page, "speakers");
        tryCompare(HearthController, "firstRunSeen", true, 2000);
    }

    function test_shortcutsAboutLicencesChain() {
        HearthController.firstRunSeen = true;
        const win = openWindow();
        const shortcuts = findChild(win, "shortcutsDialog");
        const about = findChild(win, "aboutDialog");
        const licences = findChild(win, "licencesDialog");
        verify(shortcuts !== null && about !== null && licences !== null);

        mouseClick(findChild(win, "helpButton"));
        tryVerify(function() { return shortcuts.opened; }, 5000, "? did not open Keyboard shortcuts");
        // It lists every page's shortcut.
        verify(H.textItem(shortcuts.contentItem, "Media information") !== null);

        mouseClick(findChild(win, "shortcutsAboutButton"));
        tryVerify(function() { return about.opened; }, 5000, "About... did not open About");
        // The version line is built from HearthController.versionDetails,
        // which is never empty (iclforge::ac3::version_details()).
        verify(HearthController.versionDetails.length > 0);
        verify(H.textItem(about.contentItem, "Hearth") !== null);
        verify(H.textItem(about.contentItem, about.compactVersion(HearthController.versionDetails)) !== null,
               "About does not show the version");

        mouseClick(findChild(win, "aboutLicencesButton"));
        tryVerify(function() { return licences.opened; }, 5000, "Licences... did not open Licences");
        const notices = findChild(win, "licenceNoticesText");
        verify(notices !== null);
        verify(notices.text.length > 0, "the licences view is empty");
        compare(notices.text, HearthController.licenceNotices);

        mouseClick(findChild(win, "licencesCloseButton"));
        tryVerify(function() { return !licences.visible; }, 5000);
        mouseClick(findChild(win, "aboutCloseButton"));
        tryVerify(function() { return !about.visible; }, 5000);
        mouseClick(findChild(win, "shortcutsCloseButton"));
        tryVerify(function() { return !shortcuts.visible; }, 5000);
    }
}
