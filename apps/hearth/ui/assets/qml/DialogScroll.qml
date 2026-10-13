import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Hearth

// The body of a dialog that may be taller than the window it opens in:
// Keyboard shortcuts, About and First run are text, and text grows with the
// text size (175% sets it 1.75 times as high) and with a face that stands
// taller than the Latin one (Arabic and Hebrew). A dialog that sizes itself to
// its text then runs off the top and bottom of the window and takes its
// buttons with it.
//
// This goes in the dialog's own column, above the row of buttons, and takes
// the height that is left: what is in it scrolls, and the buttons stay where
// they can be reached. At the height it asks for nothing scrolls and nothing
// is different, so a dialog that fits looks as it did. The dialog caps its own
// height at the window's (`height:` in each dialog).
//
// What goes inside is laid out as a column. Page Up, Page Down, the arrow
// keys, Home and End scroll it from wherever the focus is in the dialog: the
// dialog hands them on from its content item (`handleKey`), since the focus is
// on a button and not here.
ScrollView {
    id: root

    default property alias content: column.data
    property alias spacing: column.spacing

    // The height the column asks for, at the width there is.
    readonly property real wantedHeight: column.implicitHeight + root.topPadding + root.bottomPadding
    // Whether the column is taller than the room it has been given.
    readonly property bool overflowing: root.wantedHeight - root.height > 0.5

    // A bar is drawn over the end of the view the text is at, and takes no room of its own, so the
    // text is kept clear of it for as long as it shows. The end is the right of the view in a window
    // read left to right and the left of it in one read right to left.
    readonly property real barRoom: root.overflowing ? root.ScrollBar.vertical.width + Theme.space2 : 0

    Layout.fillWidth: true
    Layout.fillHeight: true
    Layout.preferredHeight: root.wantedHeight
    leftPadding: root.mirrored ? root.barRoom : 0
    rightPadding: root.mirrored ? 0 : root.barRoom
    contentWidth: root.availableWidth
    clip: true
    // A bar that is there while there is more to see, so that a person is told so: the
    // default one fades out when nothing is moving it.
    ScrollBar.vertical.policy: root.overflowing ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff

    ColumnLayout {
        id: column
        width: root.availableWidth
    }

    // Scrolls for a key the dialog was sent, and says whether it was one of these.
    function handleKey(event) {
        const flick = root.contentItem;
        const room = flick.contentHeight - flick.height;
        if (room <= 0) {
            return false;
        }
        const line = Math.round(Theme.fontNormal * 2);
        const page = Math.max(line, flick.height - line);
        let target = flick.contentY;
        switch (event.key) {
        case Qt.Key_Up: target -= line; break;
        case Qt.Key_Down: target += line; break;
        case Qt.Key_PageUp: target -= page; break;
        case Qt.Key_PageDown: target += page; break;
        case Qt.Key_Home: target = 0; break;
        case Qt.Key_End: target = room; break;
        default: return false;
        }
        flick.contentY = Math.max(0, Math.min(room, target));
        event.accepted = true;
        return true;
    }
}
