import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import QtQuick.Window
import QtCore

import ForgeGui

// Karaoke - the fourth entry beside Guided, Advanced and Expert
// (planning/karaoke-design.md, K6; the design sheet's "Forge GUI: Karaoke flow,
// Parts" and "Hear"): four steps - Parts, Levels, Hear, Save - that build a
// karaoke AC-3 (ETSI TS 102 366 Annex C) from the recordings a singer's backing
// track is made of.
//
// Like Guided it is a wrapper over the SAME draft: there is no flow-local copy of
// the encode. The parts are loaded as sources, the coding mode is the bed, bsmod
// is 7 and each coded channel is an assignment, so that what is set here is
// exactly what Expert shows for the same fields and the command bar's line can
// reproduce it. What the flow adds is the rule that turns parts into those
// settings (KaraokeAuthoring, tested without the window) and a way to hear the
// result the way a singer's player decodes it.
ColumnLayout {
    id: flow

    objectName: "karaokeFlow"
    spacing: 0

    // 'parts' | 'levels' | 'hear' | 'save'
    property string currentStepKey: "parts"
    readonly property var steps: [
        { key: "parts", label: qsTr("Parts"),
          assistant: qsTr("Start with what you recorded. Everything after this follows from it."),
          next: qsTr("Next: levels") },
        { key: "levels", label: qsTr("Levels"),
          assistant: qsTr("How loud the melody guide and the vocals sit when a player mixes them for you."),
          next: qsTr("Next: hear it") },
        { key: "hear", label: qsTr("Hear"),
          assistant: qsTr("If a part sounds wrong, go back to Levels: the centre and surround levels set how the aware decoder mixes them."),
          next: qsTr("Next: save") },
        { key: "save", label: qsTr("Save"),
          assistant: qsTr("Nothing is written until you press the button."),
          next: qsTr("Encode now") },
    ]
    readonly property int currentStepIndex: {
        for (let i = 0; i < steps.length; i++) {
            if (steps[i].key === currentStepKey) return i;
        }
        return 0;
    }
    readonly property var currentStep: steps[currentStepIndex]

    // The enclosing ApplicationWindow, for the encode flow and its command
    // line; the Window attached property answers it for any item.
    readonly property var appWindow: flow.Window.window

    // ---- what the author supplied -------------------------------------------
    // "parts": a file for each of the backing, the melody guide and the vocals;
    // "file": one multichannel file whose speakers name its channels.
    property string inputKind: "parts"
    property string backingUrl: ""
    property string melodyUrl: ""
    property string vocal1Url: ""
    property string vocal2Url: ""
    property string singleUrl: ""
    // The parts that did load, in sourceModel order, and the ones that did not
    // with what the controller said of them.
    property var loadedParts: []
    property var failedParts: []
    // KaraokeAuthoring's plan for them.
    property var plan: ({ buildable: false, mode: "", melody: false, vocals: 0, hasProblem: false,
                          channels: [], checks: [] })

    readonly property var partOrder: ["backing", "melody", "vocal1", "vocal2"]

    function urlOf(part) {
        switch (part) {
        case "backing": return flow.backingUrl;
        case "melody": return flow.melodyUrl;
        case "vocal1": return flow.vocal1Url;
        case "vocal2": return flow.vocal2Url;
        case "file": return flow.singleUrl;
        }
        return "";
    }

    function setUrl(part, url) {
        switch (part) {
        case "backing": flow.backingUrl = url; break;
        case "melody": flow.melodyUrl = url; break;
        case "vocal1": flow.vocal1Url = url; break;
        case "vocal2": flow.vocal2Url = url; break;
        case "file": flow.singleUrl = url; break;
        }
        flow.rebuild();
    }

    function setInputKind(kind) {
        if (kind === flow.inputKind) {
            return;
        }
        flow.inputKind = kind;
        flow.rebuild();
    }

    function fileNameOf(url) {
        const text = String(url);
        const cut = Math.max(text.lastIndexOf("/"), text.lastIndexOf("\\"));
        return cut >= 0 ? text.substring(cut + 1) : text;
    }

    // What the author set on the parts as they are loaded: each part's start offset, and the
    // trim on each of its channels. Replacing the sources takes both back to nothing, so they
    // are read first and put back after, keyed by the part and not by the file (a part
    // replaced by another take keeps what was set for its place).
    property var keptTrims: ({})
    function keptSettings() {
        const kept = { offsets: {}, trims: {} };
        const rows = EncoderController.sourceModel;
        for (let i = 0; i < flow.loadedParts.length && i < rows.length; ++i) {
            if (rows[i].offsetSeconds !== 0) {
                kept.offsets[flow.loadedParts[i].part] = rows[i].offsetSeconds;
            }
        }
        for (const row of EncoderController.assignmentRows) {
            if (row.trimDb !== 0 && row.source < flow.loadedParts.length) {
                kept.trims[flow.loadedParts[row.source].part + ":" + row.channel] = row.trimDb;
            }
        }
        return kept;
    }

    // The parts, loaded into the draft in a fixed order - the first is the
    // primary source, and loading it chooses a bed from its own layout, which
    // this then replaces with the coding mode the parts make.
    function rebuild() {
        const kept = flow.keptSettings();
        flow.keptTrims = kept.trims;
        while (EncoderController.sourceModel.length > 0) {
            EncoderController.removeSource(0);
        }
        // Annex C is AC-3's: E-AC-3 and AC-4 have no karaoke mode.
        EncoderController.codecIndex = 0;
        EncoderController.atmosEnabled = false;
        const wanted = [];
        if (flow.inputKind === "file") {
            if (flow.singleUrl.length > 0) {
                wanted.push({ part: "file", url: flow.singleUrl });
            }
        } else {
            for (const part of flow.partOrder) {
                if (flow.urlOf(part).length > 0) {
                    wanted.push({ part: part, url: flow.urlOf(part) });
                }
            }
        }
        const loaded = [];
        const failed = [];
        for (const item of wanted) {
            const before = EncoderController.sourceModel.length;
            if (before === 0) {
                EncoderController.loadSourceFile(item.url);
            } else {
                EncoderController.addSourceFile(item.url);
            }
            if (EncoderController.sourceModel.length > before) {
                loaded.push(item);
            } else {
                failed.push({ part: item.part, name: flow.fileNameOf(item.url),
                              text: EncoderController.status });
            }
        }
        flow.loadedParts = loaded;
        flow.failedParts = failed;
        for (let i = 0; i < loaded.length; ++i) {
            const offset = kept.offsets[loaded[i].part];
            if (offset !== undefined) {
                EncoderController.setSourceOffset(i, offset);
            }
        }
        // A lyrics file of the first part's name beside it is offered, unless the author has
        // chosen one (or none) by hand.
        if (!flow.lyricsChosen) {
            flow.lyricsUrl = loaded.length > 0 ? KaraokeAuthoring.lyricsBeside(loaded[0].url) : "";
        }
        flow.refreshPlan();
    }

    function refreshPlan() {
        const rows = EncoderController.sourceModel;
        let result;
        if (rows.length === 0 && flow.failedParts.length === 0) {
            // Nothing chosen yet is not a list of problems.
            result = { buildable: false, mode: "", melody: false, vocals: 0, hasProblem: false,
                       channels: [], checks: [] };
        } else if (flow.inputKind === "file") {
            result = rows.length > 0
                ? KaraokeAuthoring.planSpeakers({ name: rows[0].label, channels: rows[0].channels,
                                                  rate: rows[0].rate, seconds: rows[0].seconds,
                                                  speakers: rows[0].speakers })
                : KaraokeAuthoring.planSpeakers({});
        } else {
            const files = [];
            for (let i = 0; i < flow.loadedParts.length && i < rows.length; ++i) {
                files.push({ part: flow.loadedParts[i].part, name: rows[i].label,
                             channels: rows[i].channels, rate: rows[i].rate,
                             seconds: rows[i].seconds });
            }
            result = KaraokeAuthoring.planParts(files);
        }
        flow.plan = result;
        if (result.buildable && flow.failedParts.length === 0) {
            flow.apply(result);
        }
    }

    // The plan, written into the draft: the bed, bsmod 7, and one assignment
    // for each coded channel. Everything else stays where the author left it.
    function apply(result) {
        EncoderController.bedLfe = false;
        const beds = EncoderController.bedChoices;
        for (let i = 0; i < beds.length; ++i) {
            if (beds[i].id === result.mode) {
                EncoderController.bedIndex = i;
            }
        }
        // Table 5.7: 7 is "voice over / karaoke", which at a mode above 1/0 is
        // the karaoke main service.
        EncoderController.bsmodIndex = 7;
        for (const channel of result.channels) {
            EncoderController.setAssignment(channel.file, channel.channel, channel.position);
            // A trim the author had set on this part's channel, put back.
            const part = flow.loadedParts[channel.file] ? flow.loadedParts[channel.file].part : "";
            const trim = flow.keptTrims[part + ":" + channel.channel];
            if (trim !== undefined && trim !== 0) {
                EncoderController.setAssignmentTrim(channel.file, channel.channel, trim);
            }
        }
        // The rate a mode of this width starts at (the encoder's own 192 is stereo's), unless the
        // author has picked one.
        if (!flow.rateChosen) {
            EncoderController.bitrateKbps = KaraokeAuthoring.defaultBitrateKbps(result.mode);
        }
    }

    // Set when the author picks a rate on the Save step, after which the flow leaves it alone.
    property bool rateChosen: false

    // ---- the lyrics file that goes beside the stream --------------------------------
    // A `.lrc` the player finds by the stream's name (Hearth's L1). Offered from beside the
    // first part when one of its name is there; the author's own choice, or removal, wins.
    property string lyricsUrl: ""
    property bool lyricsChosen: false
    // What became of the copy, as a sentence; empty until a stream has been saved.
    property string lyricsNote: ""

    function placeLyrics() {
        flow.lyricsNote = "";
        if (flow.lyricsUrl.length === 0 || EncoderController.outputIsFolder()) {
            return;
        }
        const result = KaraokeAuthoring.copyLyrics(flow.lyricsUrl, EncoderController.outputPath);
        switch (result) {
        case "copied":
            flow.lyricsNote = qsTr("The lyrics are beside the stream, as the same name with .lrc.");
            break;
        case "already-there":
            flow.lyricsNote = qsTr("The lyrics are already beside the stream.");
            break;
        case "exists":
            flow.lyricsNote = qsTr("A lyrics file of that name was already beside the stream, and "
                                   + "was left as it is.");
            break;
        case "unreadable":
            flow.lyricsNote = qsTr("The lyrics file could not be read, so none was put beside the stream.");
            break;
        default:
            flow.lyricsNote = qsTr("The lyrics could not be written beside the stream.");
        }
    }

    // ---- projects ----------------------------------------------------------------
    // What a project (ProjectFile; Main.qml's captureProject and applyProject) keeps of this
    // flow. The paths are the sources', kept by the window; the flow adds which of them is
    // which part, and how the stream is heard.

    // The part each loaded source is, in sourceModel order, so that the same files are the
    // same parts when the project is opened.
    function loadedPartNames() {
        const names = [];
        for (const item of flow.loadedParts) {
            names.push(item.part);
        }
        return names;
    }

    function projectState() {
        return { input: flow.inputKind, listening: flow.listening, vocals: flow.vocalsChoice };
    }

    // Opening a project: the parts it names are loaded as if each had been chosen, and how the
    // stream is heard is put back. `parts` maps a part name (or "file") to a file URL; a part
    // it does not name is left empty. A listening or vocals the flow does not know is left
    // as it is, which is the same thing a newer project's extra value should do.
    function applyProject(parts, input, listening, vocals) {
        // A project is a state the author saved, not a draft to default: the rate it names (the
        // window applied it just before) is theirs, the previous session's offsets and trims are
        // not the project's, and its lyrics are offered afresh from beside its backing.
        flow.rateChosen = true;
        flow.lyricsChosen = false;
        flow.lyricsUrl = "";
        flow.lyricsNote = "";
        flow.loadedParts = [];
        flow.keptTrims = {};
        flow.inputKind = input === "file" || parts.file !== undefined ? "file" : "parts";
        flow.backingUrl = parts.backing || "";
        flow.melodyUrl = parts.melody || "";
        flow.vocal1Url = parts.vocal1 || "";
        flow.vocal2Url = parts.vocal2 || "";
        flow.singleUrl = parts.file || "";
        flow.currentStepKey = "parts";
        flow.rebuild();
        if (["asCoded", "aware", "capable"].indexOf(listening) >= 0) {
            flow.chooseListening(listening);
        }
        if (["none", "v1", "v2", "both"].indexOf(vocals) >= 0) {
            flow.chooseVocals(vocals);
        }
    }

    // ---- sentences ---------------------------------------------------------
    function partName(part) {
        switch (part) {
        case "backing": return qsTr("Backing");
        case "melody": return qsTr("Melody guide");
        case "vocal1": return qsTr("Vocal 1");
        case "vocal2": return qsTr("Vocal 2");
        }
        return qsTr("File");
    }

    function checkText(check) {
        const a = check.args;
        switch (check.code) {
        case "no-backing":
            return qsTr("A karaoke stream needs a backing: the music on the left and the right.");
        case "backing-not-stereo":
            return qsTr("%1 has %2 channel(s); the backing is the left and the right, so it needs two.")
                .arg(a[0]).arg(a[1]);
        case "backing-is-a-bed":
            return qsTr("%1 has %2 channels, already a surround bed. Give the backing as a stereo file, "
                        + "or use it as the one multichannel file whose speakers name the parts.")
                .arg(a[0]).arg(a[1]);
        case "part-not-mono":
            return qsTr("%1 has %2 channels; each melody and vocal is one channel.").arg(a[0]).arg(a[1]);
        case "vocal2-needs-vocal1":
            return qsTr("%1 is Vocal 2, and a stream has no mode that carries Vocal 2 without Vocal 1. "
                        + "Use it as Vocal 1.").arg(a[0]);
        case "nothing-to-sing":
            return qsTr("A backing alone is stereo, with nothing to sing along to. Add a melody guide "
                        + "or a vocal.");
        case "length-differs":
            return qsTr("%1 is %2 and the backing is %3: the shorter part is padded with silence.")
                .arg(a[0]).arg(a[1]).arg(a[2]);
        case "rate-differs":
            return qsTr("%1 is %2 kHz and the backing is %3 kHz: it is resampled to the backing's rate.")
                .arg(a[0]).arg(a[1]).arg(a[2]);
        case "parts-aligned":
            return qsTr("All parts are %1 at %2 kHz.").arg(a[0]).arg(a[1]);
        case "each-part-one-place":
            return qsTr("Each part is one place in the stream.");
        case "no-file":
            return qsTr("Choose a file.");
        case "speakers-unnamed":
            return qsTr("%1 has %2 channels and does not say which speakers they are, so the parts "
                        + "cannot be told apart. Use separate parts, or a file whose speakers are named.")
                .arg(a[0]).arg(a[1]);
        case "speakers-not-karaoke":
            return qsTr("The speakers (%1) are not one of Annex C's five layouts: L C R, L R S, "
                        + "L C R S, L R Ls Rs or L C R Ls Rs.").arg(a[0]);
        case "lfe-present":
            return qsTr("%1 has an LFE channel. Karaoke streams made here carry none: use a file without it.")
                .arg(a[0]);
        }
        return check.code;
    }

    function reasonText() {
        if (flow.plan.mode.length === 0) {
            return "";
        }
        const melody = flow.plan.melody;
        const vocals = flow.plan.vocals;
        if (melody && vocals === 0) return qsTr("Chosen from what you supplied: a backing and a melody guide.");
        if (!melody && vocals === 1) return qsTr("Chosen from what you supplied: a backing and one vocal.");
        if (melody && vocals === 1) return qsTr("Chosen from what you supplied: a melody guide and one vocal.");
        if (!melody && vocals === 2) return qsTr("Chosen from what you supplied: a backing and two vocals.");
        return qsTr("Chosen from what you supplied: a melody guide and two vocals.");
    }

    function partsText() {
        const names = [];
        for (const channel of flow.plan.channels) {
            names.push(channel.part);
        }
        return names.join(" ");
    }

    // ---- the Hear step: the stream as built, played as a singer's player does
    // "idle" | "encoding" | "decoding" | "ready" | "failed"
    property string previewState: "idle"
    property string previewError: ""
    property int previewContainer: 0
    readonly property url previewUrl: StandardPaths.writableLocation(StandardPaths.TempLocation)
                                      + "/forge-karaoke-preview.ac3"
    property string listening: "aware"
    property string vocalsChoice: "both"

    function buildPreview() {
        if (!flow.plan.buildable || EncoderController.busy) {
            return;
        }
        flow.previewState = "encoding";
        flow.previewError = "";
        StreamPlayerController.pause();
        // The player reads an elementary stream, whatever the final container.
        flow.previewContainer = EncoderController.containerIndex;
        EncoderController.containerIndex = 0;
        // The stream is built to be heard, not saved: it is not a run of its own.
        EncoderController.encodePreviewTo(flow.previewUrl);
    }

    Connections {
        target: EncoderController
        function onEncodeFinished(ok, message) {
            if (flow.previewState !== "encoding") {
                if (ok && flow.currentStepKey === "save") {
                    flow.placeLyrics();
                }
                return;
            }
            EncoderController.containerIndex = flow.previewContainer;
            if (!ok) {
                flow.previewState = "failed";
                flow.previewError = message;
                return;
            }
            // The choice is set before the file is opened so that the first
            // decode already applies it.
            StreamPlayerController.karaokeReproduction = flow.listening;
            StreamPlayerController.karaokeVocals = flow.vocalsChoice;
            flow.previewState = "decoding";
            flow.openPreview();
        }
    }

    // The player refuses a file while it is still decoding another (the choice
    // of an earlier stream, say), so the built stream waits its turn.
    property bool previewWaiting: false
    function openPreview() {
        if (StreamPlayerController.busy) {
            flow.previewWaiting = true;
            return;
        }
        flow.previewWaiting = false;
        StreamPlayerController.openFile(flow.previewUrl);
    }

    // The stream is ready to hear once the player has decoded what was built,
    // not when the encoder is done: a result from an earlier stream is not it.
    Connections {
        target: StreamPlayerController
        function onBusyChanged() {
            if (StreamPlayerController.busy) {
                return;
            }
            if (flow.previewWaiting) {
                flow.openPreview();
                return;
            }
            if (flow.previewState !== "decoding") {
                return;
            }
            if (StreamPlayerController.hasResult) {
                flow.previewState = "ready";
            } else {
                flow.previewState = "failed";
                flow.previewError = StreamPlayerController.error;
            }
        }
    }

    function chooseListening(value) {
        flow.listening = value;
        StreamPlayerController.karaokeReproduction = value;
    }

    function chooseVocals(value) {
        flow.vocalsChoice = value;
        StreamPlayerController.karaokeVocals = value;
    }

    // What each speaker plays under the choice made.
    readonly property var outputs: KaraokeAuthoring.outputs(flow.plan.melody, flow.plan.vocals,
                                                           flow.listening, flow.vocalsChoice, false)

    function joinedParts(parts) {
        return parts.length > 0 ? parts.join(" + ") : qsTr("silent");
    }

    function listeningText() {
        if (flow.listening === "asCoded") {
            return qsTr("As a plain surround player plays it: the vocals come from the rear speakers.");
        }
        if (flow.listening === "aware") {
            return qsTr("As a karaoke-aware decoder plays it: the vocals at the surround level you chose "
                        + "(Table C.2.2).");
        }
        if (flow.plan.vocals === 0) {
            return qsTr("This stream has no vocals to choose between.");
        }
        switch (flow.vocalsChoice) {
        case "none": return qsTr("No vocals: the backing and the melody guide, for singing along.");
        case "v1": return qsTr("Vocal 1 alone: a single vocal sits in the centre with the melody (Table C.2.3).");
        case "v2": return flow.plan.vocals >= 2
                          ? qsTr("Vocal 2 alone: a single vocal sits in the centre with the melody (Table C.2.3).")
                          : qsTr("This stream has no Vocal 2, so nothing is played.");
        }
        return qsTr("Both vocals: Vocal 1 on the left and Vocal 2 on the right.");
    }

    function formatTime(seconds) {
        return KaraokeAuthoring.durationText(seconds);
    }

    // ---- entering a step -------------------------------------------------------
    onCurrentStepKeyChanged: {
        if (flow.currentStepKey === "hear") {
            flow.buildPreview();
        } else {
            StreamPlayerController.pause();
        }
        resetContentScroll();
    }

    function resetContentScroll() {
        if (contentFlickable) {
            contentFlickable.contentY = 0;
        }
    }
    readonly property var contentFlickable: flowScroll.contentItem

    // Entering the tier loads whatever was chosen last, and an Expert edit made
    // meanwhile is what a rebuild would overwrite - so it only runs when asked.
    Component.onCompleted: {
        if (flow.loadedParts.length === 0 && EncoderController.sourceModel.length === 0) {
            flow.refreshPlan();
        }
    }

    // ---- step bar ----------------------------------------------------------
    RowLayout {
        Layout.fillWidth: true
        Layout.leftMargin: 24
        Layout.rightMargin: 24
        Layout.topMargin: Theme.space3
        Layout.bottomMargin: Theme.space3
        spacing: 0

        Repeater {
            model: flow.steps

            delegate: RowLayout {
                id: stepEntry
                required property var modelData
                required property int index
                readonly property bool current: index === flow.currentStepIndex
                readonly property bool completed: index < flow.currentStepIndex

                Layout.fillWidth: true
                spacing: 8

                Rectangle {
                    objectName: "karaokeStepDot-" + stepEntry.modelData.key
                    width: 22
                    height: 22
                    color: stepEntry.current ? Theme.accent
                           : stepEntry.completed ? Theme.text : "transparent"
                    border.color: stepEntry.current ? Theme.accent : Theme.divider
                    border.width: 1

                    Text {
                        anchors.centerIn: parent
                        text: stepEntry.index + 1
                        font.pixelSize: Theme.fontMono
                        font.family: Theme.monoFamily
                        color: stepEntry.current || stepEntry.completed ? Theme.bg : Theme.text
                    }
                    Accessible.role: Accessible.PageTab
                    Accessible.name: stepEntry.modelData.label
                    Accessible.selected: stepEntry.current
                    Accessible.onPressAction: flow.goTo(stepEntry.index)
                    MouseArea {
                        anchors.fill: parent
                        onClicked: flow.goTo(stepEntry.index)
                    }
                }
                Text {
                    text: stepEntry.modelData.label
                    font.pixelSize: Theme.fontBody
                    font.weight: stepEntry.current ? Font.DemiBold : Font.Normal
                    color: Theme.text
                    opacity: stepEntry.current ? 1.0 : 0.6
                }
                Rectangle {
                    visible: stepEntry.index < flow.steps.length - 1
                    Layout.fillWidth: true
                    Layout.leftMargin: 6
                    Layout.rightMargin: 14
                    Layout.preferredHeight: 1
                    color: Theme.divider
                }
            }
        }
    }
    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 2; color: Theme.divider }

    // A step is reached by Next, or by its dot once the parts can be built; a
    // step ahead of a plan that cannot be built is not offered.
    function goTo(index) {
        if (index > 0 && !flow.plan.buildable) {
            return;
        }
        flow.currentStepKey = flow.steps[index].key;
    }

    // ---- step content ---------------------------------------------------------
    ScrollView {
        id: flowScroll
        objectName: "karaokeScroll"
        Layout.fillWidth: true
        Layout.fillHeight: true
        contentWidth: availableWidth

        ColumnLayout {
            width: parent ? parent.width : 0
            spacing: 0

            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 24
                Layout.rightMargin: 24
                Layout.topMargin: Theme.space4
                Layout.bottomMargin: Theme.space4
                spacing: Theme.space4

                // ================= 1 · Parts =================
                ColumnLayout {
                    objectName: "karaokePartsStep"
                    visible: flow.currentStepKey === "parts"
                    Layout.fillWidth: true
                    spacing: Theme.space4

                    Text {
                        text: qsTr("Which recordings make the song?")
                        font.pixelSize: Math.round(30 * Theme.fontScale)
                        font.family: Theme.headingFamily
                        font.weight: Font.ExtraBold
                        color: Theme.text
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Give each part its own file. The stream is built from what you supply: "
                                   + "a melody guide and one or two vocals go in the places a centre and "
                                   + "surrounds would be, and the backing stays left and right.")
                        wrapMode: Text.WordWrap
                        font.pixelSize: Theme.fontNormal
                        color: Theme.neutral700
                    }

                    SegmentedControl {
                        objectName: "karaokeInputKind"
                        accessibleName: qsTr("Where the parts come from")
                        model: [
                            { value: "parts", label: qsTr("Separate files") },
                            { value: "file", label: qsTr("One multichannel file") }
                        ]
                        currentValue: flow.inputKind
                        onSelected: (value) => flow.setInputKind(value)
                    }

                    // ---- separate files: one row for each part -----------------
                    Repeater {
                        model: flow.inputKind === "parts"
                               ? [
                                   { part: "backing", what: qsTr("stereo · L R"), optional: false },
                                   { part: "melody", what: qsTr("mono · M"), optional: true },
                                   { part: "vocal1", what: qsTr("mono · V1"), optional: true },
                                   { part: "vocal2", what: qsTr("mono · V2"), optional: true }
                                 ]
                               : []
                        delegate: Rectangle {
                            id: partRow
                            required property var modelData
                            readonly property string url: flow.urlOf(modelData.part)
                            readonly property int rowIndex: {
                                for (let i = 0; i < flow.loadedParts.length; ++i) {
                                    if (flow.loadedParts[i].part === modelData.part) return i;
                                }
                                return -1;
                            }
                            readonly property var row: rowIndex >= 0 && rowIndex < EncoderController.sourceModel.length
                                                       ? EncoderController.sourceModel[rowIndex] : null
                            objectName: "karaokePart-" + modelData.part
                            Layout.fillWidth: true
                            Layout.preferredHeight: 64
                            color: "transparent"
                            border.color: url.length > 0 ? Theme.text : Theme.divider
                            border.width: 1

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: Theme.space4
                                anchors.rightMargin: Theme.space4
                                spacing: Theme.space3

                                ColumnLayout {
                                    Layout.preferredWidth: Math.round(200 * Theme.fontScale)
                                    spacing: 2
                                    Text {
                                        text: flow.partName(partRow.modelData.part)
                                        font.pixelSize: Theme.fontHeading
                                        font.weight: Font.DemiBold
                                        color: Theme.text
                                    }
                                    Text {
                                        text: partRow.modelData.optional
                                              ? qsTr("(optional) %1").arg(partRow.modelData.what)
                                              : partRow.modelData.what
                                        font.pixelSize: Theme.fontSmall
                                        color: Theme.neutral700
                                    }
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2
                                    Text {
                                        objectName: "karaokePartFile-" + partRow.modelData.part
                                        Layout.fillWidth: true
                                        text: partRow.url.length > 0
                                              ? flow.fileNameOf(partRow.url) : qsTr("not chosen")
                                        font.pixelSize: Theme.fontNormal
                                        font.family: partRow.url.length > 0 ? Theme.monoFamily
                                                                            : Theme.headingFamily
                                        color: partRow.url.length > 0 ? Theme.text : Theme.textMuted
                                        elide: Text.ElideMiddle
                                    }
                                    Text {
                                        visible: partRow.row !== null
                                        text: partRow.row !== null
                                              ? qsTr("%1 kHz · %2 · %3 channel(s)")
                                                    .arg(partRow.row.rate / 1000)
                                                    .arg(partRow.row.duration).arg(partRow.row.channels)
                                              : ""
                                        font.pixelSize: Theme.fontSmall
                                        font.family: Theme.monoFamily
                                        color: Theme.neutral700
                                    }
                                }
                                Button {
                                    objectName: "karaokeChoose-" + partRow.modelData.part
                                    text: partRow.url.length > 0 ? qsTr("Change") : qsTr("Choose…")
                                    onClicked: flow.openPicker(partRow.modelData.part)
                                }
                                Button {
                                    objectName: "karaokeRemove-" + partRow.modelData.part
                                    visible: partRow.url.length > 0 && partRow.modelData.optional
                                    text: qsTr("Remove")
                                    onClicked: flow.setUrl(partRow.modelData.part, "")
                                }
                            }
                        }
                    }

                    // ---- one file whose speakers name its channels ----------------
                    Rectangle {
                        objectName: "karaokeSingleFile"
                        visible: flow.inputKind === "file"
                        Layout.fillWidth: true
                        Layout.preferredHeight: 72
                        color: "transparent"
                        border.color: flow.singleUrl.length > 0 ? Theme.text : Theme.divider
                        border.width: 1
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.space4
                            anchors.rightMargin: Theme.space4
                            spacing: Theme.space3
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text {
                                    text: qsTr("One multichannel file")
                                    font.pixelSize: Theme.fontHeading
                                    font.weight: Font.DemiBold
                                    color: Theme.text
                                }
                                Text {
                                    objectName: "karaokeSingleFileName"
                                    Layout.fillWidth: true
                                    text: flow.singleUrl.length > 0
                                          ? flow.fileNameOf(flow.singleUrl)
                                          : qsTr("A WAV whose speakers are already named L C R Ls Rs "
                                                 + "(or L R S, L C R S, L R Ls Rs).")
                                    wrapMode: Text.WordWrap
                                    font.pixelSize: Theme.fontSmall
                                    color: Theme.neutral700
                                }
                            }
                            Button {
                                objectName: "karaokeChoose-file"
                                text: flow.singleUrl.length > 0 ? qsTr("Change") : qsTr("Choose…")
                                onClicked: flow.openPicker("file")
                            }
                        }
                    }
                }

                // ================= 2 · Levels =================
                ColumnLayout {
                    objectName: "karaokeLevelsStep"
                    visible: flow.currentStepKey === "levels"
                    Layout.fillWidth: true
                    spacing: Theme.space4

                    Text {
                        text: qsTr("How loud do the melody and the vocals sit?")
                        font.pixelSize: Math.round(30 * Theme.fontScale)
                        font.family: Theme.headingFamily
                        font.weight: Font.ExtraBold
                        color: Theme.text
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("These are the two levels Annex C reads from the stream. A karaoke-aware "
                                   + "decoder mixes the melody guide and the vocals with them; a "
                                   + "karaoke-capable one lets the singer choose the vocals but still folds "
                                   + "with them.")
                        wrapMode: Text.WordWrap
                        font.pixelSize: Theme.fontNormal
                        color: Theme.neutral700
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.space3
                        enabled: flow.plan.melody
                        opacity: enabled ? 1.0 : 0.45
                        Text {
                            Layout.preferredWidth: Math.round(200 * Theme.fontScale)
                            text: qsTr("Centre level (melody)")
                            font.pixelSize: Theme.fontNormal
                            color: Theme.text
                        }
                        SegmentedControl {
                            objectName: "karaokeCentreLevel"
                            accessibleName: qsTr("Centre level")
                            model: {
                                const names = EncoderController.cmixNames;
                                const out = [];
                                for (let i = 0; i < names.length; ++i) {
                                    out.push({ value: String(i), label: names[i] });
                                }
                                return out;
                            }
                            currentValue: String(EncoderController.cmixIndex)
                            onSelected: (value) => EncoderController.cmixIndex = Number(value)
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: flow.plan.melody
                              ? qsTr("cmixlev: how loud the melody guide M is in a stereo fold. The aware decoder "
                                     + "applies it to M at 2/0 (Table C.2.2).")
                              : qsTr("This stream has no melody guide, so there is no centre level.")
                        wrapMode: Text.WordWrap
                        font.pixelSize: Theme.fontSmall
                        color: Theme.textMuted
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.space3
                        enabled: flow.plan.vocals > 0
                        opacity: enabled ? 1.0 : 0.45
                        Text {
                            Layout.preferredWidth: Math.round(200 * Theme.fontScale)
                            text: qsTr("Surround level (vocals)")
                            font.pixelSize: Theme.fontNormal
                            color: Theme.text
                        }
                        SegmentedControl {
                            objectName: "karaokeSurroundLevel"
                            accessibleName: qsTr("Surround level")
                            model: {
                                const names = EncoderController.surmixNames;
                                const out = [];
                                for (let i = 0; i < names.length; ++i) {
                                    out.push({ value: String(i), label: names[i] });
                                }
                                return out;
                            }
                            currentValue: String(EncoderController.surmixIndex)
                            onSelected: (value) => EncoderController.surmixIndex = Number(value)
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: flow.plan.vocals > 0
                              ? qsTr("surmixlev: how loud the vocals V1 and V2 are in the aware decoder's "
                                     + "reproduction and in a stereo fold (Table C.2.2).")
                              : qsTr("This stream has no vocals, so there is no surround level.")
                        wrapMode: Text.WordWrap
                        font.pixelSize: Theme.fontSmall
                        color: Theme.textMuted
                    }
                }

                // ================= 3 · Hear =================
                ColumnLayout {
                    objectName: "karaokeHearStep"
                    visible: flow.currentStepKey === "hear"
                    Layout.fillWidth: true
                    spacing: Theme.space4

                    Text {
                        text: qsTr("Hear it the way a singer will")
                        font.pixelSize: Math.round(30 * Theme.fontScale)
                        font.family: Theme.headingFamily
                        font.weight: Font.ExtraBold
                        color: Theme.text
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("This plays the stream you just built, decoded the way a karaoke player "
                                   + "decodes it: not your source files. Try each choice a singer has.")
                        wrapMode: Text.WordWrap
                        font.pixelSize: Theme.fontNormal
                        color: Theme.neutral700
                    }

                    Text {
                        objectName: "karaokePreviewStatus"
                        Layout.fillWidth: true
                        visible: flow.previewState !== "ready"
                        text: (flow.previewState === "encoding" || flow.previewState === "decoding")
                              ? qsTr("Building the stream…")
                              : flow.previewState === "failed"
                                ? qsTr("Could not build the stream: %1").arg(flow.previewError)
                                : qsTr("The stream is built when you reach this step.")
                        wrapMode: Text.WordWrap
                        font.pixelSize: Theme.fontNormal
                        color: Theme.text
                    }

                    ColumnLayout {
                        visible: flow.previewState === "ready"
                        Layout.fillWidth: true
                        spacing: Theme.space3

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.space3
                            Text {
                                Layout.preferredWidth: Math.round(110 * Theme.fontScale)
                                text: qsTr("Player")
                                font.pixelSize: Theme.fontNormal
                                color: Theme.text
                            }
                            SegmentedControl {
                                objectName: "karaokeListening"
                                accessibleName: qsTr("Player")
                                model: [
                                    { value: "asCoded", label: qsTr("As coded") },
                                    { value: "aware", label: qsTr("Aware") },
                                    { value: "capable", label: qsTr("Capable") }
                                ]
                                currentValue: flow.listening
                                onSelected: (value) => flow.chooseListening(value)
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.space3
                            enabled: flow.listening === "capable" && flow.plan.vocals > 0
                            opacity: enabled ? 1.0 : 0.45
                            Text {
                                Layout.preferredWidth: Math.round(110 * Theme.fontScale)
                                text: qsTr("Vocals")
                                font.pixelSize: Theme.fontNormal
                                color: Theme.text
                            }
                            SegmentedControl {
                                objectName: "karaokeVocals"
                                accessibleName: qsTr("Vocals")
                                model: [
                                    { value: "none", label: qsTr("None") },
                                    { value: "v1", label: qsTr("V1") },
                                    { value: "v2", label: qsTr("V2") },
                                    { value: "both", label: qsTr("Both") }
                                ]
                                currentValue: flow.vocalsChoice
                                onSelected: (value) => flow.chooseVocals(value)
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.space3
                            Button {
                                objectName: "karaokePlay"
                                text: StreamPlayerController.playing ? qsTr("Pause") : qsTr("Play")
                                enabled: StreamPlayerController.hasResult && !StreamPlayerController.busy
                                onClicked: StreamPlayerController.playing
                                           ? StreamPlayerController.pause() : StreamPlayerController.play()
                            }
                            Text {
                                objectName: "karaokePosition"
                                text: qsTr("%1 / %2")
                                          .arg(flow.formatTime(StreamPlayerController.positionSeconds))
                                          .arg(flow.formatTime(StreamPlayerController.durationSeconds))
                                font.pixelSize: Theme.fontSmall
                                font.family: Theme.monoFamily
                                color: Theme.text
                            }
                            Item { Layout.fillWidth: true }
                        }

                        Text {
                            objectName: "karaokeListeningText"
                            Layout.fillWidth: true
                            text: flow.listeningText()
                            wrapMode: Text.WordWrap
                            font.pixelSize: Theme.fontSmall
                            color: Theme.neutral700
                        }

                        // What plays: the stream's parts, speaker by speaker.
                        Rectangle {
                            objectName: "karaokeWhatPlays"
                            Layout.fillWidth: true
                            Layout.preferredHeight: playsColumn.implicitHeight + Theme.space4 * 2
                            color: "transparent"
                            border.color: Theme.divider
                            border.width: 1
                            ColumnLayout {
                                id: playsColumn
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.margins: Theme.space4
                                spacing: Theme.space2
                                Text {
                                    text: qsTr("WHAT PLAYS")
                                    font.pixelSize: Theme.fontMono
                                    font.letterSpacing: 1.2
                                    color: Theme.textMuted
                                }
                                Repeater {
                                    model: {
                                        const rows = [
                                            { speaker: "L", parts: flow.outputs.l },
                                            { speaker: "C", parts: flow.outputs.c },
                                            { speaker: "R", parts: flow.outputs.r }
                                        ];
                                        if (flow.listening === "asCoded") {
                                            rows.push({ speaker: "Ls", parts: flow.outputs.ls });
                                            rows.push({ speaker: "Rs", parts: flow.outputs.rs });
                                        }
                                        return rows;
                                    }
                                    delegate: RowLayout {
                                        required property var modelData
                                        Layout.fillWidth: true
                                        spacing: Theme.space3
                                        Text {
                                            Layout.preferredWidth: Math.round(40 * Theme.fontScale)
                                            text: modelData.speaker
                                            font.family: Theme.monoFamily
                                            font.pixelSize: Theme.fontNormal
                                            font.weight: Font.DemiBold
                                            color: Theme.text
                                        }
                                        Text {
                                            objectName: "karaokePlays-" + modelData.speaker
                                            Layout.fillWidth: true
                                            text: flow.joinedParts(modelData.parts)
                                            font.pixelSize: Theme.fontNormal
                                            color: Theme.text
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // ================= 4 · Save =================
                ColumnLayout {
                    objectName: "karaokeSaveStep"
                    visible: flow.currentStepKey === "save"
                    Layout.fillWidth: true
                    spacing: Theme.space4

                    Text {
                        text: qsTr("Save the stream")
                        font.pixelSize: Math.round(30 * Theme.fontScale)
                        font.family: Theme.headingFamily
                        font.weight: Font.ExtraBold
                        color: Theme.text
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("%1 · karaoke %2 · %3 kbps").arg(qsTr("AC-3")).arg(flow.plan.mode)
                                  .arg(EncoderController.bitrateKbps)
                        font.pixelSize: Theme.fontNormal
                        font.family: Theme.monoFamily
                        color: Theme.text
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.space3
                        Text {
                            Layout.preferredWidth: Math.round(110 * Theme.fontScale)
                            text: qsTr("File type")
                            font.pixelSize: Theme.fontNormal
                            color: Theme.text
                        }
                        ComboBox {
                            objectName: "karaokeContainer"
                            Layout.preferredWidth: Math.round(260 * Theme.fontScale)
                            // The container the encoder page offers today: the
                            // raw stream, Matroska, MP4 and MPEG-TS.
                            model: EncoderController.containerNames
                            currentIndex: EncoderController.containerIndex
                            onActivated: (index) => EncoderController.containerIndex = index
                            Accessible.name: qsTr("File type")
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.space3
                        Text {
                            Layout.preferredWidth: Math.round(110 * Theme.fontScale)
                            text: qsTr("Bit rate")
                            font.pixelSize: Theme.fontNormal
                            color: Theme.text
                        }
                        ComboBox {
                            objectName: "karaokeBitrate"
                            Layout.preferredWidth: Math.round(160 * Theme.fontScale)
                            model: EncoderController.bitrates
                            currentIndex: EncoderController.bitrates.indexOf(EncoderController.bitrateKbps)
                            onActivated: (index) => {
                                flow.rateChosen = true;
                                EncoderController.bitrateKbps = EncoderController.bitrates[index];
                            }
                            Accessible.name: qsTr("Bit rate")
                        }
                        Text {
                            text: qsTr("kbps")
                            font.pixelSize: Theme.fontSmall
                            color: Theme.textMuted
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.space3
                        Text {
                            Layout.preferredWidth: Math.round(110 * Theme.fontScale)
                            text: qsTr("Lyrics")
                            font.pixelSize: Theme.fontNormal
                            color: Theme.text
                        }
                        Text {
                            objectName: "karaokeLyricsFile"
                            Layout.fillWidth: true
                            text: flow.lyricsUrl.length > 0 ? flow.fileNameOf(flow.lyricsUrl)
                                                            : qsTr("none")
                            elide: Text.ElideMiddle
                            font.pixelSize: Theme.fontNormal
                            font.family: flow.lyricsUrl.length > 0 ? Theme.monoFamily
                                                                   : Theme.headingFamily
                            color: flow.lyricsUrl.length > 0 ? Theme.text : Theme.textMuted
                        }
                        Button {
                            objectName: "karaokeChooseLyrics"
                            text: flow.lyricsUrl.length > 0 ? qsTr("Change") : qsTr("Choose…")
                            onClicked: lyricsDialog.open()
                        }
                        Button {
                            objectName: "karaokeRemoveLyrics"
                            visible: flow.lyricsUrl.length > 0
                            text: qsTr("Remove")
                            onClicked: {
                                flow.lyricsChosen = true;
                                flow.lyricsUrl = "";
                            }
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("An .lrc file is put beside the stream under the stream's name, where "
                                   + "Hearth finds it. One of the backing's name beside it is offered.")
                        wrapMode: Text.WordWrap
                        font.pixelSize: Theme.fontSmall
                        color: Theme.textMuted
                    }
                    Text {
                        objectName: "karaokeLyricsNote"
                        Layout.fillWidth: true
                        visible: text.length > 0
                        text: flow.lyricsNote
                        wrapMode: Text.WordWrap
                        font.pixelSize: Theme.fontSmall
                        color: Theme.neutral700
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: lineColumn.implicitHeight + Theme.space4 * 2
                        color: "transparent"
                        border.color: Theme.divider
                        border.width: 1
                        ColumnLayout {
                            id: lineColumn
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: Theme.space4
                            spacing: Theme.space2
                            Text {
                                text: qsTr("THE SAME FROM THE COMMAND LINE")
                                font.pixelSize: Theme.fontMono
                                font.letterSpacing: 1.2
                                color: Theme.textMuted
                            }
                            Text {
                                objectName: "karaokeCommandLine"
                                Layout.fillWidth: true
                                text: flow.appWindow ? flow.appWindow.cliLine : ""
                                wrapMode: Text.WrapAnywhere
                                font.pixelSize: Theme.fontSmall
                                font.family: Theme.monoFamily
                                color: Theme.text
                            }
                        }
                    }
                    Text {
                        objectName: "karaokeEncodeStatus"
                        Layout.fillWidth: true
                        text: EncoderController.status
                        wrapMode: Text.WordWrap
                        font.pixelSize: Theme.fontSmall
                        color: Theme.neutral700
                    }
                }
            }
        }
    }

    // ---- the coding mode, and the checks ------------------------------
    // Pinned under the step, not in its scroll: the mode is the answer to what was
    // supplied, and at 900 px it sat below the fold.
    Rectangle {
        objectName: "karaokeMode"
        visible: flow.currentStepKey === "parts"
        Layout.fillWidth: true
        Layout.leftMargin: 24
        Layout.rightMargin: 24
        Layout.bottomMargin: Theme.space3
        Layout.preferredHeight: modeColumn.implicitHeight + Theme.space4 * 2
        color: "transparent"
        border.color: Theme.divider
        border.width: 1

        ColumnLayout {
            id: modeColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Theme.space4
            spacing: Theme.space2

            // The mode and what it was chosen from on one line; the checks run on beside one
            // another, so the card is a few lines tall and the rows above it stay in view.
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space3
                Text {
                    text: qsTr("CODING MODE")
                    font.pixelSize: Theme.fontMono
                    font.letterSpacing: 1.2
                    color: Theme.textMuted
                }
                Text {
                    objectName: "karaokeModeLabel"
                    text: flow.plan.mode.length > 0
                          ? qsTr("%1 · %2").arg(flow.plan.mode).arg(flow.partsText())
                          : qsTr("not yet")
                    font.pixelSize: Theme.fontHeading
                    font.family: Theme.monoFamily
                    font.weight: Font.DemiBold
                    color: Theme.text
                }
                Text {
                    objectName: "karaokeModeReason"
                    Layout.fillWidth: true
                    visible: text.length > 0
                    text: flow.reasonText()
                    elide: Text.ElideRight
                    font.pixelSize: Theme.fontSmall
                    color: Theme.neutral700
                }
            }

            Flow {
                Layout.fillWidth: true
                spacing: Theme.space3

                // A file that would not load says why.
                Repeater {
                    model: flow.failedParts
                    delegate: Text {
                        required property var modelData
                        objectName: "karaokeCheck-load"
                        width: Math.min(implicitWidth, parent ? parent.width : implicitWidth)
                        text: "✕ " + modelData.text
                        wrapMode: Text.WordWrap
                        font.pixelSize: Theme.fontSmall
                        color: Theme.text
                    }
                }
                Repeater {
                    model: flow.plan.checks
                    delegate: Text {
                        required property var modelData
                        objectName: "karaokeCheck-" + modelData.code
                        width: Math.min(implicitWidth, parent ? parent.width : implicitWidth)
                        text: (modelData.level === "ok" ? "✓ "
                               : modelData.level === "warning" ? "! " : "✕ ")
                              + flow.checkText(modelData)
                        wrapMode: Text.WordWrap
                        font.pixelSize: Theme.fontSmall
                        color: modelData.level === "problem" ? Theme.text : Theme.neutral700
                        font.weight: modelData.level === "problem" ? Font.DemiBold : Font.Normal
                    }
                }
                Text {
                    width: Math.min(implicitWidth, parent ? parent.width : implicitWidth)
                    text: qsTr("No LFE: karaoke streams made here carry none.")
                    wrapMode: Text.WordWrap
                    font.pixelSize: Theme.fontSmall
                    color: Theme.textMuted
                }
            }
        }
    }

    // ---- the assistant line and Back / Next ---------------------------------
    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }
    RowLayout {
        Layout.fillWidth: true
        Layout.leftMargin: 24
        Layout.rightMargin: 24
        Layout.topMargin: Theme.space3
        Layout.bottomMargin: Theme.space4
        spacing: Theme.space3

        Button {
            objectName: "karaokeBackButton"
            text: qsTr("Back")
            enabled: flow.currentStepIndex > 0
            onClicked: flow.currentStepKey = flow.steps[flow.currentStepIndex - 1].key
        }

        Text {
            Layout.fillWidth: true
            text: flow.currentStep.assistant
            font.pixelSize: Theme.fontSmall
            elide: Text.ElideRight
            color: Theme.textMuted
        }

        Button {
            objectName: "karaokeNextButton"
            highlighted: true
            text: flow.currentStep.next
            enabled: flow.currentStepIndex < flow.steps.length - 1
                     ? flow.plan.buildable && !EncoderController.busy
                     : flow.plan.buildable && EncoderController.sourceReady && !EncoderController.busy
            onClicked: {
                if (flow.currentStepIndex < flow.steps.length - 1) {
                    flow.currentStepKey = flow.steps[flow.currentStepIndex + 1].key;
                    return;
                }
                // Encode now: the run strip's chip and Cancel are the feedback,
                // as for Guided.
                if (flow.appWindow) {
                    flow.appWindow.startEncodeFlow();
                }
            }
        }
    }

    // ---- choosing a file -------------------------------------------------------
    property string pickerPart: ""
    function openPicker(part) {
        flow.pickerPart = part;
        partDialog.open();
    }
    FileDialog {
        id: lyricsDialog
        objectName: "karaokeLyricsDialog"
        title: qsTr("Choose the lyrics")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Lyrics (*.lrc)"), qsTr("All files (*)")]
        onAccepted: {
            flow.lyricsChosen = true;
            flow.lyricsUrl = selectedFile.toString();
        }
    }
    FileDialog {
        id: partDialog
        objectName: "karaokePartDialog"
        title: qsTr("Choose %1").arg(flow.partName(flow.pickerPart))
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("WAV audio (*.wav)"), qsTr("All files (*)")]
        onAccepted: flow.setUrl(flow.pickerPart, selectedFile.toString())
    }
}
