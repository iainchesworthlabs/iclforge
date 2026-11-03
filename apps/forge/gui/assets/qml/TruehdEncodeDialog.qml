import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

import ForgeGui

// "Encode TrueHD" - a WAV coded losslessly into a bare .thd stream, the window's twin of
// `forge truehd-encode`. It is its own dialog, reached from the header like Open stream and QC a
// stream, and not a fourth entry of the encoder workbench's codec list: the workbench plans an
// AC-3 family encode (bit rate, coding mode, metadata, the operator's channel layout), none of
// which a lossless encode has (see truehd_encode_controller.hpp). The WAV's channel mask names the
// speakers. The command that does the same is shown at the bottom of the dialog.
Dialog {
    id: root
    objectName: "truehdEncodeDialog"

    modal: true
    anchors.centerIn: parent
    width: Math.min(760, parent ? parent.width - 60 : 760)
    padding: Theme.space6
    title: ""

    background: Rectangle {
        color: Theme.bg
        border.color: Theme.text
        border.width: 2
    }

    FileDialog {
        id: sourceDialog
        objectName: "truehdEncodeSourceDialog"
        title: qsTr("Choose a WAV file")
        nameFilters: [qsTr("WAV audio (*.wav)"), qsTr("All files (*)")]
        onAccepted: TruehdEncodeController.chooseSource(selectedFile)
    }

    FileDialog {
        id: outputDialog
        objectName: "truehdEncodeOutputDialog"
        title: qsTr("Save the TrueHD stream as")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("TrueHD stream (*.thd)")]
        onAccepted: TruehdEncodeController.chooseOutput(selectedFile)
    }

    contentItem: ColumnLayout {
        spacing: Theme.space4

        RowLayout {
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: qsTr("Encode TrueHD")
                font.pixelSize: Theme.fontArrow
                font.weight: Font.ExtraBold
                font.family: Theme.headingFamily
                color: Theme.text
            }
            Button {
                objectName: "truehdEncodeClose"
                text: qsTr("Close")
                onClicked: root.close()
            }
        }

        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Codes a WAV file losslessly as TrueHD: the stream decodes to exactly the samples that went in. The WAV's channel mask names the speakers; a file with none is taken in the order forge decode writes. The result is a bare .thd stream; forge truehd-encode also writes Matroska, MP4 and a Blu-ray transport stream.")
            font.pixelSize: Theme.fontSmall
            color: Theme.neutral700
        }

        RowLayout {
            Layout.fillWidth: true
            Button {
                objectName: "truehdEncodeChooseSource"
                text: qsTr("Choose WAV…")
                enabled: !TruehdEncodeController.busy
                onClicked: sourceDialog.open()
            }
            Text {
                objectName: "truehdEncodeSourceText"
                Layout.fillWidth: true
                elide: Text.ElideMiddle
                text: TruehdEncodeController.sourcePath.length > 0 ? TruehdEncodeController.sourcePath
                                                                   : qsTr("No file chosen")
                font.pixelSize: Theme.fontSmall
                color: Theme.text
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Button {
                objectName: "truehdEncodeChooseOutput"
                text: qsTr("Save as…")
                enabled: !TruehdEncodeController.busy
                onClicked: outputDialog.open()
            }
            Text {
                objectName: "truehdEncodeOutputText"
                Layout.fillWidth: true
                elide: Text.ElideMiddle
                text: TruehdEncodeController.outputPath.length > 0 ? TruehdEncodeController.outputPath
                                                                   : qsTr("Not chosen yet")
                font.pixelSize: Theme.fontSmall
                color: Theme.text
            }
        }

        CheckBox {
            objectName: "truehdEncodeVerify"
            text: qsTr("Verify: decode every access unit and compare it with the input")
            checked: TruehdEncodeController.verify
            enabled: !TruehdEncodeController.busy
            onToggled: TruehdEncodeController.verify = checked
        }

        CheckBox {
            objectName: "truehdEncodeDownmix"
            text: qsTr("Write the 2 and 6 channel presentations as folded-down mixes (truehd-downmix=standard)")
            checked: TruehdEncodeController.standardDownmix
            enabled: !TruehdEncodeController.busy
            onToggled: TruehdEncodeController.standardDownmix = checked
        }

        RowLayout {
            Layout.fillWidth: true
            Button {
                objectName: "truehdEncodeRun"
                text: qsTr("Encode")
                enabled: TruehdEncodeController.canEncode
                onClicked: TruehdEncodeController.encode()
            }
            Button {
                objectName: "truehdEncodeStop"
                text: qsTr("Stop")
                enabled: TruehdEncodeController.busy
                onClicked: TruehdEncodeController.cancel()
            }
            ProgressBar {
                objectName: "truehdEncodeProgress"
                Layout.fillWidth: true
                visible: TruehdEncodeController.busy
                value: TruehdEncodeController.progress
            }
        }

        Text {
            objectName: "truehdEncodeSummary"
            Layout.fillWidth: true
            visible: TruehdEncodeController.summary.length > 0
            wrapMode: Text.WordWrap
            text: TruehdEncodeController.summary
            font.pixelSize: Theme.fontSmall
            color: Theme.good
        }

        Text {
            objectName: "truehdEncodeError"
            Layout.fillWidth: true
            visible: TruehdEncodeController.error.length > 0
            wrapMode: Text.WordWrap
            text: TruehdEncodeController.error
            font.pixelSize: Theme.fontSmall
            color: Theme.bad
        }

        Text {
            objectName: "truehdEncodeCommand"
            Layout.fillWidth: true
            wrapMode: Text.WrapAnywhere
            text: TruehdEncodeController.cliLine
            font.pixelSize: Theme.fontSmall
            font.family: Theme.monoFamily
            color: Theme.neutral700
        }
    }
}
