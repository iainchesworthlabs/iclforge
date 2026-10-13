import QtQuick

import Hearth

// The tune as a ribbon of notes and the singer's own pitch as a line over it, in time with the words
// (planning/karaoke-microphone.md, M5; "What the singer sees").
//
// Time runs left to right with "now" a little in from the left edge, so the notes about to be sung
// are ahead of the line being drawn. The singer's voice is drawn on the tune's octave, as it is
// scored: a voice an octave below is right, and the line is where the notes are. Where the voice is
// within the tolerance of the tune the line is thick and solid; where it is not it is thin and dashed,
// which is a second cue beside the one colour (L4). Where the singer is not voiced there is no line,
// and a plain "Listening…" says so.
//
// The picture is of data it is given (`notes`, `trace`), so a test hands it some. When `live` is set
// it asks MicrophoneController for the window it shows, a few times a frame at most and only while it
// is visible: nothing is built for a ribbon nobody can see.
Item {
    id: root

    // The reference's notes in the window, flat: start ms, end ms, MIDI note number.
    property var notes: []
    // The singer's placed frames in the window, flat: item ms, MIDI note number (-1: not voiced), and
    // 1 where within the tolerance of the tune.
    property var trace: []
    // The item's position, in milliseconds, and how much of it the ribbon shows.
    property real positionMs: 0
    property real windowMs: 6000
    // Where "now" sits, as a share of the width from the left.
    property real nowFraction: 0.3
    // The range of the whole tune, with room: the picture does not jump as notes come into view.
    property real lowMidi: 55
    property real highMidi: 79
    // Nothing voiced just now.
    property bool listening: false
    // Fed from MicrophoneController.
    property bool live: false

    property color noteColor: Theme.accent
    property color lineColor: Theme.text
    property color missColor: Theme.textMuted
    property color nowColor: Theme.textMuted

    readonly property real fromMs: root.positionMs - root.windowMs * root.nowFraction
    readonly property real toMs: root.fromMs + root.windowMs
    readonly property int noteCount: Math.floor(root.notes.length / 3)
    readonly property int tracePointCount: Math.floor(root.trace.length / 3)
    // How many frames of the trace are voiced, and how many of those are within the tolerance.
    readonly property int voicedCount: {
        let n = 0;
        for (let i = 0; i + 2 < root.trace.length; i += 3) {
            n += root.trace[i + 1] >= 0 ? 1 : 0;
        }
        return n;
    }
    readonly property int hitCount: {
        let n = 0;
        for (let i = 0; i + 2 < root.trace.length; i += 3) {
            n += (root.trace[i + 1] >= 0 && root.trace[i + 2] > 0) ? 1 : 0;
        }
        return n;
    }

    function xOf(ms) {
        return (ms - root.fromMs) / root.windowMs * root.width;
    }
    // The top of the range is the top of the picture; a little margin keeps a note's bar whole.
    function yOf(midi) {
        const span = Math.max(1, root.highMidi - root.lowMidi);
        const margin = root.height * 0.08;
        return margin + (1 - (midi - root.lowMidi) / span) * (root.height - 2 * margin);
    }
    // The note being sung now, or -1.
    function currentNote() {
        for (let i = 0; i + 2 < root.notes.length; i += 3) {
            if (root.positionMs >= root.notes[i] && root.positionMs < root.notes[i + 1]) {
                return i / 3;
            }
        }
        return -1;
    }

    function pull() {
        const data = MicrophoneController.ribbon(Math.floor(root.fromMs), Math.ceil(root.toMs));
        root.notes = data.notes ?? [];
        root.trace = data.trace ?? [];
        if (data.lowMidi !== undefined && data.highMidi !== undefined) {
            root.lowMidi = data.lowMidi;
            root.highMidi = data.highMidi;
        }
    }
    Timer {
        interval: 33
        repeat: true
        running: root.live && root.visible
        onTriggered: root.pull()
    }

    onNotesChanged: canvas.requestPaint()
    onTraceChanged: canvas.requestPaint()
    onPositionMsChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()
    onNoteColorChanged: canvas.requestPaint()
    onLineColorChanged: canvas.requestPaint()

    Accessible.role: Accessible.Graphic
    Accessible.name: qsTr("Pitch ribbon: the tune as notes, and your voice as a line over them")
    Accessible.description: root.listening ? qsTr("Listening") : ""

    Canvas {
        id: canvas
        objectName: "pitchRibbonCanvas"
        anchors.fill: parent
        onPaint: {
            const ctx = getContext("2d");
            ctx.reset();
            // The notes, the one being sung stronger.
            const current = root.currentNote();
            const barHeight = Math.max(4, root.height * 0.07);
            for (let i = 0; i + 2 < root.notes.length; i += 3) {
                const x0 = root.xOf(root.notes[i]);
                const x1 = root.xOf(root.notes[i + 1]);
                if (x1 < 0 || x0 > root.width) {
                    continue;
                }
                const y = root.yOf(root.notes[i + 2]);
                ctx.globalAlpha = (i / 3 === current) ? 0.85 : 0.4;
                ctx.fillStyle = root.noteColor;
                ctx.fillRect(x0, y - barHeight / 2, Math.max(2, x1 - x0 - 1), barHeight);
            }
            ctx.globalAlpha = 1.0;
            // Now.
            const nowX = root.xOf(root.positionMs);
            ctx.strokeStyle = root.nowColor;
            ctx.lineWidth = 1;
            ctx.setLineDash([]);
            ctx.beginPath();
            ctx.moveTo(nowX, 0);
            ctx.lineTo(nowX, root.height);
            ctx.stroke();
            // The voice: a segment between consecutive voiced frames that are close in time.
            let previousT = 0;
            let previousY = 0;
            let havePrevious = false;
            for (let i = 0; i + 2 < root.trace.length; i += 3) {
                const t = root.trace[i];
                const midi = root.trace[i + 1];
                const hit = root.trace[i + 2] > 0;
                if (midi < 0) {
                    havePrevious = false;
                    continue;
                }
                const x = root.xOf(t);
                const y = root.yOf(midi);
                if (havePrevious && t - previousT <= 60 && t >= previousT) {
                    ctx.strokeStyle = hit ? root.lineColor : root.missColor;
                    ctx.lineWidth = hit ? Math.max(3, root.height * 0.035) : Math.max(1.5, root.height * 0.015);
                    ctx.setLineDash(hit ? [] : [4, 3]);
                    ctx.beginPath();
                    ctx.moveTo(root.xOf(previousT), previousY);
                    ctx.lineTo(x, y);
                    ctx.stroke();
                }
                previousT = t;
                previousY = y;
                havePrevious = true;
            }
            ctx.setLineDash([]);
        }
    }

    Text {
        objectName: "pitchRibbonListening"
        visible: root.listening
        anchors.verticalCenter: parent.verticalCenter
        x: Math.round(root.width * root.nowFraction) + Math.round(8 * Theme.fontScale)
        // It sits beside the marker, and what is beyond the ribbon's edge is not drawn: in a
        // narrow card, at a large text size or in a language with a longer word, it is cut short
        // with an ellipsis and does not run out of the card.
        width: Math.max(0, root.width - x)
        elide: Text.ElideRight
        text: qsTr("Listening…")
        color: root.nowColor
        font.pixelSize: Theme.fontSmall
    }
}
