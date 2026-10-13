import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
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
    // The item's position (the lyric clock's reading), which the pitch ribbon is drawn against.
    property real positionMs: 0

    ordinal: "04"
    title: qsTr("Microphone")
    summary: MicrophoneController.on ? qsTr("On") : qsTr("Off")
    framed: true

    readonly property string state: MicrophoneController.state
    readonly property bool on: MicrophoneController.on
    readonly property var score: MicrophoneController.score
    readonly property var live: MicrophoneController.live
    readonly property var lastPerformance: MicrophoneController.summary
    readonly property var lastTake: MicrophoneController.lastTake
    readonly property int labelWidth: Math.round(150 * Theme.fontScale)

    // --- how the rows fit the card ----------------------------------------------------------
    // Every row here can be as narrow as the card: a ColumnLayout is as wide as the row of it
    // that shrinks least, so one row that could not shrink would push all of them past the
    // card's edge, and the card's past the window's.
    //
    // What the rows have to share is the card's width less the box's padding (Card.qml, framed).
    readonly property real contentWidth: Math.max(0, root.width - 2 * Theme.pad)
    // A label sits beside its control where that leaves the control a width worth having, and
    // above it where it does not: in a third of the window at its own size, and at a large text
    // size in a wider card than that.
    readonly property bool stacked: root.contentWidth
                                    < root.labelWidth + Theme.gap + Math.round(240 * Theme.fontScale)
    // A button beside what it acts on keeps its whole label up to half the row, and elides past
    // that: a long word in some language shortens the button, not the page.
    readonly property real besideLimit: Math.floor(root.contentWidth / 2)
    // The widest a delay or a timing reads. The two values are given this width, so that the
    // slider beside one keeps its own while it is dragged from "0 ms" to "-1000 ms".
    TextMetrics {
        id: widestMs
        font.family: Theme.monoFamily
        font.pixelSize: Theme.fontNormal
        text: qsTr("%1 ms").arg(-MicrophoneController.maxAlignmentMs)
    }

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
    function clockText(seconds) {
        const total = Math.max(0, Math.floor(seconds));
        const s = total % 60;
        return Math.floor(total / 60) + ":" + (s < 10 ? "0" : "") + s;
    }
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
    GridLayout {
        Layout.fillWidth: true
        columns: root.stacked ? 1 : 2
        columnSpacing: Theme.gap
        rowSpacing: Theme.space1
        Text {
            text: qsTr("Input")
            color: Theme.text
            font.pixelSize: Theme.fontNormal
            wrapMode: Text.WordWrap
            Layout.fillWidth: root.stacked
            Layout.preferredWidth: root.stacked ? -1 : root.labelWidth
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.gap
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
                Layout.maximumWidth: root.besideLimit
                enabled: !root.on
                text: qsTr("Refresh")
                onClicked: MicrophoneController.refreshDevices()
            }
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

    // The button, and the indicator beside it where there is room and under it where there is not.
    Flow {
        id: toggleRow
        Layout.fillWidth: true
        spacing: Theme.gap
        AppButton {
            id: toggleButton
            objectName: "microphoneToggle"
            width: Math.min(implicitWidth, toggleRow.width)
            enabled: root.active
            text: root.on ? qsTr("Turn the microphone off") : qsTr("Turn the microphone on")
            primary: !root.on
            onClicked: root.on ? MicrophoneController.stop() : MicrophoneController.start()
        }
        // The indicator: on screen whenever a microphone is open.
        RowLayout {
            objectName: "microphoneIndicator"
            visible: root.on
            width: Math.min(implicitWidth, toggleRow.width)
            height: toggleButton.height
            spacing: Math.round(6 * Theme.fontScale)
            Rectangle {
                Layout.preferredWidth: Math.round(10 * Theme.fontScale)
                Layout.preferredHeight: Layout.preferredWidth
                radius: width / 2
                color: Theme.accent
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("Microphone on")
                color: Theme.accentInk
                font.pixelSize: Theme.fontNormal
                font.bold: true
                elide: Text.ElideRight
            }
        }
    }

    // --- a take ---------------------------------------------------------------------------------
    // Recording is its own switch, with its own indicator: the microphone being on is not a take.
    // What is kept is the voice alone, never the backing.
    Flow {
        id: recordRow
        Layout.fillWidth: true
        visible: root.on
        spacing: Theme.gap
        AppButton {
            id: recordButton
            objectName: "microphoneRecord"
            width: Math.min(implicitWidth, recordRow.width)
            text: MicrophoneController.recording ? qsTr("Stop recording") : qsTr("Record a take")
            onClicked: MicrophoneController.recording ? MicrophoneController.stopRecording()
                                                      : MicrophoneController.startRecording()
        }
        RowLayout {
            objectName: "microphoneRecordingIndicator"
            visible: MicrophoneController.recording
            width: Math.min(implicitWidth, recordRow.width)
            height: recordButton.height
            spacing: Math.round(6 * Theme.fontScale)
            Rectangle {
                Layout.preferredWidth: Math.round(10 * Theme.fontScale)
                Layout.preferredHeight: Layout.preferredWidth
                color: Theme.accent
            }
            Text {
                objectName: "microphoneTakeLength"
                Layout.fillWidth: true
                text: qsTr("Recording %1").arg(root.clockText(MicrophoneController.takeSeconds))
                color: Theme.accentInk
                font.pixelSize: Theme.fontNormal
                font.bold: true
                elide: Text.ElideRight
            }
        }
    }
    Text {
        objectName: "microphoneTakeError"
        Layout.fillWidth: true
        visible: MicrophoneController.takeError.length > 0
        text: MicrophoneController.takeError
        color: Theme.accentInk
        font.pixelSize: Theme.fontNormal
        wrapMode: Text.WordWrap
    }
    GridLayout {
        Layout.fillWidth: true
        columns: root.stacked ? 1 : 2
        columnSpacing: Theme.gap
        rowSpacing: Theme.space1
        Text {
            text: qsTr("Takes go to")
            color: Theme.text
            font.pixelSize: Theme.fontNormal
            wrapMode: Text.WordWrap
            Layout.fillWidth: root.stacked
            Layout.preferredWidth: root.stacked ? -1 : root.labelWidth
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.gap
            Text {
                objectName: "microphoneTakesFolder"
                Layout.fillWidth: true
                text: MicrophoneController.takesFolder
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                elide: Text.ElideMiddle
            }
            AppButton {
                objectName: "microphoneChooseFolder"
                Layout.maximumWidth: root.besideLimit
                enabled: !MicrophoneController.recording
                text: qsTr("Choose…")
                onClicked: takesFolderDialog.open()
            }
        }
    }
    FolderDialog {
        id: takesFolderDialog
        title: qsTr("Where takes are kept")
        onAccepted: MicrophoneController.setTakesFolder(HearthController.urlToLocalFile(selectedFolder))
    }
    // The take just made, and what can be done with it: beside its name where the two buttons
    // leave the name room, under it where they do not, and one under the other in a card too
    // narrow for the pair.
    GridLayout {
        Layout.fillWidth: true
        visible: root.lastTake.name !== undefined
        columns: root.stacked ? 1 : 2
        columnSpacing: Theme.gap
        rowSpacing: Theme.gap
        Text {
            objectName: "microphoneLastTake"
            Layout.fillWidth: true
            text: qsTr("Saved %1 (%2)").arg(root.lastTake.name ?? "").arg(root.clockText(root.lastTake.seconds ?? 0))
            color: Theme.text
            font.pixelSize: Theme.fontNormal
            elide: Text.ElideMiddle
        }
        Flow {
            id: lastTakeButtons
            Layout.fillWidth: root.stacked
            Layout.preferredWidth: root.stacked
                ? -1
                : Math.min(root.besideLimit,
                           showTakesButton.implicitWidth + spacing + deleteTakeButton.implicitWidth)
            spacing: Theme.gap
            AppButton {
                id: showTakesButton
                objectName: "microphoneShowTakes"
                width: Math.min(implicitWidth, lastTakeButtons.width)
                text: qsTr("Show in folder")
                onClicked: MicrophoneController.showTakes()
            }
            AppButton {
                id: deleteTakeButton
                objectName: "microphoneDeleteTake"
                width: Math.min(implicitWidth, lastTakeButtons.width)
                text: qsTr("Delete this take")
                onClicked: MicrophoneController.deleteLastTake()
            }
        }
    }
    // The takes in the folder, newest first. A few are shown and the rest are counted; removing
    // one asks again, because a take cannot be got back.
    ColumnLayout {
        id: takesList
        objectName: "microphoneTakesList"
        Layout.fillWidth: true
        visible: MicrophoneController.takes.length > 0
        spacing: Theme.gap
        readonly property int shown: 5
        Text {
            Layout.fillWidth: true
            text: qsTr("Your takes")
            color: Theme.text
            font.pixelSize: Theme.fontNormal
            font.bold: true
            wrapMode: Text.WordWrap
        }
        Repeater {
            model: MicrophoneController.takes.slice(0, takesList.shown)
            delegate: GridLayout {
                id: takeRow
                required property var modelData
                property bool confirming: false
                Layout.fillWidth: true
                columns: root.stacked ? 1 : 2
                columnSpacing: Theme.gap
                rowSpacing: Theme.space1
                Text {
                    objectName: "microphoneTakeRow"
                    Layout.fillWidth: true
                    text: takeRow.modelData.name + " (" + root.clockText(takeRow.modelData.seconds) + ")"
                    color: Theme.text
                    font.pixelSize: Theme.fontNormal
                    elide: Text.ElideMiddle
                }
                // Delete, or the two that answer it: as wide as the ones on show.
                Flow {
                    id: takeButtons
                    Layout.fillWidth: root.stacked
                    Layout.preferredWidth: root.stacked
                        ? -1
                        : Math.min(root.besideLimit,
                                   takeRow.confirming
                                       ? confirmButton.implicitWidth + spacing + keepButton.implicitWidth
                                       : deleteButton.implicitWidth)
                    spacing: Theme.gap
                    AppButton {
                        id: deleteButton
                        objectName: "microphoneTakeDelete"
                        visible: !takeRow.confirming
                        width: Math.min(implicitWidth, takeButtons.width)
                        text: qsTr("Delete")
                        onClicked: takeRow.confirming = true
                    }
                    AppButton {
                        id: confirmButton
                        objectName: "microphoneTakeDeleteConfirm"
                        visible: takeRow.confirming
                        width: Math.min(implicitWidth, takeButtons.width)
                        text: qsTr("Delete for good")
                        onClicked: MicrophoneController.deleteTake(takeRow.modelData.path)
                    }
                    AppButton {
                        id: keepButton
                        objectName: "microphoneTakeKeep"
                        visible: takeRow.confirming
                        width: Math.min(implicitWidth, takeButtons.width)
                        text: qsTr("Keep")
                        onClicked: takeRow.confirming = false
                    }
                }
            }
        }
        Text {
            objectName: "microphoneTakesMore"
            Layout.fillWidth: true
            visible: MicrophoneController.takes.length > takesList.shown
            text: qsTr("and %1 more in the folder").arg(MicrophoneController.takes.length - takesList.shown)
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.WordWrap
        }
    }
    Text {
        objectName: "microphoneTakeNote"
        Layout.fillWidth: true
        text: qsTr("A take is your voice alone, not the backing: a lossless file with a small "
                   + "description beside it saying where in the song it began. Through speakers the "
                   + "backing is in it too.")
        color: Theme.textMuted
        font.pixelSize: Theme.fontSmall
        wrapMode: Text.WordWrap
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
    GridLayout {
        Layout.fillWidth: true
        visible: root.on
        columns: root.stacked ? 1 : 2
        columnSpacing: Theme.gap
        rowSpacing: Theme.space1
        Text {
            text: qsTr("Level")
            color: Theme.text
            font.pixelSize: Theme.fontNormal
            wrapMode: Text.WordWrap
            Layout.fillWidth: root.stacked
            Layout.preferredWidth: root.stacked ? -1 : root.labelWidth
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.gap
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
                Layout.maximumWidth: root.besideLimit
                visible: MicrophoneController.clipping
                text: qsTr("Too loud")
                color: Theme.accentInk
                font.pixelSize: Theme.fontSmall
                elide: Text.ElideRight
            }
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
        wrapMode: Text.WordWrap
    }

    // The tune as notes and your voice as a line over them, in time with the words. Shown only
    // while the microphone is on and there is a tune: it asks the controller for its window a few
    // times a second, and not at all while hidden.
    PitchRibbon {
        objectName: "microphoneRibbon"
        Layout.fillWidth: true
        Layout.preferredHeight: Math.round(110 * Theme.fontScale)
        visible: root.on && MicrophoneController.referenceStatus === "ready"
        live: visible
        positionMs: root.positionMs
        listening: root.live.voiced !== true
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
        text: qsTr("It counts how much of the tune's time your pitch was within %1 cents of it, "
                   + "in any octave. It hears pitch only, not timing, words or feeling.")
              .arg(MicrophoneController.toleranceCents)
        color: Theme.textMuted
        font.pixelSize: Theme.fontSmall
        wrapMode: Text.WordWrap
    }

    // --- how strictly, and against whose key ---------------------------------------------------
    GridLayout {
        Layout.fillWidth: true
        columns: root.stacked ? 1 : 2
        columnSpacing: Theme.gap
        rowSpacing: Theme.space1
        Text {
            text: qsTr("How close counts")
            color: Theme.text
            font.pixelSize: Theme.fontNormal
            wrapMode: Text.WordWrap
            Layout.fillWidth: root.stacked
            Layout.preferredWidth: root.stacked ? -1 : root.labelWidth
        }
        AppComboBox {
            id: toleranceBox
            objectName: "microphoneTolerance"
            Layout.fillWidth: true
            model: [
                { cents: 50, name: qsTr("Close: within half a semitone (50 cents)") },
                { cents: 100, name: qsTr("A semitone (100 cents)") },
                { cents: 200, name: qsTr("Loose: within a whole tone (200 cents)") }
            ]
            textRole: "name"
            valueRole: "cents"
            Accessible.name: qsTr("How close your pitch must be to the tune to count")
            function sync() {
                const index = indexOfValue(MicrophoneController.toleranceCents);
                currentIndex = index >= 0 ? index : 1;
            }
            Component.onCompleted: sync()
            onActivated: MicrophoneController.toleranceCents = currentValue
            Connections {
                target: MicrophoneController
                function onToleranceChanged() { toleranceBox.sync(); }
            }
        }
    }
    AppCheckBox {
        objectName: "microphoneOwnKey"
        Layout.fillWidth: true
        text: qsTr("Score against my key")
        note: qsTr("If you sing the whole song a little sharp or flat of the tune, judge the shape of "
                   + "your melody instead: your own key is taken from your first sung notes and "
                   + "allowed for. It is off each time Hearth starts, and the card says when it is "
                   + "in use.")
        checked: MicrophoneController.ownKey
        onToggled: function(on) { MicrophoneController.ownKey = on; }
    }
    Text {
        objectName: "microphoneOwnKeyInUse"
        Layout.fillWidth: true
        visible: MicrophoneController.ownKey
        text: MicrophoneController.keyShiftCents === 0
              ? qsTr("Waiting for a few sung notes to find your key.")
              : qsTr("Scored against your key: you are about %1 cents %2 the tune's, and that is allowed for.")
                    .arg(Math.round(Math.abs(MicrophoneController.keyShiftCents)))
                    .arg(MicrophoneController.keyShiftCents > 0 ? qsTr("sharp of") : qsTr("flat of"))
        color: Theme.text
        font.pixelSize: Theme.fontSmall
        wrapMode: Text.WordWrap
    }

    // --- what the last performance came to -------------------------------------------------------
    // Kept when the microphone is turned off, the song ends or the device goes, until the next one
    // starts or another song is chosen. Nothing in it is ranked, named or sent anywhere. The basis
    // is stated: what it was compared with, within what, and what was not scored.
    ColumnLayout {
        objectName: "microphoneSummary"
        Layout.fillWidth: true
        visible: root.lastPerformance.percent !== undefined
        spacing: Math.round(4 * Theme.fontScale)
        Text {
            Layout.fillWidth: true
            text: qsTr("Last performance")
            color: Theme.text
            font.pixelSize: Theme.fontNormal
            font.bold: true
            wrapMode: Text.WordWrap
        }
        Text {
            objectName: "microphoneSummaryScore"
            Layout.fillWidth: true
            text: qsTr("Pitch score: %1 %").arg(root.lastPerformance.percent ?? 0)
            color: Theme.text
            font.pixelSize: Theme.fontNormal
            wrapMode: Text.WordWrap
        }
        Text {
            objectName: "microphoneSummaryDetail"
            Layout.fillWidth: true
            text: {
                const parts = [qsTr("You sang for %1 % of the tune's voiced time.")
                                   .arg(root.lastPerformance.coveragePercent ?? 0)];
                if (root.lastPerformance.meanCents !== undefined) {
                    parts.push(qsTr("On average you were %1.").arg(root.centsText(root.lastPerformance.meanCents)));
                }
                const lag = root.lagText(root.lastPerformance.lagMs);
                if (lag.length > 0) {
                    parts.push(lag);
                }
                return parts.join(" ");
            }
            color: Theme.text
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.WordWrap
        }
        Text {
            objectName: "microphoneSummaryBasis"
            Layout.fillWidth: true
            text: qsTr("Compared with %1, within %2 cents, in any octave%3. Lines the tune has no "
                       + "notes in were not scored. It is shown here and kept nowhere.")
                  .arg(root.lastPerformance.reference ?? "")
                  .arg(Math.round(root.lastPerformance.toleranceCents ?? 100))
                  .arg(root.lastPerformance.ownKey === true
                       ? qsTr(", against your own key (about %1 cents %2 the tune's)")
                             .arg(Math.round(Math.abs(root.lastPerformance.keyShiftCents ?? 0)))
                             .arg((root.lastPerformance.keyShiftCents ?? 0) > 0 ? qsTr("sharp of") : qsTr("flat of"))
                       : "")
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.WordWrap
        }
    }

    // --- the alignment ------------------------------------------------------------------------
    // A slider is the design's 220 px where the row has them, and gives way where it has not:
    // it is the one thing in its row that can.
    GridLayout {
        Layout.fillWidth: true
        columns: root.stacked ? 1 : 2
        columnSpacing: Theme.gap
        rowSpacing: Theme.space1
        Text {
            text: qsTr("Microphone delay")
            color: Theme.text
            font.pixelSize: Theme.fontNormal
            wrapMode: Text.WordWrap
            Layout.fillWidth: root.stacked
            Layout.preferredWidth: root.stacked ? -1 : root.labelWidth
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.gap
            AppSlider {
                id: delaySlider
                objectName: "microphoneDelay"
                Layout.fillWidth: true
                Layout.preferredWidth: Math.round(220 * Theme.fontScale)
                Layout.maximumWidth: Math.round(220 * Theme.fontScale)
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
                Layout.preferredWidth: Math.min(root.besideLimit, Math.ceil(widestMs.advanceWidth))
                text: qsTr("%1 ms").arg(MicrophoneController.inputDelayMs)
                color: Theme.textMuted
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fontNormal
                elide: Text.ElideRight
            }
            Item { Layout.fillWidth: true }
        }
    }
    GridLayout {
        Layout.fillWidth: true
        columns: root.stacked ? 1 : 2
        columnSpacing: Theme.gap
        rowSpacing: Theme.space1
        Text {
            text: qsTr("Move your timing")
            color: Theme.text
            font.pixelSize: Theme.fontNormal
            wrapMode: Text.WordWrap
            Layout.fillWidth: root.stacked
            Layout.preferredWidth: root.stacked ? -1 : root.labelWidth
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.gap
            AppSlider {
                id: offsetSlider
                objectName: "microphoneOffset"
                Layout.fillWidth: true
                Layout.preferredWidth: Math.round(220 * Theme.fontScale)
                Layout.maximumWidth: Math.round(220 * Theme.fontScale)
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
                Layout.preferredWidth: Math.min(root.besideLimit, Math.ceil(widestMs.advanceWidth))
                text: qsTr("%1 ms").arg(Theme.ltr(MicrophoneController.offsetMs))
                color: Theme.textMuted
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fontNormal
                elide: Text.ElideRight
            }
            Item { Layout.fillWidth: true }
        }
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
