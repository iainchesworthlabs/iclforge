import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Hearth

// The lyrics of the item playing (planning/karaoke-design.md, K5; the design
// sheet's "Hearth: Karaoke page, reference view" and "Hearth: singer view").
//
// Three states for a word, in three colours and one cue that is not colour
// (L4): *sung* is the accent, *not yet sung* is the muted neutral, and the word
// being sung is drawn in the text colour with the sung part of it filled in the
// accent, left to right by time, and underlined. The underline is an item of
// its own, not text decoration: decoration falls outside the clipped fill and
// goes missing (the design sheet's first render lost it). The line being sung
// is bold and larger than the lines before it (sung colour, faded) and after it
// (muted); a file that times lines and not words emphasises the whole line, and
// no word times are invented for it.
//
// Nothing here owns a clock or reads a file: `positionMs` is the reading of a
// LyricClock, and the words come from KaraokeController. The item playing is
// told to the controller from here (`follow`), so that whichever view is shown
// first loads the file and the others find it loaded.
Item {
    id: root

    // The clock's reading, in milliseconds.
    property real positionMs: 0
    // Full screen: larger type, more lines around the one being sung.
    property bool large: false
    // Load the lyrics of the queue's current item on its own. A test that
    // drives KaraokeController directly turns this off.
    property bool follow: true

    property color sungColor: Theme.accent
    property color currentColor: Theme.text
    property color restColor: Theme.textMuted
    property color textColor: Theme.text

    // --- what the controller holds -----------------------------------------
    readonly property string status: KaraokeController.status
    readonly property var lines: KaraokeController.lines

    // Where the position is in the lines, with the delay taken off
    // (KaraokeController.cursor()). Set by updateCursor(), which runs when the
    // position, the file or the delay changes: cursor() is a function of all
    // three and a binding would only follow what the expression happens to read.
    property var cursor: ({ line: -1, inLine: false, word: -1, fraction: 0, positionMs: 0, fills: [] })
    function updateCursor() {
        root.cursor = KaraokeController.cursor(Math.round(root.positionMs));
    }
    onPositionMsChanged: root.updateCursor()
    Connections {
        target: KaraokeController
        function onLyricsChanged() { root.updateCursor(); }
        function onDelayChanged() { root.updateCursor(); }
    }

    readonly property int currentLine: root.cursor.line ?? -1
    readonly property bool inLine: root.cursor.inLine === true

    // The row being sung (-1 when none: before the first line, or after the
    // last), the rows before it and after it. A line with no text marks a
    // time (an instrumental gap) and is not a row of its own, unless it is the
    // one being sung.
    readonly property int currentRow: (root.status === "timed" && root.inLine
                                       && root.currentLine < root.lines.length) ? root.currentLine : -1
    readonly property int linesBefore: root.large ? 2 : 1
    readonly property int linesAfter: root.large ? 3 : 2
    readonly property var pastRows: {
        const out = [];
        if (root.status !== "timed") {
            return out;
        }
        const last = Math.min(root.inLine ? root.currentLine - 1 : root.currentLine, root.lines.length - 1);
        for (let i = last; i >= 0 && out.length < root.linesBefore; --i) {
            if (root.lines[i].text.length > 0) {
                out.unshift(i);
            }
        }
        return out;
    }
    readonly property var nextRows: {
        const out = [];
        if (root.status !== "timed") {
            return out;
        }
        for (let i = root.currentLine + 1; i < root.lines.length && out.length < root.linesAfter; ++i) {
            if (root.lines[i].text.length > 0) {
                out.push(i);
            }
        }
        return out;
    }
    readonly property bool currentWordTimed: root.currentRow >= 0 && root.lines[root.currentRow].wordTimed
    readonly property var currentWords: root.currentWordTimed ? root.lines[root.currentRow].words : []
    // For a summary elsewhere: "line 3 of 8", with 0 before the first line.
    readonly property int lineNumber: root.status === "timed" ? root.currentLine + 1 : 0
    readonly property int lineCount: root.status === "timed" ? root.lines.length : 0

    // --- sizes: the window's text size scales them -------------------------
    readonly property real unit: Math.round((root.large ? Math.max(24, Math.min(44, root.height / 14)) : 17)
                                            * Theme.fontScale)
    readonly property real currentSize: Math.round(root.unit * (root.large ? 1.6 : 1.45))

    // --- which item -------------------------------------------------------
    readonly property string itemPath: {
        const queue = HearthController.queue;
        const index = HearthController.currentIndex;
        return (index >= 0 && index < queue.length) ? (queue[index].path ?? "") : "";
    }
    readonly property real durationMs: HearthController.durationMs

    function refresh() {
        if (root.follow) {
            KaraokeController.load(root.itemPath, root.durationMs);
        }
    }
    // A change of item changes the path and then the length: one load, after both.
    onItemPathChanged: Qt.callLater(root.refresh)
    onDurationMsChanged: Qt.callLater(root.refresh)
    Component.onCompleted: {
        root.updateCursor();
        root.refresh();
    }

    clip: true

    // A word's gap is the width of a space at this size.
    TextMetrics {
        id: spaceMetrics
        text: " "
        font.pixelSize: root.currentSize
        font.bold: true
    }

    // --- timed lyrics -----------------------------------------------------
    Item {
        id: stage
        objectName: "lyricStage"
        anchors.fill: parent
        visible: root.status === "timed"

        Column {
            id: pastColumn
            anchors.bottom: currentBox.top
            anchors.bottomMargin: root.unit * 0.6
            width: parent.width
            spacing: root.unit * 0.5
            Repeater {
                model: root.pastRows
                delegate: Text {
                    required property int modelData
                    objectName: "lyricPast"
                    width: pastColumn.width
                    text: root.lines[modelData].text
                    color: root.sungColor
                    opacity: 0.55
                    font.pixelSize: root.unit
                    wrapMode: Text.WordWrap
                }
            }
        }

        Item {
            id: currentBox
            objectName: "lyricCurrent"
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            height: root.currentRow < 0 ? 0
                    : (root.currentWordTimed ? wordFlow.implicitHeight : wholeLine.implicitHeight)

            // A line with times and no word times: the whole line, emphasised.
            Text {
                id: wholeLine
                objectName: "lyricCurrentLine"
                visible: root.currentRow >= 0 && !root.currentWordTimed
                width: parent.width
                text: root.currentRow >= 0
                      ? (root.lines[root.currentRow].text.length > 0 ? root.lines[root.currentRow].text
                                                                      : "♪")
                      : ""
                color: root.sungColor
                font.pixelSize: root.currentSize
                font.bold: true
                wrapMode: Text.WordWrap
            }

            Flow {
                id: wordFlow
                objectName: "lyricWords"
                visible: root.currentWordTimed
                width: parent.width
                spacing: 0

                Repeater {
                    model: root.currentWords
                    delegate: Item {
                        id: word
                        required property int index
                        required property var modelData
                        readonly property real fill: (root.cursor.fills && word.index < root.cursor.fills.length)
                                                     ? root.cursor.fills[word.index] : 0
                        readonly property bool being: word.index === root.cursor.word
                        objectName: "lyricWord-" + word.index
                        implicitWidth: label.implicitWidth + (word.modelData.gap ? spaceMetrics.advanceWidth : 0)
                        implicitHeight: label.implicitHeight

                        Text {
                            id: label
                            text: word.modelData.text
                            color: word.being ? root.currentColor : root.restColor
                            font.pixelSize: root.currentSize
                            font.bold: true
                        }
                        // The sung part: the same word in the sung colour,
                        // cut off at the fraction of its width.
                        Item {
                            objectName: "lyricWordFill"
                            width: label.implicitWidth * word.fill
                            height: word.height
                            clip: true
                            Text {
                                text: word.modelData.text
                                color: root.sungColor
                                font.pixelSize: root.currentSize
                                font.bold: true
                            }
                        }
                        Rectangle {
                            objectName: "lyricWordUnderline"
                            visible: word.being
                            x: 0
                            y: label.implicitHeight - Math.max(2, Math.round(root.currentSize / 12))
                            width: label.implicitWidth
                            height: Math.max(2, Math.round(root.currentSize / 12))
                            color: root.sungColor
                        }
                    }
                }
            }
        }

        Column {
            id: nextColumn
            anchors.top: currentBox.bottom
            anchors.topMargin: root.unit * 0.6
            width: parent.width
            spacing: root.unit * 0.5
            Repeater {
                model: root.nextRows
                delegate: Text {
                    required property int modelData
                    objectName: "lyricNext"
                    width: nextColumn.width
                    text: root.lines[modelData].text
                    color: root.restColor
                    font.pixelSize: root.unit
                    wrapMode: Text.WordWrap
                }
            }
        }
    }

    // --- a file with no times: shown whole, not followed --------------------
    Flickable {
        id: plain
        objectName: "lyricPlain"
        anchors.fill: parent
        visible: root.status === "plain"
        contentWidth: width
        contentHeight: plainText.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { }

        Text {
            id: plainText
            width: plain.width - 12
            text: root.lines.map(function(line) { return line.text; }).join("\n")
            color: root.textColor
            font.pixelSize: root.unit
            wrapMode: Text.WordWrap
        }
    }

    // --- no file, or one that could not be read ------------------------------
    Text {
        id: message
        objectName: "lyricMessage"
        anchors.centerIn: parent
        width: parent.width - root.unit * 2
        visible: root.status === "none" || root.status === "error"
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        color: root.restColor
        font.pixelSize: root.unit
        text: {
            if (root.status === "error") {
                return qsTr("%1 Playback is not affected.").arg(KaraokeController.errorText);
            }
            // Nothing loaded: no item, so no name it would look for.
            if (KaraokeController.expectedName.length === 0) {
                return qsTr("Nothing is playing.");
            }
            return qsTr("No lyrics. Hearth looks for %1 next to the stream.")
                .arg(KaraokeController.expectedName);
        }
    }
}
