import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Hearth

// The Karaoke page (planning/karaoke-design.md, K4; the design sheet's "Hearth:
// Karaoke page, reference view"): what a karaoke stream is, the singer's choice
// of how it is reproduced (Annex C of ETSI TS 102 366), and what comes out of
// which speaker.
//
// Nothing here decides anything the library has not: the choice is two fields of
// DecoderSettings ("karaoke" and "karaokeVocals" in HearthController.
// decoderSettings), the stream's facts are the playing item's media information,
// and the page says in words when the choice cannot apply (a stream that is not
// karaoke, a bitstream output, an Lt/Rt fold). The lyric view is the next PR.
ScrollView {
    id: root

    // Asked for by the bitstream notice's button; Main.qml opens the output
    // picker, as the Play page's signal path does.
    signal outputPickerRequested()

    clip: true
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    readonly property var settings: HearthController.decoderSettings
    readonly property var media: HearthController.currentMedia
    readonly property var programme: (root.media.programmes && root.media.programmes.length > 0)
                                     ? root.media.programmes[0] : ({})
    // A stream is loaded once its media information has been read.
    readonly property bool loaded: root.media.codec !== undefined
    readonly property bool isKaraoke: root.programme.karaoke === true
    readonly property string codec: root.media.codec ?? ""
    readonly property string reproduction: root.settings.karaoke ?? "aware"
    readonly property string vocals: root.settings.karaokeVocals ?? "both"
    // The output's mode, bound to the engine's. A suite may set it, to show the
    // notice a bitstream output earns without a receiver to take one.
    property string outputMode: HearthController.outputFormat.mode ?? "none"
    readonly property bool bitstreamOutput: root.outputMode === "bitstream"
                                            || root.outputMode === "bitstreamAsAc3"
    readonly property bool groupOutput: root.outputMode === "networkGroup"
    // One or two speakers: the decoder's own fold, whose karaoke column is
    // Annex C's 2/0 reproduction.
    readonly property int speakerCount: HearthController.speakerLabels.length
    readonly property bool folded: root.speakerCount > 0 && root.speakerCount <= 2
    readonly property bool ltrtFold: root.folded && root.speakerCount === 2
                                     && root.settings.stereoFold === "ltrt"
    // The choice has nothing to act on for a stream that is not karaoke.
    readonly property bool applicable: !root.loaded || root.isKaraoke
    readonly property var parts: root.isKaraoke ? (root.programme.karaokeParts ?? []) : []
    readonly property bool hasMelody: root.parts.indexOf("M") >= 0
    readonly property bool hasVocal1: root.parts.indexOf("V1") >= 0
    readonly property bool hasVocal2: root.parts.indexOf("V2") >= 0
    readonly property int vocalCount: (root.hasVocal1 ? 1 : 0) + (root.hasVocal2 ? 1 : 0)

    function set(key, value) {
        const next = {};
        next[key] = value;
        HearthController.setDecoderSettings(next);
    }

    function fileNameOf(path) {
        if (!path) {
            return "";
        }
        const cut = Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\"));
        return cut >= 0 ? path.substring(cut + 1) : path;
    }

    // Whether a part of the stream is heard under the current choice.
    function heard(part) {
        if (!root.isKaraoke || root.parts.indexOf(part) < 0) {
            return false;
        }
        if (part === "L" || part === "R" || part === "M") {
            return true;
        }
        if (root.reproduction !== "capable") {
            return true;  // as coded and aware both play the vocals
        }
        if (part === "V1") {
            return root.vocals === "v1" || root.vocals === "both";
        }
        return root.vocals === "v2" || root.vocals === "both";
    }

    // Which parts reach which speaker (Annex C, Tables C.2.2 and C.2.3), as
    // {L: [...], C: [...], R: [...]} with the parts' names. A fold has no C: its
    // two outputs are Annex C's 2/0 columns, and a mono layout is their sum.
    // As coded, a part is where its coded position puts it.
    function reaching() {
        const out = { L: ["L"], C: [], R: ["R"], Ls: [], Rs: [] };
        if (!root.isKaraoke) {
            return out;
        }
        const v1 = root.hasVocal1;
        const v2 = root.hasVocal2;
        const melody = root.hasMelody;
        // Played as coded to a layout that is not folded, each part is where
        // its coded position puts it: the melody in the centre, and the
        // vocals in the surrounds (a single surround sounds from both).
        if (root.reproduction === "asCoded" && !root.folded) {
            if (melody) { out.C.push("M"); }
            if (root.vocalCount === 2) {
                out.Ls.push("V1");
                out.Rs.push("V2");
            } else if (v1) {
                out.Ls.push("V1");
                out.Rs.push("V1");
            }
            return out;
        }
        // A fold takes the stream's own levels whatever is asked, which is
        // the aware 2/0 column: Annex C says any Lo/Ro downmix is it.
        const aware = root.reproduction !== "capable";
        const wantV1 = aware ? v1 : (v1 && (root.vocals === "v1" || root.vocals === "both"));
        const wantV2 = aware ? v2 : (v2 && (root.vocals === "v2" || root.vocals === "both"));
        if (!root.folded) {
            if (melody) { out.C.push("M"); }
            if (aware) {
                // A single vocal joins the melody; a pair goes to the sides.
                if (root.vocalCount === 1 && v1) { out.C.push("V1"); }
                if (root.vocalCount === 2) { out.L.push("V1"); out.R.push("V2"); }
            } else if (wantV1 && wantV2) {
                // V1+V2: left and right at unity (a stream with one vocal puts
                // it in the left alone, Table C.2.3's own reading).
                out.L.push("V1");
                if (v2) { out.R.push("V2"); }
            } else if (wantV1) {
                out.C.push("V1");
            } else if (wantV2) {
                out.C.push("V2");
            }
            return out;
        }
        // 2/0: the melody in both at the centre level, a single vocal in both
        // at 0.7, a pair on their own sides.
        if (melody) { out.L.push("M"); out.R.push("M"); }
        if (wantV1 && wantV2) {
            out.L.push("V1");
            if (v2) { out.R.push("V2"); }
        } else if (wantV1) {
            out.L.push("V1"); out.R.push("V1");
        } else if (wantV2) {
            out.L.push("V2"); out.R.push("V2");
        } else if (aware && root.vocalCount === 1 && v1) {
            out.L.push("V1"); out.R.push("V1");
        }
        return out;
    }

    // What one speaker (by the layout's own name for it) carries.
    function carried(speaker) {
        if (!root.isKaraoke) {
            return "";
        }
        const where = root.reaching();
        let names = [];
        if (speaker === "L") {
            names = where.L;
        } else if (speaker === "R") {
            names = where.R;
        } else if (speaker === "C") {
            names = where.C;
        } else if (speaker === "Ls") {
            names = where.Ls;
        } else if (speaker === "Rs") {
            names = where.Rs;
        } else if (speaker === "LFE") {
            return qsTr("the LFE, as coded");
        } else if (root.folded && root.speakerCount === 1) {
            names = where.L.concat(where.R.filter(function(p) { return where.L.indexOf(p) < 0; }));
        }
        return names.length > 0 ? names.join(" + ") : qsTr("silent");
    }

    function levelBarFraction(index) {
        const levels = HearthController.levels;
        if (index >= levels.length) {
            return 0;
        }
        const db = levels[index].peakDb;
        if (db === undefined || !isFinite(db)) {
            return 0;
        }
        return Math.max(0, Math.min(1, (db + 60) / 60));
    }

    ColumnLayout {
        width: root.availableWidth
        implicitWidth: root.availableWidth
        spacing: Theme.gap * 2

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.gap * 2

            // --- the reproduction --------------------------------------
            ColumnLayout {
                Layout.preferredWidth: 2
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                spacing: Theme.gap * 2

                Card {
                    ordinal: "01"
                    title: qsTr("Karaoke")
                    framed: true

                    Text {
                        objectName: "karaokeStreamName"
                        Layout.fillWidth: true
                        text: root.loaded ? root.fileNameOf(root.media.path) : qsTr("Nothing is playing")
                        color: Theme.text
                        font.pixelSize: Theme.fontTitle
                        font.bold: true
                        elide: Text.ElideMiddle
                    }
                    Text {
                        objectName: "karaokeStreamFacts"
                        Layout.fillWidth: true
                        text: {
                            if (!root.loaded) {
                                return qsTr("Add a karaoke stream to the queue and play it. "
                                            + "What you choose here applies to it.");
                            }
                            if (root.isKaraoke) {
                                return qsTr("Karaoke, %1 coded: %2")
                                    .arg(root.programme.acmodLabel ?? "")
                                    .arg(root.parts.join(" "));
                            }
                            return root.codec === "ac3"
                                ? qsTr("An AC-3 stream, but not a karaoke one: its service is %1.")
                                      .arg(root.programme.bsmodLabel ?? "")
                                : qsTr("Karaoke is an AC-3 mode (Annex C of ETSI TS 102 366). "
                                       + "This is not an AC-3 stream.");
                        }
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                }

                Card {
                    ordinal: "02"
                    title: qsTr("Reproduction")
                    summary: qsTr("Annex C, ETSI TS 102 366")
                    framed: true

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text { text: qsTr("Decoder"); color: Theme.text; font.pixelSize: Theme.fontNormal
                               elide: Text.ElideRight; Layout.preferredWidth: Math.round(108 * Theme.fontScale) }
                        SegmentedControl {
                            objectName: "reproductionChoice"
                            accessibleName: qsTr("Decoder")
                            enabled: root.applicable
                            currentValue: root.reproduction
                            model: [
                                { value: "asCoded", label: qsTr("As coded") },
                                { value: "aware", label: qsTr("Aware") },
                                { value: "capable", label: qsTr("Capable") }
                            ]
                            onSelected: function(value) { root.set("karaoke", value); }
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: root.reproduction === "asCoded"
                              ? qsTr("The channels as the stream codes them: the vocals come from the "
                                     + "surround speakers.")
                              : (root.reproduction === "aware"
                                 ? qsTr("L, C and R, with the vocals at the level the programme set "
                                        + "(Table C.2.2).")
                                 : qsTr("L, C and R, with the vocals you choose (Table C.2.3)."))
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text { text: qsTr("Vocals"); color: Theme.text; font.pixelSize: Theme.fontNormal
                               elide: Text.ElideRight; Layout.preferredWidth: Math.round(108 * Theme.fontScale) }
                        SegmentedControl {
                            objectName: "vocalsChoice"
                            accessibleName: qsTr("Vocals")
                            enabled: root.applicable && root.reproduction === "capable"
                            currentValue: root.vocals
                            model: [
                                { value: "none", label: qsTr("None") },
                                { value: "v1", label: qsTr("V1") },
                                { value: "v2", label: qsTr("V2") },
                                { value: "both", label: qsTr("Both") }
                            ]
                            onSelected: function(value) { root.set("karaokeVocals", value); }
                        }
                    }

                    // The reasons a choice does not apply, in words, each with
                    // the one thing that fixes it.
                    Text {
                        objectName: "notApplicableNotice"
                        Layout.fillWidth: true
                        visible: !root.applicable
                        text: qsTr("These controls do not apply to the stream that is playing.")
                        color: Theme.text
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }

                    ColumnLayout {
                        objectName: "bitstreamNotice"
                        Layout.fillWidth: true
                        visible: root.isKaraoke && root.bitstreamOutput
                        spacing: Theme.gap
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("This is going out as a bitstream, so the receiver decodes it and "
                                       + "the choice above is not applied. Unless the receiver is "
                                       + "karaoke-capable, V1 and V2 play from its surround speakers.")
                            color: Theme.text
                            font.pixelSize: Theme.fontSmall
                            wrapMode: Text.WordWrap
                        }
                        Button {
                            objectName: "chooseOutputButton"
                            text: qsTr("Choose output…")
                            onClicked: root.outputPickerRequested()
                        }
                    }

                    Text {
                        objectName: "groupNotice"
                        Layout.fillWidth: true
                        visible: root.isKaraoke && root.groupOutput
                        text: qsTr("This is going to a network group. A sink that is sent PCM plays the "
                                   + "reproduction; one that is sent coded AC-3 decodes it itself and "
                                   + "plays it as coded.")
                        color: Theme.text
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }

                    ColumnLayout {
                        objectName: "ltrtNotice"
                        Layout.fillWidth: true
                        visible: root.isKaraoke && root.ltrtFold
                        spacing: Theme.gap
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("The stereo fold is Lt/Rt. Annex C defines no Lt/Rt reproduction, "
                                       + "so the stream plays as its Lt/Rt fold and the choice above has "
                                       + "no effect. The Lo/Ro fold reproduces karaoke.")
                            color: Theme.text
                            font.pixelSize: Theme.fontSmall
                            wrapMode: Text.WordWrap
                        }
                        Button {
                            objectName: "useLoRoButton"
                            text: qsTr("Use Lo/Ro")
                            onClicked: root.set("stereoFold", "loro")
                        }
                    }
                }
            }

            // --- the stream and what plays -----------------------------
            ColumnLayout {
                Layout.preferredWidth: 1
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                spacing: Theme.gap * 2

                Card {
                    ordinal: "03"
                    title: qsTr("The stream")
                    framed: true

                    Repeater {
                        model: [
                            { part: "L", name: qsTr("Backing left") },
                            { part: "R", name: qsTr("Backing right") },
                            { part: "M", name: qsTr("Melody guide") },
                            { part: "V1", name: qsTr("Vocal 1") },
                            { part: "V2", name: qsTr("Vocal 2") }
                        ]
                        delegate: RowLayout {
                            id: partRow
                            required property var modelData
                            readonly property bool present: root.parts.indexOf(modelData.part) >= 0
                            objectName: "part-" + modelData.part
                            Layout.fillWidth: true
                            spacing: Theme.gap
                            Text { text: partRow.modelData.part; color: Theme.text
                                   font.pixelSize: Theme.fontNormal; font.bold: true
                                   Layout.preferredWidth: Math.round(32 * Theme.fontScale) }
                            Text { text: partRow.modelData.name; color: Theme.text
                                   font.pixelSize: Theme.fontNormal; Layout.fillWidth: true
                                   elide: Text.ElideRight }
                            Text {
                                objectName: "partState-" + partRow.modelData.part
                                text: !partRow.present ? (root.isKaraoke ? qsTr("not in this stream") : "")
                                      : (root.heard(partRow.modelData.part) ? qsTr("heard") : qsTr("muted"))
                                color: Theme.textMuted
                                font.pixelSize: Theme.fontSmall
                            }
                        }
                    }
                }

                Card {
                    ordinal: "04"
                    title: qsTr("What plays")
                    framed: true

                    Repeater {
                        model: root.isKaraoke ? HearthController.speakerLabels : []
                        delegate: RowLayout {
                            id: speakerRow
                            required property int index
                            required property string modelData
                            objectName: "speaker-" + speakerRow.modelData
                            Layout.fillWidth: true
                            spacing: Theme.gap
                            Text { text: speakerRow.modelData; color: Theme.text
                                   font.pixelSize: Theme.fontNormal; font.bold: true
                                   Layout.preferredWidth: Math.round(32 * Theme.fontScale) }
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 10
                                color: Theme.neutral200
                                Rectangle {
                                    width: parent.width * root.levelBarFraction(speakerRow.index)
                                    height: parent.height
                                    color: Theme.accent
                                }
                            }
                            Text {
                                objectName: "speakerCarries-" + speakerRow.modelData
                                text: root.carried(speakerRow.modelData)
                                color: Theme.textMuted
                                font.pixelSize: Theme.fontSmall
                                Layout.preferredWidth: Math.round(110 * Theme.fontScale)
                                elide: Text.ElideRight
                            }
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: !root.isKaraoke
                        text: qsTr("Nothing karaoke is playing.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                    }
                }

                Card {
                    ordinal: "05"
                    title: qsTr("Signal path")
                    framed: true

                    Text {
                        objectName: "decodeStage"
                        Layout.fillWidth: true
                        text: {
                            if (!root.isKaraoke) {
                                return qsTr("Decoded as the stream says.");
                            }
                            const reproduced = HearthController.thisFrame.karaokeReproduced;
                            const how = root.reproduction === "asCoded" ? qsTr("as coded")
                                : (root.reproduction === "aware" ? qsTr("aware")
                                   : qsTr("capable, vocals: %1").arg(
                                         root.vocals === "none" ? qsTr("none")
                                         : (root.vocals === "both" ? qsTr("both")
                                            : root.vocals.toUpperCase())));
                            return reproduced === false && root.reproduction !== "asCoded"
                                ? qsTr("AC-3 karaoke, %1: not reproduced (see the notice).").arg(how)
                                : qsTr("AC-3 karaoke, %1.").arg(how);
                        }
                        color: Theme.text
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                    Text {
                        objectName: "outputStage"
                        Layout.fillWidth: true
                        text: {
                            const format = HearthController.outputFormat;
                            if (root.outputMode === "none" || format.channels === undefined) {
                                return qsTr("Nothing is playing.");
                            }
                            return qsTr("%1 · %2 ch at %3 Hz").arg(HearthController.deviceName)
                                       .arg(format.channels).arg(format.sampleRate);
                        }
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }
    }
}
