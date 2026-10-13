import QtQuick
import QtTest

import Crucible
import CrucibleTest

// The room: the session list the controller publishes, the placed/bed
// counts that go with it, and position/unposition commands. With the
// engine stopped the list is empty and the counts are zero; with it
// running the counts add up to the list and a placement round-trips.
//
// The engine runs over the scripted machine (ui/tests/qml_test_main.cpp: the
// fake session monitor, devices, default device and full-screen seam the
// engine's Catch2 cases use), never over the platform's own sessions. The
// platform's list is whatever the person running the test has open: a call
// application's process id turned up in the failure message of a run on a
// machine with Zoom running, because an application the room cannot place
// fails a case that is about the room. Over the scripted machine the room is
// the three applications below on every machine, so a case waits for them to
// reach the controller and not for a fixed time.
TestCase {
    id: testCase
    name: "Room"
    when: windowShown
    width: 1480
    height: 820

    Component { id: roomPage; RoomPage { width: 1480; height: 700 } }

    readonly property var chrome: ({ app: 900, name: "chrome", active: true })
    readonly property var steam: ({ app: 901, name: "steam", active: true })
    readonly property var zoom: ({ app: 902, name: "zoom", active: false })

    function init() {
        CrucibleController.stop();
        CrucibleController.showSilentApps = true;
        CrucibleController.showBackgroundApps = false;
        CrucibleController.splitStereo = false;
    }

    function cleanup() {
        CrucibleController.stop();
        TestServices.clear();
    }

    // The scripted room, running, and all of it listed. Skips (not fails)
    // where the engine will not run over it: the refusal every engine suite
    // in this directory honours.
    function scripted(apps) {
        if (!TestServices.scriptSessions(apps)) {
            skip("the scripted machine is not available in this harness");
        }
        CrucibleController.start();
        if (!CrucibleController.running) {
            skip("the engine did not run over the scripted machine: " + CrucibleController.lastError);
        }
        tryVerify(function() { return CrucibleController.framesEncoded > 0; }, 5000, "frames never flowed");
        tryVerify(function() { return CrucibleController.apps.length === apps.length; }, 5000,
                  "the scripted applications reach the controller: " + JSON.stringify(CrucibleController.apps));
    }

    // One application's row as the controller publishes it now, or null.
    function row(id) {
        return CrucibleController.apps.find(function(a) { return a.app === id; }) || null;
    }

    function test_emptyRoomWithTheEngineStopped() {
        compare(CrucibleController.apps.length, 0);
        compare(CrucibleController.placedCount, 0);
        compare(CrucibleController.bedCount, 0);
        const page = createTemporaryObject(roomPage, testCase);
        verify(page);
        waitForRendering(page);
    }

    function test_threeDToggleLoadsTheViewWhenBuilt() {
        const page = createTemporaryObject(roomPage, testCase);
        verify(page);
        waitForRendering(page);
        const choice = findChild(page, "roomViewChoice");
        const loader = findChild(page, "room3dLoader");
        verify(choice);
        verify(loader);
        // Effective visibility is false for everything under a TestCase
        // (the TestCase item itself is hidden), so the toggle's presence is
        // the check; its own visible binding follows has3D.
        compare(loader.active, false);
        if (!CrucibleController.has3D) {
            skip("this build has no Qt Quick 3D");
        }
        page.threeD = true;
        compare(loader.active, true);
        // The view either comes up or reports why; it never hangs the page.
        tryVerify(function() { return loader.status === Loader.Ready || loader.status === Loader.Error; }, 10000);
        if (loader.status === Loader.Ready) {
            compare(loader.item.apps.length, CrucibleController.apps.length);
        }
        page.threeD = false;
        compare(loader.active, false);
    }

    function test_runningRoomCountsAddUp() {
        scripted([chrome, steam, zoom]);
        // The list is the three scripted applications; the invariant is that
        // every application is either placed or in the bed.
        compare(CrucibleController.apps.length, 3);
        compare(CrucibleController.placedCount + CrucibleController.bedCount, CrucibleController.apps.length);
        // The rail names an application by its executable's stem with a capital first letter.
        const names = CrucibleController.apps.map(function(a) { return a.name.toLowerCase(); }).sort();
        compare(names.join(","), "chrome,steam,zoom");
        for (const app of CrucibleController.apps) {
            verify(app.app > 0);
            verify(app.name.length > 0);
            verify(app.slot === -1 || (app.slot >= 0 && app.slot < 10), "slot " + app.slot);
        }
        // Commands for an unknown application are ignored, not fatal.
        CrucibleController.position(4294967295, 0.5, 0.5, 0);
        CrucibleController.unposition(4294967295);
        CrucibleController.reprobe();
        compare(CrucibleController.apps.length, 3);
        const page = createTemporaryObject(roomPage, testCase);
        verify(page);
        waitForRendering(page);
    }

    function test_splitTakesTwoSlotsAndMonoGivesOneBack() {
        scripted([chrome, steam, zoom]);
        const id = chrome.app;
        CrucibleController.setSplit(id, true);
        CrucibleController.position(id, 0.5, 0.5, 0);
        tryVerify(function() {
            const app = row(id);
            return app && app.width === 2 && app.slot >= 0;
        }, 5000, "after split: " + JSON.stringify(row(id)));
        const placed = CrucibleController.placedCount;
        CrucibleController.setSplit(id, false);
        tryVerify(function() {
            const app = row(id);
            return app && app.width === 1 && app.slot >= 0;
        }, 5000, "after mono: " + JSON.stringify(row(id)));
        compare(CrucibleController.placedCount, placed);  // one application either way
        // The object's extent round-trips, clamped to the room.
        CrucibleController.setSize(id, 0.4);
        tryVerify(function() { const app = row(id); return app && Math.abs(app.size - 0.4) < 1e-6; }, 5000, "size 0.4");
        CrucibleController.setSize(id, 7);
        tryVerify(function() { const app = row(id); return app && app.size === 1; }, 5000, "size clamped to 1");
        CrucibleController.setSize(id, 0);
        CrucibleController.unposition(id);
    }

    function test_placementRoundTripsForAScriptedApplication() {
        scripted([chrome, steam, zoom]);
        const id = steam.app;
        CrucibleController.position(id, 0.25, 0.75, 0.5);
        tryVerify(function() {
            const app = row(id);
            return app && app.slot >= 0 && Math.abs(app.x - 0.25) < 0.02 && Math.abs(app.y - 0.75) < 0.02;
        }, 5000, "placed: " + JSON.stringify(row(id)));
        CrucibleController.unposition(id);
        tryVerify(function() {
            const app = row(id);
            return app && app.slot === -1;
        }, 5000, "back in the bed: " + JSON.stringify(row(id)));
    }

    // A split pair's objects can be placed on their own; "standard stereo"
    // puts them back at the spread either side of the centre.
    function test_pairSidesPlaceOnTheirOwnAndResetToTheSpread() {
        scripted([chrome, steam, zoom]);
        const id = chrome.app;
        CrucibleController.setSplit(id, true);
        CrucibleController.position(id, 0.5, 0.5, 0);
        tryVerify(function() { const app = row(id); return app && app.width === 2 && !app.pairCustom; }, 5000,
                  "split at the spread: " + JSON.stringify(row(id)));
        tryVerify(function() { const app = row(id); return app && Math.abs(app.lx - 0.35) < 1e-6; }, 5000,
                  "left side at the spread: " + JSON.stringify(row(id)));
        CrucibleController.positionSide(id, 1, 0.9, 0.9, 0);
        tryVerify(function() { const app = row(id); return app && app.pairCustom; }, 5000,
                  "custom after a side was placed: " + JSON.stringify(row(id)));
        tryVerify(function() { const app = row(id); return app && Math.abs(app.rx - 0.9) < 1e-6; }, 5000,
                  "right side x: " + JSON.stringify(row(id)));
        tryVerify(function() { const app = row(id); return app && Math.abs(app.ry - 0.9) < 1e-6; }, 5000,
                  "right side y: " + JSON.stringify(row(id)));
        CrucibleController.resetPair(id);
        tryVerify(function() { const app = row(id); return app && !app.pairCustom; }, 5000,
                  "back to standard: " + JSON.stringify(row(id)));
        CrucibleController.setSplit(id, false);
        CrucibleController.stop();
    }

    // The Room's full-screen note follows the controller: it states the
    // rule while the reason is empty (the engine stopped, or a platform
    // that can answer) and prints the platform's reason when the rule is
    // off. Nothing here asserts English: the note's stopped text is the
    // rule, and the running text either is that or carries the reason.
    // Both answers are scripted, so each is read on every machine and not
    // the one a display happens to give.
    function noteFollowsTheController(unsupportedReason) {
        // Parented to the window's root item, not the TestCase, for the
        // reason tst_settings gives: the TestCase item is invisible by design.
        const page = createTemporaryObject(roomPage, testCase.parent);
        verify(page);
        waitForRendering(page);
        const note = findChild(page, "fullscreenRuleNote");
        verify(note, "the full-screen note carries objectName fullscreenRuleNote");
        verify(note.text.length > 0);
        // Stopped: no engine has said anything, so the note states the rule.
        compare(CrucibleController.fullscreenRuleReason, "");
        compare(CrucibleController.fullscreenRuleAvailable, false);
        const ruleText = note.text;
        if (!TestServices.scriptSessions([chrome])) {
            skip("the scripted machine is not available in this harness");
        }
        if (unsupportedReason.length > 0) {
            verify(TestServices.setFullscreenUnsupported(unsupportedReason));
        }
        CrucibleController.start();
        if (!CrucibleController.running) {
            skip("the engine did not run over the scripted machine: " + CrucibleController.lastError);
        }
        // The engine's first status says what the seam answered: the rule is
        // available exactly when there is no reason, and the reason is the
        // one the seam gave.
        tryVerify(function() {
            return CrucibleController.fullscreenRuleReason === unsupportedReason
                && CrucibleController.fullscreenRuleAvailable === (unsupportedReason.length === 0);
        }, 5000, "available " + CrucibleController.fullscreenRuleAvailable + " reason '" + CrucibleController.fullscreenRuleReason + "'");
        // Read together, so a reason that flips between the two reads cannot
        // split them.
        const reason = CrucibleController.fullscreenRuleReason;
        if (unsupportedReason.length > 0) {
            tryVerify(function() { return note.text.indexOf(reason) >= 0; }, 5000, note.text);
            verify(note.text !== ruleText, note.text);
        } else {
            tryCompare(note, "text", ruleText);
        }
        CrucibleController.stop();
        compare(CrucibleController.fullscreenRuleReason, "");
        compare(CrucibleController.fullscreenRuleAvailable, false);
        compare(note.text, ruleText);
    }

    function test_fullscreenRuleNoteStatesTheRuleOnAMachineThatCanAnswer() {
        noteFollowsTheController("");
    }

    function test_fullscreenRuleNotePrintsTheReasonOnAMachineThatCannot() {
        noteFollowsTheController("no display to ask");
    }

}
