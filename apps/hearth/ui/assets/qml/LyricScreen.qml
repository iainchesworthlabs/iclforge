import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Hearth

// The singer view (planning/karaoke-design.md, K5; the design sheet's "Hearth:
// singer view"): the lyric view made full screen, dark, for a room rather than
// a desk. F11 opens it (Main.qml puts the window full screen with it) and Escape
// leaves; the position bar, the transport, the queue and the vocals chooser stay at
// the bottom so a singer can move about in the song, pause, skip, pick the next song
// or pick the vocals without leaving it. Space pauses, Left and Right move five
// seconds, and Q shows the queue beside the words.
//
// Its colours are its own and the same in a light or a dark theme: it is shown
// to a room, and a light page that fills a screen is glare.
Popup {
    id: root
    objectName: "lyricScreen"

    // What is playing and how it is reproduced.
    readonly property var media: HearthController.currentMedia
    readonly property var programme: (root.media.programmes && root.media.programmes.length > 0)
                                     ? root.media.programmes[0] : ({})
    readonly property bool isKaraoke: root.programme.karaoke === true
    readonly property string reproduction: HearthController.decoderSettings.karaoke ?? "aware"
    readonly property string vocals: HearthController.decoderSettings.karaokeVocals ?? "both"

    // The queue, shown beside the words when asked for (the bar's Queue button, or Q): a room
    // that sings a list of songs wants to see what is next and to pick one without leaving.
    property bool queueOpen: false
    // How far the arrow keys move the item, as the timing window's Back and Forward do.
    readonly property int seekStepMs: 5000
    // Where the bar is being held, while it is: the title and the words show that place, and
    // the item is moved to it when the bar is let go (a drag is a preview, not a stream of
    // seeks into the decoder).
    property bool scrubbing: false
    property real scrubMs: 0
    readonly property real shownMs: root.scrubbing ? root.scrubMs : lyricClock.positionMs

    readonly property color paper: "#151313"
    readonly property color ink: "#ece7e5"
    readonly property color quiet: "#8b8583"
    readonly property color accent: "#ff5c40"
    readonly property color rule: "#353130"
    readonly property color bar: "#1d1a1a"

    parent: Overlay.overlay
    x: 0
    y: 0
    width: parent ? parent.width : 0
    height: parent ? parent.height : 0
    padding: 0
    margins: 0
    modal: true
    focus: true
    // Escape leaves, as a Popup does while it has the active focus, which it
    // takes as it opens (ShortcutsDialog.qml says the same). Main.qml's own
    // Escape shortcut closes it too, where the window's shortcuts still reach
    // a modal popup: it has no Escape shortcut here, so nothing is ambiguous.
    closePolicy: Popup.CloseOnEscape
    onOpened: root.contentItem.forceActiveFocus()

    function fileNameOf(path) {
        if (!path) {
            return "";
        }
        const cut = Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\"));
        const name = cut >= 0 ? path.substring(cut + 1) : path;
        const dot = name.lastIndexOf(".");
        return dot > 0 ? name.substring(0, dot) : name;
    }

    function clock(ms) {
        const seconds = Math.max(0, Math.floor(ms / 1000));
        return Math.floor(seconds / 60) + ":" + ("0" + (seconds % 60)).slice(-2);
    }

    // Vocals pick, as the singer view has it: choosing one is choosing the
    // capable reproduction with it, which is the only one that takes a choice.
    function chooseVocals(value) {
        HearthController.setDecoderSettings({ karaoke: "capable", karaokeVocals: value });
    }

    // Moves the item by `deltaMs` from where it is heard, inside the item. Legal in any
    // transport state and starts nothing (HearthController.seek).
    function seekBy(deltaMs) {
        const duration = HearthController.durationMs;
        if (duration <= 0) {
            return;
        }
        HearthController.seek(Math.max(0, Math.min(duration, HearthController.positionMs + deltaMs)));
    }

    // The title of the next item that can play, "" when there is none.
    function nextTitle() {
        const queue = HearthController.queue;
        const current = HearthController.currentIndex;
        if (current < 0) {
            return "";
        }
        for (let i = current + 1; i < queue.length; ++i) {
            if (queue[i].playable) {
                return queue[i].title;
            }
        }
        return "";
    }

    background: Rectangle { color: root.paper }

    LyricClock { id: lyricClock; active: root.visible }

    Shortcut {
        sequence: "Space"
        enabled: root.visible
        onActivated: HearthController.playing ? HearthController.pause() : HearthController.play()
    }
    Shortcut {
        sequence: "Left"
        enabled: root.visible
        onActivated: root.seekBy(-root.seekStepMs)
    }
    Shortcut {
        sequence: "Right"
        enabled: root.visible
        onActivated: root.seekBy(root.seekStepMs)
    }
    Shortcut {
        sequence: "Q"
        enabled: root.visible
        onActivated: root.queueOpen = !root.queueOpen
    }

    // A dark button, for the bar: the family's controls draw from Theme, which
    // is light in a light theme.
    component BarButton: Rectangle {
        id: button
        property string text: ""
        property bool primary: false
        property bool selected: false
        signal clicked()

        implicitHeight: 44
        implicitWidth: label.implicitWidth + 32
        color: (button.primary || button.selected) ? root.accent : "transparent"
        border.color: (button.primary || button.selected) ? root.accent : root.quiet
        border.width: 1
        opacity: enabled ? 1.0 : 0.45
        Accessible.role: Accessible.Button
        Accessible.name: button.text
        Accessible.onPressAction: if (button.enabled) button.clicked()

        Text {
            id: label
            anchors.centerIn: parent
            text: button.text
            color: (button.primary || button.selected) ? root.paper : root.ink
            font.pixelSize: Math.round(15 * Theme.fontScale)
            font.bold: button.primary || button.selected
        }
        MouseArea {
            anchors.fill: parent
            enabled: button.enabled
            cursorShape: Qt.PointingHandCursor
            onClicked: button.clicked()
        }
    }

    contentItem: ColumnLayout {
        spacing: 0

        // What is playing, quietly.
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 56
            Layout.leftMargin: 40
            Layout.rightMargin: 40
            Text {
                objectName: "lyricScreenTitle"
                Layout.fillWidth: true
                text: root.fileNameOf(root.media.path ?? "") + "  ·  "
                      + root.clock(root.shownMs) + " / " + root.clock(HearthController.durationMs)
                color: root.quiet
                font.pixelSize: Math.round(12 * Theme.fontScale)
                font.capitalization: Font.AllUppercase
                font.letterSpacing: 0.7
                elide: Text.ElideRight
            }
            // What plays after this one, so that a room knows whose turn is next.
            Text {
                objectName: "lyricScreenNext"
                readonly property string title: root.nextTitle()
                visible: title.length > 0
                Layout.maximumWidth: Math.round(parent.width * 0.4)
                text: qsTr("Next: %1").arg(title)
                color: root.quiet
                font.pixelSize: Math.round(12 * Theme.fontScale)
                font.capitalization: Font.AllUppercase
                font.letterSpacing: 0.7
                elide: Text.ElideRight
            }
            Text {
                text: qsTr("Esc leaves full screen")
                color: root.quiet
                font.pixelSize: Math.round(12 * Theme.fontScale)
                font.capitalization: Font.AllUppercase
                font.letterSpacing: 0.7
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            LyricView {
                id: lyrics
                objectName: "lyricScreenView"
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.leftMargin: 96
                Layout.rightMargin: root.queueOpen ? 40 : 96
                large: true
                positionMs: root.shownMs
                sungColor: root.accent
                currentColor: root.ink
                restColor: root.quiet
                textColor: root.ink
            }

            // The queue, as a list a room can read from across it: what is playing is in the
            // accent, an item that cannot play says why, and a press plays that item. A plain
            // Rectangle delegate with a MouseArea, as the Play page's list has it.
            Rectangle {
                id: queuePanel
                objectName: "lyricScreenQueue"
                visible: root.queueOpen
                Layout.preferredWidth: Math.min(420, Math.round(root.width * 0.34))
                Layout.fillHeight: true
                color: root.bar

                Rectangle {
                    width: 1
                    height: parent.height
                    color: root.rule
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 24
                    anchors.rightMargin: 24
                    anchors.topMargin: 12
                    anchors.bottomMargin: 12
                    spacing: 8

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Queue")
                        color: root.quiet
                        font.pixelSize: Math.round(12 * Theme.fontScale)
                        font.capitalization: Font.AllUppercase
                        font.letterSpacing: 0.7
                    }

                    ListView {
                        id: queueList
                        objectName: "lyricScreenQueueList"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: HearthController.queue
                        spacing: 2

                        // Brings the item that is playing into view when it changes and when the
                        // list is opened, so that the list is where the room is. One turn later,
                        // since the model is rebuilt from the same signal, and again when the list
                        // gets its height or its rows if it had neither yet (a panel that has just
                        // become visible is laid out on the next frame).
                        property bool showWhenSized: false
                        function showCurrent() {
                            if (queueList.height > 0 && queueList.count > 0
                                    && HearthController.currentIndex >= 0) {
                                queueList.positionViewAtIndex(HearthController.currentIndex,
                                                              ListView.Contain);
                                queueList.showWhenSized = false;
                            } else {
                                queueList.showWhenSized = true;
                            }
                        }
                        onHeightChanged: if (showWhenSized) showCurrent()
                        onCountChanged: if (showWhenSized) showCurrent()
                        Connections {
                            target: HearthController
                            function onQueueChanged() { Qt.callLater(queueList.showCurrent); }
                        }
                        Connections {
                            target: root
                            function onQueueOpenChanged() {
                                if (root.queueOpen) {
                                    Qt.callLater(queueList.showCurrent);
                                }
                            }
                        }

                        delegate: Rectangle {
                            id: row
                            required property var modelData
                            required property int index
                            objectName: "lyricScreenQueueRow-" + row.index
                            width: queueList.width
                            height: Math.max(52, rowText.implicitHeight + 16)
                            color: row.modelData.current ? root.rule : "transparent"
                            opacity: row.modelData.playable ? 1.0 : 0.5

                            Accessible.role: Accessible.ListItem
                            Accessible.name: row.modelData.title
                            Accessible.selected: row.modelData.current

                            Rectangle {
                                visible: row.modelData.current
                                width: 4
                                height: parent.height
                                color: root.accent
                            }
                            Text {
                                id: rowText
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.leftMargin: 16
                                anchors.rightMargin: 8
                                anchors.verticalCenter: parent.verticalCenter
                                text: row.modelData.title
                                color: row.modelData.current ? root.accent : root.ink
                                font.pixelSize: Math.round(18 * Theme.fontScale)
                                font.bold: row.modelData.current
                                wrapMode: Text.WordWrap
                                maximumLineCount: 2
                                elide: Text.ElideRight
                            }
                            MouseArea {
                                anchors.fill: parent
                                enabled: row.modelData.playable
                                cursorShape: Qt.PointingHandCursor
                                onClicked: HearthController.playItem(row.index)
                            }
                        }
                    }
                }
            }
        }

        // The controls, always reachable and out of the way.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 136
            color: root.bar

            Rectangle {
                width: parent.width
                height: 1
                color: root.rule
            }

            // Where the item has got to, and a way to move it: press anywhere on the bar and
            // drag, or use the arrow keys (five seconds each). While the bar is held the words
            // and the clock show the place it is held at, and the item moves when it is let
            // go. A plain item with a MouseArea, not a QQC2 Slider: a Slider's own drag writes
            // `value` and breaks the binding that follows the clock. Its own row above the
            // buttons, so that it is as wide as the screen is whatever the buttons add up to.
            Item {
                id: scrubber
                objectName: "lyricScreenSeek"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.leftMargin: 40
                anchors.rightMargin: 40
                anchors.topMargin: 8
                height: 40
                enabled: HearthController.durationMs > 0

                readonly property real fraction: HearthController.durationMs > 0
                    ? Math.max(0, Math.min(1, root.shownMs / HearthController.durationMs)) : 0

                function msAt(x) {
                    return Math.round(Math.max(0, Math.min(1, x / Math.max(1, scrubber.width)))
                                      * HearthController.durationMs);
                }

                Accessible.role: Accessible.Slider
                Accessible.name: qsTr("Position")
                Accessible.description: qsTr("%1 of %2").arg(root.clock(root.shownMs))
                                                       .arg(root.clock(HearthController.durationMs))
                Accessible.onIncreaseAction: root.seekBy(root.seekStepMs)
                Accessible.onDecreaseAction: root.seekBy(-root.seekStepMs)

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width
                    height: 4
                    color: root.rule
                    Rectangle {
                        objectName: "lyricScreenProgress"
                        width: parent.width * scrubber.fraction
                        height: parent.height
                        color: root.accent
                    }
                }
                // The place the bar is held at.
                Rectangle {
                    visible: root.scrubbing
                    x: scrubber.width * scrubber.fraction - width / 2
                    anchors.verticalCenter: parent.verticalCenter
                    width: 14
                    height: 14
                    radius: 7
                    color: root.accent
                }
                MouseArea {
                    objectName: "lyricScreenSeekArea"
                    anchors.fill: parent
                    enabled: scrubber.enabled
                    cursorShape: Qt.PointingHandCursor
                    onPressed: function(mouse) {
                        root.scrubMs = scrubber.msAt(mouse.x);
                        root.scrubbing = true;
                    }
                    onPositionChanged: function(mouse) {
                        if (pressed) {
                            root.scrubMs = scrubber.msAt(mouse.x);
                        }
                    }
                    onReleased: function(mouse) {
                        root.scrubMs = scrubber.msAt(mouse.x);
                        root.scrubbing = false;
                        HearthController.seek(root.scrubMs);
                        // The clock reads the place the item was moved to at once, not the
                        // one it was at until the next poll.
                        lyricClock.poll(root.scrubMs);
                    }
                    onCanceled: root.scrubbing = false
                }
            }

            RowLayout {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: scrubber.bottom
                anchors.bottom: parent.bottom
                anchors.leftMargin: 40
                anchors.rightMargin: 40
                spacing: 16

                BarButton {
                    objectName: "lyricScreenPrevious"
                    text: qsTr("Previous")
                    enabled: HearthController.currentIndex > 0
                    onClicked: HearthController.previous()
                }
                BarButton {
                    objectName: "lyricScreenPlayPause"
                    primary: true
                    text: HearthController.playing ? qsTr("Pause") : qsTr("Play")
                    enabled: HearthController.queue.length > 0
                    onClicked: HearthController.playing ? HearthController.pause() : HearthController.play()
                }
                BarButton {
                    objectName: "lyricScreenNext"
                    text: qsTr("Next")
                    enabled: HearthController.currentIndex >= 0
                             && HearthController.currentIndex + 1 < HearthController.queue.length
                    onClicked: HearthController.next()
                }

                BarButton {
                    objectName: "lyricScreenQueueButton"
                    text: qsTr("Queue")
                    selected: root.queueOpen
                    onClicked: root.queueOpen = !root.queueOpen
                }

                Item { Layout.fillWidth: true }

                Text {
                    visible: root.isKaraoke
                    text: qsTr("Vocals")
                    color: root.quiet
                    font.pixelSize: Math.round(12 * Theme.fontScale)
                    font.capitalization: Font.AllUppercase
                    font.letterSpacing: 0.7
                }
                Row {
                    id: vocalsChooser
                    objectName: "lyricScreenVocals"
                    visible: root.isKaraoke
                    spacing: 0
                    Repeater {
                        model: [
                            { value: "none", label: qsTr("None") },
                            { value: "v1", label: qsTr("V1") },
                            { value: "v2", label: qsTr("V2") },
                            { value: "both", label: qsTr("Both") }
                        ]
                        delegate: BarButton {
                            required property var modelData
                            objectName: "lyricScreenVocals-" + modelData.value
                            text: modelData.label
                            selected: root.reproduction === "capable" && root.vocals === modelData.value
                            onClicked: root.chooseVocals(modelData.value)
                        }
                    }
                }
            }
        }
    }
}
