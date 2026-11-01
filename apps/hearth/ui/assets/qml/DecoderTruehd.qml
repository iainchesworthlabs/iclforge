import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Hearth

// The TrueHD and MLP decoder page (planning/truehd-parity-hearth-player.md, H3): the controls
// DecoderSettings::truehd holds, read from and written straight back to
// HearthController.decoderSettings, as DecoderAc4.qml does for AC-4. A change reaches the playing
// item at its next restart, because a TrueHD decoder is built with its presentation and its
// strictness and joins a stream only at a unit with a major sync.
//
// Dialogue normalisation and the dynamic range words are read from the stream and applied by
// nothing (libs/truehd/ERRATA.md, entry 10), so there is no control for either, and the page says
// so rather than showing one that does nothing. The stereo and mono fold of A/52 is not TrueHD's:
// a channel the speaker layout has no speaker for is panned between those it has.
//
// "This stream" reads the playing item's media information
// (HearthController.currentMedia.truehd, hearth_controller.cpp's media_truehd_to_map()).
ScrollView {
    id: root

    readonly property int labelWidth: Math.round(108 * Theme.fontScale)
    clip: true
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    readonly property var settings: HearthController.decoderSettings
    function set(key, value) {
        const next = {};
        next[key] = value;
        HearthController.setDecoderSettings(next);
    }

    readonly property var truehd: HearthController.currentMedia.truehd

    ColumnLayout {
        width: root.availableWidth
        implicitWidth: root.availableWidth
        spacing: Theme.gap * 2

        Card {
            ordinal: "01"
            title: qsTr("Presentation")
            framed: true

            Text {
                Layout.fillWidth: true
                text: qsTr("A TrueHD stream carries a 2 channel, a 6 channel and an 8 channel presentation as "
                          + "substreams, each built on the ones before it. Best plays the highest, and reading "
                          + "fewer is less work. The channels are placed on the speaker layout; one it has no "
                          + "speaker for is panned between those it has.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gap
                Text { text: qsTr("Plays"); color: Theme.text; font.pixelSize: Theme.fontNormal
                       elide: Text.ElideRight; Layout.preferredWidth: root.labelWidth }
                SegmentedControl {
                    accessibleName: qsTr("Presentation")
                    currentValue: root.settings.truehdPresentation ?? "best"
                    model: [
                        { value: "best", label: qsTr("Best") },
                        { value: "2ch", label: qsTr("2 channels") },
                        { value: "6ch", label: qsTr("6 channels") },
                        { value: "8ch", label: qsTr("8 channels") }
                    ]
                    onSelected: function(value) { root.set("truehdPresentation", value); }
                }
            }
        }

        Card {
            ordinal: "02"
            title: qsTr("Checks")
            framed: true

            Text {
                Layout.fillWidth: true
                text: qsTr("Every restart interval of a TrueHD stream is checked against the next restart "
                          + "header, as are the parity and CRC bytes. Strict drops a unit that fails a "
                          + "check and plays silence until the stream's next restart header. Lenient counts "
                          + "the failure and decodes what it can read, which can be wrong until the next "
                          + "restart.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gap
                Text { text: qsTr("When a check fails"); color: Theme.text; font.pixelSize: Theme.fontNormal
                       elide: Text.ElideRight; Layout.preferredWidth: root.labelWidth }
                SegmentedControl {
                    accessibleName: qsTr("When a check fails")
                    currentValue: (root.settings.truehdLenient ?? false) ? "lenient" : "strict"
                    model: [
                        { value: "strict", label: qsTr("Strict") },
                        { value: "lenient", label: qsTr("Lenient") }
                    ]
                    onSelected: function(value) { root.set("truehdLenient", value === "lenient"); }
                }
            }
        }

        Card {
            objectName: "truehdTrack"
            ordinal: "03"
            title: qsTr("Track")
            framed: true

            Text {
                Layout.fillWidth: true
                text: qsTr("A file can hold a TrueHD track and an AC-3, E-AC-3 or AC-4 track, as a Blu-ray "
                          + "remux does. Hearth decodes the TrueHD itself and cannot send it on as it is "
                          + "yet. Automatic plays the AC-3, E-AC-3 or AC-4 track where the output takes "
                          + "that as a bitstream, a receiver, and the TrueHD track everywhere else. TrueHD "
                          + "always plays the TrueHD, and Compressed always the other track. A change "
                          + "applies from the next item.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.gap
                Text { text: qsTr("Plays"); color: Theme.text; font.pixelSize: Theme.fontNormal
                       elide: Text.ElideRight; Layout.preferredWidth: root.labelWidth }
                SegmentedControl {
                    accessibleName: qsTr("Track")
                    currentValue: root.settings.track ?? "automatic"
                    model: [
                        { value: "automatic", label: qsTr("Automatic") },
                        { value: "truehd", label: qsTr("TrueHD") },
                        { value: "compressed", label: qsTr("Compressed") }
                    ]
                    onSelected: function(value) { root.set("track", value); }
                }
            }
        }

        Card {
            ordinal: "04"
            title: qsTr("Level")
            framed: true

            Text {
                objectName: "truehdLevelNotice"
                Layout.fillWidth: true
                text: qsTr("The stream's dialogue normalisation and dynamic range words are read and shown, "
                          + "and nothing applies them: the level is the stream's own. The output is float32 "
                          + "through the operating system's mixer, which is not a bit-exact path.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }
        }

        Card {
            objectName: "truehdThisStream"
            ordinal: "05"
            title: qsTr("This stream")
            framed: true
            flat: true
            summary: root.truehd === undefined ? qsTr("Nothing TrueHD is playing.") : ""

            Text {
                Layout.fillWidth: true
                visible: root.truehd !== undefined
                text: root.truehd === undefined ? ""
                      : qsTr("%1 · %2 kHz · %n substream(s) of %3 channels", "", root.truehd.substreams)
                            .arg(root.truehd.codec === "mlp" ? qsTr("MLP") : qsTr("TrueHD"))
                            .arg(root.truehd.sampleRate / 1000)
                            .arg(root.truehd.channels.join(" · "))
                color: Theme.text
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                visible: root.truehd !== undefined && root.truehd.restartIntervalMs !== undefined
                text: root.truehd?.restartIntervalMs === undefined ? ""
                      : qsTr("A decoder can join it about every %1 ms; a seek decodes from the join point before "
                             + "the place asked for.").arg(Math.round(root.truehd.restartIntervalMs))
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }
        }
    }
}
