import QtQuick
import QtQuick.Layouts

import Hearth

// The words of the item playing, on the Play page (planning/karaoke.md, "Found while
// building": a lyric view beside the queue). A singer who has put a song in the queue sees
// the line being sung without a trip to the Karaoke page, and the Play page's monitor does not
// have to be left to do it.
//
// There is a card only for an item that has something to show: a lyrics file, with times or
// without, or the picture of a CD+G disc. An ordinary stream gets nothing, no empty panel and
// no "no lyrics" line, so the page is what it was. The same view as the Karaoke page's
// (LyricView.qml, one controller, one delay) with a clock of its own, which only runs while
// this card is on screen.
//
// Placed in either of two spots, as PlayObjectsCard is, by PlayPage.qml's width: its own
// column between the queue and the monitor when there is room, and the top of the monitor
// column's scroll when there is not. Each is only ever shown in one place at a time.
Card {
    id: root

    // Asked for by the button, and answered by Main.qml with the singer view.
    signal fullScreenRequested()

    // Whether the item has anything to show. KaraokeController.status is "none" until a
    // LyricView has been asked for the item, and the view below is that request, so it is
    // always instantiated: it is the card that hides, not the view.
    readonly property bool hasLyrics: KaraokeController.status === "timed"
                                      || KaraokeController.status === "plain"
                                      || KaraokeController.status === "picture"
    // Only when placed and wanted: PlayPage.qml turns this off for the spot it does not use.
    property bool placed: true
    // The height the view is given.
    property int viewHeight: Math.round(320 * Theme.fontScale)

    visible: root.placed && root.hasLyrics
    title: qsTr("Lyrics")
    summary: KaraokeController.fileName
    flat: true

    LyricClock {
        id: clock
        active: root.visible
    }

    // Loads the lyrics of the queue's current item on its own (LyricView's `follow`), also
    // while the card is hidden: a hidden view still asks, and that is what moves the
    // controller's status off "none" and shows the card.
    LyricView {
        id: view
        objectName: "playLyricsView"
        Layout.fillWidth: true
        Layout.preferredHeight: root.viewHeight
        positionMs: clock.positionMs
    }

    AppButton {
        objectName: "playLyricsFullScreenButton"
        text: qsTr("Full screen · F11")
        onClicked: root.fullScreenRequested()
    }
}
