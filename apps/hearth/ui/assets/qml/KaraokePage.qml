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
// karaoke, a bitstream output, an Lt/Rt fold). The lyrics of the item sit
// under them, and the full-screen view is one button away (K5).
ScrollView {
    id: root

    // Asked for by the bitstream notice's button; Main.qml opens the output
    // picker, as the Play page's signal path does.
    signal outputPickerRequested()
    // Asked for by the lyrics card's button, and by F11 through Main.qml, which
    // puts the window full screen with the singer view.
    signal lyricsFullScreenRequested()

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
    // The level of each part against Table C.2.3, in dB (capable only), and the
    // span of the controls: the bottom is off (ac3::kKaraokeMinLevelDb and
    // kKaraokeMaxLevelDb, iclforge/ac3/decoder/output.hpp).
    readonly property real minLevelDb: -24
    readonly property real maxLevelDb: 12
    readonly property real melodyDb: root.settings.karaokeMelodyDb ?? 0
    readonly property real vocal1Db: root.settings.karaokeVocal1Db ?? 0
    readonly property real vocal2Db: root.settings.karaokeVocal2Db ?? 0
    readonly property bool anyLevelMoved: root.melodyDb !== 0 || root.vocal1Db !== 0
                                          || root.vocal2Db !== 0
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

    // A duet (planning/karaoke-phase2.md, W11b): which lines the vocal that is playing sings,
    // and so which are left for you.
    function duetSummary() {
        const first = KaraokeController.firstSingerLabel;
        const second = KaraokeController.secondSingerLabel;
        if (!root.isKaraoke) {
            return qsTr("The lines are marked for %1 and for %2.").arg(first).arg(second);
        }
        if (root.reproduction !== "capable" || root.vocals === "both") {
            return qsTr("Both vocals play, so %1 and %2 are both in the mix.").arg(first).arg(second);
        }
        if (root.vocals === "none") {
            return qsTr("No vocal plays: you sing the lines of %1 and of %2.").arg(first).arg(second);
        }
        return root.vocals === "v1"
            ? qsTr("The vocal of %1 plays: you sing the lines of %2.").arg(first).arg(second)
            : qsTr("The vocal of %1 plays: you sing the lines of %2.").arg(second).arg(first);
    }

    function clock(ms) {
        const seconds = Math.max(0, Math.floor(ms / 1000));
        return Math.floor(seconds / 60) + ":" + ("0" + (seconds % 60)).slice(-2);
    }

    function fileNameOf(path) {
        if (!path) {
            return "";
        }
        const cut = Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\"));
        return cut >= 0 ? path.substring(cut + 1) : path;
    }

    // A part pulled to the bottom of its control is off. Only the capable
    // decoder has levels: the aware and as-coded columns play the stream's own.
    function off(part) {
        if (root.reproduction !== "capable") {
            return false;
        }
        const db = part === "M" ? root.melodyDb : (part === "V1" ? root.vocal1Db : root.vocal2Db);
        return db <= root.minLevelDb;
    }

    // Whether a level control has anything to act on: the stream has the part
    // and the vocals chosen include it. With no stream loaded they are open,
    // as the vocals' own choice is.
    function levelApplies(part) {
        if (!root.isKaraoke) {
            return true;
        }
        if (root.parts.indexOf(part) < 0) {
            return false;
        }
        if (part === "V1") {
            return root.vocals === "v1" || root.vocals === "both";
        }
        if (part === "V2") {
            return root.vocals === "v2" || root.vocals === "both";
        }
        return true;
    }

    function levelText(db) {
        if (db <= root.minLevelDb) {
            return qsTr("off");
        }
        return qsTr("%1 dB").arg((db > 0 ? "+" : "") + Math.round(db));
    }

    // Whether a part of the stream is heard under the current choice.
    function heard(part) {
        if (!root.isKaraoke || root.parts.indexOf(part) < 0) {
            return false;
        }
        if (part === "L" || part === "R") {
            return true;
        }
        if (part === "M") {
            return !root.off("M");
        }
        if (root.reproduction !== "capable") {
            return true;  // as coded and aware both play the vocals
        }
        if (part === "V1") {
            return !root.off("V1") && (root.vocals === "v1" || root.vocals === "both");
        }
        return !root.off("V2") && (root.vocals === "v2" || root.vocals === "both");
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
        // A part pulled to off is not where it would be.
        const v1 = root.hasVocal1 && !root.off("V1");
        const v2 = root.hasVocal2 && !root.off("V2");
        const melody = root.hasMelody && !root.off("M");
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
        // Table C.2.3's choice, as the decoder applies it to what the stream
        // has: "both" is the V1+V2 column - V1 left and V2 right, so a stream
        // with a single vocal puts it on the left alone - and one vocal alone
        // is a phantom centre.
        const both = !aware && root.vocals === "both";
        const onlyV1 = !aware && root.vocals === "v1" && v1;
        const onlyV2 = !aware && root.vocals === "v2" && v2;
        if (!root.folded) {
            if (melody) { out.C.push("M"); }
            if (aware) {
                // A single vocal joins the melody; a pair goes to the sides.
                if (root.vocalCount === 1 && v1) { out.C.push("V1"); }
                if (root.vocalCount === 2) { out.L.push("V1"); out.R.push("V2"); }
            } else if (both) {
                if (v1) { out.L.push("V1"); }
                if (v2) { out.R.push("V2"); }
            } else if (onlyV1) {
                out.C.push("V1");
            } else if (onlyV2) {
                out.C.push("V2");
            }
            return out;
        }
        // 2/0: the melody in both at the centre level, a single vocal in both
        // at 0.7, a pair on their own sides.
        if (melody) { out.L.push("M"); out.R.push("M"); }
        if (aware) {
            if (root.vocalCount === 1 && v1) { out.L.push("V1"); out.R.push("V1"); }
            if (root.vocalCount === 2) { out.L.push("V1"); out.R.push("V2"); }
        } else if (both) {
            if (v1) { out.L.push("V1"); }
            if (v2) { out.R.push("V2"); }
        } else if (onlyV1) {
            out.L.push("V1"); out.R.push("V1");
        } else if (onlyV2) {
            out.L.push("V2"); out.R.push("V2");
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

                    // The level of each part (Annex C.2.3's "additional
                    // flexibility", the capable decoder's).
                    Repeater {
                        model: [
                            { part: "M", key: "karaokeMelodyDb", label: qsTr("Melody") },
                            { part: "V1", key: "karaokeVocal1Db", label: qsTr("Vocal 1") },
                            { part: "V2", key: "karaokeVocal2Db", label: qsTr("Vocal 2") }
                        ]
                        delegate: RowLayout {
                            id: levelRow
                            required property var modelData
                            readonly property real db: root.settings[levelRow.modelData.key] ?? 0
                            Layout.fillWidth: true
                            spacing: Theme.gap
                            Text { text: levelRow.modelData.label; color: Theme.text; font.pixelSize: Theme.fontNormal
                                   elide: Text.ElideRight; Layout.preferredWidth: Math.round(108 * Theme.fontScale) }
                            AppSlider {
                                id: levelSlider
                                objectName: "level-" + levelRow.modelData.part
                                Layout.preferredWidth: Math.round(200 * Theme.fontScale)
                                enabled: root.applicable && root.reproduction === "capable"
                                         && root.levelApplies(levelRow.modelData.part)
                                from: root.minLevelDb; to: root.maxLevelDb; stepSize: 1
                                snapMode: Slider.SnapAlways
                                value: levelRow.db
                                Accessible.name: qsTr("%1 level, dB").arg(levelRow.modelData.label)
                                onMoved: root.set(levelRow.modelData.key, value)
                                // A drag writes `value` directly and the binding
                                // above is gone, so it is resynced from the
                                // controller - DecoderAc4.qml's sliders' shape.
                                Connections {
                                    target: HearthController
                                    function onDecoderSettingsChanged() {
                                        levelSlider.value = levelRow.db;
                                    }
                                }
                            }
                            Text {
                                objectName: "levelText-" + levelRow.modelData.part
                                text: root.levelText(levelRow.db)
                                color: Theme.textMuted
                                font.family: Theme.monoFamily
                                font.pixelSize: Theme.fontNormal
                            }
                            Item { Layout.fillWidth: true }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Each part's level against Table C.2.3: 0 dB is the table, and "
                                       + "the bottom of the control is off. Raising one part far enough "
                                       + "lowers the others, so that no speaker is driven past full scale. "
                                       + "The capable decoder only.")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                            wrapMode: Text.WordWrap
                        }
                        Button {
                            objectName: "resetLevelsButton"
                            text: qsTr("Reset levels")
                            enabled: root.anyLevelMoved
                            onClicked: HearthController.setDecoderSettings({
                                karaokeMelodyDb: 0, karaokeVocal1Db: 0, karaokeVocal2Db: 0
                            })
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
                        text: qsTr("This is going to a network group. A sink that takes the karaoke "
                                   + "settings decodes the stream itself and reproduces it as chosen "
                                   + "here; one that does not is sent PCM with the reproduction "
                                   + "applied, so the group plays one reproduction. The Network page "
                                   + "says which each sink was sent.")
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

                Card {
                    id: lyricsCard
                    ordinal: "03"
                    title: qsTr("Lyrics")
                    summary: lyricView.lineCount > 0
                             ? qsTr("%1 · line %2 of %3")
                                   .arg(root.clock(lyricClock.positionMs))
                                   .arg(Math.max(1, lyricView.lineNumber)).arg(lyricView.lineCount)
                             : ""
                    framed: true

                    LyricClock {
                        id: lyricClock
                        active: root.visible && root.isKaraoke
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap
                        Text {
                            objectName: "lyricsFile"
                            Layout.fillWidth: true
                            text: KaraokeController.fileName.length > 0
                                  ? KaraokeController.fileName
                                    + (KaraokeController.status === "timed"
                                       ? (KaraokeController.wordTimed ? qsTr(", word-timed")
                                                                       : qsTr(", line-timed"))
                                       : "")
                                  : ""
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                            elide: Text.ElideMiddle
                        }
                        AppButton {
                            objectName: "timeLyricsButton"
                            enabled: root.loaded
                            text: qsTr("Time the lyrics…")
                            onClicked: timingDialog.open()
                        }
                        AppButton {
                            objectName: "lyricsFullScreenButton"
                            text: qsTr("Full screen · F11")
                            onClicked: root.lyricsFullScreenRequested()
                        }
                    }

                    // Stamps the words of the item as it plays (planning/karaoke-phase2.md, W6).
                    LyricTimingDialog { id: timingDialog }

                    LyricView {
                        id: lyricView
                        objectName: "lyricsView"
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.round(240 * Theme.fontScale)
                        positionMs: lyricClock.positionMs
                    }

                    Text {
                        objectName: "lyricsNote"
                        Layout.fillWidth: true
                        visible: text.length > 0
                        text: KaraokeController.status === "plain"
                              ? qsTr("These lyrics have no times, so they are shown whole and not followed.")
                              : (KaraokeController.status === "picture"
                                 ? qsTr("This is the picture of a CD+G disc. Its words are part of the "
                                        + "picture, so there is no text to follow.")
                                 : (KaraokeController.status === "timed" && !KaraokeController.wordTimed
                                    ? qsTr("This file times lines, not words: a whole line is emphasised "
                                           + "as it is sung.")
                                    : ""))
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }

                    // A duet: the file marks who sings which line. Two names, typed for this
                    // run of the app and never kept for a song, say who V1 and V2 are, and one
                    // sentence says which lines the vocal that is playing sings.
                    ColumnLayout {
                        objectName: "duetBlock"
                        Layout.fillWidth: true
                        visible: KaraokeController.hasSingers
                        spacing: Theme.gap

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.gap
                            Text { text: qsTr("First singer"); color: Theme.text; font.pixelSize: Theme.fontNormal
                                   elide: Text.ElideRight; Layout.preferredWidth: Math.round(108 * Theme.fontScale) }
                            AppTextField {
                                id: firstSingerField
                                objectName: "firstSingerField"
                                Layout.fillWidth: true
                                placeholderText: KaraokeController.fileFirstSingerName.length > 0
                                                 ? KaraokeController.fileFirstSingerName : "V1"
                                text: KaraokeController.firstSingerName
                                Accessible.name: qsTr("Name of the first singer (V1)")
                                onEditingFinished: KaraokeController.firstSingerName = text
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.gap
                            Text { text: qsTr("Second singer"); color: Theme.text; font.pixelSize: Theme.fontNormal
                                   elide: Text.ElideRight; Layout.preferredWidth: Math.round(108 * Theme.fontScale) }
                            AppTextField {
                                id: secondSingerField
                                objectName: "secondSingerField"
                                Layout.fillWidth: true
                                placeholderText: KaraokeController.fileSecondSingerName.length > 0
                                                 ? KaraokeController.fileSecondSingerName : "V2"
                                text: KaraokeController.secondSingerName
                                Accessible.name: qsTr("Name of the second singer (V2)")
                                onEditingFinished: KaraokeController.secondSingerName = text
                            }
                        }
                        Text {
                            objectName: "duetSummary"
                            Layout.fillWidth: true
                            text: root.duetSummary()
                            color: Theme.text
                            font.pixelSize: Theme.fontSmall
                            wrapMode: Text.WordWrap
                        }
                        // A drag or a keystroke broke the bindings above: the controller is the
                        // one place the names are held.
                        Connections {
                            target: KaraokeController
                            function onSingerNamesChanged() {
                                firstSingerField.text = KaraokeController.firstSingerName;
                                secondSingerField.text = KaraokeController.secondSingerName;
                            }
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

                // A singer's microphone and a pitch score against the tune (planning/karaoke-
                // microphone.md, M4). Off until it is turned on, and closed when this page is left.
                MicrophoneCard {
                    objectName: "microphoneCard"
                    reproduction: root.reproduction
                    vocals: root.vocals
                    active: root.visible && root.isKaraoke
                    positionMs: lyricClock.positionMs
                }

                Card {
                    ordinal: "05"
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
                    ordinal: "06"
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
                    ordinal: "07"
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
