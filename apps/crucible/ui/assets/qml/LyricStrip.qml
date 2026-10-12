import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

import Crucible

// Lyrics for the selected application (planning/karaoke-phase2.md, W13b, D9): import an `.lrc` and
// read the line being sung. The position is the application's media session where it publishes
// one (the player's own, so a pause and a seek are followed), and start-and-nudge where it does
// not: press Start as the song begins. CrucibleLyrics keeps which, and says so here in a sentence.
//
// Three states for a word, in three colours and a cue that is not colour: *sung* is the accent, *not
// yet sung* is muted, and the word being sung is in the text colour with the sung part of it filled
// in the accent, left to right, and underlined. A file that times lines and not words has the whole
// line emphasised.
//
// A `.cdg` file (D11) is imported the same way and followed by the same Start and nudge, and what it
// draws is the disc's picture (the shared CdgPicture) where an `.lrc` draws its words: a disc
// carries no text.
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
    Connections {
        target: CrucibleLyrics
        function onSessionsChanged() {
            root.sessionsRevision++;
        }
    }

    // CrucibleLyrics matches the application to a session by what the room knows of it.
    // The id is the object's own: this runs from onAppChanged, which can come before appId has
    // been re-evaluated from it.
    function describeApp() {
        if (root.app) {
            CrucibleLyrics.setApplication(root.app.app, root.app.name ?? "", root.app.imagePath ?? "",
                                          root.app.appId ?? "");
        }
    }
    onAppChanged: root.describeApp()
    Component.onCompleted: root.describeApp()

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
    // A file with times or a disc: what Start, Stop and the nudge are for.
    readonly property bool followable: root.status === "timed" || root.status === "picture"

    // Where the position comes from: "session" (the media session, where there is one) or "hand".
    readonly property string positionSource: root.revision >= 0 && root.appId >= 0
                                             ? CrucibleLyrics.positionSource(root.appId) : "session"
    // Where that stands, as CrucibleLyrics.sessionState() says: following, waiting, no-timeline,
    // changed, unavailable, lost, hand or none.
    readonly property string sessionState: root.revision >= 0 && root.appId >= 0
                                           ? CrucibleLyrics.sessionState(root.appId) : "none"
    readonly property string sessionLabel: root.revision >= 0 && root.appId >= 0
                                           ? CrucibleLyrics.sessionLabel(root.appId) : ""
    readonly property string sessionTrack: root.revision >= 0 && root.appId >= 0
                                           ? CrucibleLyrics.sessionTrack(root.appId) : ""
    // The sessions the system has now, for choosing one when none was matched.
    property int sessionsRevision: 0
    readonly property var sessionChoices: root.sessionsRevision >= 0 && root.revision >= 0 && root.appId >= 0
                                          ? CrucibleLyrics.sessions(root.appId) : []
    readonly property string sourceText: {
        switch (root.sessionState) {
        case "following":
            return root.sessionTrack.length > 0
                   ? qsTr("Following %1: %2").arg(root.sessionLabel).arg(root.sessionTrack)
                   : qsTr("Following %1.").arg(root.sessionLabel);
        case "waiting":
            return root.sessionChoices.length > 0
                   ? qsTr("This application has not published where it is in the song (a media session). "
                          + "Press Start to begin by hand, or follow one that is playing:")
                   : qsTr("Waiting for this application to publish where it is in the song (a media session). "
                          + "Press Start to begin by hand.");
        case "no-timeline":
            return qsTr("%1 does not publish where it is in the song. Press Start to begin by hand.")
                   .arg(root.sessionLabel);
        case "unavailable":
            return qsTr("Media sessions cannot be read here: %1 Press Start to begin by hand.")
                   .arg(CrucibleLyrics.sessionsUnavailableReason());
        case "changed":
            return qsTr("The track changed to “%1”, so these lyrics are not followed. "
                        + "Import the lyrics for it, or follow it anyway.").arg(root.sessionTrack);
        case "lost":
            return qsTr("The media session went away, so the words carry on by the clock from where they were.");
        default:
            return qsTr("Press Start as the song begins. This application's position is not read, so a "
                        + "pause or a seek in it is not followed: press Start again, or nudge the words.");
        }
    }
    // The song's position for a disc's picture: where the clock has it, and the first picture
    // before it has begun.
    readonly property real pictureMs: root.revision >= 0 && root.nowMs >= 0 && root.appId >= 0
                                      ? Math.max(0, CrucibleLyrics.positionMs(root.appId, root.nowMs)) : 0
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
        nameFilters: [qsTr("Lyrics and CD+G (*.lrc *.srt *.ass *.kar *.txt *.cdg)"), qsTr("All files (*)")]
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
                        : root.status === "picture"
                          ? qsTr("%1 · a CD+G picture: its words are pixels, so none are read").arg(root.fileName)
                          : qsTr("%1 · no times, so shown whole and not followed").arg(root.fileName))
                     : "")
            color: root.status === "error" ? Theme.text : Theme.textMuted
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            visible: root.followable
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
            CrucibleButton {
                objectName: "lyricsUseSession"
                visible: root.positionSource === "hand" || root.sessionState === "changed"
                text: qsTr("Follow the media session")
                onClicked: CrucibleLyrics.useSession(root.appId)
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

        // The picture of a CD+G disc: no text, so there are no words to follow.
        CdgPicture {
            id: disc
            objectName: "lyricPicture"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.round(width * 216 / 300)
            visible: root.status === "picture"
            source: CrucibleLyrics
            key: root.appId
            positionMs: root.pictureMs
            Accessible.role: Accessible.Graphic
            Accessible.name: qsTr("CD+G picture")
        }

        Text {
            objectName: "lyricsHint"
            Layout.fillWidth: true
            visible: root.followable
            text: root.sourceText
            color: root.sessionState === "changed" || root.sessionState === "unavailable" ? Theme.text : Theme.textMuted
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.WordWrap
        }

        // None matched this application, and some are playing: follow one of them.
        Flow {
            objectName: "lyricsSessions"
            Layout.fillWidth: true
            spacing: Theme.space2
            visible: root.followable && root.sessionState === "waiting" && root.sessionChoices.length > 0
            Repeater {
                model: root.sessionChoices
                delegate: CrucibleButton {
                    required property var modelData
                    objectName: "lyricsSession"
                    text: modelData.title.length > 0 ? modelData.name + " \u00b7 " + modelData.title : modelData.name
                    onClicked: CrucibleLyrics.useSession(root.appId, modelData.id)
                }
            }
        }
    }
}
