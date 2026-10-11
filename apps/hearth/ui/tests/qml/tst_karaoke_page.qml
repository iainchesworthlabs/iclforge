import QtQuick
import QtTest

import Hearth
import HearthTest

import "HearthTestHelpers.js" as H

// The Karaoke page (KaraokePage.qml; planning/karaoke-design.md, K4): what it
// says of the stream playing, the choice it writes through to the decoder, and
// what the engine then plays, measured tone by tone at the fake device
// (TestServices.toneLevelDb()) on streams iclforge::ac3::FrameEncoder writes
// here (TestServices.writeAc3Stream(), test_room.hpp):
//
//   karaoke-3/2  bsmod 7, 3/2 with an LFE: L 440, M 800, R 620, V1 1030,
//                V2 1270 Hz, LFE 90 Hz
//   main-3/2     the same audio flagged a complete main service
//
// Annex C's tables are checked sample for sample by the library's own tests
// and by apps/hearth/engine/tests/test_stream_decoder.cpp; these hold that the
// page's controls reach them and that the page says the truth about them.
TestCase {
    id: testCase
    name: "KaraokePage"
    when: windowShown
    width: 1100
    height: 900

    readonly property string repo: Qt.resolvedUrl("../../../../../").toString()
    property string karaoke: ""
    property string main: ""
    property string plainEac3: ""
    // A tone is "there" above this level and "not there" below the other.
    readonly property real present: -45
    readonly property real absent: -70

    Component { id: mainComponent; Main { } }
    Component { id: pageComponent; KaraokePage { width: 1000; height: 800 } }

    function initTestCase() {
        verify(TestServices.useFakeRoom(), "the fake device room could not be installed");
        HearthController.firstRunSeen = true;
        karaoke = TestServices.writeAc3Stream("karaoke.ac3", "karaoke-3/2");
        main = TestServices.writeAc3Stream("main.ac3", "main-3/2");
        plainEac3 = TestServices.stageFixture(repo + "testdata/external-baseline/eac3-music-stereo-96/dee.ec3",
                                              "plain.ec3");
        verify(karaoke.length > 0 && main.length > 0 && plainEac3.length > 0, "the streams could not be made");
        HearthController.start();
        tryVerify(function() { return Object.keys(HearthController.decoderSettings).length > 0; }, 15000);
    }

    function cleanup() {
        HearthController.stop();
        for (let i = HearthController.queue.length - 1; i >= 0; --i) {
            HearthController.removeAt(i);
        }
        tryVerify(function() { return HearthController.queue.length === 0; }, 10000);
        settle({ karaoke: "aware", karaokeVocals: "both", stereoFold: "loro" });
        useLayout("2.0");
    }

    function settle(values) {
        const next = Object.assign({}, HearthController.decoderSettings);
        for (const key in values) {
            next[key] = values[key];
        }
        HearthController.setDecoderSettings(next);
        tryVerify(function() {
            for (const key in values) {
                if (HearthController.decoderSettings[key] !== values[key]) {
                    return false;
                }
            }
            return true;
        }, 10000, "the settings never reached the engine");
    }

    // By objectName through the visual tree, which reaches what a Repeater made
    // as well as what is written out. A window's tree starts at its contentItem.
    function named(root, name) {
        const start = root.contentItem !== undefined ? root.contentItem : root;
        const found = H.find(start, function(item) { return item.objectName === name; });
        return found !== null ? found : findChild(root, name);
    }

    function text(root, name) {
        const item = named(root, name);
        verify(item !== null, "no item named " + name);
        return item.text;
    }

    function clickSegment(root, group, value) {
        const choice = named(root, group);
        verify(choice !== null, "no " + group);
        const segment = named(choice, "seg-" + value);
        verify(segment !== null, "no seg-" + value + " in " + group);
        mouseClick(segment);
    }

    // Five full-bandwidth channels and an LFE need a layout that has them; the
    // room's own is two speakers.
    function useLayout(layout) {
        HearthController.setLayoutText(layout);
        tryVerify(function() {
            return HearthController.speakerLabels.length === (layout === "2.0" ? 2 : 6);
        }, 10000, "the layout never became " + layout);
    }

    function openPage() {
        const win = createTemporaryObject(mainComponent, testCase);
        verify(win !== null);
        tryCompare(win, "visible", true);
        win.page = "karaoke";
        waitForRendering(win.contentItem);
        return win;
    }

    function play(path, layout) {
        useLayout(layout === undefined ? "5.1" : layout);
        HearthController.addFiles([path]);
        tryVerify(function() { return HearthController.queue.length === 1; }, 10000);
        HearthController.playItem(0);
        tryCompare(HearthController, "state", "playing", 10000);
        tryVerify(function() { return TestServices.device().open; }, 10000, "the engine never opened the device");
        tryVerify(function() { return HearthController.currentMedia.codec !== undefined; }, 10000,
                  "the item's media information never arrived");
    }

    function slot(label) {
        const index = HearthController.speakerLabels.indexOf(label);
        verify(index >= 0, "no " + label + " slot in " + HearthController.speakerLabels);
        return index;
    }
    function level(label, hz) {
        return TestServices.toneLevelDb(slot(label), hz);
    }
    // Waits for the engine to have handed the device two windows of audio
    // since now, so that the level read is of the setting just made.
    function settled() {
        const from = TestServices.device().framesSubmitted;
        tryVerify(function() {
            return TestServices.device().framesSubmitted >= from + 2 * 16384;
        }, 30000, "the engine stopped handing the device audio");
    }

    function test_nothingPlayingSaysSoAndTheChoiceIsOpen() {
        const win = openPage();
        compare(text(win, "karaokeStreamName"), qsTr("Nothing is playing"));
        compare(named(win, "reproductionChoice").enabled, true);
        compare(named(win, "notApplicableNotice").visible, false);
        // Aware is the default, and the vocals are only a choice for capable.
        compare(HearthController.decoderSettings.karaoke, "aware");
        compare(named(win, "vocalsChoice").enabled, false);
    }

    function test_aKaraokeStreamIsNamedAndItsPartsAreHeard() {
        const win = openPage();
        play(karaoke);
        const programme = HearthController.currentMedia.programmes[0];
        compare(programme.karaoke, true);
        compare(programme.acmodLabel, "3/2");
        compare(programme.karaokeParts.join(" "), "L R M V1 V2");
        tryVerify(function() { return text(win, "karaokeStreamFacts").indexOf("3/2") >= 0; }, 5000);
        verify(text(win, "karaokeStreamFacts").indexOf("L R M V1 V2") >= 0);
        for (const part of ["L", "R", "M", "V1", "V2"]) {
            compare(text(win, "partState-" + part), qsTr("heard"), part);
        }
        compare(named(win, "notApplicableNotice").visible, false);
    }

    function test_awareReproducesAndTheSurroundsAreSilent() {
        const win = openPage();
        play(karaoke);
        settled();
        // L, C, R and the LFE; V1 beside L and V2 beside R at the stream's level.
        verify(level("L", 440) > present, "L lost its backing");
        verify(level("L", 1030) > present, "V1 did not reach L");
        verify(level("C", 800) > present, "the melody left the centre");
        verify(level("R", 620) > present);
        verify(level("R", 1270) > present, "V2 did not reach R");
        verify(level("Ls", 1030) < absent, "V1 still plays from Ls");
        verify(level("Rs", 1270) < absent, "V2 still plays from Rs");
        compare(text(win, "speakerCarries-C"), "M");
        compare(text(win, "speakerCarries-L"), "L + V1");
        compare(text(win, "speakerCarries-Ls"), qsTr("silent"));
    }

    function test_asCodedPlaysTheVocalsFromTheSurrounds() {
        const win = openPage();
        play(karaoke);
        clickSegment(win, "reproductionChoice", "asCoded");
        tryCompare(HearthController.decoderSettings, "karaoke", "asCoded", 10000);
        settled();
        verify(level("Ls", 1030) > present, "V1 is not on Ls as coded");
        verify(level("Rs", 1270) > present, "V2 is not on Rs as coded");
        verify(level("C", 800) > present);
        compare(text(win, "speakerCarries-Ls"), "V1");
    }

    function test_capableChoosesTheVocalsAndTheStreamPartsFollow() {
        const win = openPage();
        play(karaoke);
        clickSegment(win, "reproductionChoice", "capable");
        tryCompare(HearthController.decoderSettings, "karaoke", "capable", 10000);
        compare(named(win, "vocalsChoice").enabled, true);

        clickSegment(win, "vocalsChoice", "none");
        tryCompare(HearthController.decoderSettings, "karaokeVocals", "none", 10000);
        settled();
        verify(level("L", 440) > present && level("C", 800) > present && level("R", 620) > present);
        for (const label of ["L", "C", "R", "Ls", "Rs"]) {
            verify(level(label, 1030) < absent, "V1 is heard at " + label);
            verify(level(label, 1270) < absent, "V2 is heard at " + label);
        }
        compare(text(win, "partState-V1"), qsTr("muted"));
        compare(text(win, "partState-V2"), qsTr("muted"));
        compare(text(win, "partState-M"), qsTr("heard"));

        clickSegment(win, "vocalsChoice", "v1");
        tryCompare(HearthController.decoderSettings, "karaokeVocals", "v1", 10000);
        settled();
        // A single vocal joins the melody in the centre.
        verify(level("C", 800) > present && level("C", 1030) > present, "V1 is not with the melody");
        verify(level("L", 1030) < absent && level("R", 1270) < absent);
        compare(text(win, "partState-V1"), qsTr("heard"));
        compare(text(win, "partState-V2"), qsTr("muted"));
        compare(text(win, "speakerCarries-C"), "M + V1");

        clickSegment(win, "vocalsChoice", "both");
        tryCompare(HearthController.decoderSettings, "karaokeVocals", "both", 10000);
        settled();
        // A pair goes to the sides at unity.
        verify(level("L", 1030) > present && level("R", 1270) > present);
        verify(level("C", 1030) < absent);
    }

    function test_aStreamThatIsNotKaraokeSaysSoAndDisablesTheControls() {
        const win = openPage();
        play(main);
        compare(HearthController.currentMedia.programmes[0].karaoke, false);
        tryCompare(named(win, "notApplicableNotice"), "visible", true, 5000);
        compare(named(win, "reproductionChoice").enabled, false);
        compare(named(win, "vocalsChoice").enabled, false);
        verify(text(win, "karaokeStreamFacts").indexOf("not a karaoke one") >= 0);
        // Played as it was: the surrounds carry what the stream coded there.
        settled();
        verify(level("Ls", 1030) > present, "the surround was not played as coded");
    }

    function test_anEac3StreamIsNotAnAc3KaraokeStream() {
        const win = openPage();
        play(plainEac3);
        tryCompare(named(win, "notApplicableNotice"), "visible", true, 5000);
        compare(named(win, "reproductionChoice").enabled, false);
        verify(text(win, "karaokeStreamFacts").indexOf("Karaoke is an AC-3 mode") >= 0);
    }

    function test_theChoiceRoundTripsThroughTheDecoderSettings() {
        settle({ karaoke: "capable", karaokeVocals: "v2" });
        // Written through setDecoderSettings() and read back from the engine's
        // own snapshot, not from what was requested.
        const reread = HearthController.decoderSettings;
        compare(reread.karaoke, "capable");
        compare(reread.karaokeVocals, "v2");
    }

    function test_aBitstreamOutputSaysTheReceiverDecodes() {
        // The room has no receiver that takes a bitstream, so the page's output
        // mode is set to what one would give: the notice is the page's own.
        play(karaoke, "5.1");
        const page = createTemporaryObject(pageComponent, testCase.parent);
        verify(page !== null);
        waitForRendering(page);
        compare(named(page, "bitstreamNotice").visible, false);
        page.outputMode = "bitstream";
        tryCompare(named(page, "bitstreamNotice"), "visible", true, 5000);
        compare(named(page, "groupNotice").visible, false);
        // Its button asks for the output picker, which Main.qml opens.
        let asked = 0;
        page.outputPickerRequested.connect(function() { ++asked; });
        mouseClick(named(page, "chooseOutputButton"));
        compare(asked, 1);
        // A network group has its own sentence.
        page.outputMode = "networkGroup";
        tryCompare(named(page, "groupNotice"), "visible", true, 5000);
        compare(named(page, "bitstreamNotice").visible, false);
    }

    function test_anLtRtFoldIsNamedAndOneClickFixesIt() {
        const win = openPage();
        // Two speakers: the fold is the decoder's own, and Lt/Rt is a fold.
        play(karaoke, "2.0");
        compare(HearthController.speakerLabels.length, 2);
        compare(named(win, "ltrtNotice").visible, false);
        settle({ stereoFold: "ltrt" });
        tryCompare(named(win, "ltrtNotice"), "visible", true, 5000);
        mouseClick(named(win, "useLoRoButton"));
        tryCompare(HearthController.decoderSettings, "stereoFold", "loro", 10000);
        tryCompare(named(win, "ltrtNotice"), "visible", false, 5000);
    }

    function test_theDecoderPageSummarisesTheChoiceAndOpensTheKaraokePage() {
        const win = createTemporaryObject(mainComponent, testCase);
        verify(win !== null);
        tryCompare(win, "visible", true);
        win.page = "decoder";
        waitForRendering(win.contentItem);
        compare(text(win, "karaokeSummary"), qsTr("Karaoke: aware"));
        mouseClick(named(win, "karaokePageButton"));
        tryCompare(win, "page", "karaoke", 5000);
    }
}
