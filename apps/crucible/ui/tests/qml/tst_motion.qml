import QtQuick
import QtTest

import Crucible
import CrucibleTest

// Motion: what the window does when it has been asked to move less.
//
// Three things are held here. Theme.reducedMotion is the OS request, or the
// person's own Settings > Appearance > Motion answer when they gave one, and
// the answer wins. Every animation in the window takes its duration from
// Theme.motionDuration, so under reduced motion a value that would have
// glided arrives at once. And the waiting mark stops turning.
//
// The operating system is stood in for by SystemMotion.setOverride, so no case
// here reads or changes the real setting, and none depends on how fast the
// machine is: a glide is told from a jump by reading the property in the
// same turn as the assignment, when a 90 ms animation has not yet had a frame.
TestCase {
    id: testCase
    name: "Motion"
    when: windowShown
    width: 1000
    height: 700

    Component { id: roomView; RoomView { width: 400; height: 400 } }
    Component { id: appRowComponent; AppRow { width: 300 } }
    Component { id: busyComponent; BusyMark { running: true; width: 24; height: 24 } }
    Component {
        id: fakeAppComponent
        QtObject {
            property int app: 7
            property string name: "Chrome"
            property int slot: 0
            property int width: 1
            property real x: 0.2
            property real y: 0.2
            property real z: 0.0
            property real size: 0.0
            property bool fullscreen: false
            property bool silent: false
            property bool active: true
            property bool tapped: true
            property bool background: false
            property string imagePath: ""
            property string iconName: ""
            property string appId: ""
            property real level: -60
        }
    }

    function cleanup() {
        Theme.motionChoice = "system";
        SystemMotion.clearOverride();
    }

    // --- the answer ------------------------------------------------------------

    function test_theSystemAnswersWhenThePersonHasNotChosen() {
        compare(Theme.motionChoice, "system", "System is the default");
        SystemMotion.setOverride(false);
        compare(Theme.reducedMotion, false);
        compare(Theme.motionDuration(90), 90);
        SystemMotion.setOverride(true);
        compare(Theme.reducedMotion, true, "the operating system asked for less");
        compare(Theme.motionDuration(90), 0);
        SystemMotion.clearOverride();
        compare(SystemMotion.overridden, false);
    }

    function test_thePersonsOwnChoiceWinsOverTheSystem() {
        SystemMotion.setOverride(true);
        Theme.motionChoice = "full";
        compare(Theme.reducedMotion, false, "Full overrides a system that asked for less");
        compare(Theme.motionDuration(60), 60);

        SystemMotion.setOverride(false);
        Theme.motionChoice = "reduced";
        compare(Theme.reducedMotion, true, "Reduced applies on a system that asked for nothing");
        compare(Theme.motionDuration(60), 0);
    }

    function test_aChoiceTheWindowDoesNotKnowReadsAsTheSystem() {
        SystemMotion.setOverride(true);
        Theme.motionChoice = "sparkle";
        compare(Theme.reducedMotion, true);
        SystemMotion.setOverride(false);
        compare(Theme.reducedMotion, false);
    }

    // --- the room --------------------------------------------------------------

    // A marker's position is a glided copy of where the engine has the
    // application. The engine moves it in steps; the glide is what joins them.
    function markerGxAfterTheEngineMoves(choice) {
        Theme.motionChoice = choice;
        const app = createTemporaryObject(fakeAppComponent, testCase, { app: 7, slot: 0, x: 0.2, y: 0.2 });
        verify(app);
        const view = createTemporaryObject(roomView, testCase.parent, { apps: [app], selectedApp: 7 });
        verify(view);
        waitForRendering(view);
        const marker = findChild(view, "marker-7");
        verify(marker);
        fuzzyCompare(marker.gx, 0.2, 1e-9, "the marker starts where the engine has it");
        app.x = 0.9;
        return marker.gx;
    }

    function test_theMarkerGlidesWhenMotionIsFull() {
        const gx = markerGxAfterTheEngineMoves("full");
        fuzzyCompare(gx, 0.2, 1e-9, "the glide has not moved it in the turn it was asked to");
    }

    function test_theMarkerJumpsWhenMotionIsReduced() {
        const gx = markerGxAfterTheEngineMoves("reduced");
        fuzzyCompare(gx, 0.9, 1e-9, "the marker is at the engine's position at once");
    }

    // --- the level bar -----------------------------------------------------------

    function barWidthAfterALevelChange(choice) {
        Theme.motionChoice = choice;
        const app = createTemporaryObject(fakeAppComponent, testCase, { slot: 0, level: -60 });
        verify(app);
        const row = createTemporaryObject(appRowComponent, testCase.parent, { app: app });
        verify(row);
        waitForRendering(row);
        const bar = findChild(row, "levelBar");
        verify(bar, "the row's level bar carries objectName levelBar");
        compare(bar.width, 0, "silent at the start");
        app.level = -30;
        return { width: bar.width, full: bar.parent.width };
    }

    function test_theLevelBarEasesWhenMotionIsFull() {
        const bar = barWidthAfterALevelChange("full");
        verify(bar.full > 0);
        compare(bar.width, 0, "the bar has not moved in the turn the level changed");
    }

    function test_theLevelBarTakesTheReadingAtOnceWhenMotionIsReduced() {
        const bar = barWidthAfterALevelChange("reduced");
        verify(bar.full > 0);
        fuzzyCompare(bar.width, bar.full * 0.5, 1e-6, "half of the bar for -30 dBFS, at once");
    }

    // --- the waiting mark ------------------------------------------------------

    function test_theWaitingMarkStopsTurningWhenMotionIsReduced() {
        const mark = createTemporaryObject(busyComponent, testCase.parent);
        verify(mark);
        const still = findChild(mark, "busyMarkStill");
        verify(still);
        Theme.motionChoice = "full";
        compare(still.visible, false, "the spinner turns and the still squares are not drawn");
        Theme.motionChoice = "reduced";
        compare(still.visible, true, "three still squares stand in for it");
        // Still says "working": it is drawn only while there is work.
        mark.running = false;
        compare(still.visible, false);
        mark.running = true;
        compare(still.visible, true);
    }
}
