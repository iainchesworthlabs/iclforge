import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

import ForgeGui

// "Encode TrueHD" - a WAV coded losslessly into a .thd stream or a container, the window's twin of
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
        id: extraSourceDialog
        objectName: "truehdEncodeExtraSourceDialog"
        title: qsTr("Choose another WAV file")
        nameFilters: [qsTr("WAV audio (*.wav)"), qsTr("All files (*)")]
        onAccepted: TruehdEncodeController.addSource(selectedFile)
    }

    FileDialog {
        id: outputDialog
        objectName: "truehdEncodeOutputDialog"
        title: qsTr("Save the TrueHD stream as")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("TrueHD stream (*.thd)"), qsTr("Matroska (*.mkv)"), qsTr("MP4 (*.mp4)"),
                      qsTr("Blu-ray transport stream (*.m2ts *.ts)")]
        onAccepted: TruehdEncodeController.chooseOutput(selectedFile)
    }

    // One option of the command line that takes a number or a list: the text as `forge` reads it,
    // empty for the encoder's own default, and the sentence that says what it takes when it is not.
    component TextOption: ColumnLayout {
        id: field
        property string optionName
        property string label
        property string hint
        Layout.fillWidth: true
        spacing: 2
        RowLayout {
            Layout.fillWidth: true
            Text {
                Layout.preferredWidth: 220
                text: field.label
                font.pixelSize: Theme.fontSmall
                color: Theme.text
            }
            TextField {
                objectName: "truehdEncodeOption_" + field.optionName
                Layout.fillWidth: true
                text: TruehdEncodeController.options[field.optionName]
                placeholderText: field.hint
                enabled: !TruehdEncodeController.busy
                font.pixelSize: Theme.fontSmall
                onTextEdited: TruehdEncodeController.setOption(field.optionName, text)
            }
        }
        Text {
            objectName: "truehdEncodeOptionError_" + field.optionName
            Layout.fillWidth: true
            visible: text.length > 0
            wrapMode: Text.WordWrap
            text: TruehdEncodeController.optionErrors[field.optionName] || ""
            font.pixelSize: Theme.fontSmall
            color: Theme.bad
        }
    }

    // One option that is one of a few words: the first entry is the encoder's own default.
    component ChoiceOption: RowLayout {
        id: choice
        property string optionName
        property string label
        property var choices: []
        Layout.fillWidth: true
        Text {
            Layout.preferredWidth: 220
            text: choice.label
            font.pixelSize: Theme.fontSmall
            color: Theme.text
        }
        ComboBox {
            objectName: "truehdEncodeOption_" + choice.optionName
            Layout.fillWidth: true
            model: [qsTr("default")].concat(choice.choices)
            enabled: !TruehdEncodeController.busy
            currentIndex: Math.max(0, choice.choices.indexOf(TruehdEncodeController.options[choice.optionName]) + 1)
            onActivated: TruehdEncodeController.setOption(choice.optionName,
                                                          currentIndex === 0 ? "" : choice.choices[currentIndex - 1])
        }
    }

    // One field of the channel meaning that is a number: a cell of the table below, with the
    // sentence that says what the option takes under it when the text is not that.
    component MeaningNumber: ColumnLayout {
        id: cell
        property string optionName
        property string hint
        Layout.fillWidth: true
        Layout.preferredWidth: 1
        Layout.alignment: Qt.AlignTop
        spacing: 2
        TextField {
            objectName: "truehdEncodeOption_" + cell.optionName
            Layout.fillWidth: true
            text: TruehdEncodeController.options[cell.optionName]
            placeholderText: cell.hint
            enabled: !TruehdEncodeController.busy
            font.pixelSize: Theme.fontSmall
            onTextEdited: TruehdEncodeController.setOption(cell.optionName, text)
        }
        Text {
            objectName: "truehdEncodeOptionError_" + cell.optionName
            Layout.fillWidth: true
            visible: text.length > 0
            wrapMode: Text.Wrap
            text: TruehdEncodeController.optionErrors[cell.optionName] || ""
            font.pixelSize: Theme.fontSmall
            color: Theme.bad
        }
    }

    // A control-enabled bit: default (left at zero), on or off.
    component MeaningSwitch: ColumnLayout {
        id: flag
        property string optionName
        Layout.fillWidth: true
        Layout.preferredWidth: 1
        Layout.alignment: Qt.AlignTop
        spacing: 2
        ComboBox {
            objectName: "truehdEncodeOption_" + flag.optionName
            Layout.fillWidth: true
            model: [qsTr("default"), "on", "off"]
            enabled: !TruehdEncodeController.busy
            currentIndex: {
                const text = TruehdEncodeController.options[flag.optionName]
                return text === "on" ? 1 : (text === "off" ? 2 : 0)
            }
            onActivated: TruehdEncodeController.setOption(flag.optionName,
                                                          currentIndex === 0 ? "" : (currentIndex === 1 ? "on" : "off"))
        }
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
            text: qsTr("Codes a WAV file losslessly as TrueHD: the stream decodes to exactly the samples that went in. The WAV's channel mask names the speakers; a file with none is taken in the order forge decode writes. The output is a bare .thd stream, or Matroska (.mkv), MP4 (.mp4) or a Blu-ray transport stream (.m2ts, .ts), whichever its name asks for.")
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

        Button {
            objectName: "truehdEncodeMoreSources"
            text: sourcesPanel.visible ? qsTr("Hide the sources") : qsTr("Several sources…")
            onClicked: sourcesPanel.visible = !sourcesPanel.visible
        }

        ColumnLayout {
            id: sourcesPanel
            objectName: "truehdEncodeSourcesPanel"
            Layout.fillWidth: true
            visible: false
            spacing: Theme.space2

            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Each WAV is a source; the one chosen above is source 0. The map says where each channel of each source goes, as source.channel:place with the places named as TrueHD names them, separated by commas. A source can start some seconds in; a shorter one is padded with silence.")
                font.pixelSize: Theme.fontSmall
                color: Theme.neutral700
            }
            Repeater {
                model: 1 + TruehdEncodeController.extraSources.length
                delegate: ColumnLayout {
                    id: sourceRow
                    required property int index
                    Layout.fillWidth: true
                    spacing: 2
                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            Layout.preferredWidth: 90
                            text: qsTr("Source %1").arg(sourceRow.index)
                            font.pixelSize: Theme.fontSmall
                            color: Theme.text
                        }
                        Text {
                            objectName: "truehdEncodeSourceName_" + sourceRow.index
                            Layout.fillWidth: true
                            elide: Text.ElideMiddle
                            text: sourceRow.index === 0 ? TruehdEncodeController.sourcePath
                                                        : TruehdEncodeController.extraSources[sourceRow.index - 1]
                            font.pixelSize: Theme.fontSmall
                            color: Theme.text
                        }
                        Text {
                            text: qsTr("Starts at (s)")
                            font.pixelSize: Theme.fontSmall
                            color: Theme.text
                        }
                        TextField {
                            objectName: "truehdEncodeOffset_" + sourceRow.index
                            Layout.preferredWidth: 80
                            text: TruehdEncodeController.offsetTexts[sourceRow.index] || ""
                            placeholderText: "0"
                            enabled: !TruehdEncodeController.busy
                            font.pixelSize: Theme.fontSmall
                            onTextEdited: TruehdEncodeController.setOffset(sourceRow.index, text)
                        }
                        Button {
                            objectName: "truehdEncodeRemoveSource_" + sourceRow.index
                            visible: sourceRow.index > 0
                            text: qsTr("Remove")
                            enabled: !TruehdEncodeController.busy
                            onClicked: TruehdEncodeController.removeSource(sourceRow.index - 1)
                        }
                    }
                    Text {
                        objectName: "truehdEncodeOffsetError_" + sourceRow.index
                        Layout.fillWidth: true
                        visible: text.length > 0
                        wrapMode: Text.WordWrap
                        text: TruehdEncodeController.offsetErrors[sourceRow.index] || ""
                        font.pixelSize: Theme.fontSmall
                        color: Theme.bad
                    }
                }
            }
            Button {
                objectName: "truehdEncodeAddSource"
                text: qsTr("Add a WAV…")
                enabled: !TruehdEncodeController.busy
                onClicked: extraSourceDialog.open()
            }
            RowLayout {
                Layout.fillWidth: true
                Text {
                    Layout.preferredWidth: 220
                    text: qsTr("Where each channel goes")
                    font.pixelSize: Theme.fontSmall
                    color: Theme.text
                }
                TextField {
                    objectName: "truehdEncodeMap"
                    Layout.fillWidth: true
                    text: TruehdEncodeController.mapText
                    placeholderText: "0.0:L,0.1:R,1.0:C,2.0:LFE,2.1:Ls,2.2:Rs"
                    enabled: !TruehdEncodeController.busy
                    font.pixelSize: Theme.fontSmall
                    onTextEdited: TruehdEncodeController.setMap(text)
                }
            }
            Text {
                objectName: "truehdEncodeMapError"
                Layout.fillWidth: true
                visible: text.length > 0
                wrapMode: Text.WordWrap
                text: TruehdEncodeController.mapError
                font.pixelSize: Theme.fontSmall
                color: Theme.bad
            }
        }

        Button {
            objectName: "truehdEncodeMoreOptions"
            text: optionsPanel.visible ? qsTr("Fewer options") : qsTr("More options…")
            onClicked: optionsPanel.visible = !optionsPanel.visible
        }

        ColumnLayout {
            id: optionsPanel
            objectName: "truehdEncodeOptionsPanel"
            Layout.fillWidth: true
            visible: false
            spacing: Theme.space2

            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("These are the truehd- options of forge truehd-encode. Leave one empty for the encoder's own choice.")
                font.pixelSize: Theme.fontSmall
                color: Theme.neutral700
            }
            TextOption {
                optionName: "restart"
                label: qsTr("Access units between restarts")
                hint: qsTr("1 or 8 to 4096")
            }
            TextOption {
                optionName: "major-sync"
                label: qsTr("Access units between major syncs")
                hint: qsTr("0 or a multiple of the restart, up to 65536")
            }
            TextOption {
                optionName: "peak-rate"
                label: qsTr("Peak data rate (kbit/s)")
                hint: qsTr("1 to 100000")
            }
            TextOption {
                optionName: "bitrate"
                label: qsTr("Hold the rate to (kbit/s)")
                hint: qsTr("1000 to 100000")
            }
            TextOption {
                optionName: "fir-order"
                label: qsTr("Highest prediction filter order")
                hint: qsTr("0 to 8")
            }
            TextOption {
                optionName: "downmix-gains"
                label: qsTr("Downmix gains in dB")
                hint: qsTr("C, Ls and Rs, LFE, Lrs and Rrs: -3,-3,off,-3")
            }
            ChoiceOption {
                optionName: "rate-check"
                label: qsTr("Peak rate check")
                choices: ["strict", "lenient"]
            }
            ChoiceOption {
                optionName: "matrix"
                label: qsTr("Matrix search")
                choices: ["on", "off"]
            }
            ChoiceOption {
                optionName: "stereo-mode"
                label: qsTr("Stereo presentation")
                choices: ["stereo", "ltrt", "headphone"]
            }
            Button {
                objectName: "truehdEncodeResetOptions"
                text: qsTr("Back to the defaults")
                enabled: !TruehdEncodeController.busy
                onClicked: TruehdEncodeController.resetOptions()
            }
        }

        Button {
            objectName: "truehdEncodeMeaningToggle"
            text: meaningPanel.visible ? qsTr("Hide channel meaning") : qsTr("Channel meaning…")
            onClicked: meaningPanel.visible = !meaningPanel.visible
        }

        ColumnLayout {
            id: meaningPanel
            objectName: "truehdEncodeMeaningPanel"
            Layout.fillWidth: true
            visible: false
            spacing: Theme.space2

            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("The channel meaning of each presentation (TH spec §14.1), written into every major sync of the stream. Provisional: no decoder here applies it and what the values mean is not settled, so there is no oracle until the DEE set. A field left empty is written as zero.")
                font.pixelSize: Theme.fontSmall
                color: Theme.neutral700
            }

            GridLayout {
                Layout.fillWidth: true
                columns: 5
                columnSpacing: Theme.space2
                rowSpacing: Theme.space2

                Item { Layout.preferredWidth: 80 }
                Text { Layout.fillWidth: true; Layout.preferredWidth: 1; text: qsTr("Dialogue norm"); font.pixelSize: Theme.fontSmall; font.weight: Font.Bold; color: Theme.text }
                Text { Layout.fillWidth: true; Layout.preferredWidth: 1; text: qsTr("Mix level"); font.pixelSize: Theme.fontSmall; font.weight: Font.Bold; color: Theme.text }
                Text { Layout.fillWidth: true; Layout.preferredWidth: 1; text: qsTr("Source format"); font.pixelSize: Theme.fontSmall; font.weight: Font.Bold; color: Theme.text }
                Text { Layout.fillWidth: true; Layout.preferredWidth: 1; text: qsTr("Control"); font.pixelSize: Theme.fontSmall; font.weight: Font.Bold; color: Theme.text }

                Text { Layout.preferredWidth: 80; Layout.alignment: Qt.AlignTop; text: qsTr("2 channel"); font.pixelSize: Theme.fontSmall; color: Theme.text }
                MeaningNumber { optionName: "dialnorm-2ch"; hint: "0–63" }
                MeaningNumber { optionName: "mix-level-2ch"; hint: "0–63" }
                Item { Layout.fillWidth: true; Layout.preferredWidth: 1 }
                MeaningSwitch { optionName: "control-2ch" }

                Text { Layout.preferredWidth: 80; Layout.alignment: Qt.AlignTop; text: qsTr("6 channel"); font.pixelSize: Theme.fontSmall; color: Theme.text }
                MeaningNumber { optionName: "dialnorm-6ch"; hint: "0–31" }
                MeaningNumber { optionName: "mix-level-6ch"; hint: "0–63" }
                MeaningNumber { optionName: "source-format-6ch"; hint: "0–31" }
                MeaningSwitch { optionName: "control-6ch" }

                Text { Layout.preferredWidth: 80; Layout.alignment: Qt.AlignTop; text: qsTr("8 channel"); font.pixelSize: Theme.fontSmall; color: Theme.text }
                MeaningNumber { optionName: "dialnorm-8ch"; hint: "0–31" }
                MeaningNumber { optionName: "mix-level-8ch"; hint: "0–63" }
                MeaningNumber { optionName: "source-format-8ch"; hint: "0–63" }
                MeaningSwitch { optionName: "control-8ch" }
            }

            GridLayout {
                Layout.fillWidth: true
                columns: 4
                columnSpacing: Theme.space2
                rowSpacing: Theme.space2

                Text { Layout.preferredWidth: 160; Layout.alignment: Qt.AlignTop; text: qsTr("Heavy DRC start-up gain"); font.pixelSize: Theme.fontSmall; color: Theme.text; wrapMode: Text.WordWrap }
                MeaningNumber { optionName: "heavy-drc-start-up-gain"; hint: "-32–31" }
                Text { Layout.preferredWidth: 160; Layout.alignment: Qt.AlignTop; text: qsTr("DRC start-up gain"); font.pixelSize: Theme.fontSmall; color: Theme.text; wrapMode: Text.WordWrap }
                MeaningNumber { optionName: "drc-start-up-gain"; hint: "-64–63" }
            }
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
