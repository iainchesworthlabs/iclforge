import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

import Crucible

// Lyrics for the selected application (planning/karaoke-phase2.md, W13b, D9): import an `.lrc`,
// press Start as the song begins, and read the line being sung. The position is the one D9 names
// for an application that publishes none: start-and-nudge (CrucibleLyrics, which says why).
//
// Three states for a word, in three colours and a cue that is not colour: *sung* is the accent, *not
// yet sung* is muted, and the word being sung is in the text colour with the sung part of it filled
// in the accent, left to right, and underlined. A file that times lines and not words has the whole
// line emphasised.
//
// Nothing here keeps lyrics: CrucibleLyrics does, per application, and this asks it with the time.
ColumnLayout {
    id: root
    objectName: "lyricStrip"

    // The selected application (an AppEntry), or null.
    property var app: null
    readonly property int appId: root.app ? root.app.app : -1
    // Whatever the nudge buttons move by, in milliseconds.
    readonly property int nudgeStep: 100

    spacing: Theme.space3

    // CrucibleLyrics answers questions of (application, time); this makes the bindings below ask
    // again when it says the application's lyrics, clock or nudge changed, and the clock does the
    // same on a timer while the strip is following.
    property int revision: 0
    property real nowMs: CrucibleLyrics.nowMs()
    Connections {
        target: CrucibleLyrics
        function onChanged(app) {
            if (app === root.appId) {
                root.revision++;
            }
        }
    }
    Timer {
        interval: 33
        repeat: true
        running: root.following
        onTriggered: root.nowMs = CrucibleLyrics.nowMs()
    }

    // Each reads `revision` in a condition that is always true, which is what makes the binding
    // depend on it: a bare `root.revision;` statement is dropped by the compiled QML and the
    // binding then never asks again.
    readonly property string status: root.revision >= 0 && root.appId >= 0
                                     ? CrucibleLyrics.status(root.appId) : "none"
    readonly property string fileName: root.revision >= 0 && root.appId >= 0
                                       ? CrucibleLyrics.fileName(root.appId) : ""
    readonly property string errorText: root.revision >= 0 && root.appId >= 0
                                        ? CrucibleLyrics.errorText(root.appId) : ""
    readonly property var lines: root.revision >= 0 && root.appId >= 0
                                 ? CrucibleLyrics.lines(root.appId) : []
    readonly property bool following: root.revision >= 0 && root.appId >= 0
                                      && CrucibleLyrics.running(root.appId)
    readonly property int nudge: root.revision >= 0 && root.appId >= 0
                                 ? CrucibleLyrics.nudgeMs(root.appId) : 0
    // Where the song is: the cursor's line, word and fills at the clock's reading.
    readonly property var cursor: root.revision >= 0 && root.nowMs >= 0 && root.appId >= 0
                                  ? CrucibleLyrics.cursor(root.appId, root.nowMs)
                                  : ({ line: -1, inLine: false, word: -1, fraction: 0, positionMs: -1, fills: [] })
    readonly property int currentLine: root.cursor.line ?? -1
    readonly property bool inLine: root.cursor.inLine === true
    readonly property int currentRow: (root.following && root.inLine && root.currentLine < root.lines.length)
                                      ? root.currentLine : -1

    // The rows after the line being sung that have words: a line that only marks a time is not a row.
    readonly property var nextRows: {
        const out = [];
        for (let i = root.currentLine + 1; i < root.lines.length && out.length < 2; ++i) {
            if (root.lines[i].text.length > 0) {
                out.push(i);
            }
        }
        return out;
    }
    readonly property int previousRow: {
        const last = Math.min(root.inLine ? root.currentLine - 1 : root.currentLine, root.lines.length - 1);
        for (let i = last; i >= 0; --i) {
            if (root.lines[i].text.length > 0) {
                return i;
            }
        }
        return -1;
    }

    function nudgeText(ms) {
        const seconds = (ms / 1000).toFixed(1);
        return (ms > 0 ? "+" : "") + seconds + " s";
    }

    FileDialog {
        id: lyricsDialog
        objectName: "lyricsDialog"
        title: qsTr("Choose the lyrics")
        nameFilters: [qsTr("Lyrics (*.lrc *.txt)"), qsTr("All files (*)")]
        onAccepted: CrucibleLyrics.importFile(root.appId, selectedFile.toString())
    }

    Text {
        objectName: "lyricsNoApp"
        Layout.fillWidth: true
        visible: root.appId < 0
        text: qsTr("Select an application to give it lyrics.")
        color: Theme.textMuted
        font.pixelSize: Theme.fontSmall
        wrapMode: Text.WordWrap
    }

    ColumnLayout {
        Layout.fillWidth: true
        visible: root.appId >= 0
        spacing: Theme.space3

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space2
            CrucibleButton {
                objectName: "lyricsImport"
                text: qsTr("Import lyrics…")
                onClicked: lyricsDialog.open()
            }
            CrucibleButton {
                objectName: "lyricsClear"
                text: qsTr("Clear")
                enabled: root.status !== "none"
                onClicked: CrucibleLyrics.clear(root.appId)
            }
        }

        Text {
            objectName: "lyricsFile"
            Layout.fillWidth: true
            visible: text.length > 0
            text: root.status === "error" ? root.errorText
                  : (root.fileName.length > 0
                     ? (root.status === "timed" ? qsTr("%1 · %2 lines").arg(root.fileName).arg(root.lines.length)
                                                : qsTr("%1 · no times, so shown whole and not followed").arg(root.fileName))
                     : "")
            color: root.status === "error" ? Theme.text : Theme.textMuted
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            visible: root.status === "timed"
            spacing: Theme.space2
            CrucibleButton {
                objectName: "lyricsStart"
                text: root.following ? qsTr("Start again") : qsTr("Start")
                onClicked: CrucibleLyrics.start(root.appId)
            }
            CrucibleButton {
                objectName: "lyricsStop"
                text: qsTr("Stop")
                enabled: root.following
                onClicked: CrucibleLyrics.stop(root.appId)
            }
            Item { Layout.fillWidth: true }
            CrucibleButton {
                objectName: "lyricsEarlier"
                text: "−" + (root.nudgeStep / 1000).toFixed(1) + " s"
                Accessible.name: qsTr("Show the words earlier")
                onClicked: CrucibleLyrics.nudge(root.appId, -root.nudgeStep)
            }
            Text {
                objectName: "lyricsNudge"
                text: root.nudgeText(root.nudge)
                color: Theme.textMuted
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fontMono
                Accessible.name: qsTr("Words shifted by %1").arg(root.nudgeText(root.nudge))
            }
            CrucibleButton {
                objectName: "lyricsLater"
                text: "+" + (root.nudgeStep / 1000).toFixed(1) + " s"
                Accessible.name: qsTr("Show the words later")
                onClicked: CrucibleLyrics.nudge(root.appId, root.nudgeStep)
            }
        }

        // The strip: the line before, the line being sung, and the two after it.
        Item {
            id: stage
            objectName: "lyricStage"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.round(132 * Theme.fontScale)
            visible: root.status === "timed" && root.following
            clip: true

            Column {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                spacing: Theme.space2

                Text {
                    objectName: "lyricPrevious"
                    width: parent.width
                    visible: root.previousRow >= 0
                    text: root.previousRow >= 0 ? root.lines[root.previousRow].text : ""
                    color: Theme.accent
                    opacity: 0.55
                    font.pixelSize: Theme.fontBody
                    elide: Text.ElideRight
                }

                Item {
                    id: currentBox
                    objectName: "lyricCurrent"
                    width: parent.width
                    height: root.currentRow < 0 ? Math.round(Theme.fontBody * 1.8)
                            : Math.max(wholeLine.implicitHeight, wordFlow.implicitHeight)
                    readonly property bool wordTimed: root.currentRow >= 0 && root.lines[root.currentRow].wordTimed
                    readonly property var words: wordTimed ? root.lines[root.currentRow].words : []

                    Text {
                        id: wholeLine
                        objectName: "lyricCurrentLine"
                        visible: root.currentRow >= 0 && !currentBox.wordTimed
                        width: parent.width
                        text: root.currentRow >= 0 ? (root.lines[root.currentRow].text.length > 0
                                                      ? root.lines[root.currentRow].text : "♪") : ""
                        color: Theme.accent
                        font.pixelSize: Math.round(Theme.fontBody * 1.25)
                        font.bold: true
                        wrapMode: Text.WordWrap
                    }

                    Flow {
                        id: wordFlow
                        objectName: "lyricWords"
                        visible: currentBox.wordTimed
                        width: parent.width
                        spacing: 0
                        Repeater {
                            model: currentBox.words
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
                                    color: word.being ? Theme.text : Theme.textMuted
                                    font.pixelSize: Math.round(Theme.fontBody * 1.25)
                                    font.bold: true
                                }
                                // The sung part, cut at the fraction of its width.
                                Item {
                                    objectName: "lyricWordFill"
                                    width: label.implicitWidth * word.fill
                                    height: word.height
                                    clip: true
                                    Text {
                                        text: word.modelData.text
                                        color: Theme.accent
                                        font.pixelSize: Math.round(Theme.fontBody * 1.25)
                                        font.bold: true
                                    }
                                }
                                Rectangle {
                                    visible: word.being
                                    y: label.implicitHeight - 2
                                    width: label.implicitWidth
                                    height: 2
                                    color: Theme.accent
                                }
                            }
                        }
                    }

                    TextMetrics {
                        id: spaceMetrics
                        text: " "
                        font.pixelSize: Math.round(Theme.fontBody * 1.25)
                        font.bold: true
                    }
                }

                Repeater {
                    model: root.nextRows
                    delegate: Text {
                        required property int modelData
                        objectName: "lyricNext"
                        width: parent.width
                        text: root.lines[modelData].text
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontBody
                        elide: Text.ElideRight
                    }
                }
            }
        }

        Text {
            objectName: "lyricsHint"
            Layout.fillWidth: true
            visible: root.status === "timed"
            text: qsTr("Press Start as the song begins. This application's position is not read, so a "
                       + "pause or a seek in it is not followed: press Start again, or nudge the words.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.WordWrap
        }
    }
}
