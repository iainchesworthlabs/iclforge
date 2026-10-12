import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Hearth

// A singer's microphone, on the Karaoke page (planning/karaoke-microphone.md, M4).
//
// What the card promises is as much its content as its controls: the microphone is off until the
// button is pressed, and is closed again when the page is left or the item ends; nothing is
// recorded and nothing leaves the machine; and the number is shown only to someone who has said they
// wear headphones, because through open speakers the microphone hears the backing and the score reads
// as nearly perfect for no singing.
//
// Everything it shows comes from MicrophoneController. The page passes in what the choice of
// reproduction is, since the part the singer has muted is the one scored against, and whether the
// card is the one in view.
Card {
    id: root

    // The reproduction and vocals of the page's choice ("aware"/"capable"/"asCoded", "none"/"v1"/...).
    property string reproduction: "aware"
    property string vocals: "both"
    // The card is in use: the page is shown and the item is karaoke. The microphone closes when
    // this goes false.
    property bool active: true

    ordinal: "04"
    title: qsTr("Microphone")
    summary: MicrophoneController.on ? qsTr("On") : qsTr("Off")
    framed: true

    readonly property string state: MicrophoneController.state
    readonly property bool on: MicrophoneController.on
    readonly property var score: MicrophoneController.score
    readonly property var live: MicrophoneController.live
    readonly property int labelWidth: Math.round(150 * Theme.fontScale)

    // --- which item, and where in it -------------------------------------------------------
    readonly property string itemPath: {
        const queue = HearthController.queue;
        const index = HearthController.currentIndex;
        return (index >= 0 && index < queue.length) ? (queue[index].path ?? "") : "";
    }
    readonly property string preferred: MicrophoneController.preferredPart(root.reproduction, root.vocals)

    function refresh() {
        MicrophoneController.load(root.active ? root.itemPath : "", root.preferred);
    }
    onItemPathChanged: Qt.callLater(root.refresh)
    onPreferredChanged: Qt.callLater(root.refresh)
    onActiveChanged: {
        if (!root.active) {
            MicrophoneController.stop();
        }
        Qt.callLater(root.refresh);
    }
    Component.onCompleted: {
        MicrophoneController.refreshDevices();
        root.refresh();
    }
    // The page closed (or the window did): a microphone is never left on behind a page that is gone.
    Component.onDestruction: MicrophoneController.stop()

    Connections {
        target: HearthController
        function onPositionChanged() {
            MicrophoneController.anchor(HearthController.positionMs, HearthController.playing);
        }
        function onStateChanged() {
            MicrophoneController.anchor(HearthController.positionMs, HearthController.playing);
        }
    }

    // --- helpers ---------------------------------------------------------------------------
    function noteName(hz) {
        if (!(hz > 0)) {
            return "";
        }
        const names = ["C", "C♯", "D", "D♯", "E", "F", "F♯", "G", "G♯", "A", "A♯", "B"];
        const midi = Math.round(69 + 12 * Math.log(hz / 440) / Math.LN2);
        return names[((midi % 12) + 12) % 12] + (Math.floor(midi / 12) - 1);
    }
    function centsText(cents) {
        const rounded = Math.round(cents);
        if (Math.abs(rounded) < 10) {
            return qsTr("in tune");
        }
        return rounded > 0 ? qsTr("%1 cents sharp").arg(rounded) : qsTr("%1 cents flat").arg(-rounded);
    }
    function lagText(lag) {
        if (lag === undefined || Math.abs(lag) < 40) {
            return "";
        }
        return lag > 0 ? qsTr("You are about %1 ms behind the tune.").arg(Math.round(lag / 10) * 10)
                       : qsTr("You are about %1 ms ahead of the tune.").arg(Math.round(-lag / 10) * 10);
    }

    // --- the promise ------------------------------------------------------------------------
    Text {
        objectName: "microphoneNote"
        Layout.fillWidth: true
        text: qsTr("Sing along and see how close your pitch is to the tune. The microphone is off "
                   + "until you turn it on, nothing is recorded, and nothing leaves this computer.")
        color: Theme.textMuted
        font.pixelSize: Theme.fontSmall
        wrapMode: Text.WordWrap
    }

    // --- the input --------------------------------------------------------------------------
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.gap
        Text {
            text: qsTr("Input")
            color: Theme.text
            font.pixelSize: Theme.fontNormal
            Layout.preferredWidth: root.labelWidth
        }
        AppComboBox {
            id: deviceBox
            objectName: "microphoneDevice"
            Layout.fillWidth: true
            enabled: !root.on
            // "The default input" first, then every one the machine has.
            model: [{ id: "", name: qsTr("The default input") }].concat(MicrophoneController.devices)
            textRole: "name"
            valueRole: "id"
            Accessible.name: qsTr("Microphone input")
            function sync() {
                const index = indexOfValue(MicrophoneController.deviceId);
                currentIndex = index >= 0 ? index : 0;
            }
            Component.onCompleted: sync()
            onModelChanged: sync()
            onActivated: MicrophoneController.deviceId = currentValue
            Connections {
                target: MicrophoneController
                function onDeviceChanged() { deviceBox.sync(); }
            }
        }
        AppButton {
            objectName: "microphoneRefresh"
            enabled: !root.on
            text: qsTr("Refresh")
            onClicked: MicrophoneController.refreshDevices()
        }
    }

    AppCheckBox {
        objectName: "microphoneHeadphones"
        Layout.fillWidth: true
        text: qsTr("I am wearing headphones")
        note: qsTr("Through open speakers the microphone hears the backing as well as you, and the "
                  + "score can read as nearly perfect for no singing. The score is shown only when "
                  + "you say you are wearing headphones.")
        checked: MicrophoneController.headphones
        onToggled: function(on) { MicrophoneController.headphones = on; }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.gap
        AppButton {
            objectName: "microphoneToggle"
            enabled: root.active
            text: root.on ? qsTr("Turn the microphone off") : qsTr("Turn the microphone on")
            primary: !root.on
            onClicked: root.on ? MicrophoneController.stop() : MicrophoneController.start()
        }
        // The indicator: on screen whenever a microphone is open.
        Row {
            objectName: "microphoneIndicator"
            visible: root.on
            spacing: Math.round(6 * Theme.fontScale)
            Layout.alignment: Qt.AlignVCenter
            Rectangle {
                width: Math.round(10 * Theme.fontScale)
                height: width
                radius: width / 2
                color: Theme.accent
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                text: qsTr("Microphone on")
                color: Theme.accentInk
                font.pixelSize: Theme.fontNormal
                font.bold: true
                anchors.verticalCenter: parent.verticalCenter
            }
        }
        Item { Layout.fillWidth: true }
    }

    Text {
        objectName: "microphoneError"
        Layout.fillWidth: true
        visible: root.state === "error"
        text: MicrophoneController.errorText
        color: Theme.accentInk
        font.pixelSize: Theme.fontNormal
        wrapMode: Text.WordWrap
    }

    // --- the meter --------------------------------------------------------------------------
    RowLayout {
        Layout.fillWidth: true
        visible: root.on
        spacing: Theme.gap
        Text {
            text: qsTr("Level")
            color: Theme.text
            font.pixelSize: Theme.fontNormal
            Layout.preferredWidth: root.labelWidth
        }
        Rectangle {
            id: meter
            objectName: "microphoneMeter"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.round(12 * Theme.fontScale)
            color: Theme.surfaceAlt ?? Theme.surface
            border.color: Theme.border
            border.width: 1
            Rectangle {
                width: Math.round(parent.width * MicrophoneController.level)
                height: parent.height
                color: MicrophoneController.clipping ? Theme.accent : Theme.neutral600
            }
            Accessible.role: Accessible.ProgressBar
            Accessible.name: qsTr("Microphone level")
        }
        Text {
            objectName: "microphoneClip"
            visible: MicrophoneController.clipping
            text: qsTr("Too loud")
            color: Theme.accentInk
            font.pixelSize: Theme.fontSmall
        }
    }
    Text {
        objectName: "microphoneSilent"
        Layout.fillWidth: true
        visible: root.on && MicrophoneController.silentSeconds >= 1.5
        text: qsTr("No sound from the microphone. It may be muted, or the wrong input is chosen.")
        color: Theme.textMuted
        font.pixelSize: Theme.fontSmall
        wrapMode: Text.WordWrap
    }

    // --- what it is scored against -----------------------------------------------------------
    Text {
        objectName: "microphoneReference"
        Layout.fillWidth: true
        text: MicrophoneController.referenceStatus === "ready"
              ? qsTr("Scored against %1.").arg(MicrophoneController.referenceText)
              : (MicrophoneController.referenceStatus === "loading"
                 ? MicrophoneController.referenceText
                 : qsTr("No melody to score against: %1").arg(MicrophoneController.referenceText))
        color: Theme.textMuted
        font.pixelSize: Theme.fontSmall
        wrapMode: Text.WordWrap
        visible: MicrophoneController.referenceText.length > 0
    }

    // --- the singer ---------------------------------------------------------------------------
    Text {
        objectName: "microphonePitch"
        Layout.fillWidth: true
        visible: root.on
        text: {
            if (root.live.voiced !== true) {
                return qsTr("Listening…");
            }
            const note = root.noteName(root.live.hz);
            const hz = qsTr("%1 · %2 Hz").arg(note).arg(Math.round(root.live.hz));
            return root.live.centsFromReference !== undefined
                   ? qsTr("%1 · %2").arg(hz).arg(root.centsText(root.live.centsFromReference))
                   : hz;
        }
        color: Theme.text
        font.pixelSize: Theme.fontNormal
        font.family: Theme.monoFamily
    }

    Text {
        objectName: "microphoneScore"
        Layout.fillWidth: true
        visible: root.on && MicrophoneController.referenceStatus === "ready"
        text: {
            if (root.score.withheld === true) {
                return qsTr("Say you are wearing headphones to see a score.");
            }
            if (root.score.percent === undefined || root.score.percent < 0) {
                return qsTr("Sing a few notes to get a score.");
            }
            const lag = root.lagText(root.score.lagMs);
            const base = qsTr("Pitch score so far: %1 %").arg(root.score.percent);
            return lag.length > 0 ? base + " " + lag : base;
        }
        color: Theme.text
        font.pixelSize: Theme.fontNormal
        wrapMode: Text.WordWrap
    }
    Text {
        objectName: "microphoneScoreNote"
        Layout.fillWidth: true
        visible: root.on && MicrophoneController.referenceStatus === "ready"
        text: qsTr("It counts how much of the tune's time your pitch was within a semitone of it, "
                   + "in any octave. It hears pitch only, not timing, words or feeling.")
        color: Theme.textMuted
        font.pixelSize: Theme.fontSmall
        wrapMode: Text.WordWrap
    }

    // --- the alignment ------------------------------------------------------------------------
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.gap
        Text {
            text: qsTr("Microphone delay")
            color: Theme.text
            font.pixelSize: Theme.fontNormal
            elide: Text.ElideRight
            Layout.preferredWidth: root.labelWidth
        }
        AppSlider {
            id: delaySlider
            objectName: "microphoneDelay"
            Layout.preferredWidth: Math.round(220 * Theme.fontScale)
            from: 0
            to: MicrophoneController.maxAlignmentMs
            stepSize: 5
            snapMode: Slider.SnapAlways
            value: MicrophoneController.inputDelayMs
            Accessible.name: qsTr("Microphone delay, milliseconds")
            onMoved: MicrophoneController.inputDelayMs = value
            Connections {
                target: MicrophoneController
                function onAlignmentChanged() { delaySlider.value = MicrophoneController.inputDelayMs; }
            }
        }
        Text {
            objectName: "microphoneDelayText"
            text: qsTr("%1 ms").arg(MicrophoneController.inputDelayMs)
            color: Theme.textMuted
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fontNormal
        }
        Item { Layout.fillWidth: true }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.gap
        Text {
            text: qsTr("Move your timing")
            color: Theme.text
            font.pixelSize: Theme.fontNormal
            elide: Text.ElideRight
            Layout.preferredWidth: root.labelWidth
        }
        AppSlider {
            id: offsetSlider
            objectName: "microphoneOffset"
            Layout.preferredWidth: Math.round(220 * Theme.fontScale)
            from: -MicrophoneController.maxAlignmentMs
            to: MicrophoneController.maxAlignmentMs
            stepSize: 10
            snapMode: Slider.SnapAlways
            value: MicrophoneController.offsetMs
            Accessible.name: qsTr("Move your timing, milliseconds")
            onMoved: MicrophoneController.offsetMs = value
            Connections {
                target: MicrophoneController
                function onAlignmentChanged() { offsetSlider.value = MicrophoneController.offsetMs; }
            }
        }
        Text {
            objectName: "microphoneOffsetText"
            text: qsTr("%1 ms").arg(MicrophoneController.offsetMs)
            color: Theme.textMuted
            font.family: Theme.monoFamily
            font.pixelSize: Theme.fontNormal
        }
        Item { Layout.fillWidth: true }
    }
    Text {
        Layout.fillWidth: true
        text: qsTr("A microphone's sound reaches the program a little after it was sung, and some "
                   + "take a tenth of a second or more. If you are told you are behind the tune "
                   + "when you are not, raise the delay.")
        color: Theme.textMuted
        font.pixelSize: Theme.fontSmall
        wrapMode: Text.WordWrap
    }
}
