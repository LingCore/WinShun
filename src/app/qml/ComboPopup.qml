pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import WinShun

Popup {
    id: popup

    required property ComboBox control

    y: control.height + 4
    width: control.width
    height: Math.min(280, contentItem.implicitHeight + topPadding + bottomPadding)
    topMargin: 6
    bottomMargin: 6
    padding: Theme.menuPadding
    font: control.font
    palette: control.palette

    contentItem: Item {
        readonly property alias list: rows // the rows, beside their scroll bar (as FontPicker's)
        implicitHeight: rows.contentHeight
        ListView {
            id: rows
            anchors.fill: parent
            clip: true
            model: popup.control.delegateModel
            currentIndex: popup.control.highlightedIndex
            highlightMoveDuration: 0
            boundsBehavior: Flickable.StopAtBounds
        }
        ListScrollBar {
            flickable: rows
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.right: parent.right
            dpr: popup.control.Screen.devicePixelRatio
        }
    }

    background: Hook {
        part: Theme.menuFrame
        dpr: popup.control.Screen.devicePixelRatio
    }
}
