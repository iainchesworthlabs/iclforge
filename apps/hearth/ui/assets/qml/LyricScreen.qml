import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Hearth

// The singer view (planning/karaoke-design.md, K5; the design sheet's "Hearth:
// singer view"): the lyric view made full screen, dark, for a room rather than
// a desk. F11 opens it (Main.qml puts the window full screen with it) and Escape
// leaves; the transport and the vocals chooser stay at the bottom so a singer
// can pause, skip or pick the vocals without leaving it, and Space pauses.
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

    background: Rectangle { color: root.paper }

    LyricClock { id: lyricClock; active: root.visible }

    Shortcut {
        sequence: "Space"
        enabled: root.visible
        onActivated: HearthController.playing ? HearthController.pause() : HearthController.play()
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
                      + root.clock(lyricClock.positionMs) + " / " + root.clock(HearthController.durationMs)
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

        LyricView {
            id: lyrics
            objectName: "lyricScreenView"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 96
            Layout.rightMargin: 96
            large: true
            positionMs: lyricClock.positionMs
            sungColor: root.accent
            currentColor: root.ink
            restColor: root.quiet
            textColor: root.ink
        }

        // The controls, always reachable and out of the way.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 88
            color: root.bar

            Rectangle {
                width: parent.width
                height: 1
                color: root.rule
            }

            RowLayout {
                anchors.fill: parent
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

                // Where the item has got to: a bar, not a scrubber - seeking
                // is the transport bar's.
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 4
                    color: root.rule
                    Rectangle {
                        objectName: "lyricScreenProgress"
                        width: HearthController.durationMs > 0
                               ? parent.width * Math.min(1, lyricClock.positionMs / HearthController.durationMs)
                               : 0
                        height: parent.height
                        color: root.accent
                    }
                }

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
