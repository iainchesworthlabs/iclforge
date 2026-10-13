import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import ForgeGui

// The third-party notices this build carries, as the text of the NOTICES.txt
// the package installs: EncoderController.licenceNotices reads the copy
// embedded at build time, so this view and the file cannot say different
// things. Reached from About > Licences…. The same dialog, in the same shape,
// as apps/crucible/ui/assets/qml/LicencesDialog.qml and
// apps/hearth/ui/assets/qml/LicencesDialog.qml, on this window's own
// plain Controls and AboutDialog.qml's own squared border.
Dialog {
    id: root
    objectName: "licencesDialog"
    modal: true
    // The dialog takes focus and hands it to Close as it opens, so Escape
    // reaches the dialog and closes it wherever the pointer is.
    focus: true
    onOpened: closeButton.forceActiveFocus()
    anchors.centerIn: parent
    width: Math.min(760, parent ? parent.width - 60 : 760)
    height: Math.min(640, parent ? parent.height - 60 : 640)
    padding: Theme.space6
    title: ""

    background: Rectangle {
        color: Theme.bg
        border.color: Theme.text
        border.width: 2
    }

    contentItem: ColumnLayout {
        spacing: Theme.space2

        // On the content, not the dialog: a Popup is not an Item, and
        // title is "" (the styled Text below draws the visible heading), so
        // the Dialog's own title-derived accessible name has nothing to read.
        Accessible.role: Accessible.Dialog
        Accessible.name: qsTr("Third-party licences")

        Text {
            text: qsTr("THIRD-PARTY LICENCES")
            font.pixelSize: Theme.fontMicro
            font.letterSpacing: 1.2
            color: Theme.textMuted
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            TextArea {
                objectName: "licenceNoticesText"
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.Wrap
                textFormat: TextEdit.PlainText
                font.family: Theme.monoFamily
                font.pixelSize: Theme.fontMono
                color: Theme.text
                background: null
                text: EncoderController.licenceNotices
            }
        }
        RowLayout {
            Layout.topMargin: Theme.space3
            Item { Layout.fillWidth: true }
            Button {
                id: closeButton
                objectName: "licencesCloseButton"
                text: qsTr("Close")
                highlighted: true
                onClicked: root.close()
            }
        }
    }
}
