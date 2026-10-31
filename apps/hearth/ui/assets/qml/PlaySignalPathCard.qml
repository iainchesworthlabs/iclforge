import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

import Hearth

// "07 Signal path" (main-play.png / play-minimum-size.png): decode -> render
// -> output. Pulled out of PlayPage.qml for the same reason and in the same
// way as PlayObjectsCard.qml - see that file's own header comment.
Card {
    id: root
    ordinal: "07"
    title: qsTr("Signal path")
    flat: true

    // A sink paired on the Network page but never chosen as (part of) the
    // output: Play still works, it just falls back to this computer's own
    // device with nothing else said - the exact gap behind the report that
    // Hearth "wouldn't play a song" after connecting to a network sink
    // (hearth-followups-group-ux-and-live-diagnostics-2026-09-26). True
    // whether no group exists yet or one does but was never picked -
    // outputGroupName is empty in both, and the hint's own wording covers
    // both remedies rather than branching on which applies.
    readonly property bool hasUnusedPairedSink: NetworkController.sinks.some((s) => s.badge === "paired")
                                                 && HearthController.outputGroupName.length === 0

    // The item is going to a receiver as a bitstream (as it is, or E-AC-3
    // transcoded to AC-3): nothing is decoded for the output or rendered
    // here, and each stage below says so in place of what a decode would. A
    // suite may set the mode, as KaraokePage.qml's own is set.
    property string outputMode: HearthController.outputFormat.mode ?? "none"
    readonly property bool transcoded: root.outputMode === "bitstreamAsAc3"
    readonly property bool receiverDecodes: root.outputMode === "bitstream" || root.transcoded

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: decodeCol.implicitHeight + Theme.pad * 2
        color: Theme.neutral100
        border.color: Theme.border
        border.width: 1

        ColumnLayout {
            id: decodeCol
            anchors.fill: parent
            anchors.margins: Theme.pad
            spacing: 2

            Text {
                text: qsTr("1 · DECODE")
                color: Theme.textMuted; font.pixelSize: Theme.fontMicro; font.bold: true
            }
            Text {
                objectName: "signalPathDecode"
                Layout.fillWidth: true
                text: {
                    if (HearthController.currentIndex < 0) return qsTr("Nothing playing");
                    var kind = HearthController.queue[HearthController.currentIndex].streamKind;
                    var modeName = HearthController.decoderSettings.mode;
                    // The dynamic range mode is this decoder's; a receiver
                    // sent the stream applies its own.
                    // TrueHD and MLP are lossless: A/52's operating modes are not theirs, and
                    // what the decoder does with a failed check is.
                    var lossless = kind === "TrueHD" || kind === "MLP";
                    var checks = HearthController.decoderSettings.truehdLenient ? qsTr("lossless, lenient checks")
                                                                              : qsTr("lossless, strict checks");
                    var mode = lossless ? checks
                             : root.transcoded ? qsTr("transcoded to AC-3")
                             : root.receiverDecodes ? qsTr("sent as it is")
                             : modeName === "rf" ? qsTr("RF mode")
                             : modeName === "custom" ? qsTr("custom mode") : qsTr("line mode");
                    //: Shown when the kind of the stream is not known, as in 'stream · RF mode'. The stream itself.
                    return (kind.length > 0 ? kind : qsTr("stream")) + " · " + mode;
                }
                color: Theme.text
                font.bold: true
                font.pixelSize: Theme.fontBody
                wrapMode: Text.WordWrap
            }
            Text {
                objectName: "signalPathTruehd"
                Layout.fillWidth: true
                visible: HearthController.thisFrame.truehd === true
                text: qsTr("Substream %1, %n channels. Dialogue normalisation and dynamic range are read and not applied. "
                           + "Passthrough of TrueHD is not available, so it is decoded here.", "",
                           HearthController.thisFrame.truehdChannels ?? 0)
                          .arg((HearthController.thisFrame.truehdSubstream ?? 0) + 1)
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                visible: HearthController.thisFrame.dialnorm !== undefined
                text: Theme.ltrNumbers(qsTr("dialnorm −%1: %2 dB down%3")
                          .arg(HearthController.thisFrame.dialnorm)
                          .arg((31 - HearthController.thisFrame.dialnorm).toFixed(1))
                          .arg(HearthController.hasObjectMetadata
                               ? qsTr(" · %n objects", "", HearthController.objectsPlaced) : ""))
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }
        }
    }

    Text {
        Layout.alignment: Qt.AlignHCenter
        text: "↓"
        color: Theme.textMuted
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: renderCol.implicitHeight + Theme.pad * 2
        color: Theme.neutral100
        border.color: Theme.border
        border.width: 1

        ColumnLayout {
            id: renderCol
            anchors.fill: parent
            anchors.margins: Theme.pad
            spacing: 2

            Text {
                text: qsTr("2 · RENDER")
                color: Theme.textMuted; font.pixelSize: Theme.fontMicro; font.bold: true
            }
            Text {
                objectName: "signalPathRender"
                Layout.fillWidth: true
                text: root.receiverDecodes
                      ? qsTr("Not rendered here")
                      : HearthController.layoutText.length > 0
                        ? qsTr("%1 onto %2 outputs").arg(HearthController.layoutText)
                                                     .arg(HearthController.routingOutputs)
                        : qsTr("Not rendering")
                color: Theme.text
                font.bold: true
                font.pixelSize: Theme.fontBody
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                text: {
                    if (root.receiverDecodes) {
                        return qsTr("the receiver decodes it for its own speakers · the levels "
                                    + "show a decode made here");
                    }
                    var small = [];
                    for (var i = 0; i < HearthController.speakerSmall.length; i++) {
                        if (HearthController.speakerSmall[i]) {
                            small.push(HearthController.speakerLabels[i]);
                        }
                    }
                    var lede = small.length > 0
                        ? qsTr("%1 small at %2 Hz").arg(small.join(qsTr(" and ")))
                                                    .arg(HearthController.crossoverHz)
                        : qsTr("no small speakers");
                    return lede + qsTr(" · trims and delays");
                }
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }
        }
    }

    Text {
        Layout.alignment: Qt.AlignHCenter
        text: "↓"
        color: Theme.textMuted
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: hearCol.implicitHeight + Theme.pad * 2
        color: Theme.neutral100
        border.color: Theme.border
        border.width: 1

        ColumnLayout {
            id: hearCol
            anchors.fill: parent
            anchors.margins: Theme.pad
            spacing: 2

            Text {
                text: qsTr("3 · YOU HEAR IT ON")
                color: Theme.textMuted; font.pixelSize: Theme.fontMicro; font.bold: true
            }
            Text {
                objectName: "signalPathOutput"
                Layout.fillWidth: true
                text: HearthController.deviceName.length > 0
                      ? HearthController.deviceName : qsTr("No output chosen")
                color: Theme.text
                font.bold: true
                font.pixelSize: Theme.fontBody
                wrapMode: Text.WordWrap
            }
            Text {
                objectName: "signalPathOutputFormat"
                Layout.fillWidth: true
                visible: HearthController.outputFormat.channels > 0
                // A bitstream link's two channels are its carrier's, and say
                // nothing of the programme: what it carries is named instead.
                text: root.receiverDecodes
                      ? qsTr("%1 bitstream over IEC 61937 at %2 kHz")
                            .arg(HearthController.outputFormat.stream ?? "")
                            .arg(Number(HearthController.outputFormat.sampleRate / 1000).toFixed(1))
                      : qsTr("%1 ch %2 at %3 kHz")
                            .arg(HearthController.outputFormat.channels)
                            .arg(root.outputMode === "pcm" ? qsTr("PCM") : qsTr("bitstream"))
                            .arg(Number(HearthController.outputFormat.sampleRate / 1000).toFixed(1))
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.gap / 2
                Text {
                    Layout.fillWidth: true
                    text: qsTr("output: your choice")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                }
                AppButton {
                    objectName: "playChooseOutput"
                    text: qsTr("Choose…")
                    // The enclosing ApplicationWindow, reached through the
                    // Window attached property - root.parent does not
                    // reliably chain up to it (GuidedWizard.qml's own
                    // requestWindow() comment explains why) - opened the
                    // same way the header's own output pill does
                    // (Main.qml's openOutputPicker()).
                    onClicked: root.Window.window.openOutputPicker()
                }
            }

            Text {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                objectName: "playUnusedPairedSinkHint"
                visible: root.hasUnusedPairedSink
                text: qsTr("A paired network sink isn't playing: no group is chosen as the output. "
                          + "Choose one above, or make one on the Network page.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }
        }
    }
}
