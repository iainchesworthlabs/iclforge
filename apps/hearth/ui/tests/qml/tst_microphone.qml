import QtQuick
import QtTest

import Hearth
import HearthTest

import "HearthTestHelpers.js" as H

// The microphone card of the Karaoke page (MicrophoneCard.qml; planning/karaoke-microphone.md, M4):
// what it promises and that its answers reach the controller.
//
// This suite NEVER presses "Turn the microphone on": a test must not open a person's microphone,
// and on a developer's desk the default input is a real one. What is held here is the off state, the
// words of the promise, that the answers (the input, the headphones, the delay) are written to the
// controller, that a card that is not in use cannot turn the microphone on, and that the page shows
// the card. The pipeline under it is tested with a scripted input in
// apps/hearth/engine/tests/test_microphone_session.cpp and test_microphone_controller.cpp.
TestCase {
    id: testCase
    name: "MicrophoneCard"
    when: windowShown
    // A click needs the item to be shown: a TestCase is not, unless it is told to be.
    visible: true
    width: 1100
    height: 900

    Component { id: cardComponent; MicrophoneCard { width: 900 } }
    Component { id: pageComponent; KaraokePage { width: 1000; height: 800 } }

    function initTestCase() {
        verify(TestServices.useFakeRoom(), "the fake device room could not be installed");
        HearthController.firstRunSeen = true;
    }

    function init() {
        MicrophoneController.headphones = false;
        MicrophoneController.inputDelayMs = 0;
        MicrophoneController.offsetMs = 0;
        MicrophoneController.deviceId = "";
    }

    function cleanup() {
        // Whatever a case did, the microphone is off at its end.
        compare(MicrophoneController.state === "on", false);
    }

    function find(item, name) {
        const found = findChild(item, name);
        verify(found !== null, "no item named " + name);
        return found;
    }

    function test_off_and_it_says_what_it_promises() {
        const card = createTemporaryObject(cardComponent, testCase);
        verify(card);
        compare(MicrophoneController.on, false);
        compare(MicrophoneController.state, "off");
        compare(card.summary, qsTr("Off"));
        compare(find(card, "microphoneToggle").text, qsTr("Turn the microphone on"));
        compare(find(card, "microphoneIndicator").visible, false);
        // The promise is on the card in words: off until turned on, nothing recorded, nothing sent.
        const note = find(card, "microphoneNote").text;
        verify(note.indexOf("off until you turn it on") >= 0, note);
        verify(note.indexOf("nothing is recorded") >= 0, note);
        verify(note.indexOf("leaves this computer") >= 0, note);
        // No meter and no pitch line until it is on.
        compare(find(card, "microphoneMeter").visible, false);
        compare(find(card, "microphonePitch").visible, false);
    }

    function test_the_headphones_answer_reaches_the_controller() {
        const card = createTemporaryObject(cardComponent, testCase);
        verify(card);
        const box = find(card, "microphoneHeadphones");
        compare(MicrophoneController.headphones, false);
        mouseClick(box);
        compare(MicrophoneController.headphones, true);
        mouseClick(box);
        compare(MicrophoneController.headphones, false);
    }

    function test_the_delay_and_the_timing_are_the_controllers() {
        const card = createTemporaryObject(cardComponent, testCase);
        verify(card);
        MicrophoneController.inputDelayMs = 130;
        MicrophoneController.offsetMs = -40;
        compare(find(card, "microphoneDelayText").text, qsTr("%1 ms").arg(130));
        compare(find(card, "microphoneOffsetText").text, qsTr("%1 ms").arg(-40));
        compare(find(card, "microphoneDelay").value, 130);
        compare(find(card, "microphoneOffset").value, -40);
        // And clamped to what the controller allows.
        MicrophoneController.inputDelayMs = 99999;
        compare(MicrophoneController.inputDelayMs, MicrophoneController.maxAlignmentMs);
    }

    function test_a_card_that_is_not_in_use_cannot_turn_it_on() {
        const card = createTemporaryObject(cardComponent, testCase, { active: false });
        verify(card);
        compare(find(card, "microphoneToggle").enabled, false);
        const live = createTemporaryObject(cardComponent, testCase, { active: true });
        verify(live);
        compare(find(live, "microphoneToggle").enabled, true);
    }

    function test_a_take_has_its_own_controls_and_its_own_indicator() {
        const card = createTemporaryObject(cardComponent, testCase);
        verify(card);
        // Off: no way to record, and no recording indicator; the folder is shown and can be chosen.
        compare(find(card, "microphoneRecord").visible, false);
        compare(find(card, "microphoneRecordingIndicator").visible, false);
        compare(find(card, "microphoneTakesFolder").text, MicrophoneController.takesFolder);
        verify(find(card, "microphoneChooseFolder").enabled);
        compare(find(card, "microphoneLastTake").visible, false);
        // What a take is, in words: the voice alone, lossless, with a description.
        const note = find(card, "microphoneTakeNote").text;
        verify(note.indexOf("voice alone") >= 0, note);
        verify(note.indexOf("lossless") >= 0, note);
        // The two indicators are two items.
        verify(find(card, "microphoneIndicator") !== find(card, "microphoneRecordingIndicator"));
    }

    function test_how_close_counts_and_whose_key_are_the_controllers() {
        const card = createTemporaryObject(cardComponent, testCase);
        verify(card);
        const box = find(card, "microphoneTolerance");
        MicrophoneController.toleranceCents = 100;
        tryCompare(box, "currentValue", 100);
        // Choosing a preset in the card sets the controller's.
        box.currentIndex = 2;
        box.activated(2);
        compare(MicrophoneController.toleranceCents, 200);
        // And the controller's setting moves the card.
        MicrophoneController.toleranceCents = 50;
        tryCompare(box, "currentValue", 50);
        MicrophoneController.toleranceCents = 100;

        // The key choice is off, says nothing until it is on, and says what it is waiting for.
        const own = find(card, "microphoneOwnKey");
        const inUse = find(card, "microphoneOwnKeyInUse");
        compare(MicrophoneController.ownKey, false);
        compare(inUse.visible, false);
        mouseClick(own);
        compare(MicrophoneController.ownKey, true);
        compare(inUse.visible, true);
        verify(inUse.text.indexOf("Waiting") >= 0, inUse.text);
        mouseClick(own);
        compare(MicrophoneController.ownKey, false);
    }

    function test_the_takes_in_the_folder_are_listed_and_removed_after_asking() {
        // Seven takes in a folder of their own, made as files: the list is read from the
        // descriptions beside them.
        let folder = "";
        for (let i = 1; i <= 7; ++i) {
            const stem = "Song - 2020-01-0" + i + " 100000";
            const audio = TestServices.writeTextFile(stem + ".flac", "fLaC");
            verify(audio.length > 0, "could not write a take");
            folder = audio.substring(0, audio.lastIndexOf("/"));
            const description = JSON.stringify({
                format: "iclforge-take-1", audio: stem + ".flac", seconds: 60 + i,
                created_utc: "2020-01-0" + i + "T10:00:00Z", item: { title: "Song" },
                item_start_ms: 0 });
            verify(TestServices.writeTextFile(stem + ".take.json", description).length > 0);
        }
        MicrophoneController.setTakesFolder(folder);
        compare(MicrophoneController.takes.length, 7);

        const card = createTemporaryObject(cardComponent, testCase);
        verify(card);
        compare(find(card, "microphoneTakesList").visible, true);
        // The newest five, and the rest counted.
        const isRow = function(item) { return item.objectName === "microphoneTakeRow"; };
        let rows = H.findAll(card, isRow);
        compare(rows.length, 5);
        verify(rows[0].text.indexOf("2020-01-07") >= 0, rows[0].text);
        verify(rows[4].text.indexOf("2020-01-03") >= 0, rows[4].text);
        compare(find(card, "microphoneTakesMore").visible, true);
        verify(find(card, "microphoneTakesMore").text.indexOf("2") >= 0);

        // Removing asks again: the first Delete shows the question, Keep takes it back, and
        // nothing has been removed.
        const firstOf = function(name) {
            return H.find(card, function(item) { return item.objectName === name; });
        };
        compare(firstOf("microphoneTakeDeleteConfirm").visible, false);
        firstOf("microphoneTakeDelete").clicked();
        compare(firstOf("microphoneTakeDeleteConfirm").visible, true);
        compare(firstOf("microphoneTakeDelete").visible, false);
        firstOf("microphoneTakeKeep").clicked();
        compare(firstOf("microphoneTakeDeleteConfirm").visible, false);
        compare(MicrophoneController.takes.length, 7);

        // Confirmed, the take goes, and the next one is shown in its place.
        firstOf("microphoneTakeDelete").clicked();
        firstOf("microphoneTakeDeleteConfirm").clicked();
        compare(MicrophoneController.takes.length, 6);
        rows = H.findAll(card, isRow);
        compare(rows.length, 5);
        verify(rows[0].text.indexOf("2020-01-06") >= 0, rows[0].text);
        verify(find(card, "microphoneTakesMore").text.indexOf("1") >= 0);
    }

    function test_the_page_shows_the_card() {
        const page = createTemporaryObject(pageComponent, testCase);
        verify(page);
        const card = find(page, "microphoneCard");
        compare(card.ordinal, "04");
        // Not karaoke, nothing playing: the card is there and is not in use.
        compare(card.active, false);
        compare(MicrophoneController.on, false);
    }

    function test_the_part_scored_against_is_the_one_the_singer_has_muted() {
        const card = createTemporaryObject(cardComponent, testCase,
                                           { reproduction: "capable", vocals: "none" });
        verify(card);
        compare(card.preferred, "v1");
        card.vocals = "v1";
        compare(card.preferred, "v2");
        card.vocals = "both";
        compare(card.preferred, "m");
        card.reproduction = "aware";
        compare(card.preferred, "m");
    }
}
