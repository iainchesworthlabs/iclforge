import QtQuick
import QtTest

import Hearth
import HearthTest

// The two pictures of how a person sings (planning/karaoke-microphone.md, M5): a line's mark
// (LineMark.qml) and the pitch ribbon (PitchRibbon.qml). Both are pictures of an answer they are
// handed, so they are held here with answers made up for them; the answers themselves are tested
// where they are made (test_microphone_controller.cpp). This suite never turns a microphone on.
TestCase {
    id: testCase
    name: "PitchRibbonAndLineMark"
    when: windowShown
    visible: true
    width: 900
    height: 600

    Component { id: markComponent; LineMark { size: 14 } }
    Component { id: ribbonComponent; PitchRibbon { width: 600; height: 120 } }
    Component { id: cardComponent; MicrophoneCard { width: 900 } }

    function initTestCase() {
        verify(TestServices.useFakeRoom(), "the fake device room could not be installed");
        HearthController.firstRunSeen = true;
    }

    function cleanup() {
        compare(MicrophoneController.on, false);
    }

    // --- a line's mark -----------------------------------------------------------------------

    function test_aLineWithNoNoteOrNoAnswerHasNoMark() {
        const mark = createTemporaryObject(markComponent, testCase);
        verify(mark);
        compare(mark.shown, false);
        compare(mark.visible, false);
        compare(mark.implicitHeight, 0);
        // The tune has no note in the line: nothing, which is not a zero.
        mark.result = { scored: false, withheld: false, notSung: false, percent: -1, level: 0 };
        compare(mark.shown, false);
        compare(mark.visible, false);
        // No headphones answer: nothing either.
        mark.result = { scored: false, withheld: true, notSung: false, percent: -1, level: 0 };
        compare(mark.shown, false);
        compare(mark.label, "");
    }

    function test_threeLevelsAreThreePipsAndANumber() {
        const mark = createTemporaryObject(markComponent, testCase);
        verify(mark);
        const pips = findChild(mark, "lineMarkPips");
        const levels = [[3, 91], [2, 60], [1, 20]];
        for (let i = 0; i < levels.length; ++i) {
            mark.result = { scored: true, withheld: false, notSung: false,
                            percent: levels[i][1], level: levels[i][0] };
            compare(mark.shown, true);
            compare(mark.visible, true);
            compare(mark.level, levels[i][0]);
            compare(mark.label, qsTr("%1 %").arg(levels[i][1]));
            verify(mark.description.indexOf(String(levels[i][1])) >= 0, mark.description);
            compare(pips.visible, true);
            // The pips filled are the level: colour is not the only cue.
            let filled = 0;
            for (let c = 0; c < pips.children.length; ++c) {
                const pip = pips.children[c];
                if (pip.border !== undefined && pip.color.a > 0) {
                    ++filled;
                }
            }
            compare(filled, levels[i][0]);
        }
    }

    function test_aLineNotSungSaysSoInWordsAndIsNotAScore() {
        const mark = createTemporaryObject(markComponent, testCase);
        verify(mark);
        mark.result = { scored: true, withheld: false, notSung: true, percent: -1, level: 0 };
        compare(mark.shown, true);
        compare(mark.notSung, true);
        compare(mark.level, 0);
        compare(mark.label, qsTr("not sung"));
        compare(findChild(mark, "lineMarkPips").visible, false);
        verify(mark.description.indexOf("not sung") >= 0, mark.description);
    }

    // --- the ribbon ----------------------------------------------------------------------------

    function test_theRibbonPutsNowNearTheLeftAndHigherPitchHigher() {
        const ribbon = createTemporaryObject(ribbonComponent, testCase,
                                             { positionMs: 10000, lowMidi: 48, highMidi: 72 });
        verify(ribbon);
        // "now" is a share of the width in from the left; what is ahead is to the right.
        fuzzyCompare(ribbon.xOf(10000), 600 * ribbon.nowFraction, 0.001);
        verify(ribbon.xOf(11000) > ribbon.xOf(10000));
        fuzzyCompare(ribbon.xOf(ribbon.fromMs + ribbon.windowMs), 600, 0.001);
        // Higher pitch is higher up the picture, and the range fits in it.
        verify(ribbon.yOf(72) < ribbon.yOf(60));
        verify(ribbon.yOf(60) < ribbon.yOf(48));
        verify(ribbon.yOf(72) >= 0);
        verify(ribbon.yOf(48) <= ribbon.height);
    }

    function test_theRibbonKnowsWhichNoteIsBeingSungAndHowMuchOfTheVoiceIsOnIt() {
        const ribbon = createTemporaryObject(ribbonComponent, testCase, {
            notes: [0, 500, 60, 600, 1100, 64],
            // time, MIDI (-1 not voiced), on the tune
            trace: [0, 60.1, 1, 10, 60.0, 1, 20, -1, 0, 30, 70, 0, 40, 60, 1],
            positionMs: 300
        });
        verify(ribbon);
        compare(ribbon.noteCount, 2);
        compare(ribbon.tracePointCount, 5);
        compare(ribbon.voicedCount, 4);
        compare(ribbon.hitCount, 3);
        compare(ribbon.currentNote(), 0);
        ribbon.positionMs = 800;
        compare(ribbon.currentNote(), 1);
        ribbon.positionMs = 550;   // between two notes
        compare(ribbon.currentNote(), -1);
    }

    function test_theRibbonSaysListeningWhenNothingIsVoiced() {
        const ribbon = createTemporaryObject(ribbonComponent, testCase, { listening: false });
        verify(ribbon);
        const text = findChild(ribbon, "pitchRibbonListening");
        verify(text !== null);
        compare(text.visible, false);
        ribbon.listening = true;
        compare(text.visible, true);
        compare(text.text, qsTr("Listening…"));
    }

    function test_aRibbonThatIsNotLiveAsksForNothing() {
        // Handed data and not live: the controller is not asked, whatever it holds.
        const ribbon = createTemporaryObject(ribbonComponent, testCase, {
            notes: [0, 500, 60], trace: [], live: false });
        verify(ribbon);
        wait(120);
        compare(ribbon.noteCount, 1);
    }

    // --- where they are --------------------------------------------------------------------------

    function test_theCardHasARibbonAndASummaryThatAreHiddenUntilThereIsSomethingToShow() {
        const card = createTemporaryObject(cardComponent, testCase);
        verify(card);
        const ribbon = findChild(card, "microphoneRibbon");
        verify(ribbon !== null);
        compare(ribbon.visible, false);
        compare(ribbon.live, false);
        const summary = findChild(card, "microphoneSummary");
        verify(summary !== null);
        compare(summary.visible, false);
    }
}
