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
// A `.cdg` beside the item with no lyrics file is the picture of a karaoke disc
// (planning/karaoke-phase2.md, W13f): it has no text to follow, so it is shown as
// the picture it is, against the same position and the same delay.
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

    // --- a duet (planning/karaoke-phase2.md, W11b) --------------------------
    // A line that has a singer says it in words, and sits on a side: the first vocal's lines
    // at the left, the second's indented, both together across. Colour is the third cue, never
    // the only one (L4). The guide is the vocal that is playing: where the stream is karaoke,
    // a line is the guide's when its singer's vocal plays, and yours when it does not.
    readonly property bool duet: KaraokeController.hasSingers
    readonly property bool karaokeStream: {
        const programmes = HearthController.currentMedia.programmes;
        return programmes !== undefined && programmes.length > 0 && programmes[0].karaoke === true;
    }
    readonly property string reproduction: HearthController.decoderSettings.karaoke ?? "aware"
    readonly property string vocals: HearthController.decoderSettings.karaokeVocals ?? "both"

    function singerOf(index) {
        return (root.duet && index >= 0 && index < root.lines.length) ? (root.lines[index].singer ?? "") : "";
    }
    function singerName(singer) {
        switch (singer) {
        case "v1": return KaraokeController.firstSingerLabel;
        case "v2": return KaraokeController.secondSingerLabel;
        case "both": return qsTr("%1 + %2").arg(KaraokeController.firstSingerLabel)
                                          .arg(KaraokeController.secondSingerLabel);
        }
        return "";
    }
    // "guide" (the vocal that is playing sings it), "you" (it does not play, so you do), "shared"
    // (one of two that sing it plays), or "" where there is no guide to speak of.
    function cueOf(singer) {
        if (singer === "" || !root.karaokeStream) {
            return "";
        }
        if (root.reproduction !== "capable") {
            return "guide";  // as coded and aware both play every vocal the stream has
        }
        const first = root.vocals === "v1" || root.vocals === "both";
        const second = root.vocals === "v2" || root.vocals === "both";
        if (singer === "v1") {
            return first ? "guide" : "you";
        }
        if (singer === "v2") {
            return second ? "guide" : "you";
        }
        return (first && second) ? "guide" : ((!first && !second) ? "you" : "shared");
    }
    function cueText(cue) {
        switch (cue) {
        case "guide": return qsTr("guide");
        case "you": return qsTr("you");
        case "shared": return qsTr("shared");
        }
        return "";
    }
    // The tag of a row: the name, and whose line it is for the singer at the screen.
    function tagOf(index) {
        const singer = root.singerOf(index);
        if (singer === "") {
            return "";
        }
        const cue = root.cueText(root.cueOf(singer));
        return cue.length > 0 ? qsTr("%1 · %2").arg(root.singerName(singer)).arg(cue)
                              : root.singerName(singer);
    }
    function indentOf(index) {
        return root.singerOf(index) === "v2" ? Math.round(root.width * 0.18) : 0;
    }
    function tagColor(index) {
        return root.singerOf(index) === "v2" ? root.currentColor : root.sungColor;
    }

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
                delegate: Column {
                    id: pastRow
                    required property int modelData
                    width: pastColumn.width
                    spacing: 0
                    Text {
                        objectName: "lyricPastSinger"
                        visible: text.length > 0
                        x: root.indentOf(pastRow.modelData)
                        text: root.tagOf(pastRow.modelData)
                        color: root.tagColor(pastRow.modelData)
                        opacity: 0.55
                        font.pixelSize: Math.round(root.unit * 0.7)
                        font.bold: true
                    }
                    Text {
                        objectName: "lyricPast"
                        x: root.indentOf(pastRow.modelData)
                        width: pastColumn.width - x
                        text: root.lines[pastRow.modelData].text
                        color: root.sungColor
                        opacity: 0.55
                        font.pixelSize: root.unit
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }

        Item {
            id: currentBox
            objectName: "lyricCurrent"
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            // The line's own tag (a duet), above it and on its side.
            readonly property real tagHeight: currentTag.visible ? currentTag.implicitHeight : 0
            readonly property real indent: root.indentOf(root.currentRow)
            height: root.currentRow < 0 ? 0
                    : tagHeight + (root.currentWordTimed ? wordFlow.implicitHeight : wholeLine.implicitHeight)

            Text {
                id: currentTag
                objectName: "lyricCurrentSinger"
                visible: root.currentRow >= 0 && text.length > 0
                x: currentBox.indent
                text: root.currentRow >= 0 ? root.tagOf(root.currentRow) : ""
                color: root.tagColor(root.currentRow)
                font.pixelSize: Math.round(root.unit * 0.75)
                font.bold: true
            }

            // A line with times and no word times: the whole line, emphasised.
            Text {
                id: wholeLine
                objectName: "lyricCurrentLine"
                visible: root.currentRow >= 0 && !root.currentWordTimed
                x: currentBox.indent
                y: currentBox.tagHeight
                width: parent.width - currentBox.indent
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
                x: currentBox.indent
                y: currentBox.tagHeight
                width: parent.width - currentBox.indent
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
                delegate: Column {
                    id: nextRow
                    required property int modelData
                    width: nextColumn.width
                    spacing: 0
                    Text {
                        objectName: "lyricNextSinger"
                        visible: text.length > 0
                        x: root.indentOf(nextRow.modelData)
                        text: root.tagOf(nextRow.modelData)
                        color: root.tagColor(nextRow.modelData)
                        font.pixelSize: Math.round(root.unit * 0.7)
                        font.bold: true
                    }
                    Text {
                        objectName: "lyricNext"
                        x: root.indentOf(nextRow.modelData)
                        width: nextColumn.width - x
                        text: root.lines[nextRow.modelData].text
                        color: root.restColor
                        font.pixelSize: root.unit
                        wrapMode: Text.WordWrap
                    }
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
            objectName: "lyricPlainText"
            width: plain.width - 12
            // A duet's marked lines say who sings them, as the marker did.
            text: root.lines.map(function(line) {
                const name = root.duet ? root.singerName(line.singer ?? "") : "";
                return name.length > 0 ? name + ": " + line.text : line.text;
            }).join("\n")
            color: root.textColor
            font.pixelSize: root.unit
            wrapMode: Text.WordWrap
        }
    }

    // --- the picture of a CD+G disc: no text, so there are no words to follow --
    CdgPicture {
        id: picture
        objectName: "lyricPicture"
        anchors.fill: parent
        visible: root.status === "picture"
        controller: KaraokeController
        positionMs: root.positionMs
        Accessible.role: Accessible.Graphic
        Accessible.name: qsTr("CD+G picture")
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
