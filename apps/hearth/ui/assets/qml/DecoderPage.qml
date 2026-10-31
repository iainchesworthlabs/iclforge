import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Hearth

// The Decoder tab (planning/hearth-design.md, "Speakers and decoder"): a
// sub-switch between the AC-3/E-AC-3 decoder, the AC-4 decoder
// (planning/ac4.md, I2) and the TrueHD and MLP decoder. It defaults to AC-3/E-AC-3, and turns to whichever
// the playing item needs when that item changes, so the page open is the one
// whose settings are being heard; the switch still shows either at any time.
Item {
    id: root

    property string format: "eac3"
    // Asked for by the karaoke line below; Main.qml shows the Karaoke page.
    signal karaokePageRequested()

    // The playing item's format, once its media information has been read:
    // "ac4", "truehd" for TrueHD and MLP, or "eac3" for AC-3 and E-AC-3; "" while nothing is known.
    readonly property string playingFormat: {
        const codec = HearthController.currentMedia.codec;
        return codec === undefined ? ""
               : (codec === "ac4" ? "ac4" : (codec === "truehd" || codec === "mlp" ? "truehd" : "eac3"));
    }
    onPlayingFormatChanged: {
        if (root.playingFormat.length > 0) {
            root.format = root.playingFormat;
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.pad
        spacing: Theme.gap

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.gap

            SegmentedControl {
                accessibleName: qsTr("Decoder")
                currentValue: root.format
                model: [
                    { value: "eac3", label: qsTr("AC-3 and E-AC-3") },
                    { value: "ac4", label: qsTr("AC-4") },
                    { value: "truehd", label: qsTr("TrueHD and MLP") }
                ]
                onSelected: function(value) { root.format = value; }
            }
            Text {
                text: root.format === "ac4"
                      ? qsTr("Changes reach the playing item at its next frame.")
                      : (root.format === "truehd"
                         ? qsTr("Changes reach the playing item at its next restart, the next point a TrueHD stream can be joined at.")
                         : qsTr("Changes reach the playing item at its next access unit."))
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
            }
            Item { Layout.fillWidth: true }
            // The karaoke reproduction is chosen on its own page; this is
            // where it shows beside the decoder's other options.
            Text {
                objectName: "karaokeSummary"
                text: {
                    const choice = HearthController.decoderSettings.karaoke ?? "aware";
                    return choice === "asCoded" ? qsTr("Karaoke: as coded")
                         : (choice === "capable" ? qsTr("Karaoke: capable") : qsTr("Karaoke: aware"));
                }
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
            }
            Button {
                objectName: "karaokePageButton"
                text: qsTr("Karaoke page")
                onClicked: root.karaokePageRequested()
            }
        }

        // Why the controls below are not what is heard, when they are not: a
        // receiver sent a bitstream decodes it with its own settings. The
        // sentence is the engine's (EngineStatus::settings_note).
        Text {
            objectName: "receiverDecodesNotice"
            Layout.fillWidth: true
            visible: HearthController.decoderSettingsNote.length > 0
            text: HearthController.decoderSettingsNote
            color: Theme.text
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.WordWrap
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: root.format === "ac4" ? 1 : (root.format === "truehd" ? 2 : 0)

            DecoderEac3 { }
            DecoderAc4 { }
            DecoderTruehd { }
        }
    }
}
