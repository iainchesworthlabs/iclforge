import QtQuick

import Hearth

// A lyric line's result, shown after the line has been sung (planning/karaoke-microphone.md, M5): three
// pips, the percentage, or "not sung".
//
// Three levels as pips and a number, so that colour is never the only cue (L4). A line the reference
// has no note in has no mark at all, which is not a score of zero; a line the singer was not voiced for
// says so in words and carries no number, which is not a bad score; and without the headphones answer
// there is no result to show.
//
// `result` is MicrophoneController.lineResult(): scored, notSung, percent and level. Nothing here asks
// the controller: the view that places the mark does, so that this is a picture of an answer.
Item {
    id: root

    property var result: null
    property color color: Theme.accent
    property color restColor: Theme.textMuted
    property real size: Theme.fontSmall

    readonly property bool shown: root.result !== null && root.result !== undefined
                                  && root.result.scored === true
    readonly property bool notSung: root.shown && root.result.notSung === true
    // 0 to 3: the pips filled.
    readonly property int level: (root.shown && !root.notSung) ? (root.result.level ?? 0) : 0
    readonly property string label: {
        if (!root.shown) {
            return "";
        }
        return root.notSung ? qsTr("not sung") : qsTr("%1 %").arg(root.result.percent);
    }
    // For a screen reader: the words that go with the pips.
    readonly property string description: {
        if (!root.shown) {
            return "";
        }
        if (root.notSung) {
            return qsTr("Pitch: this line was not sung");
        }
        switch (root.level) {
        case 3: return qsTr("Pitch: %1 percent, close to the tune").arg(root.result.percent);
        case 2: return qsTr("Pitch: %1 percent, partly on the tune").arg(root.result.percent);
        }
        return qsTr("Pitch: %1 percent, mostly away from the tune").arg(root.result.percent);
    }

    visible: root.shown
    implicitWidth: row.implicitWidth
    implicitHeight: root.shown ? row.implicitHeight : 0
    Accessible.role: Accessible.StaticText
    Accessible.name: root.description

    Row {
        id: row
        spacing: Math.round(root.size * 0.45)

        Row {
            id: pips
            objectName: "lineMarkPips"
            spacing: Math.round(root.size * 0.25)
            anchors.verticalCenter: parent.verticalCenter
            visible: !root.notSung
            Repeater {
                model: 3
                delegate: Rectangle {
                    required property int index
                    width: Math.max(6, Math.round(root.size * 0.6))
                    height: width
                    radius: width / 2
                    color: index < root.level ? root.color : "transparent"
                    border.color: index < root.level ? root.color : root.restColor
                    border.width: 1
                }
            }
        }
        Text {
            objectName: "lineMarkText"
            text: root.label
            color: root.notSung ? root.restColor : root.color
            font.pixelSize: root.size
            font.bold: !root.notSung
            font.italic: root.notSung
            anchors.verticalCenter: parent.verticalCenter
        }
    }
}
