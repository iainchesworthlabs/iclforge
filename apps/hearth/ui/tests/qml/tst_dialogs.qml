import QtQuick
import QtTest

import Hearth
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
TestCase {
    id: testCase
    name: "Dialogs"
    when: windowShown

    Component { id: mainComponent; Main { } }

    function initTestCase() {
        verify(TestServices.useFakeRoom(), "the fake device room could not be installed");
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

    // The second way to About, beside the chain below: Settings' About card.
    // It opens the same dialog, with the same Licences... one hop further,
    // and the keyboard reference is never opened on the way.
    function test_settingsAboutOpensAboutAndItsLicences() {
        HearthController.firstRunSeen = true;
        const win = openWindow();
        const shortcuts = findChild(win, "shortcutsDialog");
        const about = findChild(win, "aboutDialog");
        const licences = findChild(win, "licencesDialog");
        verify(shortcuts !== null && about !== null && licences !== null);

        win.page = "settings";
        const card = findChild(win, "settingsAboutButton");
        verify(card !== null, "Settings has no About button");
        compare(card.text, "About Hearth…");
        card.clicked();
        tryVerify(function() { return about.opened; }, 5000, "About Hearth... did not open About");
        verify(!shortcuts.visible, "the keyboard reference opened on the way to About");
        verify(H.textItem(about.contentItem, "Hearth") !== null);

        mouseClick(findChild(win, "aboutLicencesButton"));
        tryVerify(function() { return licences.opened; }, 5000, "Licences... did not open Licences");
        compare(findChild(win, "licenceNoticesText").text, HearthController.licenceNotices);

        mouseClick(findChild(win, "licencesCloseButton"));
        tryVerify(function() { return !licences.visible; }, 5000);
        mouseClick(findChild(win, "aboutCloseButton"));
        tryVerify(function() { return !about.visible; }, 5000);
        compare(win.page, "settings");
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
