import QtQuick

import Hearth

// The clock a lyric view runs on (planning/karaoke-design.md, "Lyrics and the
// clock").
//
// HearthController polls the engine every 60 ms and publishes where the item
// has got to (`positionMs`, which is what is heard: the device's clock less its
// output path's delay). A word's fill moves far more often than that, so this
// keeps its own reading: the last position published, plus the time since it
// arrived while the transport is playing, advanced each frame and never more
// than `maxAheadMs` (two polls) past the reading, so that a stalled poll cannot
// run the words away. Every poll snaps it to the position read, and so does a
// pause, a seek, an item change and a stop: they are only another position.
//
// `live` is the one switch a test needs: with it off the clock reads `time`
// (milliseconds) instead of the wall clock, and nothing moves until the test
// says so.
QtObject {
    id: root

    // The position last read, in milliseconds from the start of the item. Set
    // through poll() so that the time it arrived is kept with it.
    property real polledMs: 0
    // Whether the transport is playing; a paused clock is the reading.
    property bool running: HearthController.playing
    // Two polls, at HearthController's 60 ms.
    property real maxAheadMs: 120
    // Whether the clock advances by itself: a view that is not shown needs no
    // frames.
    property bool active: true
    // The wall clock drives `time` while this is on.
    property bool live: true
    // Milliseconds, on whatever scale the caller keeps; the wall clock's when
    // live.
    property real time: 0
    // `time` when the reading arrived.
    property real polledAt: 0

    // What a view draws against.
    readonly property real positionMs: root.running
        ? root.polledMs + Math.max(0, Math.min(root.time - root.polledAt, root.maxAheadMs))
        : root.polledMs

    function stamp() {
        return root.live ? Date.now() : root.time;
    }

    // A new reading: the position the engine reports, as of now.
    function poll(ms) {
        const now = root.stamp();
        root.polledAt = now;
        root.time = now;
        root.polledMs = ms;
    }

    onRunningChanged: {
        // The reading is as of now, whenever the last poll was.
        const now = root.stamp();
        root.polledAt = now;
        root.time = now;
    }

    // Declared as properties rather than as data children, so that the
    // QtObject's own default property is left alone.
    property Connections feed: Connections {
        target: HearthController
        function onPositionChanged() { root.poll(HearthController.positionMs); }
    }

    property FrameAnimation frames: FrameAnimation {
        running: root.live && root.active && root.running
        onTriggered: root.time = Date.now()
    }

    Component.onCompleted: root.poll(HearthController.positionMs)
}
