import QtQuick
import QtQuick.Controls

import ForgeGui

// The family's "something is happening" mark: the style's own spinner, or -
// when Theme.reducedMotion is on - the same footprint drawn still, as three
// squares. A spinner is rotation that never stops while the work lasts, which
// is exactly what a person who asked for less motion asked not to see, and
// nothing in the style's BusyIndicator can be told to hold still short of
// hiding itself (it then says nothing at all, and several of the places that
// use it carry no text of their own beside it).
//
// The surface is BusyIndicator's: `running`, `visible`, the implicit size and
// Accessible.* are set by the caller as they were on the type this replaces.
// Zero radius, as everything in the family is.
Item {
    id: root
    property bool running: false

    implicitWidth: 24
    implicitHeight: 24

    Accessible.role: Accessible.Indicator

    BusyIndicator {
        anchors.fill: parent
        visible: !Theme.reducedMotion
        running: root.running && !Theme.reducedMotion
    }

    Row {
        objectName: "busyMarkStill"
        anchors.centerIn: parent
        visible: Theme.reducedMotion && root.running
        spacing: Math.max(2, Math.round(root.width / 8))
        Repeater {
            model: 3
            Rectangle {
                width: Math.max(4, Math.round(Math.min(root.width, root.height) / 5))
                height: width
                color: Theme.neutral600
            }
        }
    }
}
