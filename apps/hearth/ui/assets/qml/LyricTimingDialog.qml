import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

import Hearth

// Timing the lyrics (planning/karaoke-phase2.md, W6; the findings' "W3: word timing").
//
// Two passes over the item as it plays. Lines: Space stamps the next line as it starts, which alone
// turns plain words into a file that is followed line by line. Words: Estimate gives every stamped
// line its words' times by their letters, drawn as a guess (muted, slanted); playing a line again,
// Space replaces the next guess in it with a press. Every press is taken to be the reaction time
// late and has it taken off, so that what is stored is the sound's time; the lyric view elsewhere is
// untouched until Save, so an estimate is never shown as sung.
//
// A word can be chosen with a click, in either pass, and is then corrected as a line is: Stamp again
// and the nudges are for it, and in the words pass the next Space presses it, once, in place of the
// next guess. That is how one wrong word of a file that came with its times (from an aligner, say)
// is put right: the words of such a file are taken as given, so the pass would skip them all.
//
// Nothing here keeps the words or the times: KaraokeController holds the session, and this reads
// timingLines and sends it presses. The clock is LyricClock's, so a press is as exact as a frame
// (a poll alone is 60 ms).
Dialog {
    id: root
    objectName: "lyricTimingDialog"

    // The page that opens it; a modal over the whole window.
    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    focus: true
    width: Math.min(920, parent ? parent.width - 60 : 920)
    height: Math.min(700, parent ? parent.height - 60 : 700)
    padding: Theme.space6
    title: ""
    // Escape leaves, unless there is something unsaved: that has its own button.
    closePolicy: KaraokeController.timingDirty ? Popup.NoAutoClose : Popup.CloseOnEscape

    // "lines" or "words": what Space stamps.
    property string mode: "lines"
    // The row a nudge, a stamp-again and a click on a line act on.
    property int selectedLine: -1
    // The word of that row they act on instead, or -1 for the row itself. A click on a word chooses
    // it; a click on the row lets it go.
    property int selectedWord: -1
    // Whether the next Space in the words pass is for the chosen word. A press, or a stamp again,
    // spends it, so that the chosen word is not pressed again by a Space meant for the next one; a
    // click on the word arms it again.
    property bool wordArmed: false
    // How far ahead of a chosen word the item is cued, to be listening before it comes.
    property int wordLeadInMs: 1500
    // What the last Save said.
    property string notice: ""
    // The clock, for a test to hold still.
    property alias clock: timingClock

    readonly property var rows: KaraokeController.timingLines
    readonly property real position: timingClock.positionMs
    readonly property int nextLine: KaraokeController.nextLine

    background: Rectangle {
        color: Theme.bg
        border.color: Theme.text
        border.width: 2
    }

    LyricClock { id: timingClock; active: root.visible }

    function clockText(ms) {
        const total = Math.max(0, ms) / 1000;
        const minutes = Math.floor(total / 60);
        const seconds = total - minutes * 60;
        return minutes + ":" + (seconds < 10 ? "0" : "") + seconds.toFixed(2);
    }

    // What a line's singer is called on its row, as the file writes it.
    function singerTag(key) {
        return key === "v1" ? "V1" : (key === "v2" ? "V2" : (key === "both" ? "V1+V2" : ""));
    }

    // The stamped line the position is in: the last stamped line that has started.
    function lineAt(ms) {
        let at = -1;
        for (let i = 0; i < root.rows.length; ++i) {
            if (root.rows[i].stamped && root.rows[i].startMs <= ms) {
                at = i;
            }
        }
        return at;
    }

    // The word a press would set: the first one in the line the position is in that no press has
    // set. Nothing before the first stamped line, or once every word of the line has been pressed.
    function wordTarget(ms) {
        const at = root.lineAt(ms);
        if (at < 0) {
            return null;
        }
        const words = root.rows[at].words;
        for (let w = 0; w < words.length; ++w) {
            if (!words[w].tapped) {
                return { line: at, word: w };
            }
        }
        return null;
    }

    // Whether a word is chosen. The rows are replaced by every press, so the choice is checked
    // against the words as they are now.
    readonly property bool wordSelected: root.selectedLine >= 0
                                         && root.selectedLine < root.rows.length
                                         && root.selectedWord >= 0
                                         && root.selectedWord < root.rows[root.selectedLine].words.length

    // The word a press would set: the chosen one while a press is still for it, otherwise the next
    // that no press has set.
    function pressTarget(ms) {
        if (root.wordSelected && root.wordArmed) {
            return { line: root.selectedLine, word: root.selectedWord };
        }
        return root.wordTarget(ms);
    }

    // The word a press would set, in the words pass.
    readonly property var wordSlot: root.mode === "words" ? root.pressTarget(root.position) : null
    readonly property bool canTap: KaraokeController.timing
                                   && (root.mode === "lines" ? root.nextLine >= 0 : root.wordSlot !== null)
    // The row the list keeps in view: the line to be stamped, or the one being sung.
    readonly property int followIndex: root.mode === "lines"
                                       ? Math.max(0, root.nextLine < 0 ? root.rows.length - 1 : root.nextLine)
                                       : Math.max(0, root.lineAt(root.position))
    readonly property bool selectedStamped: root.selectedLine >= 0
                                            && root.selectedLine < root.rows.length
                                            && root.rows[root.selectedLine].stamped

    // The words pass starts from an estimate in every stamped line that has none, so that there is
    // something to press over. Coming back to it after stamping another line estimates that one;
    // a word that was pressed is never estimated over.
    function setMode(value) {
        root.mode = value;
        if (value === "words") {
            KaraokeController.estimateWords();
        }
    }

    // What the next press is for. The properties it reads arrive one at a time when a press changes
    // them, so each is checked against the lines as they are now.
    function nextText() {
        const lines = root.rows;
        if (root.mode === "lines") {
            const next = root.nextLine;
            return next >= 0 && next < lines.length
                   ? qsTr("Next line: %1").arg(lines[next].text)
                   : qsTr("Every line has a time. Go on to the words, or save.");
        }
        const slot = root.wordSlot;
        if (slot !== null && slot.line < lines.length && slot.word < lines[slot.line].words.length) {
            const text = lines[slot.line].words[slot.word].text;
            return root.wordSelected && root.wordArmed
                   ? qsTr("Space presses the chosen word again: %1").arg(text)
                   : qsTr("Next word: %1").arg(text);
        }
        if (KaraokeController.stampedLines === 0) {
            return qsTr("Stamp the lines first.");
        }
        return root.lineAt(root.position) < 0
               ? qsTr("Play to the first line, then press Space as each word starts.")
               : qsTr("Every word in this line is pressed. Play on to the next line.");
    }

    // One press, as the pad and Space both make it.
    function tap() {
        if (!root.canTap) {
            return false;
        }
        const ms = Math.round(root.position);
        if (root.mode === "lines") {
            const stamped = KaraokeController.tapLine(ms);
            // The last line has its time: on to the words.
            if (stamped && root.nextLine < 0) {
                root.setMode("words");
            }
            return stamped;
        }
        const chosen = root.wordSelected && root.wordArmed;
        const pressed = KaraokeController.tapWord(root.wordSlot.line, root.wordSlot.word, ms);
        // That press was the chosen word's: the next Space is for the next word again.
        if (pressed && chosen) {
            root.wordArmed = false;
        }
        return pressed;
    }

    function selectLine(index) {
        root.selectedLine = index;
        root.selectedWord = -1;
        root.wordArmed = false;
        if (index >= 0 && index < root.rows.length && root.rows[index].stamped) {
            HearthController.seek(root.rows[index].startMs);
        }
    }

    // A word is chosen as a line is, and cued a little before it, since a word is heard in an
    // instant and a line is not: it has to be playing by the time it comes. Choosing it again cues
    // it again.
    function selectWord(line, word) {
        if (line < 0 || line >= root.rows.length || word < 0 || word >= root.rows[line].words.length) {
            return;
        }
        root.selectedLine = line;
        root.selectedWord = word;
        root.wordArmed = true;
        HearthController.seek(Math.max(0, root.rows[line].words[word].startMs - root.wordLeadInMs));
    }

    // Stamp again, for the chosen word or else the selected line: where the position is now, less
    // the reaction time, as a press is.
    function stampAgain() {
        const ms = Math.round(root.position);
        if (root.wordSelected) {
            root.wordArmed = false;
            return KaraokeController.tapWord(root.selectedLine, root.selectedWord, ms);
        }
        return KaraokeController.restampLine(root.selectedLine, ms);
    }

    // A nudge's word: the chosen one, or -1 for the line.
    readonly property int nudgeWord: root.wordSelected ? root.selectedWord : -1

    function save() {
        const problem = KaraokeController.saveTiming();
        root.notice = problem.length > 0
                      ? problem
                      : qsTr("Saved as %1. A file that was there is kept once, as %1.bak.")
                            .arg(KaraokeController.fileName);
    }

    onOpened: {
        root.notice = "";
        root.mode = "lines";
        root.selectedLine = -1;
        root.selectedWord = -1;
        root.wordArmed = false;
        KaraokeController.beginTiming();
        contentItem.forceActiveFocus();
    }
    onClosed: KaraokeController.endTiming()

    FileDialog {
        id: textFileDialog
        objectName: "timingTextFileDialog"
        title: qsTr("Choose the words to time")
        nameFilters: [qsTr("Lyrics and text (*.lrc *.txt)"), qsTr("All files (*)")]
        onAccepted: KaraokeController.beginTiming(selectedFile.toString())
    }

    contentItem: ColumnLayout {
        spacing: Theme.space3
        Accessible.role: Accessible.Dialog
        Accessible.name: qsTr("Time the lyrics")

        // Space is the key under the singer's thumb while the item plays. It lives in the content
        // so that the window's shortcut matching sees an item, not a popup.
        Shortcut {
            sequence: "Space"
            enabled: root.visible && KaraokeController.timing
            onActivated: root.tap()
        }

        Text {
            Layout.fillWidth: true
            text: qsTr("Time the lyrics")
            color: Theme.text
            font.pixelSize: Theme.fontTitle
            font.family: Theme.headingFamily
            font.weight: Font.ExtraBold
        }

        // ---- no words yet ----------------------------------------------------------------
        ColumnLayout {
            visible: !KaraokeController.timing
            Layout.fillWidth: true
            spacing: Theme.space3

            Text {
                objectName: "timingNoWords"
                Layout.fillWidth: true
                text: KaraokeController.timingError.length > 0
                      ? KaraokeController.timingError
                      : qsTr("There are no words to time for this item.")
                color: Theme.text
                font.pixelSize: Theme.fontNormal
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("Choose a text file with one line of the words to a line (an .lrc file "
                           + "with no times will do), saved as UTF-8 or UTF-16.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }
            AppButton {
                objectName: "timingChooseText"
                text: qsTr("Choose a text file…")
                primary: true
                onClicked: textFileDialog.open()
            }
        }
        // Keeps the title and the choice at the top, not spread over the height.
        Item {
            visible: !KaraokeController.timing
            Layout.fillHeight: true
        }

        // ---- a session ---------------------------------------------------------------------
        ColumnLayout {
            visible: KaraokeController.timing
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.space3

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space3
                SegmentedControl {
                    objectName: "timingMode"
                    accessibleName: qsTr("What Space stamps")
                    model: [
                        { value: "lines", label: qsTr("1 · Lines") },
                        { value: "words", label: qsTr("2 · Words") }
                    ]
                    currentValue: root.mode
                    onSelected: (value) => root.setMode(value)
                }
                Item { Layout.fillWidth: true }
            }

            Text {
                Layout.fillWidth: true
                text: root.mode === "lines"
                      ? qsTr("Play the item and press Space as each line starts. A line can be "
                             + "stamped again, or nudged, afterwards.")
                      : qsTr("Every stamped line has its words spread over it by their letters, as a "
                             + "guess: slanted and muted. Play, and press Space as each word starts; "
                             + "a pressed word turns solid.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }
            // A word that is wrong, in a file that came with its times, is put right from here.
            Text {
                objectName: "timingWordHint"
                visible: root.mode === "words"
                Layout.fillWidth: true
                text: qsTr("To put one word right, choose it: the item is cued just before it. Press "
                           + "Space as it starts, or stamp it again, or nudge it 10 ms at a time.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }

            // The transport and the pad.
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space3

                AppButton {
                    objectName: "timingPlay"
                    text: HearthController.playing ? qsTr("Pause") : qsTr("Play")
                    onClicked: HearthController.playing ? HearthController.pause() : HearthController.play()
                }
                AppButton {
                    objectName: "timingBack"
                    text: qsTr("Back 5 s")
                    onClicked: HearthController.seek(Math.max(0, Math.round(root.position) - 5000))
                }
                AppButton {
                    objectName: "timingForward"
                    text: qsTr("Forward 5 s")
                    onClicked: HearthController.seek(Math.round(root.position) + 5000)
                }
                Text {
                    objectName: "timingPosition"
                    text: root.clockText(root.position)
                    color: Theme.text
                    font.family: Theme.monoFamily
                    font.pixelSize: Theme.fontNormal
                }
                Item { Layout.fillWidth: true }
                AppButton {
                    objectName: "timingTap"
                    primary: true
                    enabled: root.canTap
                    text: root.mode === "lines" ? qsTr("Start of line · Space")
                                                : qsTr("Start of word · Space")
                    onClicked: root.tap()
                }
            }

            // What the next press is for.
            Text {
                objectName: "timingNext"
                Layout.fillWidth: true
                text: root.nextText()
                color: Theme.text
                font.pixelSize: Theme.fontHeading
                font.weight: Font.DemiBold
                wrapMode: Text.WordWrap
            }

            // The lines.
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: "transparent"
                border.color: Theme.divider
                border.width: 1

                ListView {
                    id: list
                    objectName: "timingLines"
                    anchors.fill: parent
                    anchors.margins: 1
                    clip: true
                    model: root.rows
                    ScrollBar.vertical: ScrollBar {}
                    // The model is replaced whole by every press, which puts the view back at the
                    // top: keep the line being timed in view after each one.
                    function follow() {
                        positionViewAtIndex(root.followIndex, ListView.Contain);
                    }
                    Connections {
                        target: KaraokeController
                        function onTimingChanged() { Qt.callLater(list.follow); }
                    }
                    Connections {
                        target: root
                        function onFollowIndexChanged() { Qt.callLater(list.follow); }
                    }

                    delegate: Rectangle {
                        id: row
                        required property int index
                        required property var modelData
                        readonly property bool current: row.index === root.nextLine
                                                        && root.mode === "lines"
                        readonly property bool playing: row.index === root.lineAt(root.position)
                                                        && root.mode === "words"
                        objectName: "timingLine-" + row.index
                        width: list.width
                        height: lineColumn.implicitHeight + Theme.space2 * 2
                        color: row.index === root.selectedLine ? Theme.neutral200
                               : (row.current || row.playing) ? Theme.accent100 : "transparent"
                        Accessible.role: Accessible.ListItem
                        Accessible.name: (row.modelData.stamped ? root.clockText(row.modelData.startMs) + " " : "")
                                         + row.modelData.text

                        MouseArea {
                            anchors.fill: parent
                            onClicked: root.selectLine(row.index)
                        }

                        RowLayout {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.leftMargin: Theme.space3
                            anchors.rightMargin: Theme.space3
                            spacing: Theme.space3

                            Text {
                                Layout.preferredWidth: Math.round(70 * Theme.fontScale)
                                Layout.alignment: Qt.AlignTop
                                text: row.modelData.stamped ? root.clockText(row.modelData.startMs) : "-:--.--"
                                color: row.modelData.stamped ? Theme.text : Theme.textMuted
                                font.family: Theme.monoFamily
                                font.pixelSize: Theme.fontSmall
                            }

                            // Who sings it, when the file says: the marker as it is written.
                            Text {
                                objectName: "timingLineSinger"
                                visible: row.modelData.singer.length > 0
                                Layout.preferredWidth: Math.round(46 * Theme.fontScale)
                                Layout.alignment: Qt.AlignTop
                                text: root.singerTag(row.modelData.singer)
                                color: Theme.accent
                                font.family: Theme.monoFamily
                                font.pixelSize: Theme.fontSmall
                                font.weight: Font.DemiBold
                            }

                            ColumnLayout {
                                id: lineColumn
                                Layout.fillWidth: true
                                spacing: 0
                                // A line with no words yet is its text; with words, each in its
                                // own state.
                                Text {
                                    visible: row.modelData.words.length === 0
                                    Layout.fillWidth: true
                                    text: row.modelData.text
                                    color: row.modelData.stamped ? Theme.text : Theme.textMuted
                                    font.pixelSize: Theme.fontNormal
                                    wrapMode: Text.WordWrap
                                }
                                Flow {
                                    visible: row.modelData.words.length > 0
                                    Layout.fillWidth: true
                                    spacing: 0
                                    Repeater {
                                        model: row.modelData.words
                                        // A word is a button: a click chooses it. The box that says
                                        // so is an outline as well as a fill, so that colour is not
                                        // the only cue.
                                        delegate: Item {
                                            id: wordItem
                                            required property int index
                                            required property var modelData
                                            objectName: "timingWord-" + row.index + "-" + wordItem.index
                                            readonly property bool chosen: row.index === root.selectedLine
                                                                           && wordItem.index === root.selectedWord
                                            implicitWidth: wordText.implicitWidth + Theme.space1 * 2
                                            implicitHeight: wordText.implicitHeight + 4
                                            Accessible.role: Accessible.Button
                                            Accessible.name: wordItem.modelData.text
                                            Accessible.checkable: true
                                            Accessible.checked: wordItem.chosen
                                            Accessible.onPressAction: root.selectWord(row.index, wordItem.index)

                                            Rectangle {
                                                anchors.fill: parent
                                                visible: wordItem.chosen
                                                radius: 3
                                                color: Theme.accent100
                                                border.color: Theme.accent
                                                border.width: 2
                                            }
                                            Text {
                                                id: wordText
                                                objectName: "timingWord"
                                                anchors.centerIn: parent
                                                text: wordItem.modelData.text
                                                // A guess is slanted and muted; a press is solid.
                                                color: wordItem.modelData.tapped ? Theme.text : Theme.textMuted
                                                font.italic: !wordItem.modelData.tapped
                                                font.weight: wordItem.modelData.tapped ? Font.DemiBold : Font.Normal
                                                font.pixelSize: Theme.fontNormal
                                            }
                                            MouseArea {
                                                anchors.fill: parent
                                                onClicked: root.selectWord(row.index, wordItem.index)
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Which word, and when it starts, so that a nudge of 10 ms can be told from none.
            Text {
                objectName: "timingChosenWord"
                visible: root.wordSelected
                Layout.fillWidth: true
                text: root.wordSelected
                      ? qsTr("Chosen word: %1, in line %2, starts at %3.")
                            .arg(root.rows[root.selectedLine].words[root.selectedWord].text)
                            .arg(root.selectedLine + 1)
                            .arg(root.clockText(root.rows[root.selectedLine].words[root.selectedWord].startMs))
                      : ""
                color: Theme.text
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }

            // Correcting one.
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space3
                Text {
                    Layout.fillWidth: true
                    objectName: "timingProgress"
                    text: qsTr("%1 of %2 lines have a time").arg(KaraokeController.stampedLines)
                              .arg(root.rows.length)
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                }
                // For the chosen word if there is one, otherwise for the selected line.
                AppButton {
                    objectName: "timingStampAgain"
                    enabled: root.selectedLine >= 0
                    text: qsTr("Stamp again")
                    onClicked: root.stampAgain()
                }
                AppButton {
                    objectName: "timingEarlier"
                    enabled: root.wordSelected || root.selectedStamped
                    text: qsTr("−10 ms")
                    onClicked: KaraokeController.nudge(root.selectedLine, root.nudgeWord, -10)
                }
                AppButton {
                    objectName: "timingLater"
                    enabled: root.wordSelected || root.selectedStamped
                    text: qsTr("+10 ms")
                    onClicked: KaraokeController.nudge(root.selectedLine, root.nudgeWord, 10)
                }
            }

            // A duet: who sings the selected line, and what the two are called in the file.
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space3
                Text {
                    text: qsTr("Singer")
                    color: Theme.text
                    font.pixelSize: Theme.fontSmall
                }
                SegmentedControl {
                    objectName: "timingSingerChoice"
                    enabled: root.selectedLine >= 0 && root.selectedLine < root.rows.length
                    accessibleName: qsTr("Who sings the selected line")
                    model: [
                        { value: "", label: qsTr("Not marked") },
                        { value: "v1", label: "V1" },
                        { value: "v2", label: "V2" },
                        { value: "both", label: qsTr("Both") }
                    ]
                    currentValue: (root.selectedLine >= 0 && root.selectedLine < root.rows.length)
                                  ? root.rows[root.selectedLine].singer : ""
                    onSelected: (value) => KaraokeController.setLineSinger(root.selectedLine, value)
                }
                AppTextField {
                    id: firstNameField
                    objectName: "timingFirstName"
                    Layout.fillWidth: true
                    placeholderText: qsTr("Name of V1")
                    text: KaraokeController.timingFirstName
                    Accessible.name: qsTr("Name of the first singer, saved in the file")
                    onEditingFinished: KaraokeController.timingFirstName = text
                }
                AppTextField {
                    id: secondNameField
                    objectName: "timingSecondName"
                    Layout.fillWidth: true
                    placeholderText: qsTr("Name of V2")
                    text: KaraokeController.timingSecondName
                    Accessible.name: qsTr("Name of the second singer, saved in the file")
                    onEditingFinished: KaraokeController.timingSecondName = text
                }
            }

            // The reaction time, and the end.
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space3
                Text {
                    text: qsTr("Reaction time")
                    color: Theme.text
                    font.pixelSize: Theme.fontSmall
                }
                SpinBox {
                    objectName: "timingReaction"
                    from: 0
                    to: KaraokeController.maxReactionMs
                    stepSize: 10
                    editable: false
                    value: KaraokeController.reactionMs
                    onValueModified: KaraokeController.reactionMs = value
                    Accessible.name: qsTr("Reaction time in milliseconds")
                }
                Text {
                    text: qsTr("ms taken off each press")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                }
                Item { Layout.fillWidth: true }
                AppButton {
                    objectName: "timingSave"
                    primary: true
                    enabled: KaraokeController.timingDirty
                    text: qsTr("Save")
                    onClicked: root.save()
                }
                AppButton {
                    objectName: "timingClose"
                    text: KaraokeController.timingDirty ? qsTr("Discard and close") : qsTr("Close")
                    onClicked: root.close()
                }
            }
        }

        Text {
            objectName: "timingNotice"
            Layout.fillWidth: true
            // Without a session the same words are already above, as the reason.
            visible: KaraokeController.timing && text.length > 0
            text: root.notice.length > 0 ? root.notice : KaraokeController.timingError
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.WordWrap
        }
    }
}
