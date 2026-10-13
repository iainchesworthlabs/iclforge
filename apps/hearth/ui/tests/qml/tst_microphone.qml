import QtQuick
import QtTest

import Hearth
import HearthTest

import "HearthTestHelpers.js" as H

// The microphone card of the Karaoke page (MicrophoneCard.qml; planning/karaoke-microphone.md, M4):
// what it promises and that its answers reach the controller.
//
// This suite never opens a person's microphone: the one it has is a script (TestServices.
// scriptMicrophone(), qml_test_main.cpp's RealtimeScriptedInput), the machine's inputs are not
// listed, and on a developer's desk the default input is a real one. What is held here is the off
// state, the words of the promise, that the answers (the input, the headphones, the delay) are
// written to the controller, that a card that is not in use cannot turn the microphone on, that
// the page shows the card, and that the rows that are only there while a microphone is on fit the
// card. The pipeline under it is tested with a scripted input in
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
        Theme.fontScale = 1.0;
        MicrophoneController.headphones = false;
        MicrophoneController.inputDelayMs = 0;
        MicrophoneController.offsetMs = 0;
        MicrophoneController.deviceId = "";
    }

    function cleanup() {
        // The takes a case made or recorded are files in the suite's one scratch folder, which
        // the next case would count: removed, through the controller, so that each starts empty.
        for (const take of MicrophoneController.takes.slice()) {
            MicrophoneController.deleteTake(take.path);
        }
        // Whatever a case did, the microphone is off at its end.
        compare(MicrophoneController.state === "on", false);
    }

    function find(item, name) {
        const found = findChild(item, name);
        verify(found !== null, "no item named " + name);
        return found;
    }

    // Takes in the suite's own folder, made as files: the list is read from the descriptions
    // beside them. The folder is chosen and read again, since choosing the one that is already
    // chosen changes nothing and the files were made after it was. Returns the folder.
    function writeTakes(count) {
        let folder = "";
        for (let i = 1; i <= count; ++i) {
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
        MicrophoneController.refreshTakes();
        return folder;
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

    // A label sits beside its control in a card with the room for both, and above it in a card a
    // third of the window wide or at a large text size. Either way nothing in the card runs past
    // its edge: not a row of it, and not the two buttons that ask again under a take. The widths
    // are the card's at a window of 1280 px (408) and of 960 px (301), and a wide one.
    function test_the_rows_fit_a_narrow_card_and_a_wide_one_data() {
        return [
            { tag: "900 px", width: 900, fontScale: 1.0, stacked: false },
            { tag: "434 px, the narrowest that is beside", width: 434, fontScale: 1.0, stacked: false },
            { tag: "433 px, the widest that is above", width: 433, fontScale: 1.0, stacked: true },
            { tag: "408 px", width: 408, fontScale: 1.0, stacked: true },
            { tag: "301 px", width: 301, fontScale: 1.0, stacked: true },
            { tag: "408 px, 175%", width: 408, fontScale: 1.75, stacked: true },
            { tag: "727 px, 175%, the narrowest that is beside", width: 727, fontScale: 1.75,
              stacked: false },
            { tag: "900 px, 175%", width: 900, fontScale: 1.75, stacked: false }
        ];
    }

    function test_the_rows_fit_a_narrow_card_and_a_wide_one(data) {
        writeTakes(2);
        verify(MicrophoneController.takes.length >= 2, "the takes were not listed");
        Theme.fontScale = data.fontScale;
        const card = createTemporaryObject(cardComponent, testCase, { width: data.width });
        verify(card);
        compare(card.stacked, data.stacked);
        // Asked about the first take, so that the two buttons that answer are on show.
        H.find(card, function(item) { return item.objectName === "microphoneTakeDelete"; }).clicked();
        compare(find(card, "microphoneTakeDeleteConfirm").visible, true);

        // A layout settles over a few turns after it is made.
        let past = H.pastTheEdges(card);
        for (let turn = 0; turn < 30 && past.length > 0; ++turn) {
            wait(100);
            past = H.pastTheEdges(card);
        }
        verify(past.length === 0,
               past.length + " past the card's edges, the first of them: " + past.slice(0, 6).join("; "));

        const label = H.textItem(card, qsTr("Input"));
        verify(label !== null, "no Input label");
        const box = find(card, "microphoneDevice");
        const labelAt = label.mapToItem(card, 0, 0);
        const boxAt = box.mapToItem(card, 0, 0);
        if (data.stacked) {
            verify(labelAt.y + label.height <= boxAt.y + 0.5, "the label is not above its control");
            fuzzyCompare(labelAt.x, boxAt.x, 1);
        } else {
            verify(labelAt.x + label.width <= boxAt.x + 0.5, "the label is not beside its control");
            verify(labelAt.y < boxAt.y + box.height && boxAt.y < labelAt.y + label.height,
                   "the label and its control are not on one row");
        }
    }

    // The rows that are only there while a microphone is on: the level and "Too loud", the pitch,
    // the ribbon and the score, the take being made, the one just kept and the list of them, the
    // question asked before one is removed, and what the performance came to once the microphone
    // is off again. The input is a full-scale tone at the pitch of the stream's melody guide, so
    // that it is loud, voiced, and sung against a tune. Each step is measured after it, since
    // each shows rows the one before did not, and each row that is to be there is checked to be:
    // a case that shows nothing cannot pass.
    function test_the_rows_of_a_microphone_that_is_on_fit_the_card_data() {
        return [
            { tag: "900 px", width: 900, fontScale: 1.0 },
            { tag: "434 px, the narrowest that is beside", width: 434, fontScale: 1.0 },
            { tag: "408 px", width: 408, fontScale: 1.0 },
            { tag: "301 px", width: 301, fontScale: 1.0 },
            { tag: "727 px, 175%, the narrowest that is beside", width: 727, fontScale: 1.75 },
            { tag: "301 px, 175%", width: 301, fontScale: 1.75 }
        ];
    }

    function test_the_rows_of_a_microphone_that_is_on_fit_the_card(data) {
        const stream = TestServices.writeAc3Stream("microphone-rows.ac3", "karaoke-3/2");
        verify(stream.length > 0, "the stream could not be made");
        writeTakes(3);
        Theme.fontScale = data.fontScale;
        MicrophoneController.headphones = true;
        MicrophoneController.toleranceCents = 100;
        TestServices.scriptMicrophone(800, 1.0, 60);

        // Not in use, so that the card does not ask the controller to forget the item, which
        // there is none of, a turn after it is made: the stream is loaded here, once it has.
        const card = createTemporaryObject(cardComponent, testCase,
                                           { width: data.width, active: false });
        verify(card);
        wait(100);
        MicrophoneController.load(stream, "m");
        tryCompare(MicrophoneController, "referenceStatus", "ready", 20000);

        const row = function(name) {
            return H.find(card, function(item) { return item.objectName === name; });
        };
        // `shown` rows are there, and nothing of the card is past its edges.
        const fits = function(step, shown) {
            for (const name of shown) {
                const item = row(name);
                verify(item !== null && item.visible, step + ": " + name + " is not showing");
            }
            let past = H.pastTheEdges(card);
            for (let turn = 0; turn < 30 && past.length > 0; ++turn) {
                wait(100);
                past = H.pastTheEdges(card);
            }
            verify(past.length === 0, step + ": " + past.length + " past the card's edges, the first of "
                   + "them: " + past.slice(0, 6).join("; "));
        };

        try {
            MicrophoneController.anchor(0, true);
            verify(MicrophoneController.start(), "the scripted microphone would not turn on");
            tryCompare(MicrophoneController, "state", "on", 2000);
            tryVerify(function() { return MicrophoneController.clipping; }, 10000,
                      "a full-scale tone was never reported as too loud");
            tryVerify(function() { return row("microphonePitch").text.indexOf("Hz") >= 0; }, 10000,
                      "the tone was never heard as a pitch");
            tryVerify(function() { return MicrophoneController.score.percent >= 0; }, 10000,
                      "nothing was scored");
            fits("turned on", ["microphoneToggle", "microphoneIndicator", "microphoneMeter",
                               "microphoneClip", "microphonePitch", "microphoneRibbon",
                               "microphoneScore", "microphoneScoreNote", "microphoneRecord",
                               "microphoneTakesList"]);

            verify(MicrophoneController.startRecording(), "a take would not begin");
            tryCompare(MicrophoneController, "recording", true, 2000);
            fits("recording", ["microphoneRecordingIndicator", "microphoneTakeLength"]);
            wait(300);
            MicrophoneController.stopRecording();
            tryVerify(function() { return MicrophoneController.lastTake.name !== undefined; }, 5000,
                      "the take was not kept");
            fits("a take kept", ["microphoneLastTake", "microphoneShowTakes",
                                 "microphoneDeleteTake", "microphoneTakesList",
                                 "microphoneTakeDelete"]);

            // Asked about the newest one: the two buttons that answer take the place of one.
            row("microphoneTakeDelete").clicked();
            fits("asked about a take", ["microphoneTakeDeleteConfirm", "microphoneTakeKeep"]);
            row("microphoneTakeKeep").clicked();
        } finally {
            MicrophoneController.stop();
        }

        // Turned off: the meter and the pitch go, what the performance came to stays.
        tryVerify(function() { return MicrophoneController.summary.percent !== undefined; }, 5000,
                  "no summary was kept");
        fits("turned off", ["microphoneSummary", "microphoneSummaryScore",
                            "microphoneSummaryDetail", "microphoneSummaryBasis",
                            "microphoneLastTake", "microphoneTakesList"]);
        verify(!row("microphoneMeter").visible, "the meter is showing with the microphone off");

        // A microphone that does not turn on says why, at length: a long sentence of short words,
        // since a word that is wider than the card is no layout's to wrap.
        TestServices.refuseMicrophone("The input is in use by another program, which has taken it "
                                      + "for itself, and will not let this one have it until it "
                                      + "has let it go again.");
        verify(!MicrophoneController.start());
        tryCompare(MicrophoneController, "state", "error", 2000);
        fits("refused", ["microphoneError"]);
        TestServices.refuseMicrophone("");

        // One that is on and hears nothing says so.
        TestServices.scriptMicrophone(0, 0, 60);
        verify(MicrophoneController.start());
        try {
            tryVerify(function() { return row("microphoneSilent").visible; }, 8000,
                      "silence was never said to the person");
            fits("silent", ["microphoneSilent"]);
        } finally {
            MicrophoneController.stop();
        }
    }

    function test_the_takes_in_the_folder_are_listed_and_removed_after_asking() {
        // Seven takes in a folder of their own.
        writeTakes(7);
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
