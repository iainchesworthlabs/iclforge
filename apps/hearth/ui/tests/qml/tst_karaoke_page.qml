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
//   eac3-karaoke-3/2       the karaoke programme as E-AC-3, one independent substream
//   eac3-karaoke-3/2+rear  the same with the 7.1 rear pair in a dependent substream
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
    property string karaoke21: ""
    property string main: ""
    property string plainEac3: ""
    property string eac3Karaoke: ""
    property string eac3KaraokeRear: ""
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
        karaoke21 = TestServices.writeAc3Stream("karaoke21.ac3", "karaoke-2/1");
        plainEac3 = TestServices.stageFixture(repo + "testdata/external-baseline/eac3-music-stereo-96/dee.ec3",
                                              "plain.ec3");
        eac3Karaoke = TestServices.writeAc3Stream("karaoke.ec3", "eac3-karaoke-3/2");
        eac3KaraokeRear = TestServices.writeAc3Stream("karaoke-rear.ec3", "eac3-karaoke-3/2+rear");
        verify(karaoke.length > 0 && main.length > 0 && plainEac3.length > 0, "the streams could not be made");
        verify(eac3Karaoke.length > 0 && eac3KaraokeRear.length > 0, "the E-AC-3 streams could not be made");
        HearthController.start();
        tryVerify(function() { return Object.keys(HearthController.decoderSettings).length > 0; }, 15000);
    }

    function cleanup() {
        HearthController.stop();
        for (let i = HearthController.queue.length - 1; i >= 0; --i) {
            HearthController.removeAt(i);
        }
        tryVerify(function() { return HearthController.queue.length === 0; }, 10000);
        settle({ karaoke: "aware", karaokeVocals: "both", stereoFold: "loro",
                 karaokeMelodyDb: 0, karaokeVocal1Db: 0, karaokeVocal2Db: 0 });
        useLayout("2.0");
        HearthController.textScale = "100";
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

    // A 2/1 stream's coded channels are L (440 Hz), R (800) and S, the one vocal
    // (620): what the page says each speaker plays is held to what the decoder
    // does, for a stream with a single vocal (found by the forge-gui oracle
    // test, which decodes every mode: "both" is V1 on the left alone, not in
    // the centre).
    function test_aSingleVocalIsOnTheLeftForBothAndInTheCentreForV1() {
        const win = openPage();
        play(karaoke21);
        clickSegment(win, "reproductionChoice", "capable");
        tryCompare(HearthController.decoderSettings, "karaoke", "capable", 10000);
        compare(HearthController.currentMedia.programmes[0].karaokeParts.join(" "), "L R V1");

        clickSegment(win, "vocalsChoice", "both");
        tryCompare(HearthController.decoderSettings, "karaokeVocals", "both", 10000);
        settled();
        verify(level("L", 620) > present, "V1 is not on L");
        verify(level("C", 620) < absent, "V1 is in the centre");
        verify(level("R", 620) < absent, "V1 is on R");
        compare(text(win, "speakerCarries-L"), "L + V1");
        compare(text(win, "speakerCarries-C"), qsTr("silent"));

        clickSegment(win, "vocalsChoice", "v1");
        tryCompare(HearthController.decoderSettings, "karaokeVocals", "v1", 10000);
        settled();
        verify(level("C", 620) > present, "V1 is not in the centre");
        verify(level("L", 620) < absent, "V1 is still on L");
        compare(text(win, "speakerCarries-C"), "V1");
        compare(text(win, "speakerCarries-L"), "L");
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

    function test_aPlainEac3StreamIsNotKaraokeAndSaysSo() {
        const win = openPage();
        play(plainEac3);
        tryCompare(named(win, "notApplicableNotice"), "visible", true, 5000);
        compare(named(win, "reproductionChoice").enabled, false);
        compare(HearthController.currentMedia.programmes[0].karaoke, false);
        compare(HearthController.currentMedia.programmes[0].karaokeFlagged, false);
        verify(text(win, "karaokeStreamFacts").indexOf("An E-AC-3 stream, but not a karaoke one") >= 0);
    }

    // An E-AC-3 programme of one independent substream flagged bsmod 7 is a karaoke stream the
    // decoder reproduces (planning/karaoke-eac3.md, D1 to D3): named, its parts heard, and the
    // surrounds silent under the default reproduction, as the AC-3 case above.
    function test_anEac3KaraokeStreamIsNamedAndReproducedAsAnAc3OneIs() {
        const win = openPage();
        play(eac3Karaoke);
        const programme = HearthController.currentMedia.programmes[0];
        compare(programme.karaoke, true);
        compare(programme.karaokeFlagged, true);
        compare(programme.karaokeParts.join(" "), "L R M V1 V2");
        tryVerify(function() { return text(win, "karaokeStreamFacts").indexOf("L R M V1 V2") >= 0; }, 5000);
        compare(named(win, "notApplicableNotice").visible, false);
        compare(named(win, "reproductionChoice").enabled, true);
        settled();
        verify(level("L", 440) > present, "L lost its backing");
        verify(level("L", 1030) > present, "V1 did not reach L");
        verify(level("C", 800) > present, "the melody left the centre");
        verify(level("R", 1270) > present, "V2 did not reach R");
        verify(level("Ls", 1030) < absent, "V1 still plays from Ls");
        verify(level("Rs", 1270) < absent, "V2 still plays from Rs");
        compare(text(win, "speakerCarries-L"), "L + V1");
        compare(text(win, "speakerCarries-Ls"), qsTr("silent"));
        verify(HearthController.thisFrame.karaokeReproduced === true, "the report did not say reproduced");
        // The playing line names the codec.
        verify(text(win, "decodeStage").indexOf("E-AC-3 karaoke") >= 0);
    }

    function test_anEac3KaraokeStreamTakesTheCapableChoiceToo() {
        const win = openPage();
        play(eac3Karaoke);
        clickSegment(win, "reproductionChoice", "capable");
        tryCompare(HearthController.decoderSettings, "karaoke", "capable", 10000);
        clickSegment(win, "vocalsChoice", "v1");
        tryCompare(HearthController.decoderSettings, "karaokeVocals", "v1", 10000);
        settled();
        // A single vocal joins the melody in the centre.
        verify(level("C", 800) > present && level("C", 1030) > present, "V1 is not with the melody");
        verify(level("L", 1030) < absent && level("R", 1270) < absent);
    }

    // The same service type on a programme with a dependent substream is flagged and not
    // reproduced: the page says so, the controls do not apply, and the surrounds carry what the
    // stream coded there.
    function test_anEac3KaraokeProgrammeWithADependentSubstreamIsFlaggedAndPlaysAsCoded() {
        const win = openPage();
        play(eac3KaraokeRear);
        const programme = HearthController.currentMedia.programmes[0];
        compare(programme.karaokeFlagged, true);
        compare(programme.karaoke, false);
        tryCompare(named(win, "notApplicableNotice"), "visible", true, 5000);
        compare(named(win, "reproductionChoice").enabled, false);
        tryVerify(function() { return text(win, "karaokeStreamFacts").indexOf("Flagged karaoke") >= 0; }, 5000);
        settled();
        verify(level("Ls", 1500) > present, "the dependent substream's surround was not played as coded");
        verify(level("L", 1030) < absent, "V1 was reproduced into L");
        verify(HearthController.thisFrame.karaokeReproduced !== true, "the report said reproduced");
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

    // The levels (Annex C.2.3's "additional flexibility"): a slider's value is
    // dB against the table, and what plays follows it, measured tone by tone.
    function test_aPartsLevelMovesWhatPlaysAndTheBottomIsOff() {
        const win = openPage();
        play(karaoke);
        // The aware decoder has no levels, and the controls say so.
        compare(named(win, "level-M").enabled, false);
        clickSegment(win, "reproductionChoice", "capable");
        tryCompare(HearthController.decoderSettings, "karaoke", "capable", 10000);
        tryCompare(named(win, "level-M"), "enabled", true, 5000);
        compare(named(win, "level-V1").enabled, true);
        compare(named(win, "level-V2").enabled, true);
        compare(text(win, "levelText-M"), qsTr("%1 dB").arg("0"));
        settled();
        const melodyAtTable = level("C", 800);
        const vocalAtTable = level("L", 1030);
        verify(melodyAtTable > present);

        // -12 dB on the melody: its tone falls by 12 dB and the vocals do not move.
        settle({ karaokeMelodyDb: -12 });
        settled();
        const quieter = level("C", 800);
        verify(Math.abs((melodyAtTable - quieter) - 12) < 1.0,
               "the melody fell " + (melodyAtTable - quieter) + " dB, not 12");
        verify(Math.abs(level("L", 1030) - vocalAtTable) < 0.5, "the vocal moved with the melody");
        compare(text(win, "levelText-M"), qsTr("%1 dB").arg("-12"));
        compare(text(win, "partState-M"), qsTr("heard"));

        // The bottom of the range is off: the part is not there, and the page says so.
        settle({ karaokeMelodyDb: -24 });
        settled();
        verify(level("C", 800) < absent, "the melody is still heard at the bottom of its control");
        compare(text(win, "levelText-M"), qsTr("off"));
        compare(text(win, "partState-M"), qsTr("muted"));
        compare(text(win, "speakerCarries-C"), qsTr("silent"));
        verify(level("L", 440) > present, "the backing went with the melody");

        // A vocal off leaves its side silent of it.
        settle({ karaokeMelodyDb: 0, karaokeVocal2Db: -24 });
        settled();
        verify(level("R", 1270) < absent, "V2 is still heard");
        verify(level("L", 1030) > present, "V1 went with V2");
        compare(text(win, "partState-V2"), qsTr("muted"));
        compare(text(win, "speakerCarries-R"), "R");

        // Reset puts every part back to the table's, and the button is only open when one moved.
        compare(named(win, "resetLevelsButton").enabled, true);
        mouseClick(named(win, "resetLevelsButton"));
        tryCompare(HearthController.decoderSettings, "karaokeVocal2Db", 0, 10000);
        compare(HearthController.decoderSettings.karaokeMelodyDb, 0);
        tryCompare(named(win, "resetLevelsButton"), "enabled", false, 5000);
        settled();
        verify(level("R", 1270) > present, "V2 did not come back");
    }

    function test_aLevelNoVocalChoiceIncludesHasNothingToActOn() {
        const win = openPage();
        play(karaoke);
        clickSegment(win, "reproductionChoice", "capable");
        tryCompare(HearthController.decoderSettings, "karaoke", "capable", 10000);
        clickSegment(win, "vocalsChoice", "v1");
        tryCompare(HearthController.decoderSettings, "karaokeVocals", "v1", 10000);
        tryCompare(named(win, "level-V2"), "enabled", false, 5000);
        compare(named(win, "level-V1").enabled, true);
        compare(named(win, "level-M").enabled, true);
    }

    function test_theLevelsAreKeptAcrossStarts() {
        settle({ karaoke: "capable", karaokeMelodyDb: -9, karaokeVocal1Db: 4, karaokeVocal2Db: -24 });
        // What a restart reads: the file itself.
        tryVerify(function() { return TestServices.storedSetting("karaoke/melodyDb") === "-9"
                                      || TestServices.storedSetting("karaoke/melodyDb") === -9; }, 5000,
                  "the melody's level was not written to the settings");
        compare(Number(TestServices.storedSetting("karaoke/vocal1Db")), 4);
        compare(Number(TestServices.storedSetting("karaoke/vocal2Db")), -24);
        compare(String(TestServices.storedSetting("karaoke/reproduction")), "capable");
        settle({ karaokeMelodyDb: 0, karaokeVocal1Db: 0, karaokeVocal2Db: 0 });
        tryVerify(function() { return Number(TestServices.storedSetting("karaoke/melodyDb")) === 0; }, 5000);
    }

    // The page fits its window: nothing on it is laid out or drawn past the
    // page's left or right edge, each card holds what is in it, and the cards
    // keep the margin every other page has. At each text size, as it is and
    // mirrored as a right-to-left language mirrors it, at the window's own
    // width and at the smallest it goes to; with nothing playing, and for a
    // stream without a second vocal, whose rows say so at length. The window
    // starts at the largest text and is then changed as Settings changes it,
    // so both a first layout and a second are held.
    //
    // A row that cannot shrink sets the least width of every row of its card,
    // which is how two sliders once pushed a whole card past the window.
    function test_nothingOnThePageRunsPastItsEdges() {
        HearthController.textScale = "175";
        const win = openPage();
        const page = H.find(win.contentItem, function(item) {
            return item.lyricsFullScreenRequested !== undefined && item.reproduction !== undefined;
        });
        verify(page !== null, "no Karaoke page in the window");
        const cards = H.findAll(page, function(item) {
            return item.ordinal !== undefined && item.framed !== undefined;
        });
        verify(cards.length >= 7, "the page's cards were not found");

        // The page's own edges, and each card's: a row too wide for the left-hand
        // card runs into the card beside it long before it reaches the window's edge.
        const spills = function() {
            const found = H.pastTheEdges(page).map(function(what) { return "the page: " + what; });
            for (const card of cards) {
                for (const what of H.pastTheEdges(card)) {
                    found.push("card " + card.ordinal + ": " + what);
                }
            }
            return found;
        };
        const fits = function(when, width, textScale, mirrored) {
            HearthController.textScale = textScale;
            win.width = width;
            win.LayoutMirroring.enabled = mirrored;
            tryCompare(Theme, "fontScale", Number(textScale) / 100, 2000);
            tryCompare(page, "width", width, 5000);
            const where = when + ", " + width + " px at " + textScale + "%"
                          + (mirrored ? ", right to left" : "");
            // A change reaches the layouts when they are next polished, and a text that
            // wraps anew asks for one more: read once they have stopped asking.
            verify(waitForPolish(win, 5000), where + ": the layouts were never polished");
            let past = spills();
            for (let turn = 0; turn < 30 && past.length > 0; ++turn) {
                wait(100);
                past = spills();
            }
            verify(past.length === 0,
                   where + ": " + past.length + " past an edge, the first of them: "
                   + past.slice(0, 6).join("; "));
            // And that it was this layout that was read, not the one before the change: the
            // Microphone card is on the side the direction puts it, and a label of one line is
            // as tall as the text size makes it.
            const microphone = named(win, "microphoneCard");
            compare(microphone.mapToItem(page, 0, 0).x < page.width / 2, mirrored,
                    where + ": the Microphone card is on the wrong side");
            const label = H.textItem(microphone, qsTr("Input"));
            verify(label !== null, where + ": no Input label");
            verify(Math.abs(label.height - label.implicitHeight) <= 1,
                   where + ": a label is " + label.height + " px tall, for text that needs "
                   + label.implicitHeight);
            // The cards, by their outer edges: the margin of the Settings and Speakers pages.
            let left = page.width;
            let right = 0;
            for (const card of cards) {
                const x = card.mapToItem(page, 0, 0).x;
                left = Math.min(left, x);
                right = Math.max(right, x + card.width);
            }
            verify(Math.abs(left - Theme.pad) <= 1, where + ": the cards start at " + left);
            verify(Math.abs(right - (page.width - Theme.pad)) <= 1, where + ": the cards end at " + right);
        };

        fits("nothing playing", 1280, "175", false);

        play(karaoke21);
        tryCompare(page, "isKaraoke", true, 5000);
        tryVerify(function() { return text(win, "partState-V2") === qsTr("not in this stream"); }, 5000);
        fits("no second vocal", 1280, "175", false);
        fits("no second vocal", 1280, "175", true);
        HearthController.stop();
        HearthController.removeAt(0);
        tryVerify(function() { return HearthController.queue.length === 0; }, 10000);

        play(karaoke);
        tryVerify(function() { return text(win, "partState-V2") === qsTr("heard"); }, 5000);
        for (const textScale of ["175", "150", "125", "100"]) {
            fits("a karaoke stream", 1280, textScale, false);
            fits("a karaoke stream", 1280, textScale, true);
        }
        fits("a karaoke stream", 960, "100", false);
        fits("a karaoke stream", 960, "100", true);
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
